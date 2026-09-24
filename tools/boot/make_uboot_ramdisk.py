#!/usr/bin/env python3
"""Build/verify a legacy U-Boot RAMDisk image from a raw filesystem image."""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

IH_MAGIC = 0x27051956
IH_OS_LINUX = 5
IH_ARCH_ARM = 2
IH_TYPE_RAMDISK = 3
IH_COMP_NONE = 0
HEADER_FMT = ">7I4B32s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)


def build(payload: bytes, name: str, timestamp: int) -> bytes:
    dcrc = zlib.crc32(payload) & 0xFFFFFFFF
    nb = name.encode("ascii", "replace")[:32].ljust(32, b"\0")
    h0 = struct.pack(
        HEADER_FMT,
        IH_MAGIC, 0, timestamp & 0xFFFFFFFF, len(payload),
        0, 0, dcrc,
        IH_OS_LINUX, IH_ARCH_ARM, IH_TYPE_RAMDISK, IH_COMP_NONE, nb,
    )
    hcrc = zlib.crc32(h0) & 0xFFFFFFFF
    h = struct.pack(
        HEADER_FMT,
        IH_MAGIC, hcrc, timestamp & 0xFFFFFFFF, len(payload),
        0, 0, dcrc,
        IH_OS_LINUX, IH_ARCH_ARM, IH_TYPE_RAMDISK, IH_COMP_NONE, nb,
    )
    return h + payload


def verify(image: bytes) -> None:
    if len(image) < HEADER_SIZE:
        raise ValueError("image too short")
    vals = struct.unpack(HEADER_FMT, image[:HEADER_SIZE])
    magic, hcrc, _ts, size, _load, _ep, dcrc, os_id, arch, typ, comp, _name = vals
    if magic != IH_MAGIC:
        raise ValueError("bad magic")
    hz = bytearray(image[:HEADER_SIZE])
    hz[4:8] = b"\0\0\0\0"
    if (zlib.crc32(hz) & 0xFFFFFFFF) != hcrc:
        raise ValueError("bad header CRC")
    payload = image[HEADER_SIZE:HEADER_SIZE + size]
    if len(payload) != size:
        raise ValueError("bad payload size")
    if (zlib.crc32(payload) & 0xFFFFFFFF) != dcrc:
        raise ValueError("bad data CRC")
    if (os_id, arch, typ, comp) != (IH_OS_LINUX, IH_ARCH_ARM, IH_TYPE_RAMDISK, IH_COMP_NONE):
        raise ValueError("unexpected U-Boot metadata")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--name", default="Stayplaytion Firmware")
    ap.add_argument("--timestamp", type=int, default=0)
    ns = ap.parse_args()

    image = build(ns.input.read_bytes(), ns.name, ns.timestamp)
    verify(image)
    ns.output.write_bytes(image)
    print(f"wrote {ns.output} ({len(image)} bytes), RAMDisk CRC verification OK")


if __name__ == "__main__":
    main()
