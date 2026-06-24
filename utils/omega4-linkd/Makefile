include $(TOPDIR)/rules.mk

PKG_NAME:=omega4-linkd
PKG_VERSION:=0.1
PKG_RELEASE:=6

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/omega4-linkd
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Omega4 raw monitor-mode link daemon
  DEPENDS:=+iw
endef

define Package/omega4-linkd/description
 Minimal OpenHD-style raw monitor-mode link tool for Omega4 bring-up.
 It supports UDP-to-802.11 injection, 802.11-to-UDP receive, and a ping
 payload mode for basic RF transport validation. It also installs a small
 transparent MAVLink serial/UDP bridge for the telemetry layer.
endef

define Package/omega4-videod
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Omega4 RTP/RTSP video link helper
  DEPENDS:=+omega4-linkd
endef

define Package/omega4-videod/description
 Lightweight video helper for the Omega4 raw monitor-mode link. It can
 forward an existing RTP/UDP source over RF stream 1, or optionally use
 GStreamer to pull an RTSP camera and feed camera RTP packets into omega4-linkd.
 The ground side emits RTP/UDP for a player such as VLC or QGroundControl.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		$(TARGET_CONFIGURE_OPTS) \
		CFLAGS="$(TARGET_CFLAGS) $(TARGET_CPPFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define Package/omega4-linkd/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omega4-linkd $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omega4-mavlinkd $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/usr/share/doc/omega4-linkd
	$(INSTALL_DATA) ./README.link.md $(1)/usr/share/doc/omega4-linkd/
	$(INSTALL_DATA) ./README.telemetry.md $(1)/usr/share/doc/omega4-linkd/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/omega4-link $(1)/etc/init.d/
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/omega4-link $(1)/etc/config/
endef

define Package/omega4-videod/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/usr/sbin/omega4-videod $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/usr/share/doc/omega4-videod
	$(INSTALL_DATA) ./README.video.md $(1)/usr/share/doc/omega4-videod/
endef

$(eval $(call BuildPackage,omega4-linkd))
$(eval $(call BuildPackage,omega4-videod))
