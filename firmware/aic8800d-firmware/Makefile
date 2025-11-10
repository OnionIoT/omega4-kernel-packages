include $(TOPDIR)/rules.mk

PKG_NAME:=aic8800d-firmware
PKG_RELEASE:=1
  
include $(INCLUDE_DIR)/package.mk

define Package/aic8800d-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=AIC8800D firmware files
endef

define Package/aic8800d-firmware/description
 Firmware blobs, patch tables and configuration data required by the
 aic8800d-sdio kernel module.
endef

define Build/Compile
	true
endef

define Package/aic8800d-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware/
	$(CP) ./files/lib/firmware/aic8800d/aic8800D80/. $(1)/lib/firmware/aic8800d
endef

$(eval $(call BuildPackage,aic8800d-firmware))
