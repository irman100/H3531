#!/usr/bin/env python3
"""Create a legacy U-Boot script image without requiring mkimage.

Equivalent intent to:
  mkimage -A arm -O linux -T script -C none -n "H3531 RAM BOOT" \
          -d H3531-BOOT.cmd H3531.SCR

Legacy script payloads use the same component table layout as multi-images:
[first-script-size BE32][zero BE32][script bytes].
"""
from __future__ import annotations

import argparse
import struct
import time
import zlib
from pathlib import Path

IH_MAGIC = 0x27051956
IH_OS_LINUX = 5
IH_ARCH_ARM = 2
IH_TYPE_SCRIPT = 6
IH_COMP_NONE = 0
HEADER_FMT = ">7I4B32s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)


def build_script_image(script: bytes, name: str, timestamp: int) -> bytes:
    if not script.endswith(b"\n"):
        script += b"\n"

    # U-Boot treats legacy script images as a one-component multi-image.
    payload = struct.pack(">II", len(script), 0) + script
    data_crc = zlib.crc32(payload) & 0xFFFFFFFF
    name_bytes = name.encode("ascii", "replace")[:32].ljust(32, b"\0")

    header_zero_crc = struct.pack(
        HEADER_FMT,
        IH_MAGIC,
        0,
        timestamp & 0xFFFFFFFF,
        len(payload),
        0,
        0,
        data_crc,
        IH_OS_LINUX,
        IH_ARCH_ARM,
        IH_TYPE_SCRIPT,
        IH_COMP_NONE,
        name_bytes,
    )
    header_crc = zlib.crc32(header_zero_crc) & 0xFFFFFFFF
    header = struct.pack(
        HEADER_FMT,
        IH_MAGIC,
        header_crc,
        timestamp & 0xFFFFFFFF,
        len(payload),
        0,
        0,
        data_crc,
        IH_OS_LINUX,
        IH_ARCH_ARM,
        IH_TYPE_SCRIPT,
        IH_COMP_NONE,
        name_bytes,
    )
    return header + payload


def verify(image: bytes) -> None:
    if len(image) < HEADER_SIZE + 8:
        raise ValueError("image too short")
    vals = struct.unpack(HEADER_FMT, image[:HEADER_SIZE])
    magic, hcrc, _ts, size, _load, _ep, dcrc, os_id, arch, typ, comp, _name = vals
    if magic != IH_MAGIC:
        raise ValueError("bad magic")
    h = bytearray(image[:HEADER_SIZE])
    h[4:8] = b"\0\0\0\0"
    if zlib.crc32(h) & 0xFFFFFFFF != hcrc:
        raise ValueError("bad header CRC")
    payload = image[HEADER_SIZE:HEADER_SIZE + size]
    if len(payload) != size or zlib.crc32(payload) & 0xFFFFFFFF != dcrc:
        raise ValueError("bad data CRC")
    if (os_id, arch, typ, comp) != (IH_OS_LINUX, IH_ARCH_ARM, IH_TYPE_SCRIPT, IH_COMP_NONE):
        raise ValueError("unexpected legacy image metadata")
    script_len, terminator = struct.unpack(">II", payload[:8])
    if terminator != 0 or script_len > len(payload) - 8:
        raise ValueError("bad script component table")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path, nargs="?", default=Path("H3531-BOOT.cmd"))
    ap.add_argument("output", type=Path, nargs="?", default=Path("H3531.SCR"))
    ap.add_argument("--name", default="H3531 RAM BOOT")
    ap.add_argument("--timestamp", type=int, default=None)
    ns = ap.parse_args()

    ts = int(time.time()) if ns.timestamp is None else ns.timestamp
    image = build_script_image(ns.input.read_bytes(), ns.name, ts)
    verify(image)
    ns.output.write_bytes(image)
    print(f"wrote {ns.output} ({len(image)} bytes), CRC verification OK")


if __name__ == "__main__":
    main()
