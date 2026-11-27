#!/usr/bin/env python3
"""Patch a backed-up original Omega4 environment without changing other variables."""
import argparse
from pathlib import Path
import struct
import zlib

OLD = b'mtdparts=spi1.0:256K(env),1M@256K(idblock),1M(uboot),8M(boot),-(ubi)'
NEW = OLD.replace(b'-(ubi)', b'251136K(ubi)')


def fix(data):
    if len(data) != 0x40000 or struct.unpack('<I', data[:4])[0] != zlib.crc32(data[4:]):
        raise ValueError('Expected a 256 KiB environment with a valid CRC32')
    end = data.find(b'\0\0', 4)
    if end < 0:
        raise ValueError('Missing environment terminator')
    entries = data[4:end].split(b'\0')
    layouts = [e for e in entries if e.startswith(b'mtdparts=')]
    if len(layouts) != 1 or layouts[0] not in (OLD, NEW):
        raise ValueError('Unexpected partition map; only original Omega4 is supported')
    entries = [NEW if e == OLD else e for e in entries]
    payload = b'\0'.join(entries) + b'\0\0'
    if len(payload) > len(data) - 4:
        raise ValueError('Environment has no space for corrected partition map')
    payload = payload.ljust(len(data) - 4, b'\0')
    return struct.pack('<I', zlib.crc32(payload)) + payload


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('backup', type=Path)
    p.add_argument('output', type=Path)
    args = p.parse_args()
    result = fix(args.backup.read_bytes())
    with args.output.open('xb') as out:
        out.write(result)
    print('Updated UBI boundary; all other environment variables preserved.')
