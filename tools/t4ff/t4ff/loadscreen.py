"""The loading screen zone of a map (``<map>_load.ff``).

While a map loads, the game shows the material ``$levelbriefing`` of the map's load zone; without
one it shows the default material, a checkerboard. CoD Xenon's 0.2.0 maps all have the same load
zone: ``$levelbriefing`` with a 1280x720 image ``loadscreen_<map>``, ``$defeatbackdrop`` with its
image ``defeat``, references to the game's ``,2d`` technique set and ``,$victorybackdrop``, and an
empty raw file ``<map>_load``. A map's load zone is made from one of theirs (among the console
fastfiles given) with the map's own picture, the first of:

1. a picture given for it (``--loading-image``: .png, .jpg, .bmp, .tga, .dds or .iwi);
2. CoD Xenon's own loading screen when they converted the same map (their ``<map>_load.ff``);
3. the map's own PC loading screen (``images/loadscreen_<map>.iwi`` of its files);
4. a title card with the map's name (from its .arena file), for a map without a picture of its
   own (Zombie Woods, from 2008).
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
from typing import List, Optional, Tuple

import numpy as np

from . import images as img
from .commands import find_field
from .deps import NO_WINDOW
from .zone import Node, Platform, Reader, Writer, Zone, asset_name

PICTURE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".bmp", ".tga", ".dds", ".webp", ".iwi")


class LoadScreenError(Exception):
    pass


# -- pictures ------------------------------------------------------------------


def rgba_of(image: img.ImageData) -> np.ndarray:
    """The base level of an image as an RGBA array (height, width, 4)."""
    from . import dxt

    w, h, data = image.width, image.height, image.levels[0]
    if image.format in ("DXT1", "DXT3", "DXT5"):
        return dxt.decode(data, w, h, image.format)
    if image.format in ("A8R8G8B8", "X8R8G8B8"):
        rgba = dxt.bgra_to_rgba(data, w, h)
        if image.format == "X8R8G8B8":
            rgba[:, :, 3] = 255
        return rgba
    if image.format == "R8G8B8":
        return rgba_of(img.convert_rgb24(image))
    if image.format in ("L8", "A8L8"):
        a = np.frombuffer(data, dtype=np.uint8).reshape(h, w, -1)
        rgba = np.empty((h, w, 4), dtype=np.uint8)
        rgba[:, :, :3] = a[:, :, :1]
        rgba[:, :, 3] = a[:, :, 1] if a.shape[2] > 1 else 255
        return rgba
    raise LoadScreenError(f"{image.name}: {image.format} pictures are not supported")


def _ffmpeg(args: List[str], data: Optional[bytes] = None) -> Optional[bytes]:
    from .audio import ffmpeg_exe

    exe = ffmpeg_exe()
    if exe is None:
        return None
    result = subprocess.run([exe, "-hide_banner", "-loglevel", "error"] + args, input=data, capture_output=True, **NO_WINDOW)
    return result.stdout if result.returncode == 0 else None


def _resize_bilinear(rgba: np.ndarray, width: int, height: int) -> np.ndarray:
    h, w = rgba.shape[:2]
    ys = np.clip((np.arange(height) + 0.5) * h / height - 0.5, 0, h - 1)
    xs = np.clip((np.arange(width) + 0.5) * w / width - 0.5, 0, w - 1)
    y0, x0 = np.floor(ys).astype(int), np.floor(xs).astype(int)
    y1, x1 = np.minimum(y0 + 1, h - 1), np.minimum(x0 + 1, w - 1)
    fy, fx = (ys - y0)[:, None, None], (xs - x0)[None, :, None]
    src = rgba.astype(np.float32)
    top = src[y0][:, x0] * (1 - fx) + src[y0][:, x1] * fx
    bottom = src[y1][:, x0] * (1 - fx) + src[y1][:, x1] * fx
    return np.clip(top * (1 - fy) + bottom * fy + 0.5, 0, 255).astype(np.uint8)


def resize(rgba: np.ndarray, width: int, height: int) -> np.ndarray:
    """``rgba`` scaled to ``width`` x ``height`` (Lanczos with FFmpeg, else bilinear)."""
    h, w = rgba.shape[:2]
    if (w, h) == (width, height):
        return rgba
    out = _ffmpeg(
        ["-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{w}x{h}", "-i", "-", "-vf", f"scale={width}:{height}:flags=lanczos", "-f", "rawvideo", "-pix_fmt", "rgba", "-"],
        np.ascontiguousarray(rgba).tobytes(),
    )
    if out is not None and len(out) == width * height * 4:
        return np.frombuffer(out, dtype=np.uint8).reshape(height, width, 4)
    return _resize_bilinear(rgba, width, height)


def read_picture(path: str, width: int, height: int) -> np.ndarray:
    """A picture file scaled to ``width`` x ``height``, as RGBA."""
    if path.lower().endswith(".iwi"):
        with open(path, "rb") as f:
            return resize(rgba_of(img.parse_iwi(os.path.basename(path), f.read())), width, height)
    if not os.path.isfile(path):
        raise LoadScreenError(f"loading screen picture not found: {path}")
    out = _ffmpeg(["-i", path, "-frames:v", "1", "-vf", f"scale={width}:{height}:flags=lanczos", "-f", "rawvideo", "-pix_fmt", "rgba", "-"])
    if out is None or len(out) < width * height * 4:
        raise LoadScreenError(f"cannot read the picture {path} (FFmpeg reads .png, .jpg, .bmp, .tga, .dds and .webp)")
    return np.frombuffer(out[: width * height * 4], dtype=np.uint8).reshape(height, width, 4)


# -- the zone --------------------------------------------------------------------


def _string(p: Platform, node: Node, rec_name: str, field: str) -> Optional[Node]:
    ptr = node.relocs.get(find_field(p.record(rec_name), field).offset)
    target = ptr.target() if ptr is not None and ptr.kind != "null" else None
    return target if target is not None and target.string else None


def _rename(node: Optional[Node], text: str):
    if node is None:
        return
    node.data = bytearray(text.encode("latin-1") + b"\0")
    node.count = len(node.data)
    node.segments = [(node.type, node.count, node.count, False)]


def _assets(p: Platform, zone: Zone, rec_name: str) -> List[Tuple[str, Node]]:
    return [(asset_name(p, n), n) for n in zone.extra_root.walk() if n.type.name == rec_name and (n.extra.get("origin") or ("",))[0] == "asset"]


def _picture_image(p: Platform, zone: Zone) -> Optional[Node]:
    """The image of ``$levelbriefing`` (named loadscreen_*)."""
    found = [n for name, n in _assets(p, zone, "GfxImage") if name and name.lower().startswith("loadscreen_")]
    return found[0] if found else None


def _pixels(node: Node) -> Optional[Node]:
    return next((c for c in node.children if c.extra.get("delayed") or (c.extra.get("origin") or ("", "", ""))[1:] == ("GfxImage", "pixels")), None)


def template_files(files: List[str], map_name: str) -> List[str]:
    """CoD Xenon's load zones among the console fastfiles, the map's own first."""
    loads = [f for f in files if f.lower().endswith("_load.ff")]
    own = f"{map_name}_load.ff".lower()
    return sorted(loads, key=lambda f: os.path.basename(f).lower() != own)


def read_template(p: Platform, path: str) -> Optional[Zone]:
    """A load zone made like CoD Xenon's: ``$levelbriefing`` with a loadscreen_* image."""
    from .fastfile import read_fastfile

    try:
        endian, _, data = read_fastfile(path)
        if endian != ">":
            return None
        zone = Reader(p, data).load()
    except Exception:
        return None
    if not any(name == "$levelbriefing" for name, _ in _assets(p, zone, "Material")) or _picture_image(p, zone) is None:
        return None
    return zone


def image_size(p: Platform, image: Node) -> Tuple[int, int]:
    rec = p.record("GfxImage")
    get = lambda f: struct.unpack_from(p.endian + "H", image.data, find_field(rec, f).offset)[0]  # noqa: E731
    return get("width"), get("height")


def set_picture(p: Platform, zone: Zone, rgba: np.ndarray):
    """Put the picture ``rgba`` (at the template's size) in the load zone ``zone``."""
    from .assets import _decode_console_image

    image = _picture_image(p, zone)
    pixels = _pixels(image)
    template = _decode_console_image(p, image, "template")
    if template is None or pixels is None:
        raise LoadScreenError("the template's loading screen image is not a 2D texture")
    width, height = template.width, template.height
    opaque = np.array(rgba, dtype=np.uint8, copy=True)
    opaque[:, :, 3] = 255
    bgra = opaque[:, :, [2, 1, 0, 3]].tobytes()
    tex = img.build_console_texture(img.ImageData("loadscreen", "A8R8G8B8", width, height, [bgra]), keep_mips=len(template.levels) > 1)
    if tex.format.name != template.format or len(tex.pixels) != len(pixels.data):
        raise LoadScreenError(f"the template's loading screen image is {template.format} {width}x{height}, which this picture cannot replace")
    pixels.data = bytearray(tex.pixels)


def build_load_zone(p: Platform, template: Zone, map_name: str, rgba: Optional[np.ndarray]) -> bytes:
    """The load zone of ``map_name`` from a CoD Xenon load zone, with the picture ``rgba`` (None:
    the template's own). Returns the zone serialised."""
    image = _picture_image(p, template)
    if rgba is not None:
        set_picture(p, template, rgba)
    _rename(_string(p, image, "GfxImage", "name"), f"loadscreen_{map_name}")
    for _, raw in _assets(p, template, "RawFile"):
        _rename(_string(p, raw, "RawFile", "name"), f"{map_name}_load")
    return Writer(p).write(template)


def load_zone_picture(path: str) -> Optional[np.ndarray]:
    """The loading screen picture of a console load zone (made like CoD Xenon's), as RGBA."""
    from .assets import _decode_console_image
    from .platforms import x360

    p = x360()
    zone = read_template(p, path)
    if zone is None:
        return None
    image = _decode_console_image(p, _picture_image(p, zone), "loadscreen")
    return rgba_of(image) if image is not None else None


def write_preview(map_dir: str, map_name: str, log=print) -> bool:
    """preview.bin (the map's picture in the Nazi Zombies map list, see menu.py) from the map's
    loading screen, when the map folder has one."""
    from .menu import PREVIEW_FILE, preview_file

    rgba = load_zone_picture(os.path.join(map_dir, f"{map_name}_load.ff"))
    if rgba is None:
        return False
    with open(os.path.join(map_dir, PREVIEW_FILE), "wb") as f:
        f.write(preview_file(rgba))
    return True


def pc_picture(map_files, pc_load_zone: Optional[str], map_name: str) -> Optional[img.ImageData]:
    """The map's own PC loading screen: the loadscreen_* image of its PC load zone (or named after
    the map), from the map's files."""
    names = [f"loadscreen_{map_name}"]
    if pc_load_zone:
        from .fastfile import read_fastfile
        from .platforms import pc

        try:
            _, _, data = read_fastfile(pc_load_zone)
            zone = Reader(pc(), data).load()
            names = [n.lstrip(",") for n, _ in _assets(pc(), zone, "GfxImage") if n and "loadscreen" in n.lower()] + names
        except Exception:
            pass
    for name in dict.fromkeys(names):
        try:
            picture = map_files.image(name)
        except img.ImageError:
            picture = None
        if picture is not None:
            return picture
    return None


def write_load_zone(map_name: str, out_dir: str, console_files: List[str], map_files, pc_load_zone: Optional[str] = None, picture_path: str = "", jobs: int = 0, log=print, title: str = "") -> bool:
    """Write ``<map>_load.ff`` into ``out_dir`` from a CoD Xenon load zone among ``console_files``
    (fastfiles, see library.library_files). False when there is none to make it from."""
    from .fastfile import write_fastfile
    from .platforms import x360

    p = x360()
    target = os.path.join(out_dir, f"{map_name}_load.ff")
    for path in template_files(console_files, map_name):
        template = read_template(p, path)
        if template is None:
            continue
        own = os.path.basename(path).lower() == f"{map_name}_load.ff".lower()
        width, height = image_size(p, _picture_image(p, template))
        if picture_path:
            rgba, source = read_picture(picture_path, width, height), picture_path
        elif own:
            shutil.copyfile(path, target)
            log(f"loading screen: CoD Xenon's own for this map ({path})")
            return True
        else:
            rgba, source = None, ""
            picture = pc_picture(map_files, pc_load_zone, map_name)
            if picture is not None:
                rgba, source = resize(rgba_of(picture), width, height), f"the map's own {picture.name}"
            else:
                title = title or map_title(map_files, map_name)
                rgba, source = title_card(title, width, height), f'a title card "{title}" (the map has no loading screen picture: give one with --loading-image)'

        out = build_load_zone(p, template, map_name, rgba)
        write_fastfile(target, ">", out, jobs=jobs)
        log(f"loading screen: {source}, {width}x{height} (made like CoD Xenon's, from {os.path.basename(path)})")
        return True
    return False


# -- a title card --------------------------------------------------------------------

# 5x7 pixel capitals, digits and a few signs ("#": set)
_FONT = {
    'A': ['.###.', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'],
    'B': ['####.', '#...#', '#...#', '####.', '#...#', '#...#', '####.'],
    'C': ['.###.', '#...#', '#....', '#....', '#....', '#...#', '.###.'],
    'D': ['####.', '#...#', '#...#', '#...#', '#...#', '#...#', '####.'],
    'E': ['#####', '#....', '#....', '####.', '#....', '#....', '#####'],
    'F': ['#####', '#....', '#....', '####.', '#....', '#....', '#....'],
    'G': ['.###.', '#...#', '#....', '#.###', '#...#', '#...#', '.####'],
    'H': ['#...#', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'],
    'I': ['.###.', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'],
    'J': ['..###', '...#.', '...#.', '...#.', '...#.', '#..#.', '.##..'],
    'K': ['#...#', '#..#.', '#.#..', '##...', '#.#..', '#..#.', '#...#'],
    'L': ['#....', '#....', '#....', '#....', '#....', '#....', '#####'],
    'M': ['#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '#...#'],
    'N': ['#...#', '##..#', '#.#.#', '#..##', '#...#', '#...#', '#...#'],
    'O': ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'],
    'P': ['####.', '#...#', '#...#', '####.', '#....', '#....', '#....'],
    'Q': ['.###.', '#...#', '#...#', '#...#', '#.#.#', '#..#.', '.##.#'],
    'R': ['####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'],
    'S': ['.####', '#....', '#....', '.###.', '....#', '....#', '####.'],
    'T': ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'],
    'U': ['#...#', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'],
    'V': ['#...#', '#...#', '#...#', '#...#', '#...#', '.#.#.', '..#..'],
    'W': ['#...#', '#...#', '#...#', '#.#.#', '#.#.#', '#.#.#', '.#.#.'],
    'X': ['#...#', '#...#', '.#.#.', '..#..', '.#.#.', '#...#', '#...#'],
    'Y': ['#...#', '#...#', '.#.#.', '..#..', '..#..', '..#..', '..#..'],
    'Z': ['#####', '....#', '...#.', '..#..', '.#...', '#....', '#####'],
    '0': ['.###.', '#...#', '#..##', '#.#.#', '##..#', '#...#', '.###.'],
    '1': ['..#..', '.##..', '..#..', '..#..', '..#..', '..#..', '.###.'],
    '2': ['.###.', '#...#', '....#', '...#.', '..#..', '.#...', '#####'],
    '3': ['#####', '...#.', '..#..', '...#.', '....#', '#...#', '.###.'],
    '4': ['...#.', '..##.', '.#.#.', '#..#.', '#####', '...#.', '...#.'],
    '5': ['#####', '#....', '####.', '....#', '....#', '#...#', '.###.'],
    '6': ['..##.', '.#...', '#....', '####.', '#...#', '#...#', '.###.'],
    '7': ['#####', '....#', '...#.', '..#..', '.#...', '.#...', '.#...'],
    '8': ['.###.', '#...#', '#...#', '.###.', '#...#', '#...#', '.###.'],
    '9': ['.###.', '#...#', '#...#', '.####', '....#', '...#.', '.##..'],
    '-': ['.....', '.....', '.....', '.###.', '.....', '.....', '.....'],
    '.': ['.....', '.....', '.....', '.....', '.....', '.##..', '.##..'],
    "'": ['..#..', '..#..', '.#...', '.....', '.....', '.....', '.....'],
    '!': ['..#..', '..#..', '..#..', '..#..', '..#..', '.....', '..#..'],
    '?': ['.###.', '#...#', '....#', '...#.', '..#..', '.....', '..#..'],
    ':': ['.....', '.##..', '.##..', '.....', '.##..', '.##..', '.....'],
    '&': ['.##..', '#..#.', '#.#..', '.#...', '#.#.#', '#..#.', '.##.#'],
    ' ': ['.....', '.....', '.....', '.....', '.....', '.....', '.....'],
}


def _glyph(c: str) -> np.ndarray:
    rows = _FONT.get(c) or _FONT["?"]
    return np.array([[ch == "#" for ch in row] for row in rows], dtype=bool)


def map_title(map_files, map_name: str) -> str:
    """The map's name for its title card: the longname of its entry in its .arena file (which can
    list other maps too, with localization keys such as MENU_LEVEL_MAK), else from ``map_name``."""
    import re

    for rel in [n for n in (map_files.names("") if map_files is not None else []) if n.endswith(".arena")]:
        text = (map_files.read(rel) or b"").decode("latin-1", "replace")
        for block in re.findall(r"\{([^}]*)\}", text) or [text]:
            entry_map = re.search(r'\bmap\s+"([^"]+)"', block, re.I)
            found = re.search(r'longname\s+"([^"]+)"', block, re.I)
            if not found or (entry_map and entry_map.group(1).lower() != map_name.lower()):
                continue
            title = re.sub(r"\^.", "", found.group(1)).strip()
            if title and not re.fullmatch(r"[A-Z0-9_]+", title):
                return title
    return " ".join(word[:1].upper() + word[1:] for word in map_name.replace("_", " ").split())


def title_card(title: str, width: int, height: int) -> np.ndarray:
    """A dark picture with ``title`` in large red letters, as RGBA."""
    y = np.linspace(0.0, 1.0, height)[:, None]
    rgba = np.zeros((height, width, 4), dtype=np.float32)
    rgba[:, :, 0] = 10 + 22 * (1 - np.abs(y - 0.5) * 2)  # a faint red band in the middle
    rgba[:, :, 1] = 8
    rgba[:, :, 2] = 8
    rgba[:, :, 3] = 255
    words = title.upper().split()
    lines, line = [], ""
    for word in words:  # at most 16 characters a line
        if line and len(line) + 1 + len(word) > 16:
            lines.append(line)
            line = word
        else:
            line = f"{line} {word}".strip()
    lines = (lines + [line])[:3] if line else lines[:3]
    if not lines:
        return rgba.astype(np.uint8)
    # characters are 6x9 cells of 5x7 glyphs; draw at 4x the final scale and scale down (smooth edges)
    longest = max(len(line) for line in lines)
    scale = max(1, min(int(width * 0.8 / (6 * longest)), int(height * 0.6 / (9 * len(lines)))))
    big = 4
    mask = np.zeros((9 * len(lines) * big, 6 * longest * big), dtype=np.float32)
    for row, text in enumerate(lines):
        x0 = (longest - len(text)) * 3 * big
        for i, c in enumerate(text):
            g = np.kron(_glyph(c), np.ones((big, big), dtype=bool))
            top, left = row * 9 * big + big, x0 + i * 6 * big
            mask[top : top + 7 * big, left : left + 5 * big] = g
    mh, mw = mask.shape[0] * scale // big, mask.shape[1] * scale // big
    small = resize(np.repeat((mask * 255).astype(np.uint8)[:, :, None], 4, axis=2), mw, mh)[:, :, 0].astype(np.float32) / 255
    top, left = (height - mh) // 2, (width - mw) // 2
    for dx, dy, color, weight in ((scale // 2 + 1, scale // 2 + 1, (0, 0, 0), 0.8), (0, 0, (178, 16, 16), 1.0)):  # shadow, then the letters
        region = rgba[top + dy : top + dy + mh, left + dx : left + dx + mw, :3]
        alpha = small[: region.shape[0], : region.shape[1], None] * weight
        region[:] = region * (1 - alpha) + np.array(color, dtype=np.float32) * alpha
    return np.clip(rgba, 0, 255).astype(np.uint8)
