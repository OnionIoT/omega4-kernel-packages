obj-m += video_rkcif.o

video_rkcif-objs += dev.o \
	capture.o \
	cif-luma.o \
	hw.o \
	subdev-itf.o \
	procfs.o \
	cif-scale.o \
	common.o \
	cif-tools.o \
	cif-rockit.o

ccflags-y += -DCONFIG_ROCKCHIP_CIF_WORKMODE_PINGPONG
ccflags-y += -DCONFIG_ROCKCHIP_CIF_USE_DUMMY_BUF
