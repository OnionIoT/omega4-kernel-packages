# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=aic8800d-sdio
PKG_RELEASE:=1

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

# we build like other kernel modules
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

define KernelPackage/aic8800d-sdio
  SUBMENU:=Wireless Drivers
  TITLE:=AIC8800D SDIO Wi-Fi support
  DEPENDS:=+kmod-cfg80211 +kmod-mac80211 +kmod-mmc
  # if you know the exact .ko names, keep this; otherwise use the generic install() below
  FILES:= \
	$(PKG_BUILD_DIR)/aic8800_bsp/aic8800_bsp.ko \
	$(PKG_BUILD_DIR)/aic8800_fdrv/aic8800_fdrv.ko \
	$(PKG_BUILD_DIR)/aic8800_btlpm/aic8800_btlpm.ko
  AUTOLOAD:=$(call AutoLoad,52,aic8800_bsp aic8800_fdrv)
endef

define KernelPackage/aic8800d-sdio/description
 Out-of-tree AIC8800D SDIO Wi-Fi driver.
endef

# vendor headers live inside the src/ tree
NOSTDINC_FLAGS += \
	-I$(PKG_BUILD_DIR)/aic8800_fdrv \
	-I$(PKG_BUILD_DIR)/aic8800_bsp \
	-I$(PKG_BUILD_DIR)/aic8800_btlpm

# vendor Kconfig-y toggles that their Makefiles expect
KERNEL_MAKE_FLAGS += \
	CONFIG_AIC8800_WLAN_SUPPORT=m \
	CONFIG_AIC_WLAN_SUPPORT=m \
	CONFIG_AIC8800_BTLPM_SUPPORT=m \
	CONFIG_SDIO_SUPPORT=y \
	CONFIG_USB_SUPPORT=n \
	CONFIG_USE_FW_REQUEST=y \
	CONFIG_PREALLOC_RX_SKB=y \
	CONFIG_PREALLOC_TXQ=y \
	CONFIG_FILTER_TCP_ACK=y

# ---------------------------------------------------------------------
# PREPARE: copy their tree and (lightly) relax -Werror in the worst dirs
# ---------------------------------------------------------------------
define Build/Prepare
	$(call Build/Prepare/Default)

	# 1) copy vendor sources from this package into the kernel build dir
	$(CP) ./src/. $(PKG_BUILD_DIR)/

	# vendor makefiles are left untouched so build issues surface during compile
endef

# ---------------------------------------------------------------------
# COMPILE: build THROUGH the kernel and force KCFLAGS to win over any -Werror
# ---------------------------------------------------------------------
define Build/Compile
	+$(KERNEL_MAKE) \
		M="$(PKG_BUILD_DIR)" \
		EXTRA_CFLAGS="$(NOSTDINC_FLAGS)" \
		KCFLAGS=" \
		" \
		$(KERNEL_MAKE_FLAGS) \
		modules
endef

# ---------------------------------------------------------------------
# If the driver changes its .ko names, this install rule will still grab them.
# If you’re happy with the FILES:= above, you can drop this section.
# ---------------------------------------------------------------------
define KernelPackage/aic8800d-sdio/install
	$(INSTALL_DIR) $(1)/lib/modules/$(LINUX_VERSION)
	# top-level .ko
	$(CP) $(PKG_BUILD_DIR)/*.ko $(1)/lib/modules/$(LINUX_VERSION)/ 2>/dev/null || true
	# subdir .ko (what you currently have)
	$(CP) $(PKG_BUILD_DIR)/*/*.ko $(1)/lib/modules/$(LINUX_VERSION)/ 2>/dev/null || true
endef

$(eval $(call KernelPackage,aic8800d-sdio))
