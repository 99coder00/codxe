"""Asset specific PC -> Xbox 360 conversion hooks."""

from __future__ import annotations

import os
import struct
from typing import Optional

from . import images as img
from .commands import find_field
from .layout import TypeRef
from .zone import (
    ASSET_RECORDS,
    BLOCK_BY_NAME,
    BLOCK_LARGE,
    BLOCK_LARGE_RUNTIME,
    BLOCK_TEMP,
    BLOCK_VIRTUAL,
    Node,
    Ptr,
)

# PC technique indices -> console technique indices. The console build has no instanced lit
# techniques (TECHNIQUE_LIT_INSTANCED*, PC 0x24-0x2A) and no DEBUG_BUMPMAP_INSTANCED (PC 0x3A):
# PC 0x00-0x23 are identical, PC 0x2B-0x39 move down by 7. Checked against the technique names
# of the technique sets CoD Xenon embedded in its converted maps.
PC_TECHNIQUE_TO_X360 = {i: i for i in range(0x24)}
PC_TECHNIQUE_TO_X360.update({i: i - 7 for i in range(0x2B, 0x3A)})
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
    name = string_node(ref_name)
    _follow(node, name_offset(conv.dst, rec_name), name)
    _map_name_string(conv, src_node, rec_name, name)
    return node


def _map_name_string(conv, src_node: Node, rec_name: str, new_string: Node):
    """Other assets can point into the name string of a rebuilt asset (the PC linker shares equal
    strings): make those pointers resolve to the new name string."""
    node_map = getattr(conv, "node_map", None)
    if node_map is None:
        return
    try:
        ptr = src_node.relocs.get(name_offset(conv.src, rec_name))
    except KeyError:
        return
    old = ptr.target() if ptr is not None and ptr.kind != "null" else None
    if old is None or not old.string:
        return
    old_text = bytes(old.data).rstrip(b"\0")
    new_text = bytes(new_string.data).rstrip(b"\0")
    if not new_text.endswith(old_text):
        return
    shift = len(new_text) - len(old_text)
    node_map.setdefault(id(old), new_string)
    conv.offset_maps.setdefault(id(old), lambda off, shift=shift: off + shift)


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

    # Drop the state bits only the removed techniques used, numbering the rest in first use
    # order (as the console linker does).
    table = next((c for c in new.children if (c.extra.get("origin") or ("", "", ""))[1:] == ("Material", "stateBitsTable")), None)
    if table is not None and table.count:
        size = len(table.data) // table.count
        order = []
        for e in dst_entries:
            if e != 0xFF and e < table.count and e not in order:
                order.append(e)
        if order and len(order) < table.count:
            remap = {old: i for i, old in enumerate(order)}
            dst_entries = bytearray(remap.get(e, 0xFF) for e in dst_entries)
            table.data = bytearray(b"".join(bytes(table.data[i * size : (i + 1) * size]) for i in order))
            table.count = len(order)
            table.segments = [(table.type, table.count, len(table.data), False)]
            new.data[find_field(dst_rec, "stateBitsCount").offset] = table.count
        elif order:
            remap = {old: i for i, old in enumerate(order)}
            if any(remap[i] != i for i in order):
                dst_entries = bytearray(remap.get(e, 0xFF) for e in dst_entries)
                table.data = bytearray(b"".join(bytes(table.data[i * size : (i + 1) * size]) for i in order))
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
    new = build_console_image(conv, name, tex, semantic, category, source.flags, node)
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


def build_console_image(conv, name: str, tex: img.ConsoleTexture, semantic: int, category: int, iwi_flags: int, src_node: Optional[Node] = None) -> Node:
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
    name_string = string_node(name)
    _follow(image, find_field(rec, "name").offset, name_string)
    if src_node is not None:
        _map_name_string(conv, src_node, "GfxImage", name_string)

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


def xanim_hook(conv, asset_type, node, name):
    if name.startswith(","):
        return None
    from . import xanim

    new = conv.convert_node(node)
    xanim.convert_parts(conv, node, new, name)
    return new


# High mip streaming bounds of a surface whose textures are never streamed (an empty box):
# converted images are always fully resident.
NO_STREAM_BOUNDS = (131072.0, 131072.0, 131072.0, -131072.0, -131072.0, -131072.0)

# XModel members streamed after streamInfo (the console inserts highMipBounds before them)
_XMODEL_MEMBERS_AFTER_STREAM_INFO = ("physPreset", "physGeoms", "collmap", "physConstraints")


def xmodel_hook(conv, asset_type, node, name):
    if name.startswith(","):
        return None
    missing = conv.unverified_records(node)
    if missing and not conv.options.allow_unverified:
        return None  # generic path: reference
    new = conv.convert_node(node)

    dst = conv.dst
    rec = dst.record("XModel")
    lod0 = find_field(rec, "lodInfo").offset
    numsurfs = struct.unpack_from(dst.endian + "H", new.data, lod0 + find_field(dst.record("XModelLodInfo"), "numsurfs").offset)[0]
    if numsurfs:
        bounds_rec = dst.record("XModelHighMipBounds")
        t = TypeRef("record", "XModelHighMipBounds", bounds_rec.size, bounds_rec.align)
        bounds = Node(t, numsurfs, BLOCK_VIRTUAL)
        bounds.extra["align"] = bounds_rec.align
        bounds.extra["origin"] = ("member", "XModelStreamInfo", "highMipBounds")
        bounds.data = bytearray(struct.pack(dst.endian + "6f", *NO_STREAM_BOUNDS) * numsurfs)
        bounds.segments.append((t, numsurfs, len(bounds.data), False))
        offset = find_field(rec, "streamInfo").offset + find_field(dst.record("XModelStreamInfo"), "highMipBounds").offset
        pos = len(new.children)
        for i, child in enumerate(new.children):
            origin = child.extra.get("origin") or ("", "")
            member = origin[2] if origin[0] == "member" and origin[1] == "XModel" else None
            if member in _XMODEL_MEMBERS_AFTER_STREAM_INFO or (origin[0] == "asset" and origin[1] in ("PhysPreset", "PhysConstraints")):
                pos = i
                break
        ptr = Ptr("follow", bounds)
        ptr.owner = new
        ptr.offset = offset
        new.relocs[offset] = ptr
        new.children.insert(pos, bounds)
    return new


def _member_nodes(root: Node, rec: str, member: str):
    for n in root.walk():
        origin = n.extra.get("origin")
        if origin and origin[0] == "member" and origin[1] == rec and origin[2] == member:
            yield n


def gfxworld_hook(conv, asset_type, node, name):
    if name.startswith(","):
        return None
    missing = conv.unverified_records(node)
    if missing and not conv.options.allow_unverified:
        return None
    new = conv.convert_node(node)

    # Light grid rows: a (colStart, colCount, zStart, zCount) u16 header and a u32 first entry,
    # followed by a byte lookup table; rows start at 4 * rowDataStart[row].
    for grid_rows in _member_nodes(new, "GfxLightGrid", "rawRowData"):
        src_rows = next((n for n in _member_nodes(node, "GfxLightGrid", "rawRowData") if len(n.data) == len(grid_rows.data)), None)
        starts = next(_member_nodes(node, "GfxLightGrid", "rowDataStart"), None)
        if src_rows is None or starts is None:
            continue
        data = bytearray(src_rows.data)
        for start in sorted(set(struct.unpack(f"<{len(starts.data) // 2}H", starts.data))):
            o = 4 * start
            if o + 12 > len(data):
                continue
            data[o : o + 8] = struct.pack(">4H", *struct.unpack_from("<4H", data, o))
            data[o + 8 : o + 12] = data[o + 8 : o + 12][::-1]
        grid_rows.data = data

    # Vertex layer data: (u, v, RGBA color) records; the console color is ARGB.
    for layer in _member_nodes(new, "GfxWorldVertexLayerData", "data"):
        src_layer = next((n for n in _member_nodes(node, "GfxWorldVertexLayerData", "data") if len(n.data) == len(layer.data)), None)
        if src_layer is None or len(layer.data) % 12:
            conv.warn(f"gfxworld '{name}': unexpected vertex layer data size {len(layer.data)}")
            continue
        import numpy as np

        rows = np.frombuffer(bytes(src_layer.data), dtype=np.uint8).reshape(-1, 12)
        out = np.empty_like(rows)
        out[:, 0:8] = np.ascontiguousarray(rows[:, 0:8]).view("<f4").astype(">f4").view(np.uint8).reshape(-1, 8)
        out[:, 8:12] = rows[:, [11, 8, 9, 10]]
        layer.data = bytearray(out.tobytes())
    return new


# ---------------------------------------------------------------------------
# Sounds


def console_sound_name(name: str) -> str:
    """Console sound file names have no extension ('a/b.wav' -> 'a/b')."""
    prefix = "," if name.startswith(",") else ""
    base = name.lstrip(",")
    stem, ext = os.path.splitext(base)
    return prefix + (stem if ext.lower() in (".wav", ".mp3", ".ogg", ".flac", ".xma") else base)


def stream_name_hash(path: str) -> int:
    """Hash of a streamed sound 'dir\\name' (lower case), as stored in StreamFileName."""
    h = 5381
    for c in path.lower().encode("latin-1"):
        h = (h * 0x1003F + c) & 0xFFFFFFFF
    return h


def loaded_sound_hook(conv, asset_type, node, name):
    from . import audio

    console_name = console_sound_name(name)
    if name.startswith(","):
        return _reference(conv, asset_type, node, console_name)
    cache = conv.__dict__.setdefault("_loaded_sounds", {})
    encoder = conv.options.xma_encoder
    if encoder is None or not encoder.available:
        if not cache.get("_warned"):
            cache["_warned"] = True
            conv.warn("loaded sounds need xma2encode.exe (Xbox 360 XDK, --xma-encoder): they are emitted as references to console sounds")
        return _reference(conv, asset_type, node, console_name)
    data = next((c for c in node.children if (c.extra.get("origin") or ("", "", ""))[1:] == ("snd_asset", "data")), None)
    if data is None or not data.data:
        return _reference(conv, asset_type, node, console_name)
    key = (console_name, len(data.data))
    xma = cache.get(key)
    if xma is None:
        try:
            xma = audio.encode_loaded_sound(bytes(data.data), encoder, conv.options.sound_rate, conv.options.mono_sounds)
        except audio.AudioError as e:
            conv.warn(f"loaded sound '{name}': {e}, emitting a reference")
            return _reference(conv, asset_type, node, console_name)
        cache[key] = xma
    conv.stats.sound_bytes += len(xma.data)
    return build_loaded_sound(conv, console_name, xma)


def build_loaded_sound(conv, name: str, xma) -> Node:
    dst = conv.dst
    E = dst.endian
    rec = dst.record("LoadedSound")
    snd = dst.record("snd_asset")
    node = _asset_header(conv, "LoadedSound")
    node.asset = "loaded_sound"
    base = find_field(rec, "sound").offset
    struct.pack_into(E + "I", node.data, base + find_field(snd, "data_size").offset, len(xma.data))
    struct.pack_into(E + "36I", node.data, base + find_field(snd, "format").offset, *xma.format)
    _follow(node, find_field(rec, "name").offset, string_node(name))

    char = TypeRef("scalar", "char", 1, 1)
    data = Node(char, len(xma.data), BLOCK_LARGE)
    data.data = bytearray(xma.data)
    data.extra["align"] = 2048
    data.extra["origin"] = ("member", "snd_asset", "data")
    data.segments.append((char, len(xma.data), len(xma.data), False))
    _follow(node, base + find_field(snd, "data").offset, data)

    seek_rec = dst.record("XmaSeekTable360")
    seek_type = TypeRef("record", "XmaSeekTable360", seek_rec.size, seek_rec.align)
    uint = TypeRef("scalar", "uint", 4, 4)
    seek = Node(seek_type, 1, BLOCK_VIRTUAL)
    seek.data = bytearray(struct.pack(E + "II", 1, len(xma.seek_table)) + struct.pack(E + f"{len(xma.seek_table)}I", *xma.seek_table))
    seek.extra["align"] = 4
    seek.extra["origin"] = ("member", "snd_asset", "seekTable")
    seek.segments.append((seek_type, 1, 8, True))
    seek.segments.append((uint, len(xma.seek_table), 4 * len(xma.seek_table), False))
    _follow(node, base + find_field(snd, "seekTable").offset, seek)
    return node


def sound_hook(conv, asset_type, node, name):
    """Sound alias lists: streamed file names and primed buffers follow the console conventions.

    Streamed sounds of the usermap (found in its .iwd files) are converted to
    ``sounds/<dir>/<name>.xma`` next to the fastfile; their alias points to ``sounds\\<dir>``
    (CoD Xe serves ``D:\\sounds\\`` requests from the usermap folder). Other streamed sounds are
    stock console sounds: lower case directory, no extension, and the file name hash.
    """
    if name.startswith(","):
        return None
    missing = conv.unverified_records(node)
    if missing and not conv.options.allow_unverified:
        return None
    new = conv.convert_node(node)
    dst = conv.dst
    E = dst.endian
    rec = dst.record("SoundFile")
    u = find_field(rec, "u").offset
    sfn = dst.record("StreamFileName")
    ss = dst.record("StreamedSound")
    fn = u + find_field(ss, "filename").offset
    hash_off = fn + find_field(sfn, "hash").offset
    dir_off = fn + find_field(sfn, "dir").offset
    name_off = fn + find_field(sfn, "name").offset
    prime_off = u + find_field(ss, "primeSnd").offset
    done = conv.__dict__.setdefault("_sound_files_done", set())

    def string_of(sf, off):
        ptr = sf.relocs.get(off)
        target = ptr.target() if ptr is not None and ptr.kind != "null" else None
        if target is None:
            return None, None
        converted = conv.node_map.get(id(target), target)
        return converted, bytes(converted.data).rstrip(b"\0").decode("latin-1")

    for sf in new.walk():
        origin = sf.extra.get("origin") or ("", "", "")
        if origin[1:] != ("snd_alias_t", "soundFile") or not sf.data or sf.data[0] != 2 or id(sf) in done:
            continue
        done.add(id(sf))
        dir_node, directory = string_of(sf, dir_off)
        name_node, file_name = string_of(sf, name_off)
        if file_name is None:
            continue
        directory = (directory or "").replace("/", "\\")
        stem = console_sound_name(file_name)
        rel = (directory + "\\" if directory else "") + file_name
        custom = conv.library is not None and conv.library.read("sound/" + rel.replace("\\", "/")) is not None
        if custom:
            new_dir = "sounds\\" + directory if directory else "sounds"
            value = 0
        else:
            new_dir = directory.lower()
            stem = stem.lower()
            value = stream_name_hash((new_dir + "\\" if new_dir else "") + stem)
        struct.pack_into(E + "I", sf.data, hash_off, value)
        if dir_node is not None and id(dir_node) not in done:
            done.add(id(dir_node))
            dir_node.data = bytearray(new_dir.encode("latin-1") + b"\0")
            dir_node.count = len(dir_node.data)
            dir_node.segments = [(dir_node.segments[0][0], dir_node.count, dir_node.count, False)] if dir_node.segments else []
        if name_node is not None and id(name_node) not in done:
            done.add(id(name_node))
            name_node.data = bytearray(stem.encode("latin-1") + b"\0")
            name_node.count = len(name_node.data)
            name_node.segments = [(name_node.segments[0][0], name_node.count, name_node.count, False)] if name_node.segments else []
        _fix_primed_sound(conv, sf, prime_off, rel if custom else None)
    return new


PRIMED_SOUND_SIZE = 0x8000


def _fix_primed_sound(conv, sf: Node, prime_off: int, rel: Optional[str]):
    """Console primed buffers hold the start of the SDNS stream file (32 KB)."""
    ptr = sf.relocs.get(prime_off)
    if ptr is None or ptr.kind not in ("follow", "insert"):
        return
    primed = conv.node_map.get(id(ptr.node), ptr.node)
    sdns = None
    if rel is not None and conv.options.sounds_dir:
        path = os.path.join(conv.options.sounds_dir, "sounds", *os.path.splitext(rel)[0].split("\\")) + ".xma"
        if os.path.exists(path):
            with open(path, "rb") as f:
                sdns = f.read(PRIMED_SOUND_SIZE)
    buffer = next((c for c in primed.children if (c.extra.get("origin") or ("", "", ""))[1:] == ("PrimedSound", "buffer")), None)
    if sdns is None or buffer is None:
        # no console stream to prime from: play without priming
        ptr.kind, ptr.node = "null", None
        if primed in sf.children:
            sf.children.remove(primed)
        return
    buffer.data = bytearray(sdns)
    buffer.count = len(sdns)
    buffer.segments = [(buffer.type, buffer.count, len(sdns), False)]
    struct.pack_into(conv.dst.endian + "I", primed.data, find_field(conv.dst.record("PrimedSound"), "size").offset, len(sdns))


# Asset types whose console layout is known but whose PC -> console conversion is not.
UNSUPPORTED_CONVERSIONS = {}


def unsupported_hook(conv, asset_type, node, name):
    if name.startswith(",") or conv.options.allow_unverified:
        return None
    conv.warn(f"{asset_type} '{name}': {UNSUPPORTED_CONVERSIONS[asset_type]}, emitting a reference")
    return _reference(conv, asset_type, node, name)


def register_hooks(conv):
    for asset_type in UNSUPPORTED_CONVERSIONS:
        conv.hooks[asset_type] = unsupported_hook
    conv.hooks["techset"] = techset_hook
    conv.hooks["material"] = material_hook
    conv.hooks["image"] = image_hook
    conv.hooks["xanim"] = xanim_hook
    conv.hooks["xmodel"] = xmodel_hook
    conv.hooks["gfxworld"] = gfxworld_hook
    conv.hooks["loaded_sound"] = loaded_sound_hook
    conv.hooks["sound"] = sound_hook
