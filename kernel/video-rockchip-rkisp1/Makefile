# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=video-rockchip-rkisp1
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/video-rockchip-rkisp1
  SUBMENU:=Video Support
  TITLE:=Rockchip ISP1 support
  DEPENDS:=@TARGET_rockchip +kmod-video-core +kmod-video-fwnode +kmod-video-dma-contig +kmod-phy-rockchip-mipi-rx
  FILES:=$(PKG_BUILD_DIR)/video_rkisp1.ko
  AUTOLOAD:=$(call AutoProbe,video_rkisp1)
endef

define KernelPackage/video-rockchip-rkisp1/description
  Kernel module for the Rockchip ISP1 block.
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/. $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) \
		M="$(PKG_BUILD_DIR)" \
		modules
endef

$(eval $(call KernelPackage,video-rockchip-rkisp1))
