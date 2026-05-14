include $(TOPDIR)/rules.mk

PKG_NAME:=mfg-test-tools
PKG_VERSION:=1.0
PKG_RELEASE:=3
PKG_MAINTAINER:=Onion

include $(INCLUDE_DIR)/package.mk

define Package/mfg-test-tools
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Omega4 manufacturing test tools
  DEPENDS:=+omega4-pinmux
endef

define Package/mfg-test-tools/description
 Manufacturing test tools and scripts for Omega4 production.
 Includes GPIO output sequencing tests and AIC WiFi/BT MAC programming helpers.
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(PKG_BUILD_DIR)/mfg-aic-uart $(CURDIR)/src/mfg-aic-uart.c
endef

define Package/mfg-test-tools/install
	$(INSTALL_DIR) $(1)/etc/mfg-test-tools
	$(INSTALL_CONF) ./files/etc/mfg-test-tools/gpio-list $(1)/etc/mfg-test-tools/
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/usr/sbin/mfg-gpio-test $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/usr/sbin/mfg-aic-mac $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/mfg-aic-uart $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/usr/share/mfg-test-tools
	$(INSTALL_DATA) ./files/usr/share/mfg-test-tools/aic8800d80-mfg-testmode-uart.bin $(1)/usr/share/mfg-test-tools/
endef

$(eval $(call BuildPackage,mfg-test-tools))
