include $(TOPDIR)/rules.mk

PKG_NAME:=aicrf-test
PKG_VERSION:=2025-10-16
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

PKG_LICENSE:=Proprietary
PKG_LICENSE_FILES:=

include $(INCLUDE_DIR)/package.mk

define Package/aicrf-test
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=AIC8800D Wi-Fi/BT RF test tools (wifi_test, bt_test)
endef

define Package/aicrf-test/description
 Linux RF test toolkit for AIC8800D family.
 Builds:
 - wifi_test: talks to wlan netdev via private ioctl (android_wifi_priv_cmd).
 - bt_test: talks to /dev/aicbt_dev (usb/uart/wlan modes, optional socket control).
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) \
		-o $(PKG_BUILD_DIR)/wifi_test \
		$(PKG_BUILD_DIR)/wifi_test.c \
		$(TARGET_LDFLAGS)
	$(TARGET_CC) $(TARGET_CFLAGS) \
		-o $(PKG_BUILD_DIR)/bt_test \
		$(PKG_BUILD_DIR)/bt_test.c \
		$(TARGET_LDFLAGS) -lpthread
endef

define Package/aicrf-test/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/wifi_test $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bt_test $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,aicrf-test))

