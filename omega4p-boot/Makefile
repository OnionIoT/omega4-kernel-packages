# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=omega4p-boot
PKG_VERSION:=2017.09-e89cd37b
PKG_RELEASE:=3
PKG_LICENSE:=GPL-2.0+ Apache-2.0
PKG_LICENSE_FILES:=u-boot/Licenses/README rkbin/LICENSE
PKG_BUILD_PARALLEL:=1
PKG_TARGETS:=bin
PKG_FLAGS:=nonshared
PKG_CONFIG_DEPENDS:=CONFIG_OMEGA4P_BOOT_SDK_DIR

include $(INCLUDE_DIR)/package.mk

SDK_PATH=$(if $(CONFIG_OMEGA4P_BOOT_SDK_DIR),$(call qstrip,$(CONFIG_OMEGA4P_BOOT_SDK_DIR)),../../ref/1103B-SDK)
SDK_DIR=$(if $(filter /%,$(SDK_PATH)),$(SDK_PATH),$(TOPDIR)/$(SDK_PATH))
export GCC_HONOUR_COPTS=s

define Package/omega4p-boot
  SECTION:=boot
  CATEGORY:=Boot Loaders
  TITLE:=Omega4+ DDR loader, SPL, U-Boot and environment
  DEPENDS:=@TARGET_rockchip_cortexa7 @!IN_SDK
  DEFAULT:=y if TARGET_rockchip_cortexa7_DEVICE_onion_omega4p-evb
endef

define Package/omega4p-boot/config
  source "$(SOURCE)/Config.in"
endef

define Package/omega4p-boot/description
  Builds vendor U-Boot/SPL and packages the tested DDR and MCU binaries.
  Produces host-side flash images; installs nothing into the target rootfs.
endef

define Build/Prepare
	python3 ./scripts/prepare.py --sdk "$(SDK_DIR)" --output "$(PKG_BUILD_DIR)"
	$(call Build/Patch)
endef

define Build/Configure
	+$(MAKE) $(PKG_JOBS) -C $(PKG_BUILD_DIR)/u-boot \
		CROSS_COMPILE=$(TARGET_CROSS) omega4p_defconfig
endef

define Build/Compile
	+cd $(PKG_BUILD_DIR)/u-boot && \
		./make.sh --spl-new ../rkbin/RKBOOT/omega4p.ini --no-sign \
		CROSS_COMPILE=$(TARGET_CROSS)
	python3 ./scripts/verify.py --build "$(PKG_BUILD_DIR)"
endef

define Package/omega4p-boot/install
	$(CP) $(PKG_BUILD_DIR)/images/. $(1)/
	$(INSTALL_DIR) $(1)/licenses
	$(CP) ./files/LICENSE.* $(1)/licenses/
	$(CP) $(PKG_BUILD_DIR)/u-boot/Licenses/README $(1)/licenses/U-Boot-README
endef

$(eval $(call BuildPackage,omega4p-boot))
