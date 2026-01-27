# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=omega4-hpmcu-mbox
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/omega4-hpmcu-mbox
  SUBMENU:=Other modules
  TITLE:=Omega4 HPMCU mailbox client
  DEPENDS:=@TARGET_rockchip +kmod-omega4-hpmcu-loader
  FILES:=$(PKG_BUILD_DIR)/omega4_hpmcu_mbox.ko
  AUTOLOAD:=$(call AutoLoad,31,omega4_hpmcu_mbox)
endef

define KernelPackage/omega4-hpmcu-mbox/description
  Simple mailbox client to ping the RV1103B HPMCU over Rockchip mailbox.
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

$(eval $(call KernelPackage,omega4-hpmcu-mbox))
