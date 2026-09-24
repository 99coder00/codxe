"""Asset specific PC -> Xbox 360 conversion hooks."""

from __future__ import annotations

import struct
from typing import Optional

from . import images as img
from .commands import find_field
from .layout import TypeRef
from .zone import (
    ASSET_RECORDS,
    BLOCK_BY_NAME,
    BLOCK_LARGE_RUNTIME,
    BLOCK_TEMP,
    BLOCK_VIRTUAL,
    Node,
    Ptr,
)

# PC technique indices -> console technique indices. The console build has no tool/debug
# techniques (fakelight, sunlight preview, case texture, wireframe, debug bumpmap), which
# leaves 51 slots: 0..0x30 are identical, the two shadow cookie techniques move down.
PC_TECHNIQUE_TO_X360 = {i: i for i in range(0x31)}
PC_TECHNIQUE_TO_X360[0x37] = 0x31  # TECHNIQUE_SHADOWCOOKIE_CASTER
PC_TECHNIQUE_TO_X360[0x38] = 0x32  # TECHNIQUE_SHADOWCOOKIE_RECEIVER
X360_TECHNIQUE_COUNT = 51

NAME_FIELDS = {
    "Material": ("info", "name"),
    "WeaponDef": ("szInternalName",),
    "snd_alias_list_t": ("aliasName",),
    "Font_s": ("fontName",),
    "menuDef_t": ("window", "name"),
}


def name_offset(platform, rec_name: str) -> int:
    path = NAME_FIELDS.get(rec_name, ("name",))
    rec = platform.record(rec_name)
    offset = 0
    for i, part in enumerate(path):
        f = find_field(rec, part)
        if f is None:
            raise KeyError(f"{rec_name} has no {'.'.join(path)}")
        offset += f.offset
        if i < len(path) - 1:
            rec = platform.record(f.type.name)
    return offset


def string_node(text: str) -> Node:
    node = Node(TypeRef("scalar", "char", 1, 1), 0, BLOCK_VIRTUAL)
    node.string = True
    node.data = bytearray(text.encode("latin-1") + b"\0")
    node.count = len(node.data)
    node.extra["align"] = 1
    node.segments.append((TypeRef("scalar", "char", 1, 1), len(node.data), len(node.data), False))
    return node


def _follow(owner: Node, offset: int, child: Node) -> Ptr:
    ptr = Ptr("follow", child)
    ptr.owner = owner
    ptr.offset = offset
    owner.relocs[offset] = ptr
    owner.children.append(child)
    return ptr


def _asset_header(conv, rec_name: str, size: Optional[int] = None) -> Node:
    dst = conv.dst
    rec = dst.record(rec_name)
    t = TypeRef("record", rec_name, rec.size, rec.align)
    in_temp = dst.type_block(rec_name) == BLOCK_TEMP
    node = Node(t, 1, BLOCK_TEMP if in_temp else BLOCK_VIRTUAL)
    if in_temp:
        node.push_before = BLOCK_TEMP
    node.push_after = BLOCK_VIRTUAL
    node.extra["align"] = dst.type_alloc_align(rec_name, rec.align)
    node.extra["origin"] = ("asset", rec_name)
    node.data = bytearray(rec.size if size is None else size)
    node.segments.append((t, 1, len(node.data), False))
    return node


def build_reference(conv, asset_type: str, src_node: Node, ref_name: str) -> Node:
    """An asset header that only carries a (comma prefixed) name."""
    rec_name = ASSET_RECORDS[asset_type]
    node = _asset_header(conv, rec_name)
    node.asset = src_node.asset
    _follow(node, name_offset(conv.dst, rec_name), string_node(ref_name))
    return node


# ---------------------------------------------------------------------------
# Hooks


def techset_hook(conv, asset_type, node, name):
    # PC techniques carry PC shaders; the console resolves the technique set by name from its
    # own zones (this is also what CoD Xenon's converter does).
    ref = name if name.startswith(",") else "," + name
    conv.stats.count(conv.stats.referenced, asset_type)
    new = build_reference(conv, asset_type, node, ref)
    conv.node_map[id(node)] = new
    conv.offset_maps[id(node)] = lambda off: off
    return new


def material_hook(conv, asset_type, node, name):
    new = conv.convert_node(node)
    src_rec = conv.src.record("Material")
    dst_rec = conv.dst.record("Material")
    s_off = find_field(src_rec, "stateBitsEntry").offset
    d_off = find_field(dst_rec, "stateBitsEntry").offset
    src_entries = bytes(node.data[s_off : s_off + find_field(src_rec, "stateBitsEntry").type.count])
    dst_entries = bytearray(b"\xff" * X360_TECHNIQUE_COUNT)
    for pc_index, x_index in PC_TECHNIQUE_TO_X360.items():
        if pc_index < len(src_entries):
            dst_entries[x_index] = src_entries[pc_index]
    new.data[d_off : d_off + X360_TECHNIQUE_COUNT] = dst_entries
    return new


def image_hook(conv, asset_type, node, name):
    if name.startswith(","):
        return None  # plain reference: generic conversion keeps it a reference

    p = conv.src
    rec = p.record("GfxImage")

    def field_value(rec_, data, fname, base=0):
        f = find_field(rec_, fname)
        return struct.unpack_from("<" + {1: "B", 2: "H", 4: "I"}[f.type.size], data, base + f.offset)[0]

    map_type = field_value(rec, node.data, "mapType")
    semantic = field_value(rec, node.data, "semantic")
    category = field_value(rec, node.data, "category")

    source = image_source(conv, node, name)

    if source is None or map_type != 3:
        reason = "cube/volume image" if map_type != 3 else "no pixel data found (add the .iwd that contains it)"
        if conv.options.reference_missing_images:
            conv.warn(f"image '{name}': {reason}, emitting a reference to the console image")
            return _reference(conv, asset_type, node, name)
        raise img.ImageError(f"image '{name}': {reason}")

    drop = conv.image_drop_levels.get(name, 0)
    try:
        tex = img.build_console_texture(source, conv.options.max_texture_size, conv.options.keep_mips, drop, conv.options.compress_textures)
    except img.ImageError as e:
        conv.warn(str(e) + ", emitting a reference")
        return _reference(conv, asset_type, node, name)

    conv.stats.texture_bytes += len(tex.pixels)
    new = build_console_image(conv, name, tex, semantic, category, source.flags)
    conv.node_map[id(node)] = new
    conv.offset_maps[id(node)] = lambda off: off
    return new


def image_source(conv, node: Node, name: str) -> Optional[img.ImageData]:
    """Pixel data of a PC image asset: from its load def or from images/<name>.iwi (cached)."""
    cache = conv.__dict__.setdefault("_image_sources", {})
    if name in cache:
        return cache[name]
    p = conv.src
    source = None
    load_def = next((c for c in node.children if c.type.kind == "record" and c.type.name == "GfxImageLoadDef"), None)
    if load_def is not None:
        ld_rec = p.record("GfxImageLoadDef")
        f = find_field(ld_rec, "resourceSize")
        if struct.unpack_from("<I", load_def.data, f.offset)[0]:
            try:
                source = _image_from_load_def(p, name, load_def)
            except img.ImageError as e:
                conv.warn(str(e))
    if source is None and conv.library is not None:
        try:
            source = conv.library.image(name)
        except img.ImageError as e:
            conv.warn(str(e))
    cache[name] = source
    return source


def plan_textures(conv, root: Node):
    """Choose how many top mip levels to drop per image so the textures fit the memory budget."""
    options = conv.options
    sources = {}
    for node in root.walk():
        if node.type.kind == "record" and node.type.name == "GfxImage" and (node.extra.get("origin") or ("",))[0] == "asset":
            name = asset_display_name_pc(conv, node)
            if not name or name.startswith(","):
                continue
            src = image_source(conv, node, name)
            if src is not None and src.format in ("DXT1", "DXT3", "DXT5", "A8R8G8B8", "R8G8B8", "A8L8", "A8", "L8"):
                sources[name] = src

    drops = {n: 0 for n in sources}

    def size(n):
        return img.console_texture_size(sources[n], options.max_texture_size, options.keep_mips, drops[n], options.compress_textures)

    def can_drop(n):
        src = sources[n]
        level = drops[n] + 1
        return min(src.width >> level, src.height >> level) >= 64

    total = sum(size(n) for n in sources)
    before = total
    if options.texture_budget:
        while total > options.texture_budget:
            candidates = [n for n in sources if can_drop(n)]
            if not candidates:
                conv.warn(f"textures need {total / 1048576:.1f} MiB, over the {options.texture_budget / 1048576:.1f} MiB budget, and cannot be reduced further")
                break
            largest = max(candidates, key=size)
            old = size(largest)
            drops[largest] += 1
            total += size(largest) - old
    conv.image_drop_levels = drops
    if sources:
        reduced = sum(1 for n in drops if drops[n])
        conv.log(f"textures: {len(sources)} images, {before / 1048576:.1f} MiB -> {total / 1048576:.1f} MiB ({reduced} reduced)")


def asset_display_name_pc(conv, node: Node) -> str:
    from .zone import asset_name

    return asset_name(conv.src, node)


def _reference(conv, asset_type, node, name):
    conv.stats.count(conv.stats.referenced, asset_type)
    new = build_reference(conv, asset_type, node, name if name.startswith(",") else "," + name)
    conv.node_map[id(node)] = new
    conv.offset_maps[id(node)] = lambda off: off
    return new


def _image_from_load_def(p, name: str, load_def: Node) -> Optional[img.ImageData]:
    rec = p.record("GfxImageLoadDef")
    level_count, flags = load_def.data[0], load_def.data[1]
    width, height, depth = struct.unpack_from("<3H", load_def.data, find_field(rec, "dimensions").offset)
    fmt_value = struct.unpack_from("<I", load_def.data, find_field(rec, "format").offset)[0]
    fmt = img.PC_D3D_FORMATS.get(fmt_value)
    if fmt is None:
        raise img.ImageError(f"{name}: unsupported PC texture format {fmt_value:#x}")
    data = bytes(load_def.data[find_field(rec, "data").offset :])
    levels = []
    w, h = width, height
    pos = 0
    for _ in range(max(level_count, 1)):
        size = img.level_size(fmt, w, h)
        if pos + size > len(data):
            break
        levels.append(data[pos : pos + size])
        pos += size
        w, h = max(w >> 1, 1), max(h >> 1, 1)
    if not levels:
        return None
    return img.ImageData(name, fmt, width, height, levels, flags, "zone")


def build_console_image(conv, name: str, tex: img.ConsoleTexture, semantic: int, category: int, iwi_flags: int) -> Node:
    dst = conv.dst
    rec = dst.record("GfxImage")
    image = _asset_header(conv, "GfxImage")
    image.asset = "image"
    d = image.data
    E = dst.endian

    def put(fname, value, base_rec=rec, base=0):
        f = find_field(base_rec, fname)
        fmt = {1: "B", 2: "H", 4: "I"}[f.type.size if f.type.kind != "array" else f.type.elem.size]
        struct.pack_into(E + fmt, d, base + f.offset, value)

    put("mapType", 3)
    put("semantic", semantic)
    put("cardMemory", len(tex.pixels))
    put("width", tex.width)
    put("height", tex.height)
    put("depth", 1)
    put("category", category)
    put("delayLoadPixels", 1)
    put("baseSize", len(tex.pixels))
    put("streamSlot", 0xFFFF)
    put("streaming", 0)

    # name, texture (load def + header), pixels: in the console load order
    _follow(image, find_field(rec, "name").offset, string_node(name))

    ld_rec = dst.record("GfxImageLoadDef")
    ld_type = TypeRef("record", "GfxImageLoadDef", ld_rec.size, ld_rec.align)
    load_def = Node(ld_type, 1, BLOCK_TEMP)
    load_def.push_before = BLOCK_TEMP
    load_def.extra["align"] = ld_rec.align
    load_def.extra["origin"] = ("member", "GfxTexture", "loadDef")
    load_def.data = bytearray(ld_rec.size)
    struct.pack_into(E + "BB3HI", load_def.data, 0, tex.levels, _load_def_flags(iwi_flags), tex.width, tex.height, 1, tex.format.d3d)
    load_def.segments.append((ld_type, 1, ld_rec.size, False))
    texture_field = find_field(rec, "texture").offset
    _follow(image, texture_field, load_def)

    hdr_rec = dst.record("D3DBaseTexture360")
    hdr_type = TypeRef("record", "D3DBaseTexture360", hdr_rec.size, hdr_rec.align)
    header = Node(hdr_type, 1, BLOCK_VIRTUAL)
    header.push_before = BLOCK_VIRTUAL
    header.extra["align"] = hdr_rec.align
    header.extra["origin"] = ("member", "GfxImageLoadDef", "texture")
    header.data = bytearray(tex.header)
    header.segments.append((hdr_type, 1, len(header.data), False))
    _follow(load_def, find_field(ld_rec, "texture").offset, header)

    pixels = Node(TypeRef("scalar", "uchar", 1, 1), len(tex.pixels), BLOCK_LARGE_RUNTIME)
    pixels.data = bytearray(tex.pixels)
    pixels.extra["align"] = 4096
    pixels.extra["delayed"] = True
    pixels.extra["origin"] = ("member", "GfxImage", "pixels")
    pixels.segments.append((pixels.type, len(tex.pixels), len(tex.pixels), False))
    _follow(image, find_field(rec, "pixels").offset, pixels)
    return image


def _load_def_flags(iwi_flags: int) -> int:
    # The load def flags mirror the low IWI flags (no picmip / no mipmaps) in converted zones.
    return iwi_flags & 0x3


def register_hooks(conv):
    conv.hooks["techset"] = techset_hook
    conv.hooks["material"] = material_hook
    conv.hooks["image"] = image_hook
