# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=rockchip-aiq
PKG_RELEASE:=1
PKG_FLAGS:=nonshared

PKG_LICENSE:=LicenseRef-Rockchip-Binary
PKG_MAINTAINER:=Onion Corporation

RSTRIP:=:

include $(INCLUDE_DIR)/package.mk

define Package/rockchip-aiq
  SECTION:=multimedia
  CATEGORY:=Multimedia
  TITLE:=Rockchip AIQ 3A daemon and Omega4 SC3336 IQ files
  DEPENDS:=@TARGET_rockchip +kmod-video-rockchip-isp +kmod-video-sc3336
endef

define Package/rockchip-aiq/description
  Rockchip AIQ userspace daemon, uClibc runtime compatibility files, and
  Omega4 SC3336 camera IQ files for the ISP 3A pipeline.
endef

define Package/rockchip-aiq/conffiles
/etc/iqfiles/sc3336_SC3336_default.json
/etc/iqfiles/sc3336_SC3336_default.bin
endef

define Build/Compile
endef

define Package/rockchip-aiq/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/usr/bin/rkaiq_3A_server $(1)/usr/bin/
	$(INSTALL_BIN) ./files/usr/bin/rkaiq_tool_server $(1)/usr/bin/
	$(INSTALL_BIN) ./files/usr/bin/rkisp_demo $(1)/usr/bin/

	$(INSTALL_DIR) $(1)/lib
	$(INSTALL_BIN) ./files/usr/lib/rkaiq-uclibc/ld-uClibc.so.0 $(1)/lib/
	$(INSTALL_BIN) ./files/usr/lib/rkaiq-uclibc/ld-uClibc.so.1 $(1)/lib/

	$(INSTALL_DIR) $(1)/usr/lib/rkaiq-uclibc
	$(INSTALL_BIN) ./files/usr/lib/rkaiq-uclibc/ld-uClibc.so.0 $(1)/usr/lib/rkaiq-uclibc/
	$(INSTALL_BIN) ./files/usr/lib/rkaiq-uclibc/ld-uClibc.so.1 $(1)/usr/lib/rkaiq-uclibc/
	$(INSTALL_DATA) ./files/usr/lib/rkaiq-uclibc/libc.so.0 $(1)/usr/lib/rkaiq-uclibc/
	$(INSTALL_DATA) ./files/usr/lib/rkaiq-uclibc/libgcc_s.so.1 $(1)/usr/lib/rkaiq-uclibc/
	$(INSTALL_DATA) ./files/usr/lib/rkaiq-uclibc/libstdc++.so.6 $(1)/usr/lib/rkaiq-uclibc/
	$(INSTALL_DATA) ./files/usr/lib/rkaiq-uclibc/librkaiq.so $(1)/usr/lib/rkaiq-uclibc/
	$(INSTALL_DATA) ./files/usr/lib/rkaiq-uclibc/librkrawstream.so $(1)/usr/lib/rkaiq-uclibc/
	$(INSTALL_DATA) ./files/usr/lib/rkaiq-uclibc/libsmartIr.so $(1)/usr/lib/rkaiq-uclibc/

	$(INSTALL_DIR) $(1)/etc/iqfiles
	$(INSTALL_DATA) ./files/etc/iqfiles/sc3336_SC3336_default.json $(1)/etc/iqfiles/
	$(INSTALL_DATA) ./files/etc/iqfiles/sc3336_SC3336_default.bin $(1)/etc/iqfiles/

	$(INSTALL_DIR) $(1)/data
	$(INSTALL_DATA) ./files/data/cac_map_hw_b_1_2304x1296_0.272988-bigmode.bin $(1)/data/

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/rkaiq $(1)/etc/init.d/
endef

$(eval $(call BuildPackage,rockchip-aiq))
