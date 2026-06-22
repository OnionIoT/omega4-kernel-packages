# SPDX-License-Identifier: LicenseRef-Rockchip-Binary

include $(TOPDIR)/rules.mk

PKG_NAME:=rockchip-rknn
PKG_RELEASE:=3
PKG_FLAGS:=nonshared

PKG_LICENSE:=LicenseRef-Rockchip-Binary
PKG_MAINTAINER:=Onion Corporation

RSTRIP:=:

include $(INCLUDE_DIR)/package.mk

define Package/rockchip-rknn
  SECTION:=multimedia
  CATEGORY:=Multimedia
  TITLE:=Rockchip RKNN runtime and RV1103B validation demo
  DEPENDS:=@TARGET_rockchip +kmod-rknpu-rockchip +rockchip-aiq +v4l-utils
endef

define Package/rockchip-rknn/description
  Rockchip RKNN userspace runtime for RV1103B/RV1106B class SoCs. This package
  installs the uClibc RKNN runtime blob, rknn_server debug proxy, and a
  Mobilenet validation demo built against Rockchip's RV1103B RKNN runtime.
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(PKG_BUILD_DIR)/omega4-nv12-to-bmp \
		./src/omega4-nv12-to-bmp.c
endef

define Package/rockchip-rknn/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/usr/bin/rknn_server $(1)/usr/bin/rknn_server
	$(INSTALL_BIN) ./files/usr/bin/rknn_mobilenet_demo $(1)/usr/bin/rknn_mobilenet_demo
	$(INSTALL_BIN) ./files/usr/bin/omega4-rknn-mobilenet $(1)/usr/bin/omega4-rknn-mobilenet
	$(INSTALL_BIN) ./files/usr/bin/omega4-rknn-camera $(1)/usr/bin/omega4-rknn-camera
	$(INSTALL_BIN) ./files/usr/bin/omega4-rknn-yolo-live $(1)/usr/bin/omega4-rknn-yolo-live
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/omega4-nv12-to-bmp $(1)/usr/bin/omega4-nv12-to-bmp

	$(INSTALL_DIR) $(1)/usr/lib/rknn-uclibc
	$(INSTALL_DATA) ./files/usr/lib/rknn-uclibc/librknnmrt.so $(1)/usr/lib/rknn-uclibc/

	$(INSTALL_DIR) $(1)/usr/share/rockchip-rknn/models
	$(INSTALL_DATA) ./files/usr/share/rockchip-rknn/models/coco_80_labels_list.txt \
		$(1)/usr/share/rockchip-rknn/models/

	$(INSTALL_DIR) $(1)/usr/share/rockchip-rknn/models/RV1106B_RV1103B
	$(INSTALL_DATA) ./files/usr/share/rockchip-rknn/models/RV1106B_RV1103B/mobilenet_v1.rknn \
		$(1)/usr/share/rockchip-rknn/models/RV1106B_RV1103B/
	$(INSTALL_DATA) ./files/usr/share/rockchip-rknn/models/RV1106B_RV1103B/yolov5n.rknn \
		$(1)/usr/share/rockchip-rknn/models/RV1106B_RV1103B/

	$(INSTALL_DIR) $(1)/usr/share/rockchip-rknn/images
	$(INSTALL_DATA) ./files/usr/share/rockchip-rknn/images/dog_224x224.jpg \
		$(1)/usr/share/rockchip-rknn/images/
endef

$(eval $(call BuildPackage,rockchip-rknn))
