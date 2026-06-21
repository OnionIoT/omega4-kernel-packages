# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=rockchip-mpp
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/HermanChen/mpp.git
PKG_SOURCE_DATE:=2026-05-29
PKG_SOURCE_VERSION:=1375813cbbae5ad6861b166475dd8fb672183220
PKG_MIRROR_HASH:=664156171b251ea60607782556fdde434fc453865f300d22d344f49611fad313

PKG_LICENSE:=Apache-2.0 MIT
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=Onion Corporation

PKG_BUILD_PARALLEL:=1
PKG_INSTALL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/librockchip-mpp
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=Rockchip Media Process Platform userspace library
  URL:=https://github.com/HermanChen/mpp
  DEPENDS:=@TARGET_rockchip +libstdcpp +kmod-video-rockchip-mpp
endef

define Package/librockchip-mpp/description
  Rockchip MPP userspace libraries for hardware video encode/decode engines.
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include
	$(CP) $(PKG_INSTALL_DIR)/usr/include/rockchip $(1)/usr/include/
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/librockchip_mpp.so* $(1)/usr/lib/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/librockchip_vpu.so* $(1)/usr/lib/
	$(INSTALL_DIR) $(1)/usr/lib/pkgconfig
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/pkgconfig/rockchip_mpp.pc $(1)/usr/lib/pkgconfig/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/pkgconfig/rockchip_vpu.pc $(1)/usr/lib/pkgconfig/
endef

define Package/rockchip-mpp-tools
  SECTION:=multimedia
  CATEGORY:=Multimedia
  TITLE:=Rockchip MPP test tools
  URL:=https://github.com/HermanChen/mpp
  DEPENDS:=@TARGET_rockchip +librockchip-mpp
endef

define Package/rockchip-mpp-tools/description
  Test and diagnostic tools for Rockchip MPP, including mpi_enc_test.
endef

CMAKE_OPTIONS += \
	-DCMAKE_BUILD_TYPE=Release \
	-DBUILD_SHARED_LIBS=ON \
	-DBUILD_TEST=ON \
	-DMPP_SOC=RV1103 \
	-DWARNINGS_AS_ERRORS=OFF

define Package/librockchip-mpp/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/librockchip_mpp.so* $(1)/usr/lib/
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/librockchip_vpu.so* $(1)/usr/lib/
endef

define Package/rockchip-mpp-tools/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/*_test $(1)/usr/bin/
endef

$(eval $(call BuildPackage,librockchip-mpp))
$(eval $(call BuildPackage,rockchip-mpp-tools))
