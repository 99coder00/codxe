"""Texture streaming: the top mip level of textures, kept in a map's ``images.pak``.

The console keeps a streamed image one level smaller in the fastfile and reads its top mip level
from ``<base>\\highmip\\<image>.hi`` when something using it is drawn close by (the disc's
``highmip`` folder has 3212 of them; 239 of the 414 images of ``pby_fly.ff`` are streamed). What
the game does, from its code (default.xex):

- an image streams when its ``streaming`` byte is set; ``streamSlot`` 0xFFFF (bit 15) means its
  top level is not loaded. ``baseSize`` is the size of the ``.hi`` file in KiB, as the disc's
  images have it (``a6mzero_jap_body_c``: 0x100, a 262144 byte file); the streamer reads that much
  into a pool of 128 KiB slots;
- the fastfile holds the image as a texture of half its size with the rest of the mip chain; the
  GPU texture's base and mip levels have addresses of their own, so the streamer makes the loaded
  level the base and the fastfile's data the mips. That data must so be laid out as the full
  texture's mip levels are: an image streams only when it is, byte for byte (see ``split``);
- the streamer loads, one at a time, the closest image whose top level is missing, of the
  materials (``textureCount`` set; water maps never stream) of:
  - the entities drawn: each surface of a model's first level of detail has a box
    (``XModel.streamInfo.highMipBounds``, model space);
  - the world: ``GfxWorld.streamInfo`` is a tree of boxes (``aabbTrees``, 32 bytes a node: first
    leaf reference, reference count, first child, child count, mins, maxs) the streamer walks from
    node 0, skipping a node whose box is farther than ``r_streamMaxDist`` (600). A node without
    children lists its references (``leafRefs``): a world surface's index (its box is the
    surface's ``boundsCopy``), or ``~index`` of a static model (its model's boxes). Static models
    so stream only through the tree;
- a box is the region in which the top level is needed: the disc's are the triangles' boxes grown
  by 1931.2 / the texels per unit of the triangle (a wall of 4 texels per unit: 482.8; checked on
  nazi_zombie_prototype.ff's world surfaces), an empty box (131072, -131072) when nothing streams.

A ``.hi`` file is the full texture's top level as the console holds it (tiled, its endian swap
done), no header. The map's are in one pack in its folder (``usermaps/<map>/images.pak``, see
``PakWriter``): CoD Xe's ``Sys_CreateFile`` hook opens it at the image's entry when the game asks
for ``D:\\highmip\\<image>.hi``, and the game's files stay untouched. The game reads the file
unbuffered, so entries start 4 KiB aligned.
"""

from __future__ import annotations

import os
import struct
from typing import Dict, List, Optional, Set, Tuple

import numpy as np

from . import xenos
from .commands import find_field
from .layout import TypeRef
from .zone import BLOCK_VIRTUAL, Node, Platform, Ptr, Zone, asset_name

HIGHMIP_DIRECTORY = "highmip"  # where earlier conversions put their .hi files (removed)
PAK_NAME = "images.pak"
PAK_MAGIC = b"T4FFPAK1"
PAK_VERSION = 2
# a pack with PAK_EIGHTH entries: CoD Xe builds that know only PAK_DEEP refuse it (they would apply
# such an entry two levels too small), leaving the fastfile's copies
PAK_VERSION_EIGHTH = 3
PAK_ALIGN = 4096
# an entry holding the whole texture (two levels streamed, see stream_textures' deep)
PAK_DEEP = 1
# with PAK_DEEP: the fastfile keeps an eighth of the texture's size, not a quarter (three levels)
PAK_EIGHTH = 2
# the largest block the streamer allocates (its buddy allocator's 4 MB regions)
MAX_STREAM_BLOCK = 4 * 1024 * 1024
# the smallest top level worth a .hi file: the streamer's slot (the disc's smallest are 128 KiB)
MIN_HIGHMIP_BYTES = 128 * 1024
MAPTYPE_2D = 3
SEMANTIC_WATER = 11

# a triangle's box grows by this over its texels per unit (the disc's linker, see above)
STREAM_DISTANCE = 1931.2
# but not without end: the disc has boxes a million units wide (triangles of no texture area)
MAX_STREAM_GROWTH = 4096.0
# what a surface whose triangles have no texture area grows by (the disc's walls: 4 texels per unit)
DEFAULT_STREAM_GROWTH = STREAM_DISTANCE / 4
# the boxes of a map for a console's memory: half the disc's growth. The game renders 1024x600 (an 80
# degree view): a top level shows alone up to about 600 / its texels per unit, blended up to twice
# that; the disc's 1931.2 has room to spare that its 64 MB buffer can give, a converted map's world
# often not (Kino Rezurrection's large textures wanted 53-61 MB of it at once with the disc's boxes,
# and textures the view was close to never came in)
CONSOLE_STREAM_GROWTH = 0.5
NO_STREAM_BOUNDS = (131072.0, 131072.0, 131072.0, -131072.0, -131072.0, -131072.0)
# references of a node the tree splits no further (the disc's leaves have 28 on average)
LEAF_REFS = 32
USHORT_MAX = 0xFFFF


class PakWriter:
    """A map's ``images.pak``, written in one pass (big endian, as the console reads it):

    - header (32 bytes): magic ``T4FFPAK1``, version (2; 3 with ``PAK_EIGHTH`` entries), entry count,
      index offset, index size;
    - the entries' data, each at a 4 KiB aligned offset (the game reads unbuffered);
    - the index: per entry (24 bytes, sorted by name) the name's offset from the index's start, its
      length, flags, the data's offset and size, the offset of its level 1 in the data and of its
      level 2 from there (deep entries); then the names (lower case, the image names the game asks
      for, without ``.hi``).

    An entry is an image's top level (the ``.hi`` file the game asks for), or with ``PAK_DEEP`` the
    whole texture: its level 0, then from ``mip_offset`` on its mip levels, a texture of half its
    size whose own mips start ``level2_offset`` on (0: it loads whole, never in two steps). With
    ``PAK_EIGHTH`` as well the fastfile's copy is an eighth of its size (three levels streamed).
    """

    def __init__(self, path: str):
        self.path = path
        self.temp = path + ".part"
        self.file = open(self.temp, "wb")
        self.file.write(bytes(PAK_ALIGN))
        self.entries: Dict[str, Tuple[int, int, int, int, int]] = {}

    def add(self, name: str, data: bytes, flags: int = 0, mip_offset: int = 0, level2_offset: int = 0):
        offset = self.file.tell()
        self.file.write(data)
        self.file.write(bytes(-len(data) % PAK_ALIGN))
        self.entries[name.lower()] = (offset, len(data), flags, mip_offset, level2_offset)

    def close(self) -> int:
        """Finish the pack (or remove it when it is empty); returns its entry count."""
        names = sorted(self.entries)
        blob, index = b"", b""
        for name in names:
            offset, size, flags, mip_offset, level2_offset = self.entries[name]
            encoded = name.encode("latin1")
            index += struct.pack(">IHHIIII", len(names) * 24 + len(blob), len(encoded), flags, offset, size, mip_offset, level2_offset)
            blob += encoded
        index_offset = self.file.tell()
        self.file.write(index + blob)
        self.file.seek(0)
        version = PAK_VERSION_EIGHTH if any(e[2] & PAK_EIGHTH for e in self.entries.values()) else PAK_VERSION
        self.file.write(struct.pack(">8sIIII", PAK_MAGIC, version, len(names), index_offset, len(index) + len(blob)))
        self.file.close()
        if os.path.exists(self.path):
            os.remove(self.path)
        if names:
            os.replace(self.temp, self.path)
        else:
            os.remove(self.temp)
        return len(names)


def read_pak(path: str) -> Dict[str, Tuple[bytes, int, int, int]]:
    """The entries of an ``images.pak`` (name -> (data, flags, mip offset, level 2 offset))."""
    with open(path, "rb") as f:
        data = f.read()
    magic, version, count, index_offset, _ = struct.unpack_from(">8sIIII", data, 0)
    if magic != PAK_MAGIC or version not in (PAK_VERSION, PAK_VERSION_EIGHTH):
        raise ValueError(f"{path}: not a t4ff images.pak")
    out = {}
    for i in range(count):
        name_offset, name_length, flags, offset, size, mip_offset, level2_offset = struct.unpack_from(">IHHIIII", data, index_offset + 24 * i)
        name = data[index_offset + name_offset: index_offset + name_offset + name_length].decode("latin1")
        out[name] = (data[offset: offset + size], flags, mip_offset, level2_offset)
    return out


def _innermost_assets(zone: Zone) -> Dict[int, Node]:
    """id(node) -> the innermost asset node holding it."""
    owner: Dict[int, Node] = {}
    stack: List[Tuple[Node, Optional[Node]]] = [(zone.extra_root, None)]
    while stack:
        node, asset = stack.pop()
        if (node.extra.get("origin") or ("",))[0] == "asset":
            asset = node
        if asset is not None:
            owner.setdefault(id(node), asset)
        for child in node.children:
            stack.append((child, asset))
    return owner


# who may use a material whose images stream: models and the world's surfaces (MaterialMemory is
# the world's list of its materials); the streamer looks at nothing else
_DRAWN = frozenset(("XModel", "GfxWorld.GfxSurface"))
_STREAM_USERS = _DRAWN | {"GfxWorld.MaterialMemory"}


def streamable_images(p: Platform, zone: Zone) -> List[Node]:
    """The images of the zone only materials use, whose every material only models and world
    surfaces use (the effects', the menus', the sky's... would never load their top level)."""
    owner = _innermost_assets(zone)
    users: Dict[int, Set[str]] = {}  # id(material or image) -> what points to it
    materials_of: Dict[int, Set[int]] = {}  # id(image) -> ids of the materials pointing to it
    nodes: Dict[int, Node] = {}
    for node in zone.extra_root.walk():
        for ptr in node.relocs.values():
            target = ptr.target()
            if target is None or target.type.name not in ("Material", "GfxImage"):
                continue
            holder = owner.get(id(node))
            if holder is None or holder is target:  # the zone's asset list, or the asset itself
                continue
            kind = holder.type.name
            if kind == "GfxWorld":  # which of the world's arrays
                kind += "." + node.type.name
            users.setdefault(id(target), set()).add(kind)
            nodes[id(target)] = target
            if target.type.name == "GfxImage" and kind == "Material":
                materials_of.setdefault(id(target), set()).add(id(holder))
    drawn = {key for key, kinds in users.items()
             if nodes[key].type.name == "Material" and kinds <= _STREAM_USERS and kinds & _DRAWN}
    return [nodes[key] for key, kinds in users.items()
            if nodes[key].type.name == "GfxImage" and kinds == {"Material"} and materials_of.get(key) and materials_of[key] <= drawn]


def _image_parts(p: Platform, image: Node):
    rec = p.record("GfxImage")
    load_def = next((c for c in image.walk() if c.type.name == "GfxImageLoadDef"), None)
    header = next((c for c in image.walk() if c.type.name == "D3DBaseTexture360"), None)
    pixels = next((c for c in image.children if c.extra.get("delayed")), None)
    if load_def is None or header is None or pixels is None:
        return None
    ld = p.record("GfxImageLoadDef")
    levels = load_def.data[find_field(ld, "levelCount").offset]
    d3d = struct.unpack_from(p.endian + "I", load_def.data, find_field(ld, "format").offset)[0]
    fmt = xenos.format_of_d3d(d3d)
    get = lambda name, size="H": struct.unpack_from(p.endian + size, image.data, find_field(rec, name).offset)[0]
    if get("mapType", "I") != MAPTYPE_2D or fmt is None:
        return None
    return load_def, header, pixels, fmt, get("width"), get("height"), levels


def split(pixels: bytes, width: int, height: int, fmt: xenos.Format, levels: int, min_bytes: int = MIN_HIGHMIP_BYTES) -> Optional[Tuple[bytes, bytes, bytes]]:
    """(.hi data, pixels of the half size texture, its GPU header) of a tiled 2D texture, or None
    when it cannot stream: no mip chain, not a power of two, a top level under ``min_bytes`` (a
    slot), or mips the half size texture would lay out otherwise than the full one."""
    if levels < 2 or width & (width - 1) or height & (height - 1) or min(width, height) < 64:
        return None
    base_size, _, total = xenos.mip_chain_layout(width, height, fmt, levels)
    if base_size < min_bytes or total > len(pixels):
        return None
    # the streamer reads 4 times the half size texture's base level (its padded width and height
    # doubled: Title Update 7's loader, "Could not stream highmip (size %i)"), into a level whose
    # rows are the half texture's pitch doubled: that must be the full texture's
    _, _, half_pitch, _, half_base = xenos.level_layout(width // 2, height // 2, 0, fmt)
    full_pitch = xenos.level_layout(width, height, 0, fmt)[2]
    if 2 * half_pitch != full_pitch or 4 * half_base < base_size:
        return None
    chain = xenos.untile_mip_chain(pixels, width, height, fmt, levels)
    half = xenos.tile_mip_chain(chain[1:], width // 2, height // 2, fmt)
    if half != bytes(pixels[base_size:total]):
        return None
    # rows the streamer counts and the texture has not: zeros after the level (the rows' addresses
    # only depend on the pitch)
    top = bytes(pixels[:base_size]) + bytes(4 * half_base - base_size)
    return top, half, xenos.texture_header_mips(width // 2, height // 2, fmt, levels - 1)


def with_mips(pixels: bytes, width: int, height: int, fmt: xenos.Format, mip_tail: bool = True) -> Optional[Tuple[bytes, int]]:
    """(tiled pixels, level count) of a tiled single level 2D texture given the mip chain the PC's
    lacks (IWIs saved without mips: a streamed texture needs the levels below its top one), box
    filtered down to the last level of a whole block as ``images.build_console_texture`` keeps; the
    top level stays as it is (``mip_tail`` False: without the packed mip tail, images.keeps_level).
    None when it would have no mip level or the format is not encoded."""
    from . import dxt
    from .images import keeps_level

    if min(width, height) <= 16:
        return None
    top = xenos.untile_level(pixels, width, height, 0, fmt)
    channels = {"A8R8G8B8": 4, "A8L8": 2, "L8": 1}.get(fmt.name)
    if fmt.name in ("DXT1", "DXT3", "DXT5", "DXN"):
        texels = dxt.decode(top, width, height, fmt.name)
        encode = lambda a: dxt.encode(a, fmt.name)
    elif channels is not None:
        texels = np.frombuffer(top, dtype=np.uint8).reshape(height, width, channels)
        encode = lambda a: a.tobytes()
    else:
        return None
    levels = [top]
    w, h = width, height
    while max(w, h) > 1:
        w, h = max(w >> 1, 1), max(h >> 1, 1)
        if min(w, h) < fmt.block or not (mip_tail or keeps_level(w, h)):
            break
        texels = dxt.downscale(texels)
        levels.append(encode(texels))
    if len(levels) < 2:
        return None
    return xenos.tile_mip_chain(levels, width, height, fmt), len(levels)


def _images_by_name(p: Platform, zone: Zone) -> Dict[str, List[Node]]:
    images: Dict[str, List[Node]] = {}
    for node in zone.extra_root.walk():
        if node.type.name == "GfxImage" and (node.extra.get("origin") or ("",))[0] == "asset":
            name = asset_name(p, node) or ""
            if name and not name.startswith(","):
                images.setdefault(name.lower(), []).append(node)
    return images


def settle_console_streaming(p: Platform, zone: Zone, pak: PakWriter, highmip_dirs: List[str], log=print) -> Tuple[List[str], List[str]]:
    """Images copied from console fastfiles that stream already (the campaign's: their top level is
    in the game's highmip folder) get their .hi file in the map's pack when one of
    ``highmip_dirs`` has it, of the size they read; the others stop streaming and stay a level
    smaller (the streamer would ask for a file nobody has, as the disc has none for the campaign
    zones' copies Kino Der Toten's sky models use). Returns (copied, unstreamed)."""
    rec = p.record("GfxImage")
    flag = find_field(rec, "streaming").offset
    size_field = find_field(rec, "baseSize").offset
    copied, unstreamed = [], []
    for name, nodes in _images_by_name(p, zone).items():
        for node in nodes:
            if not node.data[flag]:
                continue
            size = struct.unpack_from(p.endian + "I", node.data, size_field)[0] * 1024
            source = next((os.path.join(d, name + ".hi") for d in highmip_dirs
                           if os.path.isfile(os.path.join(d, name + ".hi")) and os.path.getsize(os.path.join(d, name + ".hi")) == size), None)
            if source is not None and len(nodes) == 1:
                with open(source, "rb") as f:
                    pak.add(name, f.read())
                copied.append(name)
            else:
                node.data[flag] = 0
                unstreamed.append(name)
    if copied:
        log(f"streaming: {len(copied)} console images keep streaming, their .hi files copied from the game's highmip folder")
    if unstreamed:
        log(f"streaming: {len(unstreamed)} console images streamed from .hi files no folder given has stay a level smaller ({', '.join(sorted(set(unstreamed))[:4])}{', ...' if len(set(unstreamed)) > 4 else ''})")
    return copied, unstreamed


def deep_split(pixels: bytes, width: int, height: int, fmt: xenos.Format, levels: int, eighth: bool = False):
    """(the whole texture's data, the offset of its level 1 in it, the offset of its level 2 from
    there or 0, pixels of the quarter size texture, its GPU header) when two levels of a tiled 2D
    texture can stream at once, else None: the quarter size texture must lay out as the full one's
    levels from 2 on, the level 1 offset be 4 KiB aligned (a GPU mip address) and the texture fit
    one block of the streamer. The level 2 offset (the half size texture's base level: its mip
    address when only the half size streams in) is 0 unless 4 KiB aligned. ``eighth``: three
    levels, the fastfile keeping the eighth size texture: a texture of its own (CoD Xe takes the mips
    of what streams in from the pack, never the fastfile's copy, and the pitch from the width), more
    than 16 texels a side so that it keeps the levels below (None when it would not)."""
    first = split(pixels, width, height, fmt, levels)
    if first is None:
        return None
    second = split(first[1], width // 2, height // 2, fmt, levels - 1, min_bytes=0)
    base_size, _, total = xenos.mip_chain_layout(width, height, fmt, levels)
    if second is None or base_size % PAK_ALIGN or total + -total % PAK_ALIGN > MAX_STREAM_BLOCK:
        return None
    kept, header = second[1], second[2]
    if eighth:
        if levels < 4 or min(width, height) // 8 <= 16:
            return None
        chain = xenos.untile_mip_chain(pixels, width, height, fmt, levels)[3:]
        kept = xenos.tile_mip_chain(chain, width // 8, height // 8, fmt)
        header = xenos.texture_header_mips(width // 8, height // 8, fmt, len(chain))
    level2 = xenos.mip_chain_layout(width // 2, height // 2, fmt, levels - 1)[0]
    return bytes(pixels[:total]), base_size, level2 if level2 % PAK_ALIGN == 0 else 0, kept, header


def stream_textures(p: Platform, zone: Zone, out_dir: str, log=print, highmip_dirs: Optional[List[str]] = None, is_game_image=None,
                    stock_texture=None, deep=None, upgrade_budget: Optional[int] = None, report: Optional[dict] = None,
                    mip_tail: bool = True, eighth=None, growth: float = 1.0) -> List[str]:
    """Write the top level of the images only models and world surfaces use to the map's pack
    (``out_dir/images.pak``) and keep them a level smaller in the zone, streamed (see above);
    then the boxes that load them (``write_stream_bounds``). Images copied from console fastfiles
    that stream already are settled first (``settle_console_streaming``). ``stock_texture(name,
    semantic)``: the PC game's version of a stock texture (images.ConsoleTexture) or None; it
    replaces a smaller console copy. ``deep``: the images (lower case names, or True: all) that
    stream two levels at once where they can (``deep_split``): the fastfile keeps a texture of a
    quarter of their size, the pack the whole texture (CoD Xe applies it). A stock texture's PC
    version replaces the console's copy when it streams and what it keeps in the fastfile takes no
    more memory, or the extra bytes fit ``upgrade_budget`` (None: none); ``report`` gets
    ``upgrade_bytes``, the bytes the upgrades added, and ``steps``: the levels each image streams
    (lower case names; 2 deep, 3 an eighth kept). ``eighth``: the deep ones (lower case names, or
    True: all) whose fastfile copy is an eighth of their size where they can (``PAK_EIGHTH``).
    ``growth``: the boxes' growth, of the disc linker's (``write_stream_bounds``). Returns the names
    of the images streamed."""
    rec = p.record("GfxImage")
    ld = p.record("GfxImageLoadDef")
    E = p.endian
    flag = find_field(rec, "streaming").offset
    semantic = find_field(rec, "semantic").offset
    # the files of an earlier conversion go: the zone written now is the one they must match
    directory = os.path.join(out_dir, HIGHMIP_DIRECTORY)
    if os.path.isdir(directory):
        for old in os.listdir(directory):
            if old.lower().endswith(".hi"):
                os.remove(os.path.join(directory, old))
        if not os.listdir(directory):
            os.rmdir(directory)
    pak = PakWriter(os.path.join(out_dir, PAK_NAME))
    settle_console_streaming(p, zone, pak, highmip_dirs or [], log)
    # a .hi file is one name's: images sharing a name (different copies, or one of the game's own zones'
    # that streams: CoD Xe would serve it the map's file) stay whole
    shared_names = {name for name, nodes in _images_by_name(p, zone).items() if len(nodes) > 1 or (is_game_image is not None and is_game_image(name))}
    streamed, upgraded, deepened, mipped, eighths, saved = [], [], [], [], [], 0
    upgrade_bytes = 0
    for image in streamable_images(p, zone):
        name = asset_name(p, image) or ""
        parts = _image_parts(p, image)
        if not name or name.startswith(",") or parts is None or os.sep in name or "/" in name:
            continue
        if image.data[flag] or name.lower() in shared_names:
            continue
        load_def, header, pixels, fmt, width, height, levels = parts
        source = bytes(pixels.data)
        if levels == 1 and xenos.mip_chain_layout(width, height, fmt, 1)[0] >= MIN_HIGHMIP_BYTES:
            # saved without mips: given them, it streams as the others (kept whole, all of it
            # stays in memory)
            made = with_mips(source, width, height, fmt, mip_tail)
            if made is not None and split(made[0], width, height, fmt, made[1]) is not None:
                source, levels = made
                mipped.append(name)
        result = split(source, width, height, fmt, levels)
        # a stock texture the console sized down: the PC game's, streamed, when what it keeps in the
        # fastfile (a quarter of it deep, else a half) takes no more memory than the console's copy,
        # or the budget has the difference
        wants_deep = deep is True or bool(deep and name.lower() in deep)
        tex = stock_texture(name, image.data[semantic]) if stock_texture is not None else None
        if tex is not None and tex.faces == 1 and tex.width >= width and tex.height >= height and tex.width * tex.height > width * height:
            better = split(tex.pixels, tex.width, tex.height, tex.format, tex.levels)
            whole = deep_split(tex.pixels, tex.width, tex.height, tex.format, tex.levels) if wants_deep and better is not None else None
            if better is not None:
                extra = len(whole[3] if whole is not None else better[1]) - len(pixels.data)
                if extra <= 0 or (upgrade_budget is not None and upgrade_bytes + extra <= upgrade_budget):
                    upgrade_bytes += max(extra, 0)
                    result, fmt, width, height, levels, source = better, tex.format, tex.width, tex.height, tex.levels, tex.pixels
                    struct.pack_into(E + "I", load_def.data, find_field(ld, "format").offset, fmt.d3d)
                    upgraded.append(name)
        if result is None:
            continue
        top, kept, kept_header = result
        steps, entry = 1, len(top)
        wants_eighth = wants_deep and (eighth is True or bool(eighth and name.lower() in eighth))
        whole = deep_split(source, width, height, fmt, levels, eighth=True) if wants_eighth else None
        flags = PAK_DEEP | PAK_EIGHTH if whole is not None else PAK_DEEP
        if whole is None and wants_deep:
            whole = deep_split(source, width, height, fmt, levels)
        if whole is not None:
            data, mip_offset, level2_offset, kept, kept_header = whole
            data += bytes(-len(data) % PAK_ALIGN)  # the game reads 4 KiB multiples, unbuffered
            pak.add(name, data, flags, mip_offset, level2_offset)
            steps, entry = (3 if flags & PAK_EIGHTH else 2), len(data)
            deepened.append(name)
            if steps == 3:
                eighths.append(name)
        else:
            pak.add(name, top)
        saved += len(pixels.data) - len(kept)
        pixels.data = bytearray(kept)
        pixels.count = len(kept)
        pixels.segments = [(pixels.type, len(kept), len(kept), False)]
        header.data = bytearray(kept_header)
        image.extra["full_size"] = (width, height)  # for the streaming boxes (_MaterialTexels)
        struct.pack_into(E + "B", load_def.data, find_field(ld, "levelCount").offset, levels - steps)
        struct.pack_into(E + "HH", load_def.data, find_field(ld, "dimensions").offset, width >> steps, height >> steps)
        for field, fmt_, value in (("width", "H", width >> steps), ("height", "H", height >> steps), ("cardMemory", "I", len(kept)),
                                   ("baseSize", "I", entry // 1024), ("streamSlot", "H", 0xFFFF), ("streaming", "B", 1)):
            struct.pack_into(E + fmt_, image.data, find_field(rec, field).offset, value)
        streamed.append(name)
    entries = pak.close()
    # the stock textures that stream not (too small for a slot, or what effects and menus use): the
    # PC game's whole, in the fastfile, while the budget has it
    kept_whole = []
    done = {n.lower() for n in streamed}
    for name, nodes in (_images_by_name(p, zone).items() if stock_texture is not None and upgrade_budget is not None else ()):
        image = nodes[0]
        parts = _image_parts(p, image)
        if len(nodes) > 1 or name in done or name in shared_names or image.data[flag] or parts is None or "/" in name:
            continue
        load_def, header, pixels, fmt, width, height, levels = parts
        tex = stock_texture(name, image.data[semantic])
        if tex is None or tex.faces != 1 or tex.width < width or tex.height < height or tex.width * tex.height <= width * height:
            continue
        extra = len(tex.pixels) - len(pixels.data)
        if extra > 0 and upgrade_bytes + extra > upgrade_budget:
            continue
        upgrade_bytes += max(extra, 0)
        pixels.data = bytearray(tex.pixels)
        pixels.count = len(tex.pixels)
        pixels.segments = [(pixels.type, len(tex.pixels), len(tex.pixels), False)]
        header.data = bytearray(tex.header)
        struct.pack_into(E + "B", load_def.data, find_field(ld, "levelCount").offset, tex.levels)
        struct.pack_into(E + "HH", load_def.data, find_field(ld, "dimensions").offset, tex.width, tex.height)
        struct.pack_into(E + "I", load_def.data, find_field(ld, "format").offset, tex.format.d3d)
        for field, fmt_, value in (("width", "H", tex.width), ("height", "H", tex.height), ("cardMemory", "I", len(tex.pixels)),
                                   ("baseSize", "I", tex.base_size or len(tex.pixels))):
            struct.pack_into(E + fmt_, image.data, find_field(rec, field).offset, value)
        kept_whole.append(name)
    upgraded += kept_whole
    if streamed:
        log(f"streaming: {len(streamed)} textures keep their top level in {PAK_NAME} ({entries} entries with the console's), "
            f"loaded when what uses them is close ({saved / 1048576:.1f} MiB less in the fastfile)")
    if deepened:
        log(f"streaming: {len(deepened)} of them stream two levels at once, the fastfile keeping a quarter of their size "
            f"({', '.join(sorted(deepened)[:4])}{', ...' if len(deepened) > 4 else ''})")
    if eighths:
        log(f"streaming: {len(eighths)} of those keep an eighth of their size instead, to fit the memory target "
            f"(three levels: {', '.join(sorted(eighths)[:4])}{', ...' if len(eighths) > 4 else ''})")
    if mipped:
        log(f"streaming: {len(mipped)} of them had no mip levels (saved without), made for them so they stream "
            f"({', '.join(sorted(mipped)[:4])}{', ...' if len(mipped) > 4 else ''})")
    if report is not None:
        report["upgrade_bytes"] = upgrade_bytes
        deep_names, eighth_names = set(deepened), set(eighths)
        report["steps"] = {n.lower(): 3 if n in eighth_names else 2 if n in deep_names else 1 for n in streamed}
    if upgraded:
        log(f"streaming: {len(upgraded)} stock textures the console has smaller are the PC game's ({len(upgraded) - len(kept_whole)} streamed, "
            f"{len(kept_whole)} whole in the fastfile), {upgrade_bytes / 1048576:.1f} MiB more in the fastfile "
            f"({', '.join(sorted(upgraded)[:4])}{', ...' if len(upgraded) > 4 else ''})")
    write_stream_bounds(p, zone, log, growth)
    return streamed


# Streaming bounds


def _offset(p: Platform, rec: str, path: str) -> int:
    offset = 0
    for part in path.split("."):
        f = find_field(p.record(rec), part)
        offset += f.offset
        rec = f.type.name
    return offset


def _target(node: Optional[Node], offset: int) -> Optional[Node]:
    ptr = node.relocs.get(offset) if node is not None else None
    return ptr.target() if ptr is not None else None


class _MaterialTexels:
    """The texels of the largest image of a material that streams (its full size), or 0."""

    def __init__(self, p: Platform):
        self.p = p
        mat = p.record("Material")
        self.count = find_field(mat, "textureCount").offset
        self.table = find_field(mat, "textureTable").offset
        tex = p.record("MaterialTextureDef")
        self.size = tex.size
        self.semantic = find_field(tex, "semantic").offset
        self.image = _offset(p, "MaterialTextureDef", "u.image")
        img = p.record("GfxImage")
        self.width, self.height, self.flag = (find_field(img, n).offset for n in ("width", "height", "streaming"))
        self.cache: Dict[int, int] = {}

    def __call__(self, material: Optional[Node]) -> int:
        if material is None:
            return 0
        key = id(material)
        if key not in self.cache:
            best = 0
            table = _target(material, self.table)
            for i in range(material.data[self.count] if table is not None else 0):
                if table.data[i * self.size + self.semantic] == SEMANTIC_WATER:
                    continue
                image = _target(table, i * self.size + self.image)
                if image is not None and image.data[self.flag]:
                    full = image.extra.get("full_size")
                    if full is not None:  # streamed here: its fastfile copy a half, quarter or eighth
                        best = max(best, full[0] * full[1])
                    else:  # streaming already (the console's): it holds its half size
                        w, h = struct.unpack_from(self.p.endian + "HH", image.data, self.width)
                        best = max(best, 4 * w * h)
            self.cache[key] = best
        return self.cache[key]


def grown_box(p0: np.ndarray, p1: np.ndarray, p2: np.ndarray, t0: np.ndarray, t1: np.ndarray, t2: np.ndarray, texels: int,
              scale: float = 1.0) -> Optional[Tuple[float, ...]]:
    """The region where triangles (positions, texture coordinates: (n, 3) and (n, 2) arrays) need
    the top level of a texture of ``texels``: each triangle's box grown by STREAM_DISTANCE over its
    texels per unit (mins + maxs), times ``scale``, or None when no triangle has an area."""
    world = 0.5 * np.linalg.norm(np.cross(p1 - p0, p2 - p0), axis=1)
    f1, f2 = t1 - t0, t2 - t0
    uv = 0.5 * np.abs(f1[:, 0] * f2[:, 1] - f1[:, 1] * f2[:, 0])
    with np.errstate(all="ignore"):
        valid = (world > 1e-6) & np.isfinite(world)
        grow = np.full(len(world), DEFAULT_STREAM_GROWTH * scale)
        textured = valid & (uv > 0) & np.isfinite(uv)
        grow[textured] = np.minimum(STREAM_DISTANCE * scale / np.sqrt(uv[textured] * texels / world[textured]), MAX_STREAM_GROWTH * scale)
    if not valid.any():
        return None
    grow = grow[valid, None]
    lo = np.minimum(np.minimum(p0[valid], p1[valid]), p2[valid]) - grow
    hi = np.maximum(np.maximum(p0[valid], p1[valid]), p2[valid]) + grow
    return tuple(float(v) for v in lo.min(axis=0)) + tuple(float(v) for v in hi.max(axis=0))


def _vertex_dtype(p: Platform, rec: str, uv: str) -> np.dtype:
    E = p.endian
    return np.dtype({"names": ["xyz", "uv"], "formats": [(E + "f4", 3), (E + uv, 2)],
                     "offsets": [_offset(p, rec, "xyz"), _offset(p, rec, "texCoord")], "itemsize": p.record(rec).size})


def _triangles(verts: np.ndarray, tris: np.ndarray):
    xyz = verts["xyz"].astype(np.float64)
    uv = verts["uv"].astype(np.float64)
    return xyz[tris[:, 0]], xyz[tris[:, 1]], xyz[tris[:, 2]], uv[tris[:, 0]], uv[tris[:, 1]], uv[tris[:, 2]]


def model_stream_bounds(p: Platform, model: Node, texels: _MaterialTexels, scale: float = 1.0) -> Optional[List[Tuple[float, ...]]]:
    """The boxes of the surfaces of a model's first level of detail (model space), as the disc's
    are: empty for a surface nothing of streams. None when the model has none."""
    E = p.endian
    rec = p.record("XModel")
    lod = _offset(p, "XModel", "lodInfo")
    numsurfs, first = struct.unpack_from(E + "HH", model.data, lod + _offset(p, "XModelLodInfo", "numsurfs"))
    if not numsurfs:
        return None
    surfs = _target(model, find_field(rec, "surfs").offset)
    materials = _target(model, find_field(rec, "materialHandles").offset)
    size = p.record("XSurface").size
    o_verts, o_tris = _offset(p, "XSurface", "verts0"), _offset(p, "XSurface", "triIndices")
    o_count = _offset(p, "XSurface", "triCount")
    dtype = _vertex_dtype(p, "GfxPackedVertex", "f2")
    mins = struct.unpack_from(E + "3f", model.data, find_field(rec, "mins").offset)
    maxs = struct.unpack_from(E + "3f", model.data, find_field(rec, "maxs").offset)
    boxes = []
    for s in range(first, first + numsurfs):
        n = texels(_target(materials, 4 * s))
        box = None
        if n and surfs is not None:
            verts, tris = _target(surfs, s * size + o_verts), _target(surfs, s * size + o_tris)
            count = struct.unpack_from(E + "H", surfs.data, s * size + o_count)[0]
            if verts is not None and tris is not None and count:
                v = np.frombuffer(bytes(verts.data), dtype, len(verts.data) // dtype.itemsize)
                t = np.frombuffer(bytes(tris.data), E + "u2", 3 * count).reshape(-1, 3).astype(np.int64)
                if len(v) and t.max() < len(v):
                    box = grown_box(*_triangles(v, t), n, scale)
            if box is None and all(lo <= hi for lo, hi in zip(mins, maxs)):
                box = tuple(m - DEFAULT_STREAM_GROWTH * scale for m in mins) + tuple(m + DEFAULT_STREAM_GROWTH * scale for m in maxs)
        boxes.append(box or NO_STREAM_BOUNDS)
    return boxes


def _box_streams(box) -> bool:
    return box[0] <= box[3] and box[1] <= box[4] and box[2] <= box[5]


def stream_tree(items: List[Tuple[int, Tuple[float, ...]]], endian: str = ">") -> Tuple[bytes, List[int]]:
    """The world's streaming tree (``GfxWorldStreamInfo360.aabbTrees``) and its leaf
    references of ``items`` (reference, box): nodes split in two along their longest side until
    they have LEAF_REFS; node 0 is the root, a node's children are consecutive, a node's
    references are those of its subtree (consecutive too)."""
    def build(group):
        box = np.concatenate([np.min([b[:3] for _, b in group], axis=0), np.max([b[3:] for _, b in group], axis=0)])
        if len(group) <= LEAF_REFS:
            return {"box": box, "items": group, "kids": []}
        centers = np.array([[(b[k] + b[k + 3]) / 2 for k in range(3)] for _, b in group])
        axis = int(np.argmax(centers.max(axis=0) - centers.min(axis=0)))
        order = sorted(range(len(group)), key=lambda i: centers[i, axis])
        half = len(group) // 2
        return {"box": box, "items": [], "kids": [build([group[i] for i in order[:half]]), build([group[i] for i in order[half:]])]}

    root = build(items)
    refs: List[int] = []

    def assign(node):
        node["first"] = len(refs)
        refs.extend(ref for ref, _ in node["items"])
        for kid in node["kids"]:
            assign(kid)
        node["count"] = len(refs) - node["first"]

    assign(root)
    nodes = [root]
    for node in nodes:  # breadth first: a node's children are consecutive
        node["child"] = len(nodes) if node["kids"] else 0
        nodes.extend(node["kids"])
    data = b"".join(struct.pack(endian + "HHHH6f", n["first"], n["count"], n["child"], len(n["kids"]), *n["box"]) for n in nodes)
    return data, refs


def _set_world_array(p: Platform, world: Node, member: str, values: bytes, count: int, scalar: str):
    """Point ``GfxWorld.streamInfo.<member>`` to a new array (block virtual, as the disc's), placed
    in the stream where the console reads it."""
    offset = _offset(p, "GfxWorld", "streamInfo." + member)
    old = _target(world, offset)
    if old is not None:
        world.children.remove(old)
    world.relocs.pop(offset, None)
    if not count:
        struct.pack_into(p.endian + "I", world.data, offset, 0)
        return
    t = TypeRef("scalar", scalar, 4, 4)
    node = Node(t, count, BLOCK_VIRTUAL)
    node.data = bytearray(values)
    node.segments.append((t, count, len(node.data), False))
    node.extra["align"] = 4
    node.extra["origin"] = ("member", "GfxWorldStreamInfo360", member)
    at = {id(ptr.target()): off for off, ptr in world.relocs.items() if ptr.target() is not None}
    pos = next((i for i, child in enumerate(world.children) if at.get(id(child), -1) > offset), len(world.children))
    ptr = Ptr("follow", node)
    ptr.owner = world
    ptr.offset = offset
    world.relocs[offset] = ptr
    world.children.insert(pos, node)


def write_stream_bounds(p: Platform, zone: Zone, log=print, scale: float = 1.0) -> Dict[str, int]:
    """The boxes that load streamed images: each model's surfaces' (``highMipBounds``), each world
    surface's (``boundsCopy``) and the world's tree of them and of the static models (see above);
    ``scale``: of the disc linker's growth (CONSOLE_STREAM_GROWTH for a console's memory)."""
    E = p.endian
    texels = _MaterialTexels(p)
    stats = {"models": 0, "surfaces": 0, "static models": 0}
    model_boxes: Dict[int, List[Tuple[float, ...]]] = {}
    bounds_offset = _offset(p, "XModel", "streamInfo.highMipBounds")
    for model in zone.extra_root.walk():
        if model.type.name != "XModel" or (model.extra.get("origin") or ("",))[0] != "asset":
            continue
        node = _target(model, bounds_offset)
        boxes = model_stream_bounds(p, model, texels, scale) if node is not None else None
        if boxes is None or len(boxes) != node.count:
            continue
        node.data = bytearray(b"".join(struct.pack(E + "6f", *box) for box in boxes))
        model_boxes[id(model)] = [box for box in boxes if _box_streams(box)]
        stats["models"] += bool(model_boxes[id(model)])

    world = next((n for n in zone.extra_root.walk() if n.type.name == "GfxWorld" and (n.extra.get("origin") or ("",))[0] == "asset"), None)
    if world is None:
        return stats
    items: List[Tuple[int, Tuple[float, ...]]] = []
    # the surfaces
    surfaces = _target(world, _offset(p, "GfxWorld", "dpvs.surfaces"))
    vertices = _target(world, _offset(p, "GfxWorld", "vd.vertices"))
    indices = _target(world, _offset(p, "GfxWorld", "indices"))
    surface_count = struct.unpack_from(E + "i", world.data, _offset(p, "GfxWorld", "surfaceCount"))[0]
    if surfaces is not None and vertices is not None and indices is not None:
        size = p.record("GfxSurface").size
        o = {name: _offset(p, "GfxSurface", path) for name, path in (
            ("first", "tris.firstVertex"), ("tris", "tris.triCount"), ("base", "tris.baseIndex"),
            ("box", "boundsCopy"), ("material", "material"))}
        dtype = _vertex_dtype(p, "GfxWorldVertex", "f4")
        verts = np.frombuffer(bytes(vertices.data), dtype, len(vertices.data) // dtype.itemsize)
        index = np.frombuffer(bytes(indices.data), E + "u2", len(indices.data) // 2).astype(np.int64)
        for s in range(min(surface_count, len(surfaces.data) // size)):
            at = s * size
            n = texels(_target(surfaces, at + o["material"]))
            box = None
            if n:
                first = struct.unpack_from(E + "i", surfaces.data, at + o["first"])[0]
                count = struct.unpack_from(E + "H", surfaces.data, at + o["tris"])[0]
                base = struct.unpack_from(E + "i", surfaces.data, at + o["base"])[0]
                tris = index[base: base + 3 * count]
                if len(tris) == 3 * count and count:
                    tris = tris.reshape(-1, 3) + first
                    if tris.max() < len(verts):
                        box = grown_box(*_triangles(verts, tris), n, scale)
                if box is None:
                    bounds = struct.unpack_from(E + "6f", surfaces.data, at + _offset(p, "GfxSurface", "bounds"))
                    grow = DEFAULT_STREAM_GROWTH * scale
                    box = tuple(v - grow for v in bounds[:3]) + tuple(v + grow for v in bounds[3:])
                items.append((s, box))
            struct.pack_into(E + "6f", surfaces.data, at + o["box"], *(box or NO_STREAM_BOUNDS))
        stats["surfaces"] = len(items)
    # the static models: their box in the world, a sphere around their origin holding their
    # surfaces' (the streamer turns the view into model space without the model's scale)
    insts = _target(world, _offset(p, "GfxWorld", "dpvs.smodelDrawInsts"))
    smodel_count = struct.unpack_from(E + "I", world.data, _offset(p, "GfxWorld", "dpvs.smodelCount"))[0]
    if insts is not None:
        size = p.record("GfxStaticModelDrawInst").size
        o_origin, o_model = _offset(p, "GfxStaticModelDrawInst", "origin"), _offset(p, "GfxStaticModelDrawInst", "model")
        for i in range(min(smodel_count, len(insts.data) // size)):
            boxes = model_boxes.get(id(_target(insts, i * size + o_model)))
            if not boxes:
                continue
            radius = max(float(np.linalg.norm([box[3 * (corner >> k & 1) + k] for k in range(3)])) for box in boxes for corner in range(8))
            origin = struct.unpack_from(E + "3f", insts.data, i * size + o_origin)
            items.append((~i, tuple(v - radius for v in origin) + tuple(v + radius for v in origin)))
            stats["static models"] += 1
    if len(items) > USHORT_MAX:
        log(f"warning: streaming: {len(items)} surfaces and static models stream, the world's tree holds {USHORT_MAX}: the rest keep their textures' top level unloaded")
        items = items[:USHORT_MAX]
    tree, refs = stream_tree(items, E) if items else (b"", [])
    _set_world_array(p, world, "aabbTrees", tree, len(tree) // 4, "uint")
    _set_world_array(p, world, "leafRefs", struct.pack(E + "%di" % len(refs), *refs), len(refs), "int")
    struct.pack_into(E + "i", world.data, _offset(p, "GfxWorld", "streamInfo.aabbTreeCount"), len(tree) // 32)
    struct.pack_into(E + "i", world.data, _offset(p, "GfxWorld", "streamInfo.leafRefCount"), len(refs))
    stats["nodes"] = len(tree) // 32
    if items:
        log(f"streaming: the world's tree has {stats['surfaces']} surfaces and {stats['static models']} static models "
            f"({stats['nodes']} nodes); {stats['models']} models have surfaces that stream")
    return stats


def model_bounds(p: Platform, model: Node) -> Tuple[float, ...]:
    """A model's box (mins, maxs), for its surfaces' streaming bounds."""
    rec = p.record("XModel")
    mins = struct.unpack_from(p.endian + "3f", model.data, find_field(rec, "mins").offset)
    maxs = struct.unpack_from(p.endian + "3f", model.data, find_field(rec, "maxs").offset)
    return mins + maxs
