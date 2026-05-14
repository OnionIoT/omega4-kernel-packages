include $(TOPDIR)/rules.mk

PKG_NAME:=mfg-test-tools
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_MAINTAINER:=Onion

include $(INCLUDE_DIR)/package.mk

define Package/mfg-test-tools
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Omega4 manufacturing test tools
endef

define Package/mfg-test-tools/description
 Manufacturing test tools and scripts for Omega4 production.
 Includes GPIO output sequencing tests.
endef

define Build/Compile
endef

define Package/mfg-test-tools/install
	$(INSTALL_DIR) $(1)/etc/mfg-test-tools
	$(INSTALL_CONF) ./files/etc/mfg-test-tools/gpio-list $(1)/etc/mfg-test-tools/
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/usr/sbin/mfg-gpio-test $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,mfg-test-tools))
