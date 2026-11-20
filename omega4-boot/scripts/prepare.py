#!/usr/bin/env python3
"""Export pinned SDK sources into OpenWrt's private package build directory."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess

REVISION = "e89cd37b1e87acb3f76c793c41a14e2efb2e77d6"
BLOBS = {
    "ddr.bin": "9907da36f1da5cffa594918aa9f1ed9210e5e4d3eea719f49fe4cb02a68973a8",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    files = Path(__file__).resolve().parent.parent / "files"
    for name, digest in BLOBS.items():
        if hashlib.sha256((files / name).read_bytes()).hexdigest() != digest:
            parser.error(f"Pinned blob hash mismatch: {name}")
    subprocess.run(["git", "-C", str(args.sdk), "cat-file", "-e", REVISION], check=True)
    args.output.mkdir(parents=True, exist_ok=True)
    # git archive exports committed files, excluding stale SDK objects/configs.
    archive = args.output / "sdk-source.tar"
    with archive.open("wb") as out:
        subprocess.run(["git", "-C", str(args.sdk), "archive",
                        f"{REVISION}:sysdrv/source/uboot", "u-boot", "rkbin"],
                       stdout=out, check=True)
    subprocess.run(["tar", "-xf", str(archive), "-C", str(args.output)], check=True)
    archive.unlink()
    for src, dest in {
        "ddr.bin": "rkbin/bin/rv11/omega4-ddr.bin",
        "omega4.ini": "rkbin/RKBOOT/omega4.ini",
        "omega4_defconfig": "u-boot/configs/omega4_defconfig",
        "omega4-compress.py": "u-boot/scripts/omega4-compress.py",
    }.items():
        shutil.copyfile(files / src, args.output / dest)


if __name__ == "__main__":
    main()
