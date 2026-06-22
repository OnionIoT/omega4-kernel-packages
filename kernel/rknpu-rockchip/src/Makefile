# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_ROCKCHIP_RKNPU) += rknpu.o

ccflags-y += -I$(srctree)/$(src)/include
ccflags-y += -I$(src)/include
ccflags-y += -Werror
ccflags-$(CONFIG_ROCKCHIP_RKNPU_DRM_GEM) += -DCONFIG_ROCKCHIP_RKNPU_DRM_GEM
ccflags-$(CONFIG_ROCKCHIP_RKNPU_DMA_HEAP) += -DCONFIG_ROCKCHIP_RKNPU_DMA_HEAP
ccflags-$(CONFIG_ROCKCHIP_RKNPU_DEBUG_FS) += -DCONFIG_ROCKCHIP_RKNPU_DEBUG_FS
ccflags-$(CONFIG_ROCKCHIP_RKNPU_PROC_FS) += -DCONFIG_ROCKCHIP_RKNPU_PROC_FS

rknpu-y += rknpu_drv.o
rknpu-y += rknpu_reset.o
rknpu-y += rknpu_job.o
rknpu-y += rknpu_debugger.o
rknpu-y += rknpu_iommu.o
rknpu-$(CONFIG_PM_DEVFREQ) += rknpu_devfreq.o
rknpu-$(CONFIG_ROCKCHIP_RKNPU_SRAM) += rknpu_mm.o
rknpu-$(CONFIG_ROCKCHIP_RKNPU_FENCE) += rknpu_fence.o
rknpu-$(CONFIG_ROCKCHIP_RKNPU_DRM_GEM) += rknpu_gem.o
rknpu-$(CONFIG_ROCKCHIP_RKNPU_DMA_HEAP) += rknpu_mem.o
