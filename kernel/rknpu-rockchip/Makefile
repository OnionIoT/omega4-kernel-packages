# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=rknpu-rockchip
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/rknpu-rockchip
  SUBMENU:=Other modules
  TITLE:=Rockchip RKNPU driver
  DEPENDS:=@TARGET_rockchip
  FILES:=$(PKG_BUILD_DIR)/rknpu.ko
  AUTOLOAD:=$(call AutoLoad,45,rknpu)
endef

define KernelPackage/rknpu-rockchip/description
  Out-of-tree Rockchip RKNPU kernel module for RV1103B/RV1106 class SoCs.
endef

KERNEL_MAKE_FLAGS += \
	CONFIG_ROCKCHIP_RKNPU=m \
	CONFIG_ROCKCHIP_RKNPU_DEBUG_FS=y \
	CONFIG_ROCKCHIP_RKNPU_PROC_FS=y \
	CONFIG_ROCKCHIP_RKNPU_FENCE=n \
	CONFIG_ROCKCHIP_RKNPU_SRAM=n \
	CONFIG_ROCKCHIP_RKNPU_DRM_GEM=y \
	CONFIG_ROCKCHIP_RKNPU_DMA_HEAP=n

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/. $(PKG_BUILD_DIR)/
	$(SED) 's/-Werror/-Wno-error/g' $(PKG_BUILD_DIR)/Makefile
endef

define Build/Compile
	+$(KERNEL_MAKE) \
		M="$(PKG_BUILD_DIR)" \
		$(KERNEL_MAKE_FLAGS) \
		modules
endef

$(eval $(call KernelPackage,rknpu-rockchip))
