"""Wavelet compressed IWI images (formats 6 to 10), decoded as the PC game does.

World at War still loads the wavelet IWIs of Call of Duty 2's era. Maps with weapons ported from
Black Ops carry some (Kino Rezurrection: 26, mostly the weapons' normal maps). Each level is made from
the next smaller one: per 2x2 block and channel, a parity bit and three Huffman coded differences
(horizontal, vertical, diagonal), optionally after per texel corrections of the smaller level; the
smallest levels (one texel wide or high) are raw bytes. Ported from OpenAssetTools'
``IwiWaveletDecoder`` (GPL-3.0): its codeword tables and its reconstruction.
"""

from __future__ import annotations

import struct
from typing import List, Sequence, Tuple

import numpy as np

ESCAPE = -32768

# (code, bit count, value), codes read least significant bit first
_BLUE = (
    (0x001, 3, 0), (0x004, 5, 4), (0x005, 5, 2), (0x007, 5, 1), (0x00A, 5, 3), (0x014, 5, -4),
    (0x015, 5, -2), (0x017, 5, -1), (0x01A, 5, -3), (0x000, 6, 12), (0x002, 6, 10), (0x003, 6, 7),
    (0x006, 6, 9), (0x00B, 6, 6), (0x018, 6, 11), (0x01E, 6, 8), (0x01F, 6, 5), (0x020, 6, -12),
    (0x022, 6, -10), (0x023, 6, -7), (0x026, 6, -9), (0x02B, 6, -6), (0x038, 6, -11), (0x03C, 6, ESCAPE),
    (0x03E, 6, -8), (0x03F, 6, -5), (0x00F, 7, 13), (0x012, 7, 19), (0x016, 7, 18), (0x01B, 7, 14),
    (0x028, 7, 21), (0x02C, 7, 20), (0x02D, 7, 16), (0x02E, 7, 17), (0x030, 7, 22), (0x03D, 7, 15),
    (0x04F, 7, -13), (0x052, 7, -19), (0x056, 7, -18), (0x05B, 7, -14), (0x068, 7, -21), (0x06C, 7, -20),
    (0x06D, 7, -16), (0x06E, 7, -17), (0x070, 7, -22), (0x07D, 7, -15), (0x008, 8, 34), (0x00D, 8, 28),
    (0x00E, 8, 29), (0x013, 8, 26), (0x01D, 8, 27), (0x02F, 8, 23), (0x033, 8, 25), (0x03B, 8, 24),
    (0x048, 8, 33), (0x04C, 8, 32), (0x05C, 8, 31), (0x072, 8, 30), (0x088, 8, -34), (0x08D, 8, -28),
    (0x08E, 8, -29), (0x093, 8, -26), (0x09D, 8, -27), (0x0AF, 8, -23), (0x0B3, 8, -25), (0x0BB, 8, -24),
    (0x0C8, 8, -33), (0x0CC, 8, -32), (0x0DC, 8, -31), (0x0F2, 8, -30), (0x00C, 9, 47), (0x01C, 9, 46),
    (0x032, 9, 45), (0x036, 9, 44), (0x050, 9, 48), (0x076, 9, 43), (0x07B, 9, 37), (0x090, 9, 49),
    (0x0CD, 9, 40), (0x0CE, 9, 41), (0x0D3, 9, 38), (0x0DD, 9, 39), (0x0EF, 9, 35), (0x0F6, 9, 42),
    (0x0FB, 9, 36), (0x10C, 9, -47), (0x11C, 9, -46), (0x132, 9, -45), (0x136, 9, -44), (0x150, 9, -48),
    (0x176, 9, -43), (0x17B, 9, -37), (0x190, 9, -49), (0x1CD, 9, -40), (0x1CE, 9, -41), (0x1D3, 9, -38),
    (0x1DD, 9, -39), (0x1EF, 9, -35), (0x1F6, 9, -42), (0x1FB, 9, -36), (0x010, 10, 65), (0x04D, 10, 56),
    (0x04E, 10, 57), (0x05D, 10, 55), (0x08C, 10, 62), (0x09C, 10, 61), (0x110, 10, 64), (0x153, 10, 53),
    (0x15D, 10, 54), (0x16F, 10, 50), (0x173, 10, 52), (0x19C, 10, 60), (0x1B2, 10, 59), (0x1B6, 10, 58),
    (0x1D0, 10, 63), (0x1F3, 10, 51), (0x210, 10, -65), (0x24D, 10, -56), (0x24E, 10, -57), (0x25D, 10, -55),
    (0x28C, 10, -62), (0x29C, 10, -61), (0x310, 10, -64), (0x353, 10, -53), (0x35D, 10, -54), (0x36F, 10, -50),
    (0x373, 10, -52), (0x39C, 10, -60), (0x3B2, 10, -59), (0x3B6, 10, -58), (0x3D0, 10, -63), (0x3F3, 10, -51),
    (0x053, 11, 70), (0x06F, 11, 66), (0x073, 11, 69), (0x0B2, 11, 77), (0x0B6, 11, 75), (0x0D0, 11, 81),
    (0x14E, 11, 73), (0x18C, 11, 79), (0x273, 11, 68), (0x2B2, 11, 76), (0x2B6, 11, 74), (0x2D0, 11, 80),
    (0x2F3, 11, 67), (0x34D, 11, 71), (0x34E, 11, 72), (0x38C, 11, 78), (0x453, 11, -70), (0x46F, 11, -66),
    (0x473, 11, -69), (0x4B2, 11, -77), (0x4B6, 11, -75), (0x4D0, 11, -81), (0x54E, 11, -73), (0x58C, 11, -79),
    (0x673, 11, -68), (0x6B2, 11, -76), (0x6B6, 11, -74), (0x6D0, 11, -80), (0x6F3, 11, -67), (0x74D, 11, -71),
    (0x74E, 11, -72), (0x78C, 11, -78), (0x0F3, 12, 85), (0x14D, 12, 89), (0x253, 12, 87), (0x26F, 12, 83),
    (0x4F3, 12, 84), (0x54D, 12, 88), (0x653, 12, 86), (0x66F, 12, 82), (0x8F3, 12, -85), (0x94D, 12, -89),
    (0xA53, 12, -87), (0xA6F, 12, -83), (0xCF3, 12, -84), (0xD4D, 12, -88), (0xE53, 12, -86), (0xE6F, 12, -82),
)

_RED_GREEN = (
    (0x003, 2, 0), (0x002, 3, 1), (0x006, 3, -1), (0x001, 4, 2), (0x009, 4, -2), (0x004, 5, 4),
    (0x00D, 5, 3), (0x014, 5, -4), (0x01D, 5, -3), (0x00C, 6, 6), (0x010, 6, 7), (0x015, 6, 5),
    (0x02C, 6, -6), (0x030, 6, -7), (0x035, 6, -5), (0x018, 7, 10), (0x01C, 7, 9), (0x020, 7, 11),
    (0x025, 7, 8), (0x058, 7, -10), (0x05C, 7, -9), (0x060, 7, -11), (0x065, 7, -8), (0x068, 7, ESCAPE),
    (0x038, 8, 14), (0x040, 8, 16), (0x045, 8, 12), (0x048, 8, 15), (0x07C, 8, 13), (0x0B8, 8, -14),
    (0x0C0, 8, -16), (0x0C5, 8, -12), (0x0C8, 8, -15), (0x0FC, 8, -13), (0x080, 9, 22), (0x085, 9, 17),
    (0x088, 9, 21), (0x0A8, 9, 20), (0x0BC, 9, 18), (0x0F8, 9, 19), (0x180, 9, -22), (0x185, 9, -17),
    (0x188, 9, -21), (0x1A8, 9, -20), (0x1BC, 9, -18), (0x1F8, 9, -19), (0x000, 10, 30), (0x03C, 10, 25),
    (0x078, 10, 26), (0x100, 10, 29), (0x105, 10, 23), (0x108, 10, 28), (0x128, 10, 27), (0x13C, 10, 24),
    (0x200, 10, -30), (0x23C, 10, -25), (0x278, 10, -26), (0x300, 10, -29), (0x305, 10, -23), (0x308, 10, -28),
    (0x328, 10, -27), (0x33C, 10, -24), (0x005, 11, 31), (0x008, 11, 37), (0x028, 11, 35), (0x178, 11, 33),
    (0x208, 11, 36), (0x228, 11, 34), (0x378, 11, 32), (0x405, 11, -31), (0x408, 11, -37), (0x428, 11, -35),
    (0x578, 11, -33), (0x608, 11, -36), (0x628, 11, -34), (0x778, 11, -32), (0x205, 12, 39), (0x605, 12, 38),
    (0xA05, 12, -39), (0xE05, 12, -38),
)

_ALPHA = (
    (0x001, 1, 0), (0x000, 4, ESCAPE), (0x002, 4, 1), (0x00A, 4, -1), (0x00C, 5, 2), (0x01C, 5, -2), (0x016, 6, 3),
    (0x018, 6, 4), (0x036, 6, -3), (0x038, 6, -4), (0x004, 7, 7), (0x02E, 7, 5), (0x034, 7, 6), (0x044, 7, -7),
    (0x06E, 7, -5), (0x074, 7, -6), (0x006, 8, 11), (0x008, 8, 14), (0x014, 8, 12), (0x01E, 8, 9), (0x048, 8, 15),
    (0x066, 8, 10), (0x068, 8, 13), (0x07E, 8, 8), (0x086, 8, -11), (0x088, 8, -14), (0x094, 8, -12), (0x09E, 8, -9),
    (0x0C8, 8, -15), (0x0E6, 8, -10), (0x0E8, 8, -13), (0x0FE, 8, -8), (0x028, 9, 23), (0x046, 9, 19), (0x054, 9, 20),
    (0x08E, 9, 17), (0x0A4, 9, 22), (0x0A8, 9, 24), (0x0C6, 9, 18), (0x0DE, 9, 16), (0x0E4, 9, 21), (0x128, 9, -23),
    (0x146, 9, -19), (0x154, 9, -20), (0x18E, 9, -17), (0x1A4, 9, -22), (0x1A8, 9, -24), (0x1C6, 9, -18), (0x1DE, 9, -16),
    (0x1E4, 9, -21), (0x00E, 10, 29), (0x024, 10, 37), (0x026, 10, 31), (0x04E, 10, 28), (0x064, 10, 35), (0x0BE, 10, 32),
    (0x0D4, 10, 33), (0x124, 10, 36), (0x126, 10, 30), (0x13E, 10, 25), (0x15E, 10, 26), (0x164, 10, 34), (0x1A6, 10, 127),
    (0x1CE, 10, 27), (0x1D4, 10, 128), (0x20E, 10, -29), (0x224, 10, -37), (0x226, 10, -31), (0x24E, 10, -28), (0x264, 10, -35),
    (0x2BE, 10, -32), (0x2D4, 10, -33), (0x324, 10, -36), (0x326, 10, -30), (0x33E, 10, -25), (0x35E, 10, -26), (0x364, 10, -34),
    (0x3A6, 10, -127), (0x3CE, 10, -27), (0x3D4, 10, -128), (0x03E, 11, 41), (0x05E, 11, 43), (0x0A6, 11, 50), (0x0CE, 11, 48),
    (0x10E, 11, 49), (0x14E, 11, 64), (0x1BE, 11, 39), (0x23E, 11, 40), (0x25E, 11, 42), (0x2A6, 11, 47), (0x2CE, 11, 44),
    (0x30E, 11, 46), (0x34E, 11, 45), (0x3BE, 11, 38), (0x43E, 11, -41), (0x45E, 11, -43), (0x4A6, 11, -50), (0x4CE, 11, -48),
    (0x50E, 11, -49), (0x54E, 11, -64), (0x5BE, 11, -39), (0x63E, 11, -40), (0x65E, 11, -42), (0x6A6, 11, -47), (0x6CE, 11, -44),
    (0x70E, 11, -46), (0x74E, 11, -45), (0x7BE, 11, -38),
)

LOOKUP_BITS = 12


def _lookup(codewords) -> Tuple[List[int], List[int]]:
    """Value and bit count for every 12 bit window (the codes are prefix free and complete)."""
    values = [None] * (1 << LOOKUP_BITS)
    bits = [0] * (1 << LOOKUP_BITS)
    for code, count, value in codewords:
        for index in range(code, 1 << LOOKUP_BITS, 1 << count):
            assert values[index] is None, "overlapping codewords"
            values[index], bits[index] = value, count
    assert all(v is not None for v in values), "incomplete code"
    return values, bits


# (values, bit counts, escape bits, escape bias) of the three codes, as the game uses them
BLUE = _lookup(_BLUE) + (9, 0xFF)
RED_GREEN = _lookup(_RED_GREEN) + (10, 0x1FE)
ALPHA = _lookup(_ALPHA) + (9, 0xFF)
BIT = None  # a single raw bit (a block's parity)

# IWI format: (t4ff format name, channels coded, bytes a texel)
FORMATS = {
    0x06: ("A8R8G8B8", 4, 4),  # B, G, R, A
    0x07: ("X8R8G8B8", 3, 4),  # B, G, R, then 255
    0x08: ("A8L8", 2, 2),  # L, A
    0x09: ("L8", 1, 1),
    0x0A: ("A8", 1, 1),
}


class WaveletError(Exception):
    pass


class _Reader:
    def __init__(self, data: bytes):
        self.data = bytes(data) + b"\0" * 8  # room to peek past the end
        self.size = len(data)
        self.byte = 0  # raw bytes come first
        self.bit = None  # then bits, least significant first

    def raw(self, count: int) -> bytes:
        if self.bit is not None or self.byte + count > self.size:
            raise WaveletError("raw texels after coded ones, or past the end")
        out = self.data[self.byte : self.byte + count]
        self.byte += count
        return out

    def symbols(self, count: int, plan: Sequence) -> List[int]:
        """``count`` times the symbols of ``plan`` (BIT or a code), in order."""
        if self.bit is None:
            self.bit = self.byte * 8
        data, pos = self.data, self.bit
        out = [0] * (count * len(plan))
        unpack = struct.Struct("<I").unpack_from
        i = 0
        for _ in range(count):
            for code in plan:
                word = unpack(data, pos >> 3)[0] >> (pos & 7)
                if code is None:
                    out[i] = word & 1
                    pos += 1
                else:
                    values, bits, escape_bits, bias = code
                    k = word & 0xFFF
                    value = values[k]
                    pos += bits[k]
                    if value == ESCAPE:
                        value = ((unpack(data, pos >> 3)[0] >> (pos & 7)) & ((1 << escape_bits) - 1)) - bias
                        pos += escape_bits
                    out[i] = value
                i += 1
        if pos > self.size * 8:
            raise WaveletError("coded data ends early")
        self.bit = pos
        return out


def _block_plan(channels: int) -> Tuple[list, list]:
    """Symbols of one 2x2 block, and for each coded channel (index, offset of its parity bit in
    the block, offset of its first difference, whether the first channel's differences are added)."""
    plan, layout = [], []
    if channels != 1:
        layout.append((0, len(plan), len(plan) + 1, False))
        plan += [BIT, BLUE, BLUE, BLUE]
        if channels >= 3:
            for channel in (1, 2):
                layout.append((channel, len(plan), len(plan) + 1, True))
                plan += [BIT, RED_GREEN, RED_GREEN, RED_GREEN]
    if channels != 3:
        layout.append((channels - 1, len(plan), len(plan) + 1, False))
        plan += [BIT, ALPHA, ALPHA, ALPHA]
    return plan, layout


def _level(reader: _Reader, smaller: np.ndarray, width: int, height: int, channels: int, texel: int) -> np.ndarray:
    """One level (height, width, texel) from the next smaller one."""
    if width <= 1 or height <= 1:
        level = np.full((height, width, texel), 255, dtype=np.uint8)
        level[:, :, :channels] = np.frombuffer(reader.raw(width * height * channels), dtype=np.uint8).reshape(height, width, channels)
        return level
    hw, hh = width // 2, height // 2
    source = smaller.astype(np.int32)
    if reader.symbols(1, [BIT])[0]:
        deltas = np.array(reader.symbols(hw * hh * channels, [ALPHA]), dtype=np.int32).reshape(hh, hw, channels)
        source[:, :, :channels] = (source[:, :, :channels] + deltas) & 0xFF
    plan, layout = _block_plan(channels)
    blocks = np.array(reader.symbols(hw * hh, plan), dtype=np.int32).reshape(hh, hw, len(plan))
    level = np.full((height, width, texel), 255, dtype=np.uint8)
    first = None
    for channel, parity_at, at, add_first in layout:
        h, v, d = (blocks[:, :, at + k] for k in range(3))
        if channel == 0 and channels != 1:
            first = (h, v, d)
        if add_first:
            h, v, d = h + first[0], v + first[1], d + first[2]
        base = 2 * source[:, :, channel]
        level[0::2, 0::2, channel] = (blocks[:, :, parity_at] + ((d + v + h + base) >> 1)) & 0xFF
        level[0::2, 1::2, channel] = ((h + base - d - v) >> 1) & 0xFF
        level[1::2, 0::2, channel] = ((v - d + base - h) >> 1) & 0xFF
        level[1::2, 1::2, channel] = ((base - h - v + d) >> 1) & 0xFF
    return level


def decode(fmt_code: int, width: int, height: int, faces: int, mipped: bool, payload: bytes) -> Tuple[str, List[bytes]]:
    """The levels (largest first, the faces of a cube map one after the other in each) of a wavelet
    IWI's ``payload`` (the data after its 28 byte header), and their t4ff format name."""
    if fmt_code not in FORMATS:
        raise WaveletError(f"not a wavelet format: {fmt_code:#x}")
    name, channels, texel = FORMATS[fmt_code]
    if width & (width - 1) or height & (height - 1):
        raise WaveletError("wavelet textures are powers of two")
    if not mipped and width > 1 and height > 1:
        raise WaveletError("wavelet textures larger than 1x1 have mip levels")
    sizes = [(width, height)]
    while mipped and sizes[-1] != (1, 1):
        w, h = sizes[-1]
        sizes.append((max(w >> 1, 1), max(h >> 1, 1)))
    reader = _Reader(payload)
    levels: List[List[np.ndarray]] = [[] for _ in sizes]
    for index in range(len(sizes) - 1, -1, -1):
        w, h = sizes[index]
        for face in range(faces):
            smaller = levels[index + 1][face] if index + 1 < len(sizes) else None
            levels[index].append(_level(reader, smaller, w, h, channels, texel))
    return name, [b"".join(face.tobytes() for face in level) for level in levels]
