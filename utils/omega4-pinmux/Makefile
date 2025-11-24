include $(TOPDIR)/rules.mk

PKG_NAME:=omega4-pinmux
PKG_VERSION:=1.0
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/omega4-pinmux
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Omega4 pinmux helper
endef

define Package/omega4-pinmux/description
 A small CLI tool to inspect and switch pinmux on Omega4 platforms
 using symbolic pin and function names (e.g. gpio0_a1, pwm0_ch0_m0).
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		$(TARGET_CONFIGURE_OPTS) \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define Package/omega4-pinmux/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omega4-pinmux $(1)/usr/bin/
endef

$(eval $(call BuildPackage,omega4-pinmux))
