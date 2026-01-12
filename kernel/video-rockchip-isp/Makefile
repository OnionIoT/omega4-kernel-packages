# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=video-rockchip-isp
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/video-rockchip-isp
  SUBMENU:=Video Support
  TITLE:=Rockchip ISP support
  DEPENDS:=@TARGET_rockchip +kmod-video-core +kmod-video-fwnode +kmod-video-rockchip-mipi-csi2
  FILES:=$(PKG_BUILD_DIR)/video_rkisp.ko
  AUTOLOAD:=$(call AutoLoad,45,video_rkisp)
endef

define KernelPackage/video-rockchip-isp/description
  Kernel module for the Rockchip ISP block.
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

$(eval $(call KernelPackage,video-rockchip-isp))
