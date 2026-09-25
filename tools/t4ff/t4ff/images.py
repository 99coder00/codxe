"""PC image sources (IWI files, zone load defs) and conversion to Xbox 360 textures."""

from __future__ import annotations

import os
import struct
import zipfile
from dataclasses import dataclass, field
from typing import Dict, List, Optional

from . import xenos

# IWI v6 (World at War) image formats
IWI_FORMATS = {
    0x01: "A8R8G8B8",
    0x02: "R8G8B8",
    0x03: "A8L8",
    0x04: "L8",
    0x05: "A8",
    0x0B: "DXT1",
    0x0C: "DXT3",
    0x0D: "DXT5",
}

IWI_FLAG_NOPICMIP = 0x01
IWI_FLAG_NOMIPMAPS = 0x02
IWI_FLAG_CUBEMAP = 0x04
IWI_FLAG_VOLMAP = 0x08

# PC D3DFORMAT values found in zone load defs
PC_D3D_FORMATS = {
    0x31545844: "DXT1",  # 'DXT1'
    0x33545844: "DXT3",
    0x35545844: "DXT5",
    21: "A8R8G8B8",
    22: "X8R8G8B8",
    50: "L8",
    51: "A8L8",
    28: "A8",
}


@dataclass
class ImageData:
    """A 2D texture with its mip chain (largest first), linear PC layout."""

    name: str
    format: str
    width: int
    height: int
    levels: List[bytes]
    flags: int = 0
    source: str = ""

    @property
    def mipped(self) -> bool:
        return len(self.levels) > 1


class ImageError(Exception):
    pass


def level_size(fmt: str, width: int, height: int) -> int:
    if fmt in ("DXT1",):
        return max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * 8
    if fmt in ("DXT3", "DXT5", "DXN"):
        return max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * 16
    bpp = {"A8R8G8B8": 4, "X8R8G8B8": 4, "R8G8B8": 3, "A8L8": 2, "L8": 1, "A8": 1}[fmt]
    return width * height * bpp


def mip_count(width: int, height: int) -> int:
    n = 1
    while width > 1 or height > 1:
        width = max(width >> 1, 1)
        height = max(height >> 1, 1)
        n += 1
    return n


def parse_iwi(name: str, data: bytes) -> ImageData:
    if data[:3] != b"IWi":
        raise ImageError(f"{name}: not an IWI file")
    version = data[3]
    if version != 6:
        raise ImageError(f"{name}: unsupported IWI version {version}")
    fmt_code, flags = data[4], data[5]
    width, height, depth = struct.unpack_from("<3H", data, 6)
    fmt = IWI_FORMATS.get(fmt_code)
    if fmt is None:
        raise ImageError(f"{name}: unsupported IWI format {fmt_code:#x}")
    if flags & (IWI_FLAG_CUBEMAP | IWI_FLAG_VOLMAP):
        raise ImageError(f"{name}: cube and volume maps are not supported yet")

    count = 1 if flags & IWI_FLAG_NOMIPMAPS else mip_count(width, height)
    sizes = []
    w, h = width, height
    for _ in range(count):
        sizes.append(level_size(fmt, w, h))
        w, h = max(w >> 1, 1), max(h >> 1, 1)

    payload = data[28:]
    if len(payload) < sum(sizes):
        # Some IWIs only carry the base level
        if len(payload) >= sizes[0]:
            return ImageData(name, fmt, width, height, [payload[: sizes[0]]], flags, "iwi")
        raise ImageError(f"{name}: truncated IWI ({len(payload)} < {sum(sizes)})")

    # IWI files store the mip chain from the smallest level to the largest.
    levels = []
    end = sum(sizes)
    for size in sizes:
        levels.append(payload[end - size : end])
        end -= size
    return ImageData(name, fmt, width, height, levels, flags, "iwi")


class IwdLibrary:
    """Looks up ``images/<name>.iwi`` and sound files in a list of .iwd archives / folders."""

    def __init__(self, paths: List[str]):
        self.entries: Dict[str, tuple] = {}
        for path in paths:
            self.add(path)

    def add(self, path: str):
        if os.path.isdir(path):
            for root, _, files in os.walk(path):
                for f in files:
                    full = os.path.join(root, f)
                    if f.lower().endswith(".iwd"):
                        self.add(full)
                    else:
                        rel = os.path.relpath(full, path).replace("\\", "/").lower()
                        self.entries.setdefault(rel, ("file", full))
            return
        try:
            archive = zipfile.ZipFile(path)
        except (zipfile.BadZipFile, FileNotFoundError, IsADirectoryError) as e:
            raise ImageError(f"cannot open {path}: {e}")
        for info in archive.infolist():
            self.entries.setdefault(info.filename.replace("\\", "/").lower(), ("zip", archive, info.filename))

    def read(self, rel: str) -> Optional[bytes]:
        entry = self.entries.get(rel.replace("\\", "/").lower())
        if entry is None:
            return None
        if entry[0] == "file":
            with open(entry[1], "rb") as f:
                return f.read()
        return entry[1].read(entry[2])

    def names(self, prefix: str) -> List[str]:
        prefix = prefix.lower()
        return [n for n in self.entries if n.startswith(prefix)]

    def image(self, name: str) -> Optional[ImageData]:
        data = self.read(f"images/{name}.iwi")
        if data is None:
            return None
        return parse_iwi(name, data)


# ---------------------------------------------------------------------------
# Console textures


@dataclass
class ConsoleTexture:
    format: xenos.Format
    width: int
    height: int
    levels: int
    header: bytes  # D3DBaseTexture360
    pixels: bytes  # tiled data, base level followed by the mip levels
    dropped_levels: int = 0


def to_console_format(fmt: str) -> Optional[str]:
    return {
        "DXT1": "DXT1",
        "DXT3": "DXT3",
        "DXT5": "DXT5",
        "DXN": "DXN",
        "A8R8G8B8": "A8R8G8B8",
        "A8L8": "A8L8",
        "L8": "L8",
    }.get(fmt)


def convert_rgb24(image: ImageData) -> ImageData:
    """R8G8B8 is not a console texture format: expand it to A8R8G8B8."""
    import numpy as np

    levels = []
    for level in image.levels:
        rgb = np.frombuffer(level, dtype=np.uint8).reshape(-1, 3)
        bgra = np.empty((rgb.shape[0], 4), dtype=np.uint8)
        bgra[:, :3] = rgb
        bgra[:, 3] = 255
        levels.append(bgra.tobytes())
    return ImageData(image.name, "A8R8G8B8", image.width, image.height, levels, image.flags, image.source)


def expand_a8(image: ImageData) -> ImageData:
    """A8 textures are stored as A8L8 with white luminance."""
    import numpy as np

    levels = []
    for level in image.levels:
        a = np.frombuffer(level, dtype=np.uint8)
        la = np.empty((a.size, 2), dtype=np.uint8)
        la[:, 0] = 255
        la[:, 1] = a
        levels.append(la.tobytes())
    return ImageData(image.name, "A8L8", image.width, image.height, levels, image.flags, image.source)


def compress_image(image: ImageData) -> ImageData:
    """Compress uncompressed colour textures to DXT1 (opaque) or DXT5 (4x-8x less memory)."""
    from . import dxt

    if image.format not in ("A8R8G8B8", "X8R8G8B8", "R8G8B8"):
        return image
    if image.format == "R8G8B8":
        image = convert_rgb24(image)
    rgba_levels = []
    w, h = image.width, image.height
    for level in image.levels:
        rgba = dxt.bgra_to_rgba(level, w, h)
        if image.format == "X8R8G8B8":
            rgba[:, :, 3] = 255
        rgba_levels.append(rgba)
        w, h = max(w >> 1, 1), max(h >> 1, 1)
    opaque = all((lv[:, :, 3] >= 255).all() for lv in rgba_levels)
    fmt = "DXT1" if opaque else "DXT5"
    levels = [dxt.encode(lv, fmt) for lv in rgba_levels]
    return ImageData(image.name, fmt, image.width, image.height, levels, image.flags, image.source)


def reduce_image(image: ImageData, count: int) -> ImageData:
    """Remove ``count`` top levels. When the mip chain runs out the base is downscaled (DXT data
    is decoded, box filtered and encoded again)."""
    from . import dxt

    levels = list(image.levels)
    w, h = image.width, image.height
    for _ in range(count):
        if min(w, h) <= 4:
            break
        if len(levels) > 1:
            levels.pop(0)
        elif image.format in ("DXT1", "DXT3", "DXT5"):
            rgba = dxt.downscale(dxt.decode(levels[0], w, h, image.format))
            fmt = "DXT5" if image.format == "DXT3" else image.format
            levels = [dxt.encode(rgba, fmt)]
            image = ImageData(image.name, fmt, w, h, levels, image.flags, image.source)
        elif image.format in ("A8R8G8B8", "X8R8G8B8"):
            rgba = dxt.downscale(dxt.bgra_to_rgba(levels[0], w, h))
            levels = [rgba[:, :, [2, 1, 0, 3]].tobytes()]
        else:
            break
        w, h = max(w >> 1, 1), max(h >> 1, 1)
    return ImageData(image.name, image.format, w, h, levels, image.flags, image.source)


def _drop_count(image: ImageData, max_size: int, drop_levels: int) -> int:
    count, w, h = 0, image.width, image.height
    while min(w, h) > 4 and ((max_size and (w > max_size or h > max_size)) or count < drop_levels):
        count += 1
        w, h = max(w >> 1, 1), max(h >> 1, 1)
    return count


def build_console_texture(image: ImageData, max_size: int = 0, keep_mips: bool = True, drop_levels: int = 0, compress: bool = True) -> ConsoleTexture:
    """Tile ``image`` for the Xbox 360.

    ``max_size`` limits the base level dimensions and ``drop_levels`` removes additional top
    levels. Uncompressed colour textures are compressed to DXT when ``compress`` is set.
    """

    if compress:
        image = compress_image(image)
    if image.format == "R8G8B8":
        image = convert_rgb24(image)
    if image.format == "A8":
        image = expand_a8(image)
    if image.format == "X8R8G8B8":
        image = ImageData(image.name, "A8R8G8B8", image.width, image.height, image.levels, image.flags, image.source)
    cfmt_name = to_console_format(image.format)
    if cfmt_name is None:
        raise ImageError(f"{image.name}: no console equivalent for {image.format}")
    fmt = xenos.FORMATS[cfmt_name]

    drop = _drop_count(image, max_size, drop_levels)
    if drop:
        image = reduce_image(image, drop)
    width, height = image.width, image.height

    levels = [image.levels[0]]
    if keep_mips and min(width, height) > 16:
        w, h = width, height
        for level in image.levels[1:]:
            w, h = max(w >> 1, 1), max(h >> 1, 1)
            # stop before levels smaller than one compression block
            if min(w, h) < fmt.block:
                break
            levels.append(level)

    if len(levels) > 1:
        pixels = xenos.tile_mip_chain(levels, width, height, fmt)
        header = xenos.texture_header_mips(width, height, fmt, len(levels))
    else:
        pixels = xenos.tile_level(levels[0], width, height, 0, fmt)
        header = xenos.texture_header(width, height, fmt, 1)
    return ConsoleTexture(fmt, width, height, len(levels), header, pixels, drop)


def console_texture_size(image: ImageData, max_size: int = 0, keep_mips: bool = True, drop_levels: int = 0, compress: bool = True) -> int:
    """Size of the console texture :func:`build_console_texture` would produce (without tiling)."""
    fmt_name = image.format
    if compress and fmt_name in ("A8R8G8B8", "X8R8G8B8", "R8G8B8"):
        fmt_name = "DXT5"  # upper bound, DXT1 when opaque
    fmt_name = {"R8G8B8": "A8R8G8B8", "X8R8G8B8": "A8R8G8B8", "A8": "A8L8"}.get(fmt_name, fmt_name)
    cfmt = to_console_format(fmt_name)
    if cfmt is None:
        return 0
    fmt = xenos.FORMATS[cfmt]
    drop = _drop_count(image, max_size, drop_levels)
    width, height = max(image.width >> drop, 1), max(image.height >> drop, 1)
    count = 1
    available = max(len(image.levels) - drop, 1)
    if keep_mips and min(width, height) > 16:
        w, h = width, height
        for _ in range(available - 1):
            w, h = max(w >> 1, 1), max(h >> 1, 1)
            if min(w, h) < fmt.block:
                break
            count += 1
    return xenos.mip_chain_layout(width, height, fmt, count)[2]
