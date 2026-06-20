# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=video-rockchip-mpp
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/video-rockchip-mpp
  SUBMENU:=Video Support
  TITLE:=Rockchip MPP video encoder support
  DEPENDS:=@TARGET_rockchip +kmod-dma-buf
  KCONFIG:= \
	CONFIG_DMABUF_HEAPS=y \
	CONFIG_DMABUF_HEAPS_SYSTEM=y
  FILES:=$(PKG_BUILD_DIR)/rk_vcodec.ko
  AUTOLOAD:=$(call AutoLoad,47,rk_vcodec)
endef

define KernelPackage/video-rockchip-mpp/description
  Kernel module for the Rockchip MPP service and RV1103B RKVENC hardware encoder.
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/. $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) \
		M="$(PKG_BUILD_DIR)" \
		modules
endef

$(eval $(call KernelPackage,video-rockchip-mpp))
