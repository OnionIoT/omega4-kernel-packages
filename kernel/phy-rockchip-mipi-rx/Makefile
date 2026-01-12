# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=phy-rockchip-mipi-rx
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/phy-rockchip-mipi-rx
  SUBMENU:=Video Support
  TITLE:=Rockchip MIPI RX DPHY
  DEPENDS:=@TARGET_rockchip +kmod-video-core +kmod-video-fwnode
  FILES:=$(PKG_BUILD_DIR)/phy-rockchip-mipi-rx.ko
  AUTOLOAD:=$(call AutoProbe,phy-rockchip-mipi-rx)
endef

define KernelPackage/phy-rockchip-mipi-rx/description
  Kernel module for the Rockchip MIPI RX DPHY.
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

$(eval $(call KernelPackage,phy-rockchip-mipi-rx))
