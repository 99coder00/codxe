"""Fastfile container handling (header + zlib compressed zone)."""

from __future__ import annotations

import struct
import zlib

MAGIC_UNSIGNED = b"IWffu100"
VERSION_T4 = 0x183


class FastFileError(Exception):
    pass


def read_fastfile(path: str):
    """Return (endian, version, zone bytes) of a T4 fastfile (PC or Xbox 360)."""

    with open(path, "rb") as f:
        data = f.read()

    if data[:8] != MAGIC_UNSIGNED:
        raise FastFileError(f"{path}: unsupported fastfile magic {data[:8]!r} (only unsigned IWffu100 fastfiles are supported)")

    le = struct.unpack_from("<I", data, 8)[0]
    be = struct.unpack_from(">I", data, 8)[0]
    if le == VERSION_T4:
        endian = "<"
    elif be == VERSION_T4:
        endian = ">"
    else:
        raise FastFileError(f"{path}: not a World at War fastfile (version {le:#x})")

    zone = zlib.decompress(data[12:])
    return endian, VERSION_T4, zone


def write_fastfile(path: str, endian: str, zone: bytes, level: int = 9):
    header = MAGIC_UNSIGNED + struct.pack(endian + "I", VERSION_T4)
    with open(path, "wb") as f:
        f.write(header)
        f.write(zlib.compress(zone, level))
