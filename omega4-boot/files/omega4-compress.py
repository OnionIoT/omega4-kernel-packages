#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Emit SDK LZMA-alone data followed by its little-endian size trailer."""
import lzma
from pathlib import Path
import struct
import sys

source = Path(sys.argv[1])
data = source.read_bytes()
if not data:
    raise ValueError("Refusing to compress an empty boot payload")
compressed = lzma.compress(data, format=lzma.FORMAT_ALONE, preset=9)
Path(str(source) + ".lzma").write_bytes(compressed + struct.pack("<I", len(data)))
