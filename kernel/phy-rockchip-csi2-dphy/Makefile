# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=phy-rockchip-csi2-dphy
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/phy-rockchip-csi2-dphy
  SUBMENU:=Video Support
  TITLE:=Rockchip CSI2 D-PHY support
  DEPENDS:=@TARGET_rockchip +kmod-video-core +kmod-video-fwnode
  FILES:= \
	$(PKG_BUILD_DIR)/phy-rockchip-csi2-dphy-hw.ko \
	$(PKG_BUILD_DIR)/phy-rockchip-csi2-dphy.ko
  AUTOLOAD:=$(call AutoLoad,40,phy-rockchip-csi2-dphy-hw phy-rockchip-csi2-dphy)
endef

define KernelPackage/phy-rockchip-csi2-dphy/description
  Kernel module for the Rockchip CSI2 D-PHY receiver.
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

$(eval $(call KernelPackage,phy-rockchip-csi2-dphy))
