#!/usr/bin/env python3
"""Verify packed components and emit boot images with build provenance."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from prepare import BLOBS, REVISION


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def same_payload(actual, expected):
    data, payload = actual.read_bytes(), expected.read_bytes()
    if not data.startswith(payload) or any(data[len(payload):]):
        raise ValueError(f"Packed payload mismatch: {actual.name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    uboot = build / "u-boot"
    merger = build / "rkbin/tools/boot_merger"
    output = build / "images"
    verify = build / "verify"
    shutil.rmtree(verify, ignore_errors=True)
    verify.mkdir()
    source = {
        "download.bin": uboot / "rv1103b_download_v1.07.000.bin",
        "idblock.img": uboot / "rv1103b_idblock_v1.07.000.img",
        "uboot.img": uboot / "uboot.img",
    }
    limits = {"download.bin": 1024 * 1024, "idblock.img": 1024 * 1024,
              "uboot.img": 2 * 1024 * 1024}
    for name, path in source.items():
        if not 0 < path.stat().st_size <= limits[name]:
            raise ValueError(f"Invalid image size: {name}")
    if source["uboot.img"].read_bytes()[:4] != bytes.fromhex("d00dfeed"):
        raise ValueError("uboot.img is not a FIT")
    subprocess.run([str(merger), "unpack", "-i", str(source["download.bin"]),
                    "-o", str(verify)], check=True)
    same_payload(verify / "FlashData.bin", build / "rkbin/bin/rv11/omega4p-ddr.bin")
    same_payload(verify / "omega4p-ddr.bin", build / "rkbin/bin/rv11/omega4p-ddr.bin")
    same_payload(verify / "Hpmcu.bin", build / "rkbin/bin/rv11/omega4p-hpmcu.bin")
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
        "package": "omega4p-boot",
        "board": "onion,rv1106b-omega4p-evb",
        "sdk_commit": REVISION,
        "vendor_inputs": BLOBS,
        "uboot_config_sha256": sha256(uboot / ".config"),
        "spl_sha256": sha256(uboot / "spl/u-boot-spl.bin"),
        "ddr": {"version": "1.07", "configured_mhz": 667, "uart_baud": 115200},
        "hardware_validated": False,
        "images": {name: {"sha256": sha256(path), "bytes": path.stat().st_size}
                   for name, path in source.items()},
    }, indent=2) + "\n")
    print("PASS: USB/NAND DDR, MCU, compiled SPL, USB plug and regenerated idblock")


if __name__ == "__main__":
    main()
