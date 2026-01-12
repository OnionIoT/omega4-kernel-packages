# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=usb-rockchip-host
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

# Populate these with module base names (no .ko) once sources are added.
USB_HOST_MODULES:=
USB_HOST_AUTOLOAD:=

define KernelPackage/usb-rockchip-host
  SUBMENU:=USB Support
  TITLE:=Rockchip RV1103B USB host support (OOT)
  DEPENDS:=@TARGET_rockchip +kmod-usb-core
  FILES:=$(if $(USB_HOST_MODULES),$(patsubst %,$(PKG_BUILD_DIR)/%.ko,$(USB_HOST_MODULES)))
  AUTOLOAD:=$(if $(USB_HOST_AUTOLOAD),$(call AutoLoad,40,$(USB_HOST_AUTOLOAD)))
endef

define KernelPackage/usb-rockchip-host/description
  Out-of-tree kernel modules for RV1103B USB host/PHY bring-up.
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

$(eval $(call KernelPackage,usb-rockchip-host))
