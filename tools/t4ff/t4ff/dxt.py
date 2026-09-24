"""Vectorised DXT1/DXT3/DXT5 decoding and encoding (numpy).

Used to compress uncompressed PC textures for the console and to downscale
textures that have no mip levels to drop. The encoder is a range fit along the
principal colour axis, which is fast and good enough for game textures.
"""

from __future__ import annotations

import numpy as np


def _blocks(rgba: np.ndarray) -> np.ndarray:
    """(h, w, 4) -> (bh * bw, 16, 4), padding to multiples of 4 by edge replication."""
    h, w = rgba.shape[:2]
    ph, pw = (-h) % 4, (-w) % 4
    if ph or pw:
        rgba = np.pad(rgba, ((0, ph), (0, pw), (0, 0)), mode="edge")
    bh, bw = rgba.shape[0] // 4, rgba.shape[1] // 4
    return rgba.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * bw, 16, 4)


def _unblocks(blocks: np.ndarray, w: int, h: int) -> np.ndarray:
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    img = blocks.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w]


def _565_to_rgb(c: np.ndarray) -> np.ndarray:
    c = c.astype(np.uint32)
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], axis=-1).astype(np.int32)


def _rgb_to_565(rgb: np.ndarray) -> np.ndarray:
    rgb = np.clip(np.round(rgb), 0, 255).astype(np.uint32)
    return (((rgb[..., 0] * 31 + 127) // 255) << 11 | ((rgb[..., 1] * 63 + 127) // 255) << 5 | ((rgb[..., 2] * 31 + 127) // 255)).astype(np.uint16)


# ---------------------------------------------------------------------------
# Decoding


def _decode_color(color: np.ndarray, allow_punchthrough: bool):
    c0 = color[:, 0].astype(np.uint16) | (color[:, 1].astype(np.uint16) << 8)
    c1 = color[:, 2].astype(np.uint16) | (color[:, 3].astype(np.uint16) << 8)
    idx = color[:, 4].astype(np.uint32) | (color[:, 5].astype(np.uint32) << 8) | (color[:, 6].astype(np.uint32) << 16) | (color[:, 7].astype(np.uint32) << 24)
    p0, p1 = _565_to_rgb(c0), _565_to_rgb(c1)
    four = (c0 > c1) | (not allow_punchthrough)
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    palette = np.stack([p0, p1, p2, p3], axis=1)  # (n, 4, 3)
    shifts = np.arange(16, dtype=np.uint32) * 2
    indices = (idx[:, None] >> shifts) & 3  # (n, 16)
    rgb = np.take_along_axis(palette, indices[:, :, None].astype(np.int64).repeat(3, axis=2), axis=1)
    alpha = np.where(((indices == 3) & (~four)[:, None]), 0, 255)
    return rgb, alpha


def _decode_alpha5(alpha: np.ndarray) -> np.ndarray:
    a0 = alpha[:, 0].astype(np.int32)
    a1 = alpha[:, 1].astype(np.int32)
    bits = np.zeros(alpha.shape[0], dtype=np.uint64)
    for i in range(6):
        bits |= alpha[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    idx = ((bits[:, None] >> (np.arange(16, dtype=np.uint64) * np.uint64(3))) & np.uint64(7)).astype(np.int64)
    eight = a0 > a1
    pal = np.zeros((alpha.shape[0], 8), dtype=np.int32)
    pal[:, 0] = a0
    pal[:, 1] = a1
    for i in range(1, 7):
        pal[:, 1 + i] = np.where(eight, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        pal[:, 1 + i] = np.where(eight, pal[:, 1 + i], ((5 - i) * a0 + i * a1) // 5)
    pal[:, 6] = np.where(eight, pal[:, 6], 0)
    pal[:, 7] = np.where(eight, pal[:, 7], 255)
    return np.take_along_axis(pal, idx, axis=1)


def decode(data: bytes, width: int, height: int, fmt: str) -> np.ndarray:
    """Decode DXT data (linear PC layout) to an (h, w, 4) RGBA uint8 array."""
    bpb = 8 if fmt == "DXT1" else 16
    n = max(1, (width + 3) // 4) * max(1, (height + 3) // 4)
    raw = np.frombuffer(data[: n * bpb], dtype=np.uint8).reshape(n, bpb)
    if fmt == "DXT1":
        rgb, alpha = _decode_color(raw, True)
    else:
        rgb, _ = _decode_color(raw[:, 8:], False)
        if fmt == "DXT3":
            a = raw[:, :8]
            nibbles = np.stack([a & 15, a >> 4], axis=-1).reshape(n, 16).astype(np.int32)
            alpha = nibbles * 17
        else:
            alpha = _decode_alpha5(raw[:, :8])
    out = np.concatenate([rgb, alpha[:, :, None]], axis=2).astype(np.uint8)
    return _unblocks(out, width, height)


# ---------------------------------------------------------------------------
# Encoding


def _encode_color(block_rgb: np.ndarray, transparent: np.ndarray = None) -> np.ndarray:
    """block_rgb: (n, 16, 3) float. Returns (n, 8) uint8 DXT colour blocks."""
    n = block_rgb.shape[0]
    mean = block_rgb.mean(axis=1, keepdims=True)
    centered = block_rgb - mean
    cov = np.einsum("nij,nik->njk", centered, centered)
    # principal axis by power iteration
    axis = np.ones((n, 3)) / np.sqrt(3)
    for _ in range(4):
        axis = np.einsum("njk,nk->nj", cov, axis)
        norm = np.linalg.norm(axis, axis=1, keepdims=True)
        axis = np.where(norm > 1e-9, axis / np.maximum(norm, 1e-9), np.ones((n, 3)) / np.sqrt(3))
    proj = np.einsum("nij,nj->ni", centered, axis)
    lo = mean[:, 0] + axis * proj.min(axis=1, keepdims=True)
    hi = mean[:, 0] + axis * proj.max(axis=1, keepdims=True)

    c_hi = _rgb_to_565(hi)
    c_lo = _rgb_to_565(lo)
    punch = transparent.any(axis=1) if transparent is not None else np.zeros(n, dtype=bool)
    # 4 colour mode needs c0 > c1, 3 colour (punch through) mode c0 <= c1
    c0 = np.where(punch, np.minimum(c_hi, c_lo), np.maximum(c_hi, c_lo))
    c1 = np.where(punch, np.maximum(c_hi, c_lo), np.minimum(c_hi, c_lo))
    p0, p1 = _565_to_rgb(c0).astype(np.float64), _565_to_rgb(c1).astype(np.float64)
    p2 = np.where(punch[:, None], (p0 + p1) / 2, (2 * p0 + p1) / 3)
    p3 = np.where(punch[:, None], np.inf, (p0 + 2 * p1) / 3)
    palette = np.stack([p0, p1, p2, p3], axis=1)
    dist = ((block_rgb[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(axis=3)
    dist = np.nan_to_num(dist, nan=np.inf, posinf=np.inf)
    indices = dist.argmin(axis=2)
    if transparent is not None:
        indices = np.where(transparent, 3, indices)
    # identical endpoints in 4 colour mode would switch to 3 colour mode: use index 0 only
    same = (c0 == c1) & ~punch
    indices = np.where(same[:, None], 0, indices)

    bits = np.zeros(n, dtype=np.uint32)
    for i in range(16):
        bits |= indices[:, i].astype(np.uint32) << np.uint32(2 * i)
    out = np.zeros((n, 8), dtype=np.uint8)
    out[:, 0] = c0 & 0xFF
    out[:, 1] = c0 >> 8
    out[:, 2] = c1 & 0xFF
    out[:, 3] = c1 >> 8
    for i in range(4):
        out[:, 4 + i] = (bits >> np.uint32(8 * i)) & 0xFF
    return out


def _encode_alpha5(alpha: np.ndarray) -> np.ndarray:
    """alpha: (n, 16) int. Returns (n, 8) uint8 DXT5 alpha blocks (8 alpha mode)."""
    n = alpha.shape[0]
    a0 = alpha.max(axis=1)
    a1 = alpha.min(axis=1)
    a0 = np.where(a0 == a1, np.minimum(a0 + 1, 255), a0)
    a1 = np.where(a0 == a1, a1 - 1, a1)
    pal = np.zeros((n, 8), dtype=np.float64)
    pal[:, 0] = a0
    pal[:, 1] = a1
    for i in range(1, 7):
        pal[:, 1 + i] = ((7 - i) * a0 + i * a1) / 7.0
    idx = np.abs(alpha[:, :, None] - pal[:, None, :]).argmin(axis=2).astype(np.uint64)
    bits = np.zeros(n, dtype=np.uint64)
    for i in range(16):
        bits |= idx[:, i] << np.uint64(3 * i)
    out = np.zeros((n, 8), dtype=np.uint8)
    out[:, 0] = a0
    out[:, 1] = a1
    for i in range(6):
        out[:, 2 + i] = ((bits >> np.uint64(8 * i)) & np.uint64(0xFF)).astype(np.uint8)
    return out


def encode(rgba: np.ndarray, fmt: str) -> bytes:
    """Encode an (h, w, 4) RGBA uint8 array to DXT1/DXT5 (linear PC layout)."""
    blocks = _blocks(rgba).astype(np.float64)
    rgb = blocks[:, :, :3]
    alpha = blocks[:, :, 3]
    if fmt == "DXT1":
        out = _encode_color(rgb, alpha < 128)
    elif fmt == "DXT5":
        out = np.concatenate([_encode_alpha5(alpha.astype(np.int64)), _encode_color(rgb)], axis=1)
    else:
        raise ValueError(fmt)
    return out.tobytes()


def downscale(rgba: np.ndarray) -> np.ndarray:
    """2x box filter."""
    h, w = rgba.shape[:2]
    if h > 1:
        rgba = rgba[: h // 2 * 2]
    if w > 1:
        rgba = rgba[:, : w // 2 * 2]
    f = rgba.astype(np.float64)
    if f.shape[0] > 1:
        f = (f[0::2] + f[1::2]) / 2
    if f.shape[1] > 1:
        f = (f[:, 0::2] + f[:, 1::2]) / 2
    return np.clip(np.round(f), 0, 255).astype(np.uint8)


def bgra_to_rgba(data: bytes, width: int, height: int) -> np.ndarray:
    a = np.frombuffer(data[: width * height * 4], dtype=np.uint8).reshape(height, width, 4)
    return a[:, :, [2, 1, 0, 3]].copy()
