obj-m += video_rkisp.o

video_rkisp-objs += hw.o \
	dev.o \
	rkisp.o \
	regs.o \
	common.o \
	isp_stats.o \
	isp_params.o \
	capture.o \
	dmarx.o \
	csi.o \
	procfs.o \
	capture_v33.o \
	isp_params_v33.o \
	isp_stats_v33.o \
	isp_rockit.o

ccflags-y += -DCONFIG_VIDEO_ROCKCHIP_ISP
ccflags-y += -DCONFIG_VIDEO_ROCKCHIP_ISP_VERSION_V33
