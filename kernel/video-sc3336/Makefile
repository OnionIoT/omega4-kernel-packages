# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=video-sc3336
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/video-sc3336
  SUBMENU:=Video Support
  TITLE:=SmartSens SC3336 sensor support
  DEPENDS:=+kmod-video-core +kmod-video-fwnode +kmod-i2c-core
  FILES:=$(PKG_BUILD_DIR)/sc3336.ko
  AUTOLOAD:=$(call AutoProbe,sc3336)
endef

define KernelPackage/video-sc3336/description
  Kernel module for the SmartSens SC3336 camera sensor.
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

$(eval $(call KernelPackage,video-sc3336))
