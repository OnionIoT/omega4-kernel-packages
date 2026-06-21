# SPDX-License-Identifier: Apache-2.0 OR MIT

include $(TOPDIR)/rules.mk

PKG_NAME:=omega4-vencd
PKG_RELEASE:=1

PKG_MAINTAINER:=Onion Corporation
PKG_LICENSE:=Apache-2.0 MIT

include $(INCLUDE_DIR)/package.mk

define Package/omega4-vencd
  SECTION:=multimedia
  CATEGORY:=Multimedia
  TITLE:=Omega4 hardware video encoder service
  DEPENDS:=@TARGET_rockchip +librockchip-mpp
endef

define Package/omega4-vencd/description
  Omega4 hardware H.264 encoder command and procd service using the Rockchip
  MPP userspace API and the V4L2 camera DMA-BUF path.
endef

TARGET_CFLAGS += -I$(STAGING_DIR)/usr/include/rockchip
TARGET_LDFLAGS += -lrockchip_mpp -lm

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-o $(PKG_BUILD_DIR)/omega4-vencd \
		$(CURDIR)/src/omega4-vencd.c \
		$(TARGET_LDFLAGS)
endef

define Package/omega4-vencd/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omega4-vencd $(1)/usr/bin/omega4-vencd
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/omega4-venc $(1)/etc/config/omega4-venc
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/omega4-vencd $(1)/etc/init.d/omega4-vencd
endef

$(eval $(call BuildPackage,omega4-vencd))
