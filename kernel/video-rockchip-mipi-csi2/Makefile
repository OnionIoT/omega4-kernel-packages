# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=video-rockchip-mipi-csi2
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/video-rockchip-mipi-csi2
  SUBMENU:=Video Support
  TITLE:=Rockchip MIPI CSI2 host support
  DEPENDS:=@TARGET_rockchip +kmod-video-core +kmod-video-fwnode +kmod-phy-rockchip-csi2-dphy
  FILES:=$(PKG_BUILD_DIR)/rockchip-mipi-csi2.ko
  AUTOLOAD:=$(call AutoLoad,35,rockchip-mipi-csi2)
endef

define KernelPackage/video-rockchip-mipi-csi2/description
  Kernel module for Rockchip MIPI CSI2 host and HW.
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

$(eval $(call KernelPackage,video-rockchip-mipi-csi2))
