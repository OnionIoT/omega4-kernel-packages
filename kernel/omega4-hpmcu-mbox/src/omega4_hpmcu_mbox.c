// SPDX-License-Identifier: GPL-2.0-only
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define OMEGA4_MBOX_PING_CMD  0x4f4d4355u /* "OMCU" */
#define OMEGA4_MBOX_PING_DATA 0x00000001u
#define OMEGA4_MBOX_TIMEOUT_MS 500

#define MBOX_V2_A2B_STATUS	0x04
#define MBOX_V2_A2B_CMD		0x08
#define MBOX_V2_A2B_DATA	0x0c
#define MBOX_V2_A2B_INTEN	0x00
#define MBOX_V2_B2A_INTEN	0x10
#define MBOX_V2_B2A_STATUS	0x14
#define MBOX_V2_B2A_CMD		0x18
#define MBOX_V2_B2A_DATA	0x1c
#define MBOX_V2_TRIGGER_SHIFT	8
#define MBOX_V2_WRITEABLE_SHIFT	16
#define MBOX_V2_INT_MASK	BIT(0)
#define MBOX_V2_INT_CLR		BIT(0)
#define OMEGA4_MBOX_TRIGGER_METHOD 1
#define MBOX_V2_A2B_INTEN_VAL (BIT(MBOX_V2_WRITEABLE_SHIFT + MBOX_V2_TRIGGER_SHIFT) | \
			       (OMEGA4_MBOX_TRIGGER_METHOD << MBOX_V2_TRIGGER_SHIFT) | \
			       BIT(MBOX_V2_WRITEABLE_SHIFT) | MBOX_V2_INT_MASK)
#define MBOX_V2_B2A_INTEN_VAL (BIT(MBOX_V2_WRITEABLE_SHIFT) | MBOX_V2_INT_MASK)

struct rockchip_mbox_msg {
	u32 cmd;
	u32 data;
};

struct omega4_hpmcu_mbox {
	struct device *dev;
	struct mbox_client cl;
	struct mbox_chan *chan;
	void __iomem *mbox_base;
	struct completion rx_done;
	struct mutex lock;
	struct rockchip_mbox_msg msg;
	atomic_t rx_count;
	u32 last_cmd;
	u32 last_data;
	int last_status;
};

static void omega4_hpmcu_mbox_rx(struct mbox_client *cl, void *msg)
{
	struct omega4_hpmcu_mbox *mb = container_of(cl, struct omega4_hpmcu_mbox, cl);
	struct rockchip_mbox_msg *rx = msg;

	if (rx) {
		mb->last_cmd = rx->cmd;
		mb->last_data = rx->data;
		dev_info(mb->dev, "rx cmd=0x%08x data=0x%08x\n",
			 rx->cmd, rx->data);
	}
	atomic_inc(&mb->rx_count);
	complete(&mb->rx_done);
}

static bool omega4_hpmcu_mbox_poll_rx(struct omega4_hpmcu_mbox *mb)
{
	struct rockchip_mbox_msg rx;
	u32 status;

	if (!mb->mbox_base)
		return false;

	status = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_STATUS);
	if (!(status & MBOX_V2_INT_MASK))
		return false;

	rx.cmd = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_CMD);
	rx.data = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_DATA);
	writel_relaxed(MBOX_V2_INT_CLR, mb->mbox_base + MBOX_V2_B2A_STATUS);

	omega4_hpmcu_mbox_rx(&mb->cl, &rx);
	return true;
}

static void omega4_hpmcu_mbox_reinit(struct omega4_hpmcu_mbox *mb)
{
	u32 a2b_inten;
	u32 b2a_inten;

	if (!mb->mbox_base)
		return;

	a2b_inten = readl_relaxed(mb->mbox_base + MBOX_V2_A2B_INTEN);
	b2a_inten = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_INTEN);
	if ((a2b_inten & MBOX_V2_INT_MASK) && (b2a_inten & MBOX_V2_INT_MASK))
		return;

	writel_relaxed(MBOX_V2_INT_CLR, mb->mbox_base + MBOX_V2_A2B_STATUS);
	writel_relaxed(MBOX_V2_INT_CLR, mb->mbox_base + MBOX_V2_B2A_STATUS);
	writel_relaxed(MBOX_V2_A2B_INTEN_VAL, mb->mbox_base + MBOX_V2_A2B_INTEN);
	writel_relaxed(MBOX_V2_B2A_INTEN_VAL, mb->mbox_base + MBOX_V2_B2A_INTEN);
}

static int omega4_hpmcu_mbox_send(struct omega4_hpmcu_mbox *mb, u32 cmd, u32 data)
{
	int ret;

	mutex_lock(&mb->lock);
	mb->msg.cmd = cmd;
	mb->msg.data = data;
	reinit_completion(&mb->rx_done);
	omega4_hpmcu_mbox_reinit(mb);

	ret = mbox_send_message(mb->chan, &mb->msg);
	if (ret < 0)
		goto out;

	if (!wait_for_completion_timeout(&mb->rx_done,
					 msecs_to_jiffies(OMEGA4_MBOX_TIMEOUT_MS))) {
		if (!omega4_hpmcu_mbox_poll_rx(mb)) {
			if (mb->mbox_base) {
				u32 a2b = readl_relaxed(mb->mbox_base + MBOX_V2_A2B_STATUS);
				u32 b2a = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_STATUS);
				u32 cmd = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_CMD);
				u32 data = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_DATA);

				dev_warn(mb->dev,
					 "mbox timeout: a2b_status=0x%08x b2a_status=0x%08x b2a_cmd=0x%08x b2a_data=0x%08x\n",
					 a2b, b2a, cmd, data);
			}
			ret = -ETIMEDOUT;
			goto out;
		}
	}

	ret = 0;
out:
	mb->last_status = ret;
	mutex_unlock(&mb->lock);
	return ret;
}

static ssize_t ping_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct omega4_hpmcu_mbox *mb = dev_get_drvdata(dev);
	unsigned int cmd = OMEGA4_MBOX_PING_CMD;
	unsigned int data = OMEGA4_MBOX_PING_DATA;
	int parsed;
	int ret;

	parsed = sscanf(buf, "%i %i", &cmd, &data);
	if (parsed < 1) {
		cmd = OMEGA4_MBOX_PING_CMD;
		data = OMEGA4_MBOX_PING_DATA;
	} else if (parsed < 2) {
		data = OMEGA4_MBOX_PING_DATA;
	}

	ret = omega4_hpmcu_mbox_send(mb, cmd, data);
	if (ret)
		return ret;

	return count;
}

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct omega4_hpmcu_mbox *mb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "rx_count=%d last_cmd=0x%08x last_data=0x%08x last_status=%d\n",
			  atomic_read(&mb->rx_count), mb->last_cmd, mb->last_data,
			  mb->last_status);
}

static ssize_t regs_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct omega4_hpmcu_mbox *mb = dev_get_drvdata(dev);
	u32 a2b_status, a2b_cmd, a2b_data;
	u32 b2a_status, b2a_cmd, b2a_data, b2a_inten;

	if (!mb->mbox_base)
		return sysfs_emit(buf, "mbox_base=none\n");

	a2b_status = readl_relaxed(mb->mbox_base + MBOX_V2_A2B_STATUS);
	a2b_cmd = readl_relaxed(mb->mbox_base + MBOX_V2_A2B_CMD);
	a2b_data = readl_relaxed(mb->mbox_base + MBOX_V2_A2B_DATA);
	b2a_inten = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_INTEN);
	b2a_status = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_STATUS);
	b2a_cmd = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_CMD);
	b2a_data = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_DATA);

	return sysfs_emit(buf,
			  "a2b_status=0x%08x a2b_cmd=0x%08x a2b_data=0x%08x\n"
			  "b2a_inten=0x%08x b2a_status=0x%08x b2a_cmd=0x%08x b2a_data=0x%08x\n",
			  a2b_status, a2b_cmd, a2b_data,
			  b2a_inten, b2a_status, b2a_cmd, b2a_data);
}

static void omega4_hpmcu_mbox_log_regs(struct omega4_hpmcu_mbox *mb,
				       const char *tag)
{
	u32 a2b_status, a2b_cmd, a2b_data;
	u32 b2a_status, b2a_cmd, b2a_data, b2a_inten;

	if (!mb->mbox_base) {
		dev_info(mb->dev, "mbox regs %s: base unavailable\n", tag);
		return;
	}

	a2b_status = readl_relaxed(mb->mbox_base + MBOX_V2_A2B_STATUS);
	a2b_cmd = readl_relaxed(mb->mbox_base + MBOX_V2_A2B_CMD);
	a2b_data = readl_relaxed(mb->mbox_base + MBOX_V2_A2B_DATA);
	b2a_inten = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_INTEN);
	b2a_status = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_STATUS);
	b2a_cmd = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_CMD);
	b2a_data = readl_relaxed(mb->mbox_base + MBOX_V2_B2A_DATA);

	dev_info(mb->dev,
		 "mbox regs %s: a2b_status=0x%08x a2b_cmd=0x%08x a2b_data=0x%08x b2a_inten=0x%08x b2a_status=0x%08x b2a_cmd=0x%08x b2a_data=0x%08x\n",
		 tag, a2b_status, a2b_cmd, a2b_data,
		 b2a_inten, b2a_status, b2a_cmd, b2a_data);
}

static ssize_t clear_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct omega4_hpmcu_mbox *mb = dev_get_drvdata(dev);

	if (!mb->mbox_base)
		return -ENODEV;

	writel_relaxed(MBOX_V2_INT_CLR, mb->mbox_base + MBOX_V2_A2B_STATUS);
	writel_relaxed(0, mb->mbox_base + MBOX_V2_A2B_CMD);
	writel_relaxed(0, mb->mbox_base + MBOX_V2_A2B_DATA);

	writel_relaxed(MBOX_V2_INT_CLR, mb->mbox_base + MBOX_V2_B2A_STATUS);
	writel_relaxed(0, mb->mbox_base + MBOX_V2_B2A_CMD);
	writel_relaxed(0, mb->mbox_base + MBOX_V2_B2A_DATA);

	return count;
}

static DEVICE_ATTR_WO(ping);
static DEVICE_ATTR_RO(status);
static DEVICE_ATTR_RO(regs);
static DEVICE_ATTR_WO(clear);

static struct attribute *omega4_hpmcu_attrs[] = {
	&dev_attr_ping.attr,
	&dev_attr_status.attr,
	&dev_attr_regs.attr,
	&dev_attr_clear.attr,
	NULL,
};

static const struct attribute_group omega4_hpmcu_attr_group = {
	.attrs = omega4_hpmcu_attrs,
};

static int omega4_hpmcu_mbox_probe(struct platform_device *pdev)
{
	struct omega4_hpmcu_mbox *mb;
	struct device_node *np = pdev->dev.of_node;
	u32 cmd = OMEGA4_MBOX_PING_CMD;
	u32 data = OMEGA4_MBOX_PING_DATA;
	bool auto_ping;
	int ret;

	mb = devm_kzalloc(&pdev->dev, sizeof(*mb), GFP_KERNEL);
	if (!mb)
		return -ENOMEM;

	mb->dev = &pdev->dev;
	mb->cl.dev = &pdev->dev;
	mb->cl.rx_callback = omega4_hpmcu_mbox_rx;
	mb->cl.tx_block = false;
	mb->cl.knows_txdone = false;
	mb->cl.tx_tout = OMEGA4_MBOX_TIMEOUT_MS;

	init_completion(&mb->rx_done);
	mutex_init(&mb->lock);
	atomic_set(&mb->rx_count, 0);
	mb->last_status = -ENODATA;

	mb->chan = mbox_request_channel_byname(&mb->cl, "hpmcu");
	if (IS_ERR(mb->chan)) {
		ret = PTR_ERR(mb->chan);
		if (ret == -ENODEV || ret == -EINVAL)
			mb->chan = mbox_request_channel(&mb->cl, 0);
		if (IS_ERR(mb->chan))
			return PTR_ERR(mb->chan);
	}

	platform_set_drvdata(pdev, mb);

	if (np) {
		struct device_node *mbox_np;
		struct resource res;

		mbox_np = of_parse_phandle(np, "mboxes", 0);
		if (mbox_np && !of_address_to_resource(mbox_np, 0, &res)) {
			mb->mbox_base = devm_ioremap(&pdev->dev, res.start,
						     resource_size(&res));
		}
		of_node_put(mbox_np);
	}

	omega4_hpmcu_mbox_log_regs(mb, "probe");

	ret = sysfs_create_group(&pdev->dev.kobj, &omega4_hpmcu_attr_group);
	if (ret) {
		mbox_free_channel(mb->chan);
		return ret;
	}

	auto_ping = np && of_property_read_bool(np, "auto-ping");
	if (auto_ping) {
		of_property_read_u32(np, "ping-cmd", &cmd);
		of_property_read_u32(np, "ping-data", &data);
		ret = omega4_hpmcu_mbox_send(mb, cmd, data);
		if (ret)
			dev_warn(&pdev->dev, "auto ping failed: %d\n", ret);
		else
			dev_info(&pdev->dev, "auto ping ok (cmd=0x%08x data=0x%08x)\n",
				 cmd, data);
	}

	return 0;
}

static int omega4_hpmcu_mbox_remove(struct platform_device *pdev)
{
	struct omega4_hpmcu_mbox *mb = platform_get_drvdata(pdev);

	sysfs_remove_group(&pdev->dev.kobj, &omega4_hpmcu_attr_group);
	mbox_free_channel(mb->chan);
	return 0;
}

static const struct of_device_id omega4_hpmcu_mbox_of_match[] = {
	{ .compatible = "onion,omega4-hpmcu-mailbox" },
	{ }
};
MODULE_DEVICE_TABLE(of, omega4_hpmcu_mbox_of_match);

static struct platform_driver omega4_hpmcu_mbox_driver = {
	.probe = omega4_hpmcu_mbox_probe,
	.remove = omega4_hpmcu_mbox_remove,
	.driver = {
		.name = "omega4-hpmcu-mailbox",
		.of_match_table = omega4_hpmcu_mbox_of_match,
	},
};
module_platform_driver(omega4_hpmcu_mbox_driver);

MODULE_DESCRIPTION("Omega4 RV1103B HPMCU mailbox ping client");
MODULE_AUTHOR("OpenAI Codex");
MODULE_LICENSE("GPL");
