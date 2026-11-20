# Original Omega4 boot package

`omega4-boot` builds U-Boot and SPL for the original RV1103B Omega4 and emits
`download.bin`, `idblock.img`, `uboot.img`, `SHA256SUMS` and `build-info.json`.
It is an OpenWrt `bin` package: nothing is installed in the device rootfs, and
building it never flashes a device. It is independent of `omega4p-boot`.

## Build

From the OpenWrt root, with this local feed configured:

```sh
./scripts/feeds update omega4ko
./scripts/feeds install omega4-boot
make menuconfig   # select Rockchip / Cortex-A7 / Onion Omega4 EVB
make defconfig
make package/omega4-boot/compile -j20 V=s
```

Use the feed name in your `feeds.conf`; `feeds.conf.default` calls the feed
`omega4_kernel_packages`. The original Omega4 profile selects this package by
default. A full OpenWrt build then builds it automatically. Outputs are under
`bin/targets/rockchip/cortexa7/omega4-boot/`.

As with `omega4p-boot`, this currently requires the reference SDK Git checkout.
`CONFIG_OMEGA4_BOOT_SDK_DIR` defaults to `../../ref/1103B-SDK` relative to the
OpenWrt root; an absolute path also works. Preparation exports committed
U-Boot/rkbin source at `e89cd37b1e87acb3f76c793c41a14e2efb2e77d6` into a private
build directory and applies the included GCC 13 compatibility patch. It neither
uses SDK build outputs nor modifies that checkout. Host requirements include
Git, Python 3, tar, Bash, DTC and standard U-Boot host build dependencies;
the SDK packing tool requires x86-64 Linux.

The SDK compression script expects the xz-style `lzma` CLI, while OpenWrt's
host `lzma` uses a different syntax. The package uses Python's LZMA-alone
encoder with the SDK's four-byte decompressed-size trailer. This prevents a
silent empty U-Boot payload. Python must include its standard `lzma` module.

## Original-board inputs and boot path

The baseline is the original board's existing flasher fallback images under
`omega4-flasher/backend/fallback-images/`. Unpacking that downloader confirms
its DDR payload is exactly SDK `rv1103b_ddr_924MHz_v1.05.bin`, with zero padding.
The USB plug also comes from the pinned SDK revision. There is no MCU payload.

- DDR v1.05 SHA256:
  `9907da36f1da5cffa594918aa9f1ed9210e5e4d3eea719f49fe4cb02a68973a8`.
- Decoded DDR settings: DDR3 924 MHz, DDR2 528 MHz, UART0 M0 at 115200 baud.
  These are configuration values, not measured operating frequencies.
- U-Boot/SPL use the SDK `rv1103b_defconfig` with explicit 115200 baud and
  `CONFIG_MTD_BLK_U_BOOT_OFFS=0xA00` (512-byte sectors, byte offset `0x140000`).
- SPL boots U-Boot; `CONFIG_SPL_KERNEL_BOOT` is disabled. This preserves the
  original OpenWrt FIT's U-Boot relocation values (`0xffffff00/1`) and resource
  image. Omega4+'s direct-SPL Linux path must not be substituted.
- The baseline U-Boot environment contains
  `bootcmd=boot_fit;boot_android ${devtype} ${devnum};` and `baudrate=115200`.

The existing original Omega4 OpenWrt layout stays unchanged:

| Partition | Byte offset | Capacity |
| --- | --- | --- |
| env | 0 | 0x40000 |
| idblock | 0x40000 | 0x100000 |
| uboot | 0x140000 | 0x100000 |
| boot | 0x240000 | 0x800000 |
| ubi | 0xa40000 | Remaining NAND |

This package does not create or overwrite env, kernel or rootfs. Normal
sysupgrade continues to preserve the installed bootloader. Original Omega4
and Omega4+ boot packages are not interchangeable.

## Verification

The build checks UART, U-Boot offset and SPL boot mode, verifies both DDR copies,
the USB plug and newly compiled SPL after unpacking download.bin, rejects an
unexpected MCU payload, and regenerates idblock for byte-for-byte comparison.
Images must fit the original board's partition capacities. The manifest records
board identity, input/source hashes and output hashes. New manifests always
start with `hardware_validated=false`.
The verifier also decompresses the U-Boot FIT payload and compares it with the
newly compiled binary, checks its DTB, and rejects empty/truncated payloads.

Host build/test results are recorded in the workspace
`hardware-validation/omega4-boot/`. Hardware validation requires a selected
original Omega4 lab target. A29C is an Omega4+ and must not receive these images.

Host validation passed on 19 November 2026: package compilation, unpacked
loader checks, idblock regeneration, compiled U-Boot/DTB comparison, empty
payload rejection, output hashes and per-board default package selection.
Hardware validation was explicitly deferred until an original Omega4 is available.
