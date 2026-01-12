# usb-rockchip-host (OOT)

Skeleton package for RV1103B USB host/PHY bring-up.

Notes:
- Stage driver sources in `src/` (use `/home/zh/workspace/linux-stable` as the primary source).
- Use `/home/zh/workspace/1103B-SDK` only as a reference when porting.
- Kernel tree changes are required for RV1103B USB2 PHY support:
  - add `rv1103b_usb2phy_tuning` + `rv1103b_phy_cfgs` and DT match entry in
    `drivers/phy/rockchip/phy-rockchip-inno-usb2.c`.
  - define `EXTCON_USB_VBUS_EN` in `include/linux/extcon.h`.
- Add module objects to `src/Makefile`.
- List module names in `USB_HOST_MODULES` and `USB_HOST_AUTOLOAD` in `Makefile`.
- See `DT-MAP.md` for the USB host/PHY device-tree mapping and enablement notes.
