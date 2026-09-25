"""Fastfile container handling (header + zlib compressed zone)."""

from __future__ import annotations

import os
import struct
import zlib
from concurrent.futures import ThreadPoolExecutor

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


def compress(data: bytes, level: int = 9, jobs: int = 0) -> bytes:
    """One zlib stream of ``data``, compressed on ``jobs`` threads (0: one per processor).

    As pigz does: 1 MiB chunks are deflated separately, each one primed with the 32 KiB before it
    (so it can refer to them as a single stream would) and ended on a byte boundary (a sync flush),
    which makes their concatenation one deflate stream. zlib releases the interpreter lock while it
    compresses, so the threads run in parallel.
    """
    jobs = jobs or os.cpu_count() or 1
    size = len(data)
    if jobs <= 1 or size <= 2 * COMPRESS_CHUNK:
        return zlib.compress(data, level)
    view = memoryview(data)

    def part(start: int) -> bytes:
        end = min(start + COMPRESS_CHUNK, size)
        if start:
            c = zlib.compressobj(level, zlib.DEFLATED, -15, 8, zlib.Z_DEFAULT_STRATEGY, bytes(view[max(0, start - 32768) : start]))
        else:
            c = zlib.compressobj(level, zlib.DEFLATED, -15)
        return c.compress(view[start:end]) + c.flush(zlib.Z_FINISH if end == size else zlib.Z_SYNC_FLUSH)

    with ThreadPoolExecutor(jobs) as pool:
        parts = list(pool.map(part, range(0, size, COMPRESS_CHUNK)))
    header = zlib.compress(b"", level)[:2]
    return header + b"".join(parts) + struct.pack(">I", zlib.adler32(data))


COMPRESS_CHUNK = 1 << 20


def write_fastfile(path: str, endian: str, zone: bytes, level: int = 9, jobs: int = 0):
    header = MAGIC_UNSIGNED + struct.pack(endian + "I", VERSION_T4)
    with open(path, "wb") as f:
        f.write(header)
        f.write(compress(zone, level, jobs))
