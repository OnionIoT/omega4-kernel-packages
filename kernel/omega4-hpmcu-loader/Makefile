# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=omega4-hpmcu-loader
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/omega4-hpmcu-loader
  SUBMENU:=Other modules
  TITLE:=Omega4 HPMCU firmware loader
  DEPENDS:=@TARGET_rockchip
  FILES:=$(PKG_BUILD_DIR)/omega4_hpmcu_loader.ko
  AUTOLOAD:=$(call AutoLoad,30,omega4_hpmcu_loader)
endef

define KernelPackage/omega4-hpmcu-loader/description
  Kernel module that loads and boots RV1103B HPMCU firmware from Linux.
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

define KernelPackage/omega4-hpmcu-loader/install
	$(INSTALL_DIR) $(1)/lib/firmware/omega4
	$(INSTALL_DATA) ./files/lib/firmware/omega4/rv1103b-hpmcu-bare.bin $(1)/lib/firmware/omega4/
	$(INSTALL_DATA) ./files/lib/firmware/omega4/rv1103b-hpmcu-alt.bin $(1)/lib/firmware/omega4/
endef

$(eval $(call KernelPackage,omega4-hpmcu-loader))
