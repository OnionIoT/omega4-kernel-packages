# SPDX-License-Identifier: (GPL-2.0+ OR MIT)

MPP_REVISION := omega4-openwrt-rv1103b

obj-m += rk_vcodec.o

rk_vcodec-objs := \
	mpp_service.o \
	mpp_common.o \
	mpp_iommu.o \
	mpp_rkvenc.o \
	mpp_rkvenc2.o

ccflags-y += \
	-DCONFIG_ROCKCHIP_MPP_SERVICE=1 \
	-DCONFIG_ROCKCHIP_MPP_PROC_FS=1 \
	-DCONFIG_ROCKCHIP_MPP_RKVENC=1 \
	-DCONFIG_ROCKCHIP_MPP_RKVENC2=1 \
	-I$(srctree)/drivers/devfreq \
	-Wno-error

CFLAGS_mpp_service.o += -DMPP_VERSION="\"$(MPP_REVISION)\""
