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
D3DFMT_A8L8 = 0x0800014A
# The swizzle of a D3DFORMAT sits in bits 18 to 29 (the texture header holds it too); older t4ff
# versions wrote A8L8 textures with another swizzle there.
D3DFMT_SWIZZLE_SHIFT = 18

# GPUDIMENSION
GPUDIMENSION_2D = 1
GPUDIMENSION_CUBEMAP = 3


@dataclass(frozen=True)
class Format:
    name: str
    gpu: int
    d3d: int
    endian: int
    block: int  # block width/height in texels
    bytes_per_block: int
    swizzle: int  # dword 3 swizzle bits (XYZW)


# Swizzles as Treyarch's textures have them (3 bits a channel, 4: 0, 5: 1): 0x688 = X:0 Y:1 Z:2 W:3;
# 8_8_8_8 (PC BGRA bytes) 0x60A = X:2 Y:1 Z:0 W:3; L8 0xA00 = LLL1; A8L8 0x200 = LLLA.
FORMATS = {
    "DXT1": Format("DXT1", GPUTEXTUREFORMAT_DXT1, D3DFMT_DXT1, GPUENDIAN_8IN16, 4, 8, 0x688),
    "DXT3": Format("DXT3", GPUTEXTUREFORMAT_DXT2_3, D3DFMT_DXT3, GPUENDIAN_8IN16, 4, 16, 0x688),
    "DXT5": Format("DXT5", GPUTEXTUREFORMAT_DXT4_5, D3DFMT_DXT5, GPUENDIAN_8IN16, 4, 16, 0x688),
    "DXN": Format("DXN", GPUTEXTUREFORMAT_DXN, D3DFMT_DXN, GPUENDIAN_8IN16, 4, 16, 0x688),
    "A8R8G8B8": Format("A8R8G8B8", GPUTEXTUREFORMAT_8_8_8_8, D3DFMT_A8R8G8B8, GPUENDIAN_8IN32, 1, 4, 0x60A),
    "L8": Format("L8", GPUTEXTUREFORMAT_8, D3DFMT_L8, GPUENDIAN_NONE, 1, 1, 0xA00),
    "A8L8": Format("A8L8", GPUTEXTUREFORMAT_8_8, D3DFMT_A8L8, GPUENDIAN_8IN16, 1, 2, 0x200),
}


def format_of_d3d(d3d: int) -> Optional[Format]:
    """The format of a D3DFORMAT value, whatever swizzle it names."""
    low = d3d & ((1 << D3DFMT_SWIZZLE_SHIFT) - 1)
    return next((f for f in FORMATS.values() if f.d3d & ((1 << D3DFMT_SWIZZLE_SHIFT) - 1) == low), None)


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


def fetch_constant(width: int, height: int, fmt: Format, levels: int, tiled: bool = True, mip_address: int = 0, dimension: int = GPUDIMENSION_2D) -> bytes:
    """GPU texture fetch constant (6 dwords) for a 2D or cube texture, little endian as stored in T4 zones."""
    pitch = pitch_units(width, fmt)
    dword0 = 2 | (pitch << 22) | (int(tiled) << 31)
    dword1 = fmt.gpu | (fmt.endian << 6)
    dword2 = (width - 1) | ((height - 1) << 13)
    dword3 = fmt.swizzle << 1
    dword4 = ((levels - 1) & 0xF) << 6
    dword5 = (dimension << 9) | ((mip_address & 0xFFFFF) << 12)
    return struct.pack("<6I", dword0, dword1, dword2, dword3, dword4, dword5)


def texture_header(width: int, height: int, fmt: Format, levels: int, mip_address: int = 0, dimension: int = GPUDIMENSION_2D) -> bytes:
    """D3DBaseTexture (52 bytes, little endian) as stored in T4 360 zones."""
    return struct.pack("<7I", 3, 1, 0, 0, 0, 0xFFFF0000, 0xFFFF0000) + fetch_constant(width, height, fmt, levels, True, mip_address, dimension)


# ---------------------------------------------------------------------------
# Mip chains


def _log2_ceil(value: int) -> int:
    return max(0, (value - 1).bit_length())


def packed_mip_level(width: int, height: int) -> int:
    """First mip level stored in the packed mip tail (its smaller side is 16 texels or less)."""
    log2_size = _log2_ceil(min(width, height))
    return log2_size - 4 if log2_size > 4 else 0


def packed_mip_offset(width: int, height: int, level: int, fmt: Format):
    """Block offset (x, y) of ``level`` inside the packed mip tail (Xenia's GetPackedMipOffset)."""
    log2_w = _log2_ceil(width)
    log2_h = _log2_ceil(height)
    log2_size = min(log2_w, log2_h)
    if log2_size > 4 + level:
        return 0, 0
    base = log2_size - 4 if log2_size > 4 else 0
    packed = level - base
    if packed < 3:
        if log2_w > log2_h:
            x, y = 0, 16 >> packed
        else:
            x, y = 16 >> packed, 0
    else:
        if log2_w > log2_h:
            x, y = (1 << (log2_w - base)) >> (packed - 2), 0
        else:
            x, y = 0, (1 << (log2_h - base)) >> (packed - 2)
    return x // fmt.block, y // fmt.block


def _tiled_index(xs: np.ndarray, ys: np.ndarray, stored_w: int, bpb: int) -> np.ndarray:
    log2 = _log2_bpb(bpb)
    y = ys.astype(np.int64)
    x = xs.astype(np.int64)
    macro = ((y // 32) * (stored_w // 32)) << (log2 + 7)
    micro = ((y & 6) << 2) << log2
    row = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2)) + ((y & 1) << 4)
    macro = (x // 32) << (log2 + 7)
    micro = (x & 7) << log2
    offset = row + macro + ((micro & ~0xF) << 1) + (micro & 0xF)
    tiled = ((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)
    return tiled >> log2


def mip_chain_layout(width: int, height: int, fmt: Format, levels: int, faces: int = 1):
    """Byte offset of every level region and the total size.

    Returns (base size, [(level, region offset, region stored width blocks, x blocks, y blocks,
    face stride)], total size). Levels from the packed mip level on share the region of the packed
    level. A cube map (``faces`` 6) holds every level for its faces one after the other, each face
    4 KiB aligned: the base level of the faces, then each mip level of the faces, then the packed
    mip tail of the faces (as retail cube maps, e.g. the reflection probes of the maps).
    """
    stride = level_layout(width, height, 0, fmt)[4]
    base_size = stride * faces
    packed = packed_mip_level(width, height)
    placements = []
    offset = base_size
    region = None
    for level in range(1, levels):
        if packed and level >= packed:
            if region is None:
                _, _, stored_w, _, size = level_layout(width, height, packed, fmt)
                region = (offset, stored_w, size)
                offset += size * faces
            x, y = packed_mip_offset(width, height, level, fmt)
            placements.append((level, region[0], region[1], x, y, region[2]))
        else:
            _, _, stored_w, _, size = level_layout(width, height, level, fmt)
            placements.append((level, offset, stored_w, 0, 0, size))
            offset += size * faces
    return base_size, placements, offset


def _level_blocks(width: int, height: int, level: int, fmt: Format):
    mw = max(width >> level, 1)
    mh = max(height >> level, 1)
    return max(1, (mw + fmt.block - 1) // fmt.block), max(1, (mh + fmt.block - 1) // fmt.block)


def tile_mip_chain(levels: list, width: int, height: int, fmt: Format, faces: int = 1) -> bytes:
    """Tile a mip chain (linear PC data, largest level first; each level holds its ``faces`` one
    after the other). The base level must be larger than 16 texels in both dimensions when more
    than one level is given."""
    base_size, placements, total = mip_chain_layout(width, height, fmt, len(levels), faces)
    stride = base_size // faces
    out = np.zeros(total, dtype=np.uint8)
    bpb = fmt.bytes_per_block
    wb, hb = _level_blocks(width, height, 0, fmt)
    for face in range(faces):
        face_data = levels[0][face * wb * hb * bpb : (face + 1) * wb * hb * bpb]
        # tile_level applies the endian swap, undo it: the whole allocation is swapped at the end
        out[face * stride : (face + 1) * stride] = np.frombuffer(endian_swap(tile_level(face_data, width, height, 0, fmt), fmt.endian), dtype=np.uint8)
    for level, region, stored_w, bx, by, face_stride in placements:
        wb, hb = _level_blocks(width, height, level, fmt)
        ys, xs = np.meshgrid(np.arange(hb) + by, np.arange(wb) + bx, indexing="ij")
        idx = _tiled_index(xs.reshape(-1), ys.reshape(-1), stored_w, bpb)
        src = np.frombuffer(levels[level], dtype=np.uint8)
        for face in range(faces):
            start = region + face * face_stride
            view = out[start : start + face_stride].reshape(-1, bpb)
            view[idx] = src[face * wb * hb * bpb : (face + 1) * wb * hb * bpb].reshape(wb * hb, bpb)
    # the GPU endian swap applies to the whole allocation
    return endian_swap(out.tobytes(), fmt.endian)


def untile_mip_chain(data: bytes, width: int, height: int, fmt: Format, levels: int, faces: int = 1) -> list:
    base_size, placements, total = mip_chain_layout(width, height, fmt, levels, faces)
    stride = base_size // faces
    raw = np.frombuffer(endian_swap(data[:total], fmt.endian), dtype=np.uint8)
    result = [b"".join(untile_level(endian_swap(raw[f * stride : (f + 1) * stride].tobytes(), fmt.endian), width, height, 0, fmt) for f in range(faces))]
    bpb = fmt.bytes_per_block
    for level, region, stored_w, bx, by, face_stride in placements:
        wb, hb = _level_blocks(width, height, level, fmt)
        ys, xs = np.meshgrid(np.arange(hb) + by, np.arange(wb) + bx, indexing="ij")
        idx = _tiled_index(xs.reshape(-1), ys.reshape(-1), stored_w, bpb)
        result.append(b"".join(raw[region + f * face_stride : region + (f + 1) * face_stride].reshape(-1, bpb)[idx].tobytes() for f in range(faces)))
    return result


def texture_header_mips(width: int, height: int, fmt: Format, levels: int, faces: int = 1) -> bytes:
    """D3DBaseTexture360 for a texture with a mip chain (mip address relative to the base), or a
    cube map (``faces`` 6)."""
    base_size, placements, _ = mip_chain_layout(width, height, fmt, levels, faces)
    packed = any(bx or by for _, _, _, bx, by, _ in placements) or (packed_mip_level(width, height) and levels > packed_mip_level(width, height))
    dimension = GPUDIMENSION_CUBEMAP if faces == 6 else GPUDIMENSION_2D
    header = bytearray(texture_header(width, height, fmt, levels, (base_size >> 12) if levels > 1 else 0, dimension))
    if levels > 1 and packed:
        dword5 = struct.unpack_from("<I", header, 28 + 20)[0] | (1 << 11)
        struct.pack_into("<I", header, 28 + 20, dword5)
    return bytes(header)
