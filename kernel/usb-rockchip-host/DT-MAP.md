# RV1103B USB Host DT Map (SDK reference)

This maps the USB host/PHY device-tree nodes from the 1103B SDK into the
OpenWrt RV1103B device trees used for Omega4.

## SDK reference nodes
Source: `sysdrv/source/kernel/arch/arm/boot/dts/rv1103b.dtsi`

- `usbdrd` (DWC3 wrapper)
  - compatible: `rockchip,rv1103b-dwc3`, `rockchip,rk3399-dwc3`
  - clocks: `CLK_REF_USBOTG`, `CLK_UTMI_USBOTG`, `ACLK_USBOTG`
  - status: `disabled`
- `usbdrd_dwc3` (`usb@20b00000`)
  - compatible: `snps,dwc3`
  - dr_mode: `otg`
  - maximum-speed: `high-speed`
  - phys: `u2phy_otg` (`usb2-phy`)
  - status: `disabled`
- `u2phy` (`usb2-phy@20e10000`)
  - compatible: `rockchip,rv1103b-usb2phy`
  - clocks: `CLK_REF_USBPHY`, `PCLK_USBPHY`
  - resets: `SRST_RESETN_USBPHY_POR`, `SRST_RESETN_USBPHY_OTG`
  - status: `disabled`
- `u2phy_otg` (child of `u2phy`)
  - interrupts: otg-bvalid, otg-id, linestate, disconnect
  - status: `disabled`

SDK board enablement:
- `sysdrv/source/kernel/arch/arm/boot/dts/rv1103b-evb.dtsi`
  - `&u2phy` -> `status = "okay"`
  - `&u2phy_otg` -> `rockchip,vbus-always-on; status = "okay"`
  - `&usbdrd` -> `status = "okay"`
  - `&usbdrd_dwc3` -> `extcon = <&u2phy>; status = "okay"`

## OpenWrt mapping (Omega4)
OpenWrt nodes match the SDK definitions:
- `target/linux/rockchip/dts/rv1103b.dtsi`
- `target/linux/rockchip/dts/rv1103b-omega4.dtsi` keeps USB nodes disabled;
  the board DTS overrides them for bring-up.

Board override used for builds:
- `target/linux/rockchip/image/cortexa7.mk` selects
  `rv1103b-omega4-evb.dts`.
- `target/linux/rockchip/dts/rv1103b-omega4-evb.dts` enables `&u2phy`,
  `&u2phy_otg`, `&usbdrd`, and `&usbdrd_dwc3`, sets `dr_mode = "host"`,
  and deletes the `extcon` property to avoid probe deferral in host-only
  mode.

## Host bring-up checklist
- Ensure `&u2phy`, `&u2phy_otg`, `&usbdrd`, and `&usbdrd_dwc3` are `okay`.
- Keep `rockchip,vbus-always-on` on `&u2phy_otg` if VBUS is always powered.
- For host mode, set `dr_mode = "host"` (or revert to `"otg"` if ID/VBUS
  detection is wired).
- Keep `extcon = <&u2phy>` on `&usbdrd_dwc3` when using OTG signaling.
