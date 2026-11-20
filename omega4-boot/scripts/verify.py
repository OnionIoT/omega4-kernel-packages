#!/usr/bin/env python3
"""Verify packed components and emit boot images with build provenance."""
import argparse
import hashlib
import json
import lzma
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

from prepare import BLOBS, REVISION


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def same_payload(actual, expected):
    data, payload = actual.read_bytes(), expected.read_bytes()
    if not data.startswith(payload) or any(data[len(payload):]):
        raise ValueError(f"Packed payload mismatch: {actual.name}")


def verify_fit_payload(image, node, expected):
    path = "/images/" + node
    def prop(name, kind=None):
        cmd = ["fdtget"] + (["-t", kind] if kind else [])
        return subprocess.check_output(cmd + [str(image), path, name], text=True).strip()
    offset, size = int(prop("data-position", "u")), int(prop("data-size", "u"))
    data = image.read_bytes()[offset:offset + size]
    if not size or len(data) != size:
        raise ValueError(f"Empty or truncated U-Boot FIT payload: {node}")
    compression = prop("compression")
    if compression == "lzma":
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
        data = decoder.decompress(data)
        if not decoder.eof or decoder.unused_data != struct.pack("<I", len(data)):
            raise ValueError("Invalid SDK LZMA stream or size trailer")
    elif compression != "none":
        raise ValueError(f"Unexpected FIT compression: {compression}")
    if data != expected.read_bytes():
        raise ValueError(f"U-Boot FIT payload differs from compiled {expected.name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    uboot = build / "u-boot"
    config = set((uboot / ".config").read_text().splitlines())
    required = {"CONFIG_MTD_BLK_U_BOOT_OFFS=0xA00", "CONFIG_BAUDRATE=115200",
                "CONFIG_ENVF=y", "CONFIG_ROCKCHIP_FIT_IMAGE=y"}
    if not required <= config or "CONFIG_SPL_KERNEL_BOOT=y" in config:
        raise ValueError("Configuration does not match the Omega4 U-Boot boot path")
    merger = build / "rkbin/tools/boot_merger"
    output = build / "images"
    verify = build / "verify"
    shutil.rmtree(verify, ignore_errors=True)
    verify.mkdir()
    env_source = Path(__file__).resolve().parent.parent / "files/uboot.env"
    env_image = build / "env.img"
    subprocess.run([str(uboot / "tools/mkenvimage"), "-s", "0x40000", "-p", "0x0",
                    "-o", str(env_image), str(env_source)], check=True)
    env = env_image.read_bytes()
    if len(env) != 0x40000 or struct.unpack("<I", env[:4])[0] != zlib.crc32(env[4:]):
        raise ValueError("Invalid environment size or CRC")
    expected = b"\0".join(env_source.read_bytes().splitlines()) + b"\0\0"
    if env[4:] != expected.ljust(0x40000 - 4, b"\0"):
        raise ValueError("Environment payload differs from board configuration")
    source = {
        "env.img": env_image,
        "download.bin": uboot / "rv1103b_download_v1.05.100.bin",
        "idblock.img": uboot / "rv1103b_idblock_v1.05.100.img",
        "uboot.img": uboot / "uboot.img",
    }
    limits = {"env.img": 0x40000, "download.bin": 1024 * 1024, "idblock.img": 1024 * 1024,
              "uboot.img": 1024 * 1024}
    for name, path in source.items():
        if not 0 < path.stat().st_size <= limits[name]:
            raise ValueError(f"Invalid image size: {name}")
    if source["uboot.img"].read_bytes()[:4] != bytes.fromhex("d00dfeed"):
        raise ValueError("uboot.img is not a FIT")
    verify_fit_payload(source["uboot.img"], "uboot", uboot / "u-boot-nodtb.bin")
    verify_fit_payload(source["uboot.img"], "fdt", uboot / "u-boot.dtb")
    subprocess.run([str(merger), "unpack", "-i", str(source["download.bin"]),
                    "-o", str(verify)], check=True)
    if (verify / "Hpmcu.bin").exists():
        raise ValueError("Original Omega4 loader must not include the Omega4+ MCU")
    same_payload(verify / "FlashData.bin", build / "rkbin/bin/rv11/omega4-ddr.bin")
    same_payload(verify / "omega4-ddr.bin", build / "rkbin/bin/rv11/omega4-ddr.bin")
    same_payload(verify / "FlashBoot.bin", uboot / "spl/u-boot-spl.bin")
    same_payload(verify / "rv1103b_usbplug_v1.bin", build / "rkbin/bin/rv11/rv1103b_usbplug_v1.00.bin")
    subprocess.run([str(merger), "idb", "-n", "-l", "-o", str(verify / "idblock.img"),
                    str(source["download.bin"])], check=True)
    if sha256(verify / "idblock.img") != sha256(source["idblock.img"]):
        raise ValueError("idblock does not match download.bin")
    output.mkdir(exist_ok=True)
    for name, path in source.items():
        shutil.copyfile(path, output / name)
    (output / "SHA256SUMS").write_text("".join(
        f"{sha256(path)}  {name}\n" for name, path in sorted(source.items())))
    (output / "build-info.json").write_text(json.dumps({
        "package": "omega4-boot",
        "board": "onion,rv1103b-omega4-evb",
        "sdk_commit": REVISION,
        "vendor_inputs": BLOBS,
        "environment_source_sha256": sha256(env_source),
        "uboot_config_sha256": sha256(uboot / ".config"),
        "spl_sha256": sha256(uboot / "spl/u-boot-spl.bin"),
        "ddr": {"version": "1.05", "configured_mhz": 924, "uart_baud": 115200},
        "hardware_validated": False,
        "images": {name: {"sha256": sha256(path), "bytes": path.stat().st_size}
                   for name, path in source.items()},
    }, indent=2) + "\n")
    print("PASS: USB/NAND DDR, compiled SPL, USB plug and regenerated idblock")


if __name__ == "__main__":
    main()
