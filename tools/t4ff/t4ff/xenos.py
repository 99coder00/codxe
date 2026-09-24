"""Xbox 360 (Xenos) texture helpers: formats, tiling, endian swapping and texture headers.

The tiling math is a port of ``src/image/xenos_texture.cpp`` (CoD Xe), vectorised with numpy.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Optional

import numpy as np

# GPUTEXTUREFORMAT
GPUTEXTUREFORMAT_8 = 2
GPUTEXTUREFORMAT_8_8_8_8 = 6
GPUTEXTUREFORMAT_8_8 = 10
GPUTEXTUREFORMAT_DXT1 = 18
GPUTEXTUREFORMAT_DXT2_3 = 19
GPUTEXTUREFORMAT_DXT4_5 = 20
GPUTEXTUREFORMAT_DXN = 49
GPUTEXTUREFORMAT_DXT3A = 58
GPUTEXTUREFORMAT_DXT5A = 59

# GPUENDIAN
GPUENDIAN_NONE = 0
GPUENDIAN_8IN16 = 1
GPUENDIAN_8IN32 = 2
GPUENDIAN_16IN32 = 3

# D3DFORMAT values of the 360 SDK
D3DFMT_DXT1 = 0x1A200152
D3DFMT_DXT3 = 0x1A200153
D3DFMT_DXT5 = 0x1A200154
D3DFMT_DXN = 0x1A200171
D3DFMT_A8R8G8B8 = 0x18280186
D3DFMT_L8 = 0x28000102
D3DFMT_A8L8 = 0x2800014A


@dataclass(frozen=True)
class Format:
    name: str
    gpu: int
    d3d: int
    endian: int
    block: int  # block width/height in texels
    bytes_per_block: int
    swizzle: int  # dword 3 swizzle bits (XYZW)


# Swizzle 0x688 = X:0 Y:1 Z:2 W:3, as used by CoD Xenon's converted textures.
FORMATS = {
    "DXT1": Format("DXT1", GPUTEXTUREFORMAT_DXT1, D3DFMT_DXT1, GPUENDIAN_8IN16, 4, 8, 0x688),
    "DXT3": Format("DXT3", GPUTEXTUREFORMAT_DXT2_3, D3DFMT_DXT3, GPUENDIAN_8IN16, 4, 16, 0x688),
    "DXT5": Format("DXT5", GPUTEXTUREFORMAT_DXT4_5, D3DFMT_DXT5, GPUENDIAN_8IN16, 4, 16, 0x688),
    "DXN": Format("DXN", GPUTEXTUREFORMAT_DXN, D3DFMT_DXN, GPUENDIAN_8IN16, 4, 16, 0x688),
    "A8R8G8B8": Format("A8R8G8B8", GPUTEXTUREFORMAT_8_8_8_8, D3DFMT_A8R8G8B8, GPUENDIAN_8IN32, 1, 4, 0x688),
    "L8": Format("L8", GPUTEXTUREFORMAT_8, D3DFMT_L8, GPUENDIAN_NONE, 1, 1, 0x000),
    "A8L8": Format("A8L8", GPUTEXTUREFORMAT_8_8, D3DFMT_A8L8, GPUENDIAN_8IN16, 1, 2, 0x200),
}


def _align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def _next_pow2(value: int) -> int:
    result = 1
    while result < value:
        result <<= 1
    return result


def pitch_units(width: int, fmt: Format) -> int:
    """Pitch as stored in the fetch constant (multiples of 32 texels)."""
    if fmt.block > 1:
        blocks = max(1, (width + fmt.block - 1) // fmt.block)
        return _align(blocks, 32) // 8
    return _align(width, 32) // 32


def level_layout(width: int, height: int, level: int, fmt: Format):
    """Return (width blocks, height blocks, stored width blocks, stored height blocks, level size)."""
    mip_w = max(width >> level, 1)
    mip_h = max(height >> level, 1)
    wb = max(1, (mip_w + fmt.block - 1) // fmt.block)
    hb = max(1, (mip_h + fmt.block - 1) // fmt.block)
    if level == 0:
        row_pitch_texels = pitch_units(width, fmt) << 5
        row_pitch = max(1, (row_pitch_texels + fmt.block - 1) // fmt.block) * fmt.bytes_per_block
        stored_w = row_pitch // fmt.bytes_per_block
        stored_h = _align(hb, 32)
    else:
        mw = max(_next_pow2(width) >> level, 1)
        mh = max(_next_pow2(height) >> level, 1)
        stored_w = _align((mw + fmt.block - 1) // fmt.block, 32)
        stored_h = _align((mh + fmt.block - 1) // fmt.block, 32)
        row_pitch = stored_w * fmt.bytes_per_block
    size = _align(row_pitch * stored_h, 4096)
    return wb, hb, stored_w, stored_h, size


def _log2_bpb(bytes_per_block: int) -> int:
    return (bytes_per_block // 4) + ((bytes_per_block // 2) >> (bytes_per_block // 4))


def tiled_offsets(wb: int, hb: int, stored_w: int, bpb: int) -> np.ndarray:
    """Tiled block index for every linear block (row major, wb x hb)."""
    log2 = _log2_bpb(bpb)
    y = np.arange(hb, dtype=np.int64)[:, None]
    x = np.arange(wb, dtype=np.int64)[None, :]

    macro = ((y // 32) * (stored_w // 32)) << (log2 + 7)
    micro = ((y & 6) << 2) << log2
    row = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2)) + ((y & 1) << 4)

    macro = (x // 32) << (log2 + 7)
    micro = (x & 7) << log2
    offset = row + macro + ((micro & ~0xF) << 1) + (micro & 0xF)
    tiled = ((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)
    return (tiled >> log2).reshape(-1)


def endian_swap(data: bytes, endian: int) -> bytes:
    if endian == GPUENDIAN_NONE:
        return bytes(data)
    arr = np.frombuffer(data, dtype=np.uint8)
    if endian == GPUENDIAN_8IN16:
        return arr.reshape(-1, 2)[:, ::-1].tobytes()
    if endian == GPUENDIAN_8IN32:
        return arr.reshape(-1, 4)[:, ::-1].tobytes()
    if endian == GPUENDIAN_16IN32:
        return arr.reshape(-1, 2, 2)[:, ::-1, :].tobytes()
    raise ValueError(endian)


def tile_level(linear: bytes, width: int, height: int, level: int, fmt: Format) -> bytes:
    """Tile one mip level of linear (PC order) data into Xenos memory order (incl. endian swap)."""
    wb, hb, stored_w, stored_h, size = level_layout(width, height, level, fmt)
    bpb = fmt.bytes_per_block
    src = np.frombuffer(linear, dtype=np.uint8)
    if src.size < wb * hb * bpb:
        raise ValueError("not enough texture data for level")
    blocks = src[: wb * hb * bpb].reshape(wb * hb, bpb)
    out = np.zeros((size // bpb, bpb), dtype=np.uint8)
    out[tiled_offsets(wb, hb, stored_w, bpb)] = blocks
    return endian_swap(out.tobytes(), fmt.endian)


def untile_level(tiled: bytes, width: int, height: int, level: int, fmt: Format) -> bytes:
    """Inverse of :func:`tile_level`: returns linear PC order data."""
    wb, hb, stored_w, stored_h, size = level_layout(width, height, level, fmt)
    bpb = fmt.bytes_per_block
    data = np.frombuffer(endian_swap(tiled[:size], fmt.endian), dtype=np.uint8).reshape(-1, bpb)
    return data[tiled_offsets(wb, hb, stored_w, bpb)].tobytes()


def fetch_constant(width: int, height: int, fmt: Format, levels: int, tiled: bool = True, mip_address: int = 0) -> bytes:
    """GPU texture fetch constant (6 dwords) for a 2D texture, little endian as stored in T4 zones."""
    pitch = pitch_units(width, fmt)
    dword0 = 2 | (pitch << 22) | (int(tiled) << 31)
    dword1 = fmt.gpu | (fmt.endian << 6)
    dword2 = (width - 1) | ((height - 1) << 13)
    dword3 = fmt.swizzle << 1
    dword4 = ((levels - 1) & 0xF) << 6
    dword5 = (1 << 9) | ((mip_address & 0xFFFFF) << 12)
    return struct.pack("<6I", dword0, dword1, dword2, dword3, dword4, dword5)


def texture_header(width: int, height: int, fmt: Format, levels: int, mip_address: int = 0) -> bytes:
    """D3DBaseTexture (52 bytes, little endian) as stored in T4 360 zones."""
    return struct.pack("<7I", 3, 1, 0, 0, 0, 0xFFFF0000, 0xFFFF0000) + fetch_constant(width, height, fmt, levels, True, mip_address)
