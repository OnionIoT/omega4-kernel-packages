// SPDX-License-Identifier: GPL-2.0-only

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/device/bus.h>
#include <linux/dma-mapping.h>
#include <linux/memremap.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/rockchip/rockchip_sip.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <asm/psci.h>

#define OMEGA4_HPMCU_DEFAULT_FIRMWARE "omega4/rv1103b-hpmcu-bare.bin"
#define OMEGA4_HPMCU_DEFAULT_FW_ID_OFFSET 0x800
#define OMEGA4_HPMCU_DEFAULT_HEARTBEAT_OFFSET 0x804
#define OMEGA4_HPMCU_SYSFS_ALIAS "mcu"
#define RV1103B_SYS_GRF_OFFSET 0x50000
#define RV1103B_SYS_GRF_CACHE_PERI_START (RV1103B_SYS_GRF_OFFSET + 0x0200)
#define RV1103B_SYS_GRF_CACHE_PERI_END (RV1103B_SYS_GRF_OFFSET + 0x0204)
#define RV1103B_SYS_GRF_HPMCU_CODE_ADDR (RV1103B_SYS_GRF_OFFSET + 0x0208)
#define RV1103B_SYS_GRF_HPMCU_SRAM_ADDR (RV1103B_SYS_GRF_OFFSET + 0x020c)
#define RV1103B_SYS_GRF_HPMCU_EXSRAM_ADDR (RV1103B_SYS_GRF_OFFSET + 0x0210)
#define RV1103B_SYS_GRF_HPMCU_CACHE_MISC (RV1103B_SYS_GRF_OFFSET + 0x0214)
#define RV1103B_SYS_GRF_HPMCU_CACHE_STATUS (RV1103B_SYS_GRF_OFFSET + 0x0218)
#define RV1103B_SGRF_CON0 0x08
#define RV1103B_SGRF_CON1 0x0c
#define RV1103B_SGRF_CON0_VAL 0x00080008
#define RV1103B_HPMCU_BOOT_ADDR 0x00040000
#define RV1103B_SGRF_CON1_VAL 0x80000000
#define RV1103B_IOC_GPIO0B_JTAG_CON0 0x50804
#define RV1103B_IOC_GPIO0B_JTAG_CON0_VAL 0x00020002
#define RV1103B_CRU_SOFTRST_CON05 0x0a28
#define RV1103B_CRU_SOFTRST_ASSERT 0x00f000f0
#define RV1103B_CRU_SOFTRST_DEASSERT 0x00f00000

struct omega4_hpmcu_loader {
	struct device *dev;
	void *sram;
	void __iomem *sram_io;
	resource_size_t sram_phys;
	size_t sram_size;
	bool sram_is_io;
	bool sram_is_wc;
	void __iomem *boot_addr;
	void __iomem *cache_status;
	struct regmap *grf;
	struct regmap *sgrf;
	struct regmap *ioc;
	void __iomem *cru_base;
	struct reset_control *reset;
	struct reset_control *reset_core;
	struct reset_control *reset_cluster;
	struct reset_control *reset_pwup;
	struct clk_bulk_data *clks;
	int num_clks;
	const char *fw_name;
	char *fw_name_override;
	u32 boot_offset;
	u32 entry_addr;
	u32 cache_misc;
	u32 cache_peri_start;
	u32 cache_peri_end;
	bool cache_misc_valid;
	bool cache_peri_valid;
	bool entry_addr_valid;
	bool program_grf_map;
	bool use_sip;
	bool sip_code_valid;
	bool sip_sram_valid;
	bool sip_exsram_valid;
	u32 sip_mcu_id;
	u32 sip_code_addr;
	u32 sip_sram_addr;
	u32 sip_exsram_addr;
	bool running;
	struct delayed_work heartbeat_work;
	u32 heartbeat_offset;
	u32 heartbeat_last;
	u8 heartbeat_reads_left;
	bool heartbeat_valid;
	u32 fw_id_offset;
	bool fw_id_valid;
	bool alias_parent;
	bool alias_bus;
	struct mutex lock;
};

static u32 omega4_hpmcu_readl(struct omega4_hpmcu_loader *loader, u32 offset)
{
	dma_addr_t dma = DMA_MAPPING_ERROR;
	void __iomem *base;
	u32 val;

	if (offset > loader->sram_size - sizeof(u32))
		return 0;

	if (loader->sram_is_io) {
		base = loader->sram_io;
	} else {
		dma = dma_map_resource(loader->dev,
				       loader->sram_phys + offset,
				       sizeof(u32), DMA_FROM_DEVICE, 0);
		if (!dma_mapping_error(loader->dev, dma))
			dma_sync_single_for_cpu(loader->dev, dma, sizeof(u32),
						DMA_FROM_DEVICE);
		base = (void __iomem *)loader->sram;
	}

	val = readl(base + offset);
	if (!loader->sram_is_io && !dma_mapping_error(loader->dev, dma))
		dma_unmap_resource(loader->dev, dma, sizeof(u32),
				   DMA_FROM_DEVICE, 0);

	return val;
}

static void omega4_hpmcu_heartbeat_work(struct work_struct *work)
{
	struct omega4_hpmcu_loader *loader =
		container_of(to_delayed_work(work),
			     struct omega4_hpmcu_loader, heartbeat_work);
	u32 val;

	if (!loader->heartbeat_valid || !loader->running)
		return;

	val = omega4_hpmcu_readl(loader, loader->heartbeat_offset);
	if (loader->heartbeat_reads_left == 2)
		dev_info(loader->dev, "HPMCU heartbeat[0]=%u (offset=0x%x)\n",
			 val, loader->heartbeat_offset);
	else
		dev_info(loader->dev, "HPMCU heartbeat[1]=%u delta=%d\n",
			 val, (int)(val - loader->heartbeat_last));

	loader->heartbeat_last = val;
	if (--loader->heartbeat_reads_left)
		schedule_delayed_work(&loader->heartbeat_work,
				      msecs_to_jiffies(1000));
}

static u32 omega4_hpmcu_cache_status(struct omega4_hpmcu_loader *loader)
{
	if (!loader->cache_status)
		return 0;

	return readl(loader->cache_status);
}

static bool omega4_hpmcu_sip_config(struct omega4_hpmcu_loader *loader)
{
	int ret;
	bool applied = false;

	if (!loader->use_sip)
		return false;
	if (!psci_smp_available()) {
		dev_warn(loader->dev, "PSCI not available; skipping SIP MCU config\n");
		return false;
	}

	if (!loader->sip_code_valid)
		loader->sip_code_addr = (u32)loader->sram_phys;
	if (!loader->sip_sram_valid)
		loader->sip_sram_addr = (u32)loader->sram_phys;

	ret = sip_smc_mcu_config(loader->sip_mcu_id,
				 CONFIG_MCU_CODE_START_ADDR,
				 loader->sip_code_addr);
	if (ret)
		dev_warn(loader->dev, "SIP MCU code addr config failed: %d\n", ret);
	else {
		dev_info(loader->dev, "SIP MCU code addr=0x%08x\n",
			 loader->sip_code_addr);
		applied = true;
	}

	ret = sip_smc_mcu_config(loader->sip_mcu_id,
				 CONFIG_MCU_SRAM_START_ADDR,
				 loader->sip_sram_addr);
	if (ret)
		dev_warn(loader->dev, "SIP MCU sram addr config failed: %d\n", ret);
	else {
		dev_info(loader->dev, "SIP MCU sram addr=0x%08x\n",
			 loader->sip_sram_addr);
		applied = true;
	}

	if (!loader->sip_exsram_valid)
		return applied;

	ret = sip_smc_mcu_config(loader->sip_mcu_id,
				 CONFIG_MCU_EXSRAM_START_ADDR,
				 loader->sip_exsram_addr);
	if (ret)
		dev_warn(loader->dev, "SIP MCU exsram addr config failed: %d\n", ret);
	else {
		dev_info(loader->dev, "SIP MCU exsram addr=0x%08x\n",
			 loader->sip_exsram_addr);
		applied = true;
	}

	return applied;
}

static int omega4_hpmcu_stop_locked(struct omega4_hpmcu_loader *loader)
{
	int ret = 0;

	if (loader->reset_core) {
		dev_info(loader->dev, "HPMCU pre-stop cache status=0x%08x\n",
			 omega4_hpmcu_cache_status(loader));
		ret = reset_control_assert(loader->reset_core);
		if (ret)
			dev_err(loader->dev, "failed to assert core reset: %d\n", ret);
		else
			dev_info(loader->dev, "HPMCU post-stop cache status=0x%08x\n",
				 omega4_hpmcu_cache_status(loader));
	} else if (loader->reset) {
		dev_info(loader->dev, "HPMCU pre-stop cache status=0x%08x\n",
			 omega4_hpmcu_cache_status(loader));
		ret = reset_control_assert(loader->reset);
		if (ret)
			dev_err(loader->dev, "failed to assert reset: %d\n", ret);
		else
			dev_info(loader->dev, "HPMCU post-stop cache status=0x%08x\n",
				 omega4_hpmcu_cache_status(loader));
	}

	loader->running = false;
	cancel_delayed_work_sync(&loader->heartbeat_work);
	dev_info(loader->dev, "HPMCU stopped\n");
	return ret;
}

static int omega4_hpmcu_start_locked(struct omega4_hpmcu_loader *loader)
{
	const struct firmware *fw = NULL;
	int ret;
	int grf_ret;
	bool sip_applied;
	bool do_grf_map;
	resource_size_t entry;
	resource_size_t default_entry;

	ret = request_firmware(&fw, loader->fw_name, loader->dev);
	if (ret) {
		dev_err(loader->dev, "failed to load firmware %s: %d\n",
			loader->fw_name, ret);
		return ret;
	}

	if (!fw->size || fw->size > loader->sram_size) {
		dev_err(loader->dev, "invalid firmware size %zu (max %zu)\n",
			fw->size, loader->sram_size);
		ret = -EINVAL;
		goto out_release;
	}

	dev_info(loader->dev, "loading %s (%zu bytes) to %pa\n",
		 loader->fw_name, fw->size, &loader->sram_phys);

	if (loader->reset_core) {
		dev_info(loader->dev, "HPMCU pre-reset cache status=0x%08x\n",
			 omega4_hpmcu_cache_status(loader));
		ret = reset_control_assert(loader->reset_core);
		if (ret) {
			dev_err(loader->dev, "failed to assert core reset: %d\n", ret);
			goto out_release;
		}
		usleep_range(1000, 2000);
	} else if (loader->reset) {
		dev_info(loader->dev, "HPMCU pre-reset cache status=0x%08x\n",
			 omega4_hpmcu_cache_status(loader));
		ret = reset_control_assert(loader->reset);
		if (ret) {
			dev_err(loader->dev, "failed to assert reset: %d\n", ret);
			goto out_release;
		}
		usleep_range(1000, 2000);
	}
	if (loader->cru_base) {
		writel(RV1103B_CRU_SOFTRST_ASSERT,
		       loader->cru_base + RV1103B_CRU_SOFTRST_CON05);
		dev_info(loader->dev, "HPMCU CRU SRST assert=0x%08x\n",
			 RV1103B_CRU_SOFTRST_ASSERT);
		udelay(5);
	}

	if (loader->sram_is_io) {
		memset_io(loader->sram_io, 0, loader->sram_size);
		memcpy_toio(loader->sram_io, fw->data, fw->size);
	} else {
		memset(loader->sram, 0, loader->sram_size);
		dma_addr_t dma_addr;

		memcpy(loader->sram, fw->data, fw->size);
		dma_addr = dma_map_resource(loader->dev, loader->sram_phys,
					    fw->size, DMA_TO_DEVICE, 0);
		if (dma_mapping_error(loader->dev, dma_addr)) {
			dev_warn(loader->dev,
				 "failed to sync SRAM cache for device\n");
		} else {
			dma_unmap_resource(loader->dev, dma_addr, fw->size,
					   DMA_TO_DEVICE, 0);
		}
	}
	wmb();

	if (loader->sgrf) {
		regmap_write(loader->sgrf, RV1103B_SGRF_CON0, RV1103B_SGRF_CON0_VAL);
		dev_info(loader->dev, "HPMCU SGRF_CON0=0x%08x\n", RV1103B_SGRF_CON0_VAL);
	}
	if (loader->ioc) {
		regmap_write(loader->ioc, RV1103B_IOC_GPIO0B_JTAG_CON0,
			     RV1103B_IOC_GPIO0B_JTAG_CON0_VAL);
		dev_info(loader->dev, "HPMCU JTAG_CON0=0x%08x\n",
			 RV1103B_IOC_GPIO0B_JTAG_CON0_VAL);
	}

	sip_applied = omega4_hpmcu_sip_config(loader);
	default_entry = loader->sram_phys + loader->boot_offset;
	if (loader->entry_addr_valid)
		entry = (resource_size_t)loader->entry_addr + loader->boot_offset;
	else
		entry = default_entry;
	if (!sip_applied && entry == default_entry)
		entry = RV1103B_HPMCU_BOOT_ADDR + loader->boot_offset;

	dev_info(loader->dev, "HPMCU boot addr reg before=0x%08x\n",
		 readl(loader->boot_addr));
	writel((u32)entry, loader->boot_addr);
	dev_info(loader->dev, "HPMCU boot addr reg after=0x%08x\n",
		 readl(loader->boot_addr));

	do_grf_map = loader->program_grf_map || !sip_applied;
	if (loader->grf && do_grf_map) {
		if (!loader->program_grf_map && !sip_applied)
			dev_info(loader->dev,
				 "HPMCU GRF map programming forced (SIP not applied)\n");
		resource_size_t map_base = loader->sram_phys;
		u32 code_addr;
		u32 sram_addr;

		if (entry == RV1103B_HPMCU_BOOT_ADDR + loader->boot_offset &&
		    loader->sram_phys >= RV1103B_HPMCU_BOOT_ADDR)
			map_base = loader->sram_phys - RV1103B_HPMCU_BOOT_ADDR;
		code_addr = (u32)(map_base >> 12);
		sram_addr = code_addr;
		regmap_write(loader->grf, RV1103B_SYS_GRF_HPMCU_CODE_ADDR,
			     code_addr);
		regmap_write(loader->grf, RV1103B_SYS_GRF_HPMCU_SRAM_ADDR,
			     sram_addr);
		if (!regmap_read(loader->grf, RV1103B_SYS_GRF_HPMCU_CODE_ADDR, &code_addr))
			dev_info(loader->dev, "HPMCU GRF code addr=0x%08x\n", code_addr);
		if (!regmap_read(loader->grf, RV1103B_SYS_GRF_HPMCU_SRAM_ADDR, &sram_addr))
			dev_info(loader->dev, "HPMCU GRF sram addr=0x%08x\n", sram_addr);
	}
	if (loader->grf && loader->cache_peri_valid) {
		grf_ret = regmap_write(loader->grf, RV1103B_SYS_GRF_CACHE_PERI_START,
				       loader->cache_peri_start);
		if (grf_ret)
			dev_warn(loader->dev, "failed to write cache peri start: %d\n", grf_ret);
		grf_ret = regmap_write(loader->grf, RV1103B_SYS_GRF_CACHE_PERI_END,
				       loader->cache_peri_end);
		if (grf_ret)
			dev_warn(loader->dev, "failed to write cache peri end: %d\n", grf_ret);
	}
	if (loader->grf && loader->cache_misc_valid) {
		grf_ret = regmap_write(loader->grf, RV1103B_SYS_GRF_HPMCU_CACHE_MISC,
				       loader->cache_misc);
		if (grf_ret)
			dev_warn(loader->dev, "failed to write cache misc: %d\n", grf_ret);
	}
	if (loader->cru_base) {
		writel(RV1103B_CRU_SOFTRST_DEASSERT,
		       loader->cru_base + RV1103B_CRU_SOFTRST_CON05);
		dev_info(loader->dev, "HPMCU CRU SRST deassert=0x%08x\n",
			 RV1103B_CRU_SOFTRST_DEASSERT);
		udelay(5);
	}

	if (loader->reset_cluster) {
		ret = reset_control_deassert(loader->reset_cluster);
		if (ret) {
			dev_err(loader->dev, "failed to deassert cluster reset: %d\n", ret);
			goto out_release;
		}
	}
	if (loader->reset_pwup) {
		ret = reset_control_deassert(loader->reset_pwup);
		if (ret) {
			dev_err(loader->dev, "failed to deassert pwup reset: %d\n", ret);
			goto out_release;
		}
	}
	if (loader->reset_cluster || loader->reset_pwup)
		usleep_range(1000, 2000);

	if (loader->reset_core) {
		ret = reset_control_deassert(loader->reset_core);
		if (ret) {
			dev_err(loader->dev, "failed to deassert core reset: %d\n", ret);
			goto out_release;
		}
		usleep_range(1000, 2000);
	} else if (loader->reset) {
		ret = reset_control_deassert(loader->reset);
		if (ret) {
			dev_err(loader->dev, "failed to deassert reset: %d\n", ret);
			goto out_release;
		}
		usleep_range(1000, 2000);
	}

	loader->running = true;
	dev_info(loader->dev, "HPMCU booted (entry=%pa)\n", &entry);
	dev_info(loader->dev, "HPMCU post-boot cache status=0x%08x\n",
		 omega4_hpmcu_cache_status(loader));
	if (loader->heartbeat_valid) {
		loader->heartbeat_reads_left = 2;
		schedule_delayed_work(&loader->heartbeat_work,
				      msecs_to_jiffies(2000));
	}

out_release:
	release_firmware(fw);
	return ret;
}

static ssize_t start_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct omega4_hpmcu_loader *loader = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&loader->lock);
	ret = omega4_hpmcu_start_locked(loader);
	mutex_unlock(&loader->lock);

	return ret ? ret : count;
}

static ssize_t stop_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct omega4_hpmcu_loader *loader = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&loader->lock);
	ret = omega4_hpmcu_stop_locked(loader);
	mutex_unlock(&loader->lock);

	return ret ? ret : count;
}

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct omega4_hpmcu_loader *loader = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", loader->running ? "running" : "stopped");
}

static ssize_t heartbeat_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct omega4_hpmcu_loader *loader = dev_get_drvdata(dev);
	u32 val;

	if (!loader->heartbeat_valid)
		return sysfs_emit(buf, "unsupported\n");

	val = omega4_hpmcu_readl(loader, loader->heartbeat_offset);
	return sysfs_emit(buf, "%u (0x%08x)\n", val, val);
}

static ssize_t fw_id_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct omega4_hpmcu_loader *loader = dev_get_drvdata(dev);
	u32 val;

	if (!loader->fw_id_valid)
		return sysfs_emit(buf, "unsupported\n");

	val = omega4_hpmcu_readl(loader, loader->fw_id_offset);
	return sysfs_emit(buf, "0x%08x\n", val);
}

static ssize_t firmware_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct omega4_hpmcu_loader *loader = dev_get_drvdata(dev);
	const char *fw_name;
	ssize_t ret;

	mutex_lock(&loader->lock);
	fw_name = loader->fw_name;
	ret = sysfs_emit(buf, "%s\n", fw_name ? fw_name : "unknown");
	mutex_unlock(&loader->lock);

	return ret;
}

static ssize_t firmware_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct omega4_hpmcu_loader *loader = dev_get_drvdata(dev);
	char *name;
	char *trimmed;

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOMEM;

	trimmed = strim(name);
	if (!*trimmed) {
		kfree(name);
		return -EINVAL;
	}
	if (trimmed != name)
		memmove(name, trimmed, strlen(trimmed) + 1);

	mutex_lock(&loader->lock);
	if (loader->fw_name_override &&
	    strcmp(loader->fw_name_override, name) == 0) {
		mutex_unlock(&loader->lock);
		kfree(name);
		return count;
	}

	kfree(loader->fw_name_override);
	loader->fw_name_override = name;
	loader->fw_name = loader->fw_name_override;
	mutex_unlock(&loader->lock);

	return count;
}

static DEVICE_ATTR_WO(start);
static DEVICE_ATTR_WO(stop);
static DEVICE_ATTR_RO(status);
static DEVICE_ATTR_RO(heartbeat);
static DEVICE_ATTR_RO(fw_id);
static DEVICE_ATTR_RW(firmware);

static struct attribute *omega4_hpmcu_attrs[] = {
	&dev_attr_start.attr,
	&dev_attr_stop.attr,
	&dev_attr_status.attr,
	&dev_attr_heartbeat.attr,
	&dev_attr_fw_id.attr,
	&dev_attr_firmware.attr,
	NULL,
};

static const struct attribute_group omega4_hpmcu_group = {
	.attrs = omega4_hpmcu_attrs,
};

static void omega4_hpmcu_alias_remove(struct omega4_hpmcu_loader *loader)
{
	struct kset *bus_kset;

	if (loader->alias_parent && loader->dev->parent)
		sysfs_remove_link(&loader->dev->parent->kobj,
				  OMEGA4_HPMCU_SYSFS_ALIAS);

	if (loader->alias_bus) {
		bus_kset = bus_get_kset(&platform_bus_type);
		if (bus_kset)
			sysfs_remove_link(&bus_kset->kobj,
					  OMEGA4_HPMCU_SYSFS_ALIAS);
	}
}

static void omega4_hpmcu_alias_create(struct omega4_hpmcu_loader *loader)
{
	struct kset *bus_kset;
	int ret;

	if (loader->dev->parent) {
		ret = sysfs_create_link(&loader->dev->parent->kobj,
					&loader->dev->kobj,
					OMEGA4_HPMCU_SYSFS_ALIAS);
		if (!ret)
			loader->alias_parent = true;
		else if (ret != -EEXIST)
			dev_warn(loader->dev,
				 "failed to create sysfs alias '%s' under parent: %d\n",
				 OMEGA4_HPMCU_SYSFS_ALIAS, ret);
	}

	bus_kset = bus_get_kset(&platform_bus_type);
	if (bus_kset) {
		ret = sysfs_create_link(&bus_kset->kobj, &loader->dev->kobj,
					OMEGA4_HPMCU_SYSFS_ALIAS);
		if (!ret)
			loader->alias_bus = true;
		else if (ret != -EEXIST)
			dev_warn(loader->dev,
				 "failed to create sysfs alias '%s' under bus: %d\n",
				 OMEGA4_HPMCU_SYSFS_ALIAS, ret);
	}
}

static int omega4_hpmcu_parse_sram(struct device *dev,
				  struct omega4_hpmcu_loader *loader)
{
	struct device_node *rmem_np;
	struct resource res;
	int ret;

	rmem_np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!rmem_np)
		return -EINVAL;

	ret = of_address_to_resource(rmem_np, 0, &res);
	of_node_put(rmem_np);
	if (ret)
		return ret;

	loader->sram_phys = res.start;
	loader->sram_size = resource_size(&res);
	loader->sram_io = devm_ioremap(dev, loader->sram_phys, loader->sram_size);
	if (loader->sram_io) {
		loader->sram_is_io = true;
		loader->sram_is_wc = false;
		dev_info(dev, "HPMCU SRAM mapped as io\n");
		return 0;
	}

	loader->sram = devm_memremap(dev, loader->sram_phys, loader->sram_size,
				     MEMREMAP_WC);
	if (!IS_ERR(loader->sram)) {
		loader->sram_is_io = false;
		loader->sram_is_wc = true;
		dev_info(dev, "HPMCU SRAM mapped as mem (wc)\n");
		return 0;
	}

	loader->sram = devm_memremap(dev, loader->sram_phys, loader->sram_size,
				     MEMREMAP_WB);
	if (IS_ERR(loader->sram))
		return PTR_ERR(loader->sram);

	loader->sram_is_io = false;
	loader->sram_is_wc = false;
	dev_info(dev, "HPMCU SRAM mapped as mem (wb)\n");

	return 0;
}

static int omega4_hpmcu_loader_probe(struct platform_device *pdev)
{
	struct omega4_hpmcu_loader *loader;
	struct resource *res;
	bool auto_boot;
	int ret;

	loader = devm_kzalloc(&pdev->dev, sizeof(*loader), GFP_KERNEL);
	if (!loader)
		return -ENOMEM;

	loader->dev = &pdev->dev;
	mutex_init(&loader->lock);
	loader->fw_name = OMEGA4_HPMCU_DEFAULT_FIRMWARE;
	loader->fw_name_override = NULL;
	loader->boot_offset = 0;
	loader->fw_id_offset = OMEGA4_HPMCU_DEFAULT_FW_ID_OFFSET;
	loader->fw_id_valid = true;
	loader->heartbeat_offset = OMEGA4_HPMCU_DEFAULT_HEARTBEAT_OFFSET;
	loader->heartbeat_valid = true;
	INIT_DELAYED_WORK(&loader->heartbeat_work, omega4_hpmcu_heartbeat_work);
	loader->grf = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "rockchip,grf");
	if (IS_ERR(loader->grf)) {
		loader->grf = NULL;
	} else {
		u32 val;

		if (!regmap_read(loader->grf, RV1103B_SYS_GRF_HPMCU_CODE_ADDR, &val))
			dev_info(&pdev->dev, "HPMCU GRF code addr init=0x%08x\n", val);
		if (!regmap_read(loader->grf, RV1103B_SYS_GRF_HPMCU_SRAM_ADDR, &val))
			dev_info(&pdev->dev, "HPMCU GRF sram addr init=0x%08x\n", val);
		if (!regmap_read(loader->grf, RV1103B_SYS_GRF_HPMCU_EXSRAM_ADDR, &val))
			dev_info(&pdev->dev, "HPMCU GRF exsram addr init=0x%08x\n", val);
		if (!regmap_read(loader->grf, RV1103B_SYS_GRF_HPMCU_CACHE_MISC, &val))
			dev_info(&pdev->dev, "HPMCU GRF cache misc init=0x%08x\n", val);
		if (!regmap_read(loader->grf, RV1103B_SYS_GRF_HPMCU_CACHE_STATUS, &val))
			dev_info(&pdev->dev, "HPMCU GRF cache status init=0x%08x\n", val);
	}
	loader->sgrf = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "rockchip,sgrf");
	if (IS_ERR(loader->sgrf))
		loader->sgrf = NULL;
	loader->ioc = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "rockchip,ioc");
	if (IS_ERR(loader->ioc))
		loader->ioc = NULL;
	if (of_property_present(pdev->dev.of_node, "rockchip,cru")) {
		struct device_node *cru_np;
		struct resource cru_res;

		cru_np = of_parse_phandle(pdev->dev.of_node, "rockchip,cru", 0);
		if (!cru_np) {
			dev_err(&pdev->dev, "failed to parse rockchip,cru phandle\n");
			return -EINVAL;
		}

		ret = of_address_to_resource(cru_np, 0, &cru_res);
		of_node_put(cru_np);
		if (ret) {
			dev_err(&pdev->dev, "failed to get CRU resource: %d\n", ret);
			return ret;
		}

		loader->cru_base = devm_ioremap_resource(&pdev->dev, &cru_res);
		if (IS_ERR(loader->cru_base))
			return PTR_ERR(loader->cru_base);
	}

	ret = omega4_hpmcu_parse_sram(&pdev->dev, loader);
	if (ret) {
		dev_err(&pdev->dev, "failed to parse memory-region: %d\n", ret);
		return ret;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "boot-addr");
	if (!res)
		return -EINVAL;
	loader->boot_addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(loader->boot_addr))
		return PTR_ERR(loader->boot_addr);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cache-status");
	if (!res)
		return -EINVAL;
	loader->cache_status = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(loader->cache_status))
		return PTR_ERR(loader->cache_status);

	loader->reset_core = devm_reset_control_get_optional_exclusive(&pdev->dev, "core");
	if (IS_ERR(loader->reset_core))
		return PTR_ERR(loader->reset_core);
	loader->reset_cluster = devm_reset_control_get_optional_exclusive(&pdev->dev, "cluster");
	if (IS_ERR(loader->reset_cluster))
		return PTR_ERR(loader->reset_cluster);
	loader->reset_pwup = devm_reset_control_get_optional_exclusive(&pdev->dev, "pwup");
	if (IS_ERR(loader->reset_pwup))
		return PTR_ERR(loader->reset_pwup);
	if (!loader->reset_core && !loader->reset_cluster && !loader->reset_pwup) {
		loader->reset = devm_reset_control_array_get_optional_exclusive(&pdev->dev);
		if (IS_ERR(loader->reset))
			return PTR_ERR(loader->reset);
	}

	loader->num_clks = devm_clk_bulk_get_all(&pdev->dev, &loader->clks);
	if (loader->num_clks < 0)
		return loader->num_clks;
	if (loader->num_clks > 0) {
		ret = clk_bulk_prepare_enable(loader->num_clks, loader->clks);
		if (ret)
			return ret;
	}

	of_property_read_string(pdev->dev.of_node, "firmware-name",
				&loader->fw_name);
	of_property_read_u32(pdev->dev.of_node, "boot-offset", &loader->boot_offset);
	if (!of_property_read_u32(pdev->dev.of_node, "entry-addr",
				  &loader->entry_addr))
		loader->entry_addr_valid = true;
	loader->program_grf_map = of_property_read_bool(pdev->dev.of_node,
							"rockchip,program-grf-map");
	loader->use_sip = of_property_read_bool(pdev->dev.of_node,
						"rockchip,use-sip");
	if (loader->use_sip)
		loader->sip_mcu_id = RK_SIP_CFG_BUSMCU_0_ID;
	of_property_read_u32(pdev->dev.of_node, "rockchip,sip-mcu-id",
			     &loader->sip_mcu_id);
	if (!of_property_read_u32(pdev->dev.of_node, "rockchip,sip-code-addr",
				  &loader->sip_code_addr))
		loader->sip_code_valid = true;
	if (!of_property_read_u32(pdev->dev.of_node, "rockchip,sip-sram-addr",
				  &loader->sip_sram_addr))
		loader->sip_sram_valid = true;
	if (!of_property_read_u32(pdev->dev.of_node, "rockchip,sip-exsram-addr",
				  &loader->sip_exsram_addr))
		loader->sip_exsram_valid = true;
	if (loader->grf && !loader->program_grf_map)
		dev_info(&pdev->dev, "HPMCU GRF map programming disabled\n");
	if (loader->use_sip)
		dev_info(&pdev->dev, "HPMCU SIP config enabled (mcu-id=0x%08x)\n",
			 loader->sip_mcu_id);
	if (!of_property_read_u32(pdev->dev.of_node, "rockchip,cache-misc",
				  &loader->cache_misc))
		loader->cache_misc_valid = true;
	if (!of_property_read_u32(pdev->dev.of_node, "rockchip,cache-peri-start",
				  &loader->cache_peri_start) &&
	    !of_property_read_u32(pdev->dev.of_node, "rockchip,cache-peri-end",
				  &loader->cache_peri_end))
		loader->cache_peri_valid = true;
	of_property_read_u32(pdev->dev.of_node, "heartbeat-offset",
			     &loader->heartbeat_offset);
	if (!of_property_read_u32(pdev->dev.of_node, "fw-id-offset",
				  &loader->fw_id_offset))
		loader->fw_id_valid = true;
	if (loader->heartbeat_offset > loader->sram_size - sizeof(u32))
		loader->heartbeat_valid = false;
	if (loader->fw_id_offset > loader->sram_size - sizeof(u32))
		loader->fw_id_valid = false;

	auto_boot = of_property_read_bool(pdev->dev.of_node, "auto-boot");
	platform_set_drvdata(pdev, loader);

	ret = devm_device_add_group(&pdev->dev, &omega4_hpmcu_group);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "HPMCU SRAM %pa size 0x%zx\n",
		 &loader->sram_phys, loader->sram_size);

	if (auto_boot) {
		mutex_lock(&loader->lock);
		ret = omega4_hpmcu_start_locked(loader);
		mutex_unlock(&loader->lock);
		if (ret)
			return ret;
	}

	omega4_hpmcu_alias_create(loader);
	return 0;
}

static int omega4_hpmcu_loader_remove(struct platform_device *pdev)
{
	struct omega4_hpmcu_loader *loader = platform_get_drvdata(pdev);

	mutex_lock(&loader->lock);
	omega4_hpmcu_stop_locked(loader);
	mutex_unlock(&loader->lock);

	omega4_hpmcu_alias_remove(loader);
	if (loader->num_clks > 0)
		clk_bulk_disable_unprepare(loader->num_clks, loader->clks);

	kfree(loader->fw_name_override);
	return 0;
}

static const struct of_device_id omega4_hpmcu_loader_of_match[] = {
	{ .compatible = "onion,omega4-hpmcu-loader" },
	{ }
};
MODULE_DEVICE_TABLE(of, omega4_hpmcu_loader_of_match);

static struct platform_driver omega4_hpmcu_loader_driver = {
	.probe = omega4_hpmcu_loader_probe,
	.remove = omega4_hpmcu_loader_remove,
	.driver = {
		.name = "omega4-hpmcu-loader",
		.of_match_table = omega4_hpmcu_loader_of_match,
	},
};

module_platform_driver(omega4_hpmcu_loader_driver);

MODULE_AUTHOR("Onion Corporation");
MODULE_DESCRIPTION("Omega4 RV1103B HPMCU firmware loader");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(OMEGA4_HPMCU_DEFAULT_FIRMWARE);
