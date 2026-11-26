# Omega4+ boot package

`omega4p-boot` builds U-Boot and SPL using the OpenWrt ARM toolchain and
packages the Omega4+ USB downloader and NAND idblock. It is a `bin` build
package: no IPK is installed on the device and no flashing happens at build
time. Normal sysupgrade continues to preserve the bootloader and environment.

## Build

The Omega4+ profile selects this package by default. From the OpenWrt root:

```sh
./scripts/feeds update omega4ko
./scripts/feeds install omega4p-boot
make defconfig
make package/omega4p-boot/compile -j20 V=s
```

Use the local feed name from `feeds.conf` (`omega4ko` in this workspace;
`omega4_kernel_packages` in `feeds.conf.default`). A normal `make -j20` also
builds the package once the feed is installed and the profile selected.

Outputs are in `bin/targets/rockchip/cortexa7/omega4p-boot/`:

- `download.bin`: USB DDR initialization and flashing runtime, plus boot payloads.
- `idblock.img`: NAND boot payloads, including DDR initialization, MCU and SPL.
- `uboot.img`: vendor U-Boot FIT, including its configured trust payload.
- `SHA256SUMS`, `build-info.json`, and license notices.

The build currently requires the reference SDK **Git checkout**, defaulting to
`../../ref/1103B-SDK` relative to the OpenWrt root. Change
`CONFIG_OMEGA4P_BOOT_SDK_DIR` in menuconfig for another location. No SDK compiler,
kernel build, or finished bootloader image is used. `prepare.py` exports only
the committed `sysdrv/source/uboot` subtree at SDK revision
`e89cd37b1e87acb3f76c793c41a14e2efb2e77d6` into OpenWrt's private build directory.
SDK working-tree edits, stale objects and generated configuration are excluded;
the reference checkout is never written. This is a local-source package, not
yet an independently downloadable source release.

The SDK host packing tool is an x86-64 Linux executable. Host requirements
include Git, Python 3, tar, Bash, DTC and the usual U-Boot host build dependencies.
OpenWrt GCC 13 needs the small compatibility patch included here: match the
`cmd_process` enum declaration and explicitly guard uninitialized pinctrl
lookup pointers. The script inherits OpenWrt's parallel make jobserver.

## Pinned inputs

The working SDK release is
`output/rv1106b-256nand-256ram-ddr667-v107-uart115200/`.

| Input | Provenance |
| --- | --- |
| U-Boot/SPL, USB plug, trust binary, packing tool | Pinned SDK Git revision above |
| `files/omega4p_defconfig` | SDK `rv1106b-tb-spi-nand-uart115200_defconfig` |
| `files/ddr.bin` | Working release's configured DDR v1.07, 667 MHz, UART 115200 |
| `files/hpmcu.bin` | `Hpmcu.bin` extracted from the working release's `download.bin` |

DDR source upstream was Rockchip rkbin commit
`3e288fe814e059dd06833495f845cab04ac20a5c`,
`rv1103b_ddr_924MHz_tb_v1.07.bin`, SHA256
`20659d447bb47262f116d8041f4bffda8f3ceba19e9fab40405f8f05b867de58`.
The SDK configured it for 667 MHz and UART 115200 and added its identifying
banner. We retain the exact tested result:
`c88ff2396becbed8621c65579028c77fb832a1281c662cbc9c23ccc797f5fa9e`.

The MCU input is the SDK's RT-Thread firmware, not the stock rkbin MCU image.
Its extracted/padded SHA256 is
`1a8be624e8e3745e285717aec6c6de3fa6fcdb480a92c236d96b7c6ba2527e49`.
This package retains that prebuilt firmware; it does not rebuild the MCU.
The DDR/rkbin terms and RT-Thread Apache license are preserved under `files/`.
U-Boot licensing is recorded in the exported source's `Licenses/README`.

## Verification and use

Every build unpacks the new downloader and compares USB DDR, NAND DDR, MCU,
USB plug and SPL against their inputs. SPL must match the newly compiled
`spl/u-boot-spl.bin`; it cannot silently fall back to the SDK SPL blob.
Regenerating idblock from download.bin must match the emitted idblock exactly.
The build also checks image capacities and U-Boot FIT magic, then records
image hashes and source/configuration provenance. Hardware validation is not
inferred from these checks: new manifests start with `hardware_validated=false`.

To bundle these images with the OpenWrt kernel/rootfs:

```sh
python3 scripts/omega4p/bundle.py \
  --boot-dir bin/targets/rockchip/cortexa7/omega4p-boot \
  --images bin/targets/rockchip/cortexa7 \
  --output bin/targets/rockchip/cortexa7/omega4p-package-bundle
```

Keep a known-working loader and backup before testing. For an existing
Omega4+ OpenWrt installation, only idblock and U-Boot need writing to test this
package: byte offsets `0x40000` and `0x140000`, capacities 1 MiB and 2 MiB.
Use the new `download.bin` for the USB runtime. Preserve env, meta, kernel,
rootfs and Ethernet settings. Detailed hardware results belong in the workspace
`hardware-validation/omega4p-boot/` directory.

## Hardware result — 19 November 2026

Tested on A29C using the existing screen console. New USB downloader worked;
idblock/U-Boot flash readbacks matched. Normal boot, cold boot, persistent
overlay and the Ctrl+C U-Boot console path passed. U-Boot reports OpenWrt GCC
13.3.0. Ethernet negotiated 100 Mb/s and gateway ping passed; Wi-Fi scan worked.
Environment and network configuration hashes were preserved. A pre-flash UBI
read timeout was recorded on the old installation and did not recur in these
tests. Camera/audio remain outside this package's validation. Full evidence:
`hardware-validation/omega4p-boot/REPORT.md` in the workspace.

## Boot environment (package release 2)

The package also builds `env.img` from `files/uboot.env` with the compiled
U-Boot `mkenvimage`: 256 KiB, CRC32 header, zero padding. Verification checks
size, CRC and exact payload. SHA256SUMS and build-info.json include env.img
and the environment source hash. OpenWrt image generation copies this artifact
rather than generating another environment from a separate template.

Include env.img at byte offset zero for initial USB provisioning: SPL needs its
partition table, including on Omega4+. A blank environment fails before loading
Linux. Full web flashing selects this image by default. Deselect it to preserve
a provisioned MAC and boot settings; normal sysupgrade continues to preserve it.

Both packages were rebuilt and host-verified on 20 November 2026. Environment
images match the previous v0.0.15 release bytes exactly. The Omega4+ environment
was written, read back in full and used to boot the previously blank board to
OpenWrt 0.0.15 with a writable UBIFS overlay. The newly repacked boot binaries
were host-verified; original Omega4 hardware validation remains deferred.

## SDK metadata isolation (package release 3)

OpenWrt does not generate meta.img. Older package revisions nevertheless enabled
CONFIG_ROCKCHIP_META in SPL. A retained valid SDK meta partition caused the
bootloader to load camera metadata and notify its vendor RT-Thread HPMCU, before
the Linux HPMCU loader could assert reset or establish its reserved DDR mapping.
This path assumes the SDK memory layout, not the OpenWrt memory layout.

On 26 November, adding the SDK meta image to an otherwise working A29C with the
exact v0.0.18 IDBlock/kernel/rootfs reproduced the factory cgroup_attach_task
panic at +0x8a, reading 0x111. In failsafe, physical 0x64440..0x6445f contained
32 ASCII '#' bytes over the kernel function entry. The vendor RT-Thread source
uses '#' to fill thread stacks; the specific writing instruction was not traced.
The pre-Linux HPMCU mapping was zero, boot address 0x40000 and core reset
deasserted. Linux's later MCU reservation at 0x01000000 cannot protect against
this earlier SDK path.

Release 3 disables CONFIG_ROCKCHIP_META and rejects builds that enable it.
Metadata remains on flash but is not loaded, appended to boot arguments or used
to notify the vendor MCU. The board's normal Linux camera and MCU firmware remain
available. The original Omega4 boot configuration does not enable this option.

**Existing boards need the corrected idblock.img written to the IDBlock
partition. Sysupgrade preserves IDBlock and therefore cannot install this fix.**
Preserve env, meta and user data. A full reflash or factory reset is unnecessary.
Use the matching Omega4+ USB helper and verify IDBlock readback before reboot.
Evidence and validation: ../hardware-validation/2026-11-26-meta-panic in the
parent development workspace (outside this repository).
