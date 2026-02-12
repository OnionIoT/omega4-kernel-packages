# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=aic8800d-sdio
PKG_RELEASE:=1

STAMP_CONFIGURED_DEPENDS := $(STAGING_DIR)/usr/include/mac80211-backport/backport/autoconf.h

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

# we build like other kernel modules
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

define KernelPackage/aic8800d-sdio
  SUBMENU:=Wireless Drivers
  TITLE:=AIC8800D SDIO Wi-Fi support
  DEPENDS:=+kmod-cfg80211 +kmod-mac80211 +kmod-mmc +aic8800d-firmware +@DRIVER_11AC_SUPPORT +@DRIVER_11AX_SUPPORT
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
NOSTDINC_FLAGS := \
	$(KERNEL_NOSTDINC_FLAGS) \
	-I$(PKG_BUILD_DIR)/aic8800_fdrv \
	-I$(PKG_BUILD_DIR)/aic8800_bsp \
	-I$(PKG_BUILD_DIR)/aic8800_btlpm

MAC80211_MAKEFILE:=$(TOPDIR)/package/kernel/mac80211/Makefile
ifneq ($(wildcard $(MAC80211_MAKEFILE)),)
  MAC80211_BACKPORT_VERSION:=$(strip $(shell sed -n 's/^PKG_VERSION:=//p' $(MAC80211_MAKEFILE)))
  ifneq ($(MAC80211_BACKPORT_VERSION),)
    MAC80211_BACKPORT_VER_MAJ:=$(word 1,$(subst ., ,$(MAC80211_BACKPORT_VERSION)))
    MAC80211_BACKPORT_VER_MIN:=$(word 2,$(subst ., ,$(MAC80211_BACKPORT_VERSION)))
    MAC80211_BACKPORT_VER_PAT:=$(word 3,$(subst ., ,$(MAC80211_BACKPORT_VERSION)))
    MAC80211_BACKPORT_VER_MAJ:=$(if $(MAC80211_BACKPORT_VER_MAJ),$(MAC80211_BACKPORT_VER_MAJ),0)
    MAC80211_BACKPORT_VER_MIN:=$(if $(MAC80211_BACKPORT_VER_MIN),$(MAC80211_BACKPORT_VER_MIN),0)
    MAC80211_BACKPORT_VER_PAT:=$(if $(MAC80211_BACKPORT_VER_PAT),$(MAC80211_BACKPORT_VER_PAT),0)
    MAC80211_BACKPORT_VER_CODE:=$(shell printf '0x%02x%02x%02x' $(MAC80211_BACKPORT_VER_MAJ) $(MAC80211_BACKPORT_VER_MIN) $(MAC80211_BACKPORT_VER_PAT))
    NOSTDINC_FLAGS += \
      -DCFG80211_VERSION_CODE=$(MAC80211_BACKPORT_VER_CODE) \
      -DAIC_LINUX_VERSION_CODE=$(MAC80211_BACKPORT_VER_CODE)
  endif
endif

# vendor Kconfig-y toggles that their Makefiles expect
KERNEL_MAKE_FLAGS += \
	CONFIG_AIC8800_WLAN_SUPPORT=m \
	CONFIG_AIC_WLAN_SUPPORT=m \
	CONFIG_AIC8800_BTLPM_SUPPORT=m \
	CONFIG_SDIO_SUPPORT=y \
	CONFIG_USB_SUPPORT=n \
	CONFIG_SDIO_BT=y \
	CONFIG_USB_BT=n \
	CONFIG_USE_FW_REQUEST=y \
	CONFIG_PREALLOC_RX_SKB=y \
	CONFIG_PREALLOC_TXQ=y \
	CONFIG_FILTER_TCP_ACK=y

# ---------------------------------------------------------------------
# PREPARE: copy their tree and (lightly) relax -Werror in the worst dirs
# ---------------------------------------------------------------------
define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./src/. $(PKG_BUILD_DIR)/
endef

# Tell Kbuild where to find backports/mac80211 headers
# and force-include the backport shim headers
NOSTDINC_FLAGS += \
  -I$(STAGING_DIR)/usr/include/mac80211-backport/uapi \
  -I$(STAGING_DIR)/usr/include/mac80211-backport \
  -I$(STAGING_DIR)/usr/include/mac80211/uapi \
  -I$(STAGING_DIR)/usr/include/mac80211 \
  -include backport/autoconf.h \
  -include backport/backport.h

# Disable treating warnings as errors
NOSTDINC_FLAGS += \
	-Wno-error


define Build/Compile
	+$(KERNEL_MAKE) \
		M="$(PKG_BUILD_DIR)" \
		NOSTDINC_FLAGS="$(NOSTDINC_FLAGS)" \
		$(KERNEL_MAKE_FLAGS) \
		modules
endef

# ---------------------------------------------------------------------
# If the driver changes its .ko names, this install rule will still grab them.
# ---------------------------------------------------------------------
define KernelPackage/aic8800d-sdio/install
	$(INSTALL_DIR) $(1)/lib/modules/$(LINUX_VERSION)
	# top-level .ko
	$(CP) $(PKG_BUILD_DIR)/*.ko $(1)/lib/modules/$(LINUX_VERSION)/ 2>/dev/null || true
	# subdir .ko (what you currently have)
	$(CP) $(PKG_BUILD_DIR)/*/*.ko $(1)/lib/modules/$(LINUX_VERSION)/ 2>/dev/null || true
endef

$(eval $(call KernelPackage,aic8800d-sdio))
