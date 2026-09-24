"""Generic T4 zone (decompressed fastfile) reader and writer.

The reader interprets OpenAssetTools' zone code commands against the struct
layouts of a platform, exactly mirroring the game's ``DB_Load*`` functions. It
produces a tree of :class:`Node` objects: every node is one allocation made by
the loader (an asset header, an array, a string, ...) together with the raw
bytes that were streamed for it and the pointers inside those bytes.

The writer replays the same tree, recomputing block offsets, alignment and
pointer encodings for the target platform. Reading a zone and writing it back
for the same platform reproduces the original bytes, which is how the reader
is validated.
"""

from __future__ import annotations

import bisect
import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

from .commands import NEVER, Commands, MemberInfo, find_field
from .layout import Field, Layout, Record, SCALARS, TypeRef

FOLLOW = 0xFFFFFFFF
INSERT = 0xFFFFFFFE

BLOCK_TEMP = 0
BLOCK_RUNTIME = 1
BLOCK_LARGE_RUNTIME = 2
BLOCK_PHYSICAL_RUNTIME = 3
BLOCK_VIRTUAL = 4
BLOCK_LARGE = 5
BLOCK_PHYSICAL = 6
BLOCK_COUNT = 7

BLOCK_NAMES = [
    "XFILE_BLOCK_TEMP",
    "XFILE_BLOCK_RUNTIME",
    "XFILE_BLOCK_LARGE_RUNTIME",
    "XFILE_BLOCK_PHYSICAL_RUNTIME",
    "XFILE_BLOCK_VIRTUAL",
    "XFILE_BLOCK_LARGE",
    "XFILE_BLOCK_PHYSICAL",
]
BLOCK_BY_NAME = {n: i for i, n in enumerate(BLOCK_NAMES)}
BLOCK_BITS = 3
BLOCK_SHIFT = 32 - BLOCK_BITS
OFFSET_MASK = (1 << BLOCK_SHIFT) - 1

PC_ASSET_TYPES = [
    "xmodelpieces", "physpreset", "physconstraints", "destructibledef", "xanim", "xmodel", "material",
    "techset", "image", "sound", "loaded_sound", "clipmap", "clipmap_pvs", "comworld", "gameworld_sp",
    "gameworld_mp", "map_ents", "gfxworld", "lightdef", "ui_map", "font", "menulist", "menu", "localize",
    "weapon", "snddriverglobals", "fx", "impactfx", "aitype", "mptype", "character", "xmodelalias",
    "rawfile", "stringtable", "packindex",
]  # fmt: skip

X360_ASSET_TYPES = PC_ASSET_TYPES[:7] + ["pixelshader"] + PC_ASSET_TYPES[7:]

ASSET_RECORDS = {
    "physpreset": "PhysPreset",
    "physconstraints": "PhysConstraints",
    "destructibledef": "DestructibleDef",
    "xanim": "XAnimParts",
    "xmodel": "XModel",
    "material": "Material",
    "pixelshader": "MaterialPixelShader",
    "techset": "MaterialTechniqueSet",
    "image": "GfxImage",
    "sound": "snd_alias_list_t",
    "loaded_sound": "LoadedSound",
    "clipmap": "clipMap_t",
    "clipmap_pvs": "clipMap_t",
    "comworld": "ComWorld",
    "gameworld_sp": "GameWorldSp",
    "gameworld_mp": "GameWorldMp",
    "map_ents": "MapEnts",
    "gfxworld": "GfxWorld",
    "lightdef": "GfxLightDef",
    "font": "Font_s",
    "menulist": "MenuList",
    "menu": "menuDef_t",
    "localize": "LocalizeEntry",
    "weapon": "WeaponDef",
    "snddriverglobals": "SndDriverGlobals",
    "fx": "FxEffectDef",
    "impactfx": "FxImpactTable",
    "rawfile": "RawFile",
    "stringtable": "StringTable",
    "packindex": "PackIndex",
}


class ZoneError(Exception):
    pass


# ---------------------------------------------------------------------------
# Data model


class Ptr:
    """A pointer stored inside a node's data.

    kind:
      'null'   - nullptr
      'follow' - data follows in the stream (-1); ``node`` is the loaded child
      'insert' - like follow but an alias slot is reserved in the insert block (-2)
      'ref'    - offset into previously loaded data: ``node`` + ``index``/``inner``
      'alias'  - offset to a previously loaded pointer slot: ``slot`` is that Ptr
    """

    __slots__ = ("kind", "node", "index", "inner", "slot", "owner", "offset", "addr", "insert_addr")

    def __init__(self, kind: str, node: "Node" = None, index: int = 0, inner: int = 0, slot: "Ptr" = None):
        self.kind = kind
        self.node = node
        self.index = index
        self.inner = inner
        self.slot = slot
        self.owner: Optional[Node] = None
        self.offset = 0
        self.addr: Optional[int] = None  # zone address of this pointer slot (normal blocks only)
        self.insert_addr: Optional[int] = None  # zone address of the reserved insert slot

    def target(self) -> Optional["Node"]:
        """The node this pointer ultimately refers to."""
        p = self
        while p is not None and p.kind == "alias":
            p = p.slot
        return None if p is None else p.node

    def __repr__(self):
        if self.kind == "null":
            return "Ptr(null)"
        if self.kind == "alias":
            return f"Ptr(alias->{self.target()!r})"
        return f"Ptr({self.kind} {self.node!r})"


class Node:
    """One allocation made by the zone loader."""

    __slots__ = (
        "type", "count", "data", "block", "relocs", "children", "push_before", "push_after", "insert",
        "string", "asset", "offset", "extra", "runtime_size", "name", "segments",
    )  # fmt: skip

    def __init__(self, type_: TypeRef, count: int, block: int):
        self.type = type_  # element type
        self.count = count
        self.data = bytearray()
        self.block = block
        self.relocs: Dict[int, Ptr] = {}
        self.children: List[Node] = []
        self.push_before: Optional[int] = None  # block pushed before the allocation
        self.push_after: Optional[int] = None  # block pushed after the data was read (asset members)
        self.insert = False  # an insert slot is reserved right after the allocation
        self.string = False
        self.asset: Optional[str] = None  # asset type name for asset header nodes
        self.offset = 0  # block offset of the allocation (source platform while reading)
        self.extra: dict = {}
        self.runtime_size = 0  # size of allocations in non-streamed runtime blocks
        self.name: Optional[str] = None
        # How the node data is composed, in stream order: (type, count, size, partial). ``partial``
        # marks a structure streamed only up to its dynamic member.
        self.segments: List[Tuple[TypeRef, int, int, bool]] = []

    @property
    def elem_size(self) -> int:
        return 1 if self.string else self.type.size

    def __repr__(self):
        what = "string" if self.string else repr(self.type)
        return f"<Node {what} x{self.count} blk={self.block} off={self.offset:#x} size={len(self.data)}>"

    def walk(self):
        stack = [self]
        while stack:
            n = stack.pop()
            yield n
            stack.extend(reversed(n.children))


@dataclass
class ZoneAsset:
    type: str  # asset type name (see PC_ASSET_TYPES)
    ptr: Ptr  # pointer from the asset list entry
    name: str = ""

    @property
    def node(self) -> Optional[Node]:
        return self.ptr.target()


@dataclass
class Zone:
    platform: str
    script_strings: List[Optional[str]]
    assets: List[ZoneAsset]
    block_sizes: List[int]
    size: int = 0
    external_size: int = 0
    # the raw nodes of the script string table and asset list, kept for round-tripping
    script_node: Optional[Node] = None
    assets_node: Optional[Node] = None


# ---------------------------------------------------------------------------
# Platform description


class Platform:
    def __init__(self, name: str, endian: str, layout: Layout, commands: Commands, asset_types: List[str], streamed_blocks):
        self.name = name
        self.endian = endian
        self.layout = layout
        self.cmds = commands
        self.asset_types = asset_types
        self.asset_type_index = {n: i for i, n in enumerate(asset_types)}
        # blocks whose content is part of the stream (temp + normal, plus console runtime blocks)
        self.streamed_blocks = set(streamed_blocks)
        self.temp_padding = 16 if name == "pc" else 0
        self.u16 = struct.Struct(endian + "H")
        self.u32 = struct.Struct(endian + "I")
        self.asset_records = {rec for rec in commands.assets}
        self._leaf: Dict[str, bool] = {}
        self._dynamic: Dict[str, Optional[Field]] = {}
        self._ordered: Dict[str, List[Field]] = {}

    def record(self, name: str) -> Record:
        return self.layout.records[name]

    def is_asset(self, rec_name: str) -> bool:
        return rec_name in self.asset_records

    def type_block(self, rec_name: str) -> Optional[int]:
        info = self.cmds.types.get(rec_name)
        if info is None or info.block is None:
            return None
        return BLOCK_BY_NAME[info.block]

    def type_alloc_align(self, rec_name: str, default: int) -> int:
        info = self.cmds.types.get(rec_name)
        if info is not None and info.allocalign is not None:
            return info.allocalign.eval(None)
        return default

    # -- member directives ---------------------------------------------------

    def member_infos(self, owner: str, member: str) -> Dict[str, MemberInfo]:
        return self.cmds.member_infos(owner, member)

    def member_ignored(self, owner: str, member: str) -> bool:
        infos = self.member_infos(owner, member)
        return any(i.condition is NEVER for i in infos.values())

    def member_is_leaf(self, rec: Record, f: Field) -> bool:
        infos = self.member_infos(rec.name, f.name)
        if any(i.condition is NEVER for i in infos.values()):
            return True
        if any(i.arraysize is not None or i.string for i in infos.values()):
            return False
        return self.type_is_leaf(f.type)

    def type_is_leaf(self, t: TypeRef) -> bool:
        if t.kind in ("scalar", "enum", "void"):
            return True
        if t.kind == "pointer":
            return False
        if t.kind == "array":
            return self.type_is_leaf(t.elem)
        if t.kind == "record":
            return self.record_is_leaf(t.name)
        raise TypeError(t)

    def record_is_leaf(self, name: str) -> bool:
        if name not in self._leaf:
            self._leaf[name] = True  # guards recursion through self referencing records
            rec = self.record(name)
            self._leaf[name] = all(self.member_is_leaf(rec, f) for f in rec.fields)
        return self._leaf[name]

    def dynamic_member(self, name: str) -> Optional[Field]:
        """The member of ``name`` whose size is only known at load time, if any."""
        if name not in self._dynamic:
            self._dynamic[name] = None
            rec = self.record(name)
            found = None
            for f in rec.fields:
                infos = self.member_infos(rec.name, f.name)
                if any(i.arraysize is not None for i in infos.values()):
                    found = f
                elif f.type.kind == "record" and self.dynamic_member(f.type.name) is not None:
                    found = f
            self._dynamic[name] = found
        return self._dynamic[name]

    def ordered_members(self, rec: Record) -> List[Field]:
        if rec.name in self._ordered:
            return self._ordered[rec.name]
        fields = [f for f in rec.fields if f.name]
        info = self.cmds.types.get(rec.name)
        if info is not None and info.reorder:
            order = info.reorder
            named = [n for n in order if n != "..."]
            reordered = [find_field(rec, n) for n in named]
            if "..." in order:
                rest = [f for f in fields if f.name not in named]
                idx = order.index("...")
                before = [find_field(rec, n) for n in order[:idx]]
                after = [find_field(rec, n) for n in order[idx + 1 :]]
                fields = before + rest + after
            else:
                rest = [f for f in fields if f.name not in named]
                fields = reordered + rest
        self._ordered[rec.name] = fields
        return fields


# ---------------------------------------------------------------------------
# Expression evaluation context


class _Frame:
    __slots__ = ("rec", "node", "base")

    def __init__(self, rec: Record, node: Node, base: int):
        self.rec = rec
        self.node = node
        self.base = base


class _EvalContext:
    def __init__(self, platform: Platform, stack: List[_Frame], context: str):
        self.p = platform
        self.stack = stack
        self.context = context

    def frame_for(self, rec_name: str) -> _Frame:
        for frame in reversed(self.stack):
            if frame.rec.name == rec_name:
                return frame
        raise ZoneError(f"no {rec_name} instance on the load stack")

    def lookup(self, path: List[str], indices: List[int]):
        records = self.p.layout.records
        ctx_frame = self.frame_for(self.context)
        if len(path) > 1 and path[0] in records and find_field(ctx_frame.rec, path[0]) is None:
            frame = self.frame_for(path[0])
            path = path[1:]
        else:
            frame = ctx_frame

        rec = frame.rec
        offset = frame.base
        t = None
        for i, part in enumerate(path):
            f = find_field(rec, part)
            if f is None:
                raise ZoneError(f"{rec.name} has no field {part}")
            offset += f.offset
            t = f.type
            if i < len(path) - 1:
                rec = records[t.name]
        idx = list(indices)
        while t.kind == "array":
            i = idx.pop(0) if idx else 0
            offset += i * t.elem.size
            t = t.elem
        return read_scalar(self.p, frame.node.data, offset, t)


def read_scalar(p: Platform, data, offset: int, t: TypeRef):
    if t.kind == "enum":
        code = {1: "B", 2: "H", 4: "i"}[t.size]
    elif t.kind == "scalar":
        code = SCALARS[t.name][0]
    elif t.kind == "pointer":
        code = "I"
    else:
        raise ZoneError(f"cannot evaluate {t!r}")
    return struct.unpack_from(p.endian + code, data, offset)[0]


# ---------------------------------------------------------------------------
# Reader


def zone_addr(block: int, offset: int) -> int:
    return (block << BLOCK_SHIFT) | (offset & OFFSET_MASK)


class Reader:
    def __init__(self, platform: Platform, data: bytes):
        self.p = platform
        self.data = data
        self.pos = 0
        self.offsets = [0] * BLOCK_COUNT
        self.stack: List[int] = []
        self.temp_saved: List[int] = []
        self.index = {b: ([], []) for b in (BLOCK_VIRTUAL, BLOCK_LARGE, BLOCK_PHYSICAL)}
        self.slots: Dict[int, Ptr] = {}  # zone address of pointer slots -> slot
        self.frames: List[_Frame] = []
        self.parents: List[Node] = []
        self.delayed: List[Node] = []

    # -- raw stream ---------------------------------------------------------

    def read(self, size: int) -> bytes:
        if self.pos + size > len(self.data):
            raise ZoneError(f"read past end of zone ({self.pos:#x} + {size:#x})")
        b = self.data[self.pos : self.pos + size]
        self.pos += size
        return b

    def u32(self) -> int:
        v = self.p.u32.unpack_from(self.data, self.pos)[0]
        self.pos += 4
        return v

    # -- blocks -------------------------------------------------------------

    @property
    def block(self) -> int:
        return self.stack[-1]

    def push(self, block: int):
        self.stack.append(block)
        if block == BLOCK_TEMP:
            self.temp_saved.append(self.offsets[BLOCK_TEMP])

    def pop(self):
        block = self.stack.pop()
        if block == BLOCK_TEMP:
            self.offsets[BLOCK_TEMP] = self.temp_saved.pop()

    def align(self, block: int, alignment: int):
        if alignment > 1:
            self.offsets[block] = (self.offsets[block] + alignment - 1) & ~(alignment - 1)

    trace = None  # optional callable(node, stream position) for debugging

    def new_node(self, type_: TypeRef, count: int, alignment: int) -> Node:
        block = self.block
        if self.trace is not None:
            self.trace(type_, count, block, self.pos)
        self.align(block, alignment)
        node = Node(type_, count, block)
        node.offset = self.offsets[block]
        node.extra["align"] = alignment
        if self.parents:
            self.parents[-1].children.append(node)
        if block in self.index:
            starts, nodes = self.index[block]
            if starts and starts[-1] > node.offset:
                raise ZoneError("block allocations out of order")
            starts.append(node.offset)
            nodes.append(node)
        return node

    def load_into(self, node: Node, size: int, seg_type: Optional[TypeRef] = None, seg_count: int = 1, partial: bool = False):
        """Stream ``size`` bytes into ``node`` at the current block position."""
        block = self.block
        if seg_type is None:
            seg_type = TypeRef("scalar", "uchar", 1, 1)
            seg_count = size
        node.segments.append((seg_type, seg_count, size, partial))
        if self.offsets[block] != node.offset + len(node.data) + node.runtime_size or (node.children and size):
            raise ZoneError(f"non contiguous load into {node!r}")
        if block in self.p.streamed_blocks:
            node.data += self.read(size)
        else:
            node.runtime_size += size
        self.offsets[block] += size

    def finish_node(self, node: Node):
        pass

    def insert_slot(self) -> int:
        self.align(BLOCK_VIRTUAL, 4)
        addr = zone_addr(BLOCK_VIRTUAL, self.offsets[BLOCK_VIRTUAL])
        self.offsets[BLOCK_VIRTUAL] += 4
        return addr

    # -- pointers -----------------------------------------------------------

    def ptr_value(self, node: Node, offset: int) -> int:
        return self.p.u32.unpack_from(node.data, offset)[0]

    def set_ptr(self, node: Node, offset: int, ptr: Ptr):
        ptr.owner = node
        ptr.offset = offset
        node.relocs[offset] = ptr
        if node.block in self.index:
            ptr.addr = zone_addr(node.block, node.offset + offset)
            self.slots[ptr.addr] = (ptr, 0)

    def resolve_ref(self, raw: int) -> Ptr:
        addr = raw - 1
        block = addr >> BLOCK_SHIFT
        offset = addr & OFFSET_MASK
        if block not in self.index:
            raise ZoneError(f"reference into non referencable block {block}")
        starts, nodes = self.index[block]
        i = bisect.bisect_right(starts, offset) - 1
        # Several (empty) allocations can share a start offset; prefer the one containing the offset.
        candidates = []
        j = i
        while j >= 0 and (j == i or nodes[j].offset + len(nodes[j].data) >= offset):
            candidates.append(nodes[j])
            j -= 1
        for n in candidates:
            if n.offset <= offset < n.offset + len(n.data):
                inner = offset - n.offset
                return Ptr("ref", n, inner // n.elem_size, inner % n.elem_size)
        for n in candidates:
            if offset == n.offset + len(n.data):
                inner = offset - n.offset
                return Ptr("ref", n, inner // n.elem_size, inner % n.elem_size)
        raise ZoneError(f"dangling reference {raw:#x} (block {block} offset {offset:#x})")

    def resolve_alias(self, raw: int) -> Ptr:
        addr = raw - 1
        entry = self.slots.get(addr)
        if entry is None:
            raise ZoneError(f"alias to unknown pointer slot {raw:#x}")
        # index 1: the alias refers to the insert slot reserved for ``slot``, 0: to the pointer itself
        return Ptr("alias", slot=entry[0], index=entry[1])

    # -- strings ------------------------------------------------------------

    def load_xstring(self, node: Node, offset: int):
        raw = self.ptr_value(node, offset)
        if raw == 0:
            self.set_ptr(node, offset, Ptr("null"))
            return
        if raw == FOLLOW:
            child = self.new_node(TypeRef("scalar", "char", 1, 1), 1, 1)
            child.string = True
            end = self.data.index(b"\0", self.pos) + 1
            self.load_into(child, end - self.pos, TypeRef("scalar", "char", 1, 1), end - self.pos)
            child.count = len(child.data)
            self.set_ptr(node, offset, Ptr("follow", child))
        else:
            self.set_ptr(node, offset, self.resolve_ref(raw))

    # -- entry point --------------------------------------------------------

    def load(self) -> Zone:
        p = self.p
        header = [self.u32() for _ in range(9)]
        size, external_size, block_sizes = header[0], header[1], header[2:]

        list_node = Node(TypeRef("scalar", "uint", 4, 4), 4, -1)
        list_node.data += self.read(16)
        string_count, strings_ptr, asset_count, assets_ptr = struct.unpack(p.endian + "4I", list_node.data)

        self.push(BLOCK_VIRTUAL)
        self.parents.append(list_node)

        script_strings: List[Optional[str]] = []
        script_node = None
        if strings_ptr:
            if strings_ptr != FOLLOW:
                raise ZoneError("script string list must follow")
            script_node = self.new_node(TypeRef("pointer", "", 4, 4, to=TypeRef("scalar", "char", 1, 1)), string_count, 4)
            self.load_into(script_node, 4 * string_count, script_node.type, string_count)
            self.parents.append(script_node)
            for i in range(string_count):
                self.load_xstring(script_node, 4 * i)
                target = script_node.relocs[4 * i].target()
                script_strings.append(None if target is None else bytes(target.data[:-1]).decode("latin-1"))
            self.parents.pop()

        assets: List[ZoneAsset] = []
        assets_node = None
        if assets_ptr:
            if assets_ptr != FOLLOW:
                raise ZoneError("asset list must follow")
            assets_node = self.new_node(TypeRef("scalar", "uint", 4, 4), 2 * asset_count, 4)
            self.load_into(assets_node, 8 * asset_count, assets_node.type, 2 * asset_count)
            self.parents.append(assets_node)
            for i in range(asset_count):
                type_index = p.u32.unpack_from(assets_node.data, 8 * i)[0]
                if type_index >= len(p.asset_types):
                    raise ZoneError(f"asset {i}: invalid type {type_index}")
                type_name = p.asset_types[type_index]
                rec_name = ASSET_RECORDS.get(type_name)
                if rec_name is None:
                    raise ZoneError(f"asset {i}: unsupported type {type_name}")
                try:
                    self.load_asset_ptr(assets_node, 8 * i + 4, rec_name)
                except ZoneError as e:
                    raise ZoneError(f"asset {i} ({type_name}): {e}") from e
                ptr = assets_node.relocs[8 * i + 4]
                asset = ZoneAsset(type_name, ptr)
                node = ptr.target()
                if node is not None:
                    node.asset = type_name
                    asset.name = asset_name(p, node)
                assets.append(asset)
            self.parents.pop()

        self.parents.pop()
        self.pop()

        for node in self.delayed:
            self.push(node.block)
            self.align(node.block, node.extra["align"])
            node.offset = self.offsets[node.block]
            node.data += self.read(node.count * node.type.size)
            node.segments.append((node.type, node.count, len(node.data), False))
            self.offsets[node.block] += len(node.data)
            self.pop()

        zone = Zone(p.name, script_strings, assets, block_sizes, size, external_size, script_node, assets_node)
        zone.extra_root = list_node
        if self.pos != len(self.data):
            raise ZoneError(f"{len(self.data) - self.pos} unread bytes at end of zone")
        return zone

    # -- structures ---------------------------------------------------------

    def load_asset_ptr(self, node: Node, offset: int, rec_name: str):
        """LoadPtr_<asset>: handles following, insert and alias pointers."""
        p = self.p
        raw = self.ptr_value(node, offset)
        if raw == 0:
            self.set_ptr(node, offset, Ptr("null"))
            return

        rec = p.record(rec_name)
        in_temp = p.type_block(rec_name) == BLOCK_TEMP
        if in_temp:
            self.push(BLOCK_TEMP)

        if raw == FOLLOW or (in_temp and raw == INSERT):
            child = self.new_node(TypeRef("record", rec_name, rec.size, rec.align), 1, p.type_alloc_align(rec_name, rec.align))
            child.extra["origin"] = ("asset", rec_name)
            if in_temp:
                child.push_before = BLOCK_TEMP
            ptr = Ptr("follow" if raw == FOLLOW else "insert", child)
            child.extra["ptr"] = ptr
            if raw == INSERT:
                child.insert = True
                ptr.insert_addr = self.insert_slot()
                self.slots[ptr.insert_addr] = (ptr, 1)
            self.load_struct(child, rec, stream_start=True)
            self.set_ptr(node, offset, ptr)
        elif in_temp:
            self.set_ptr(node, offset, self.resolve_alias(raw))
        else:
            self.set_ptr(node, offset, self.resolve_ref(raw))

        if in_temp:
            self.pop()

    def load_struct(self, node: Node, rec: Record, stream_start: bool, base: int = 0):
        """Load_<rec>: stream the structure (when at stream start) and process its members."""
        p = self.p
        if stream_start:
            dyn = p.dynamic_member(rec.name)
            size = rec.size if dyn is None else dyn.offset
            self.load_into(node, size, TypeRef("record", rec.name, rec.size, rec.align), 1, dyn is not None)

        pushed = None
        if p.is_asset(rec.name):
            pushed = BLOCK_VIRTUAL
        elif p.type_block(rec.name) is not None:
            pushed = p.type_block(rec.name)
        if pushed is not None:
            self.push(pushed)
            if node.push_after is None and base == 0:
                node.push_after = pushed

        self.parents.append(node)
        self.frames.append(_Frame(rec, node, base))
        try:
            self.load_members(rec, node, base, stream_start)
        finally:
            self.frames.pop()
            self.parents.pop()

        if pushed is not None:
            self.pop()

    def pick_info(self, rec: Record, f: Field) -> Optional[MemberInfo]:
        infos = self.p.member_infos(rec.name, f.name)
        if not infos:
            return None
        if len(infos) == 1:
            return next(iter(infos.values()))
        for frame in reversed(self.frames):
            if frame.rec.name in infos:
                return infos[frame.rec.name]
        return next(iter(infos.values()))

    def eval(self, expr, info: MemberInfo, rec: Record):
        ctx = _EvalContext(self.p, self.frames, info.context if info is not None else rec.name)
        return expr.eval(ctx)

    def load_members(self, rec: Record, node: Node, base: int, after_partial: bool):
        p = self.p
        dyn = p.dynamic_member(rec.name) if after_partial else None

        if rec.is_union:
            used = []
            for f in p.ordered_members(rec):
                info = self.pick_info(rec, f)
                if info is not None and info.condition is NEVER:
                    continue
                if dyn is None and p.member_is_leaf(rec, f):
                    continue
                used.append((f, info))
            for f, info in used:
                if info is not None and info.condition is not None and not self.eval(info.condition, info, rec):
                    continue
                self.load_member(rec, f, info, node, base, dyn is not None)
                break
            return

        for f in p.ordered_members(rec):
            info = self.pick_info(rec, f)
            if info is not None and info.condition is NEVER:
                continue
            is_after_partial = dyn is not None and f.offset >= dyn.offset
            if not is_after_partial and p.member_is_leaf(rec, f):
                continue
            if info is not None and info.condition is not None and not self.eval(info.condition, info, rec):
                continue
            self.load_member(rec, f, info, node, base, is_after_partial)

    def load_member(self, rec: Record, f: Field, info: Optional[MemberInfo], node: Node, base: int, after_partial: bool):
        p = self.p
        t = f.type
        offset = base + f.offset

        if info is not None and info.arraysize is not None:
            count = self.eval(info.arraysize, info, rec)
            elem = t.elem if t.kind == "array" else t
            start = len(node.data)
            if start != offset:
                raise ZoneError(f"dynamic member {rec.name}::{f.name} not at end of data")
            self.load_into(node, count * elem.size, elem, count)
            if elem.kind == "record" and not p.record_is_leaf(elem.name):
                erec = p.record(elem.name)
                for i in range(count):
                    self.load_struct(node, erec, False, offset + i * elem.size)
            return

        if info is not None and info.string:
            self.load_string_member(node, offset, t, info, rec)
            return

        if t.kind == "record":
            sub = p.record(t.name)
            if after_partial:
                # the embedded member is (or contains) the dynamic member: stream it now
                dyn = p.dynamic_member(sub.name)
                size = sub.size if dyn is None else dyn.offset
                if len(node.data) != offset:
                    raise ZoneError(f"partial member {rec.name}::{f.name} not at end of data")
                self.load_into(node, size, t, 1, dyn is not None)
            self.frames.append(_Frame(sub, node, offset))
            try:
                self.load_members(sub, node, offset, after_partial)
            finally:
                self.frames.pop()
            return

        if t.kind == "array":
            if after_partial:
                self.load_into(node, t.size, t.elem, t.count)
            self.load_embedded_array(rec, f, info, node, offset, t, ())
            return

        if t.kind == "pointer":
            if after_partial:
                self.load_into(node, 4, t, 1)
            self.load_pointer(rec, f, info, node, offset, t.to, ())
            return

        if after_partial:
            self.load_into(node, t.size, t, 1)

    def load_embedded_array(self, rec, f, info, node, offset, t: TypeRef, index: Tuple[int, ...]):
        p = self.p
        elem = t.elem
        for i in range(t.count):
            eoff = offset + i * elem.size
            if elem.kind == "array":
                self.load_embedded_array(rec, f, info, node, eoff, elem, index + (i,))
            elif elem.kind == "pointer":
                self.load_pointer(rec, f, info, node, eoff, elem.to, index + (i,))
            elif elem.kind == "record" and not p.record_is_leaf(elem.name):
                sub = p.record(elem.name)
                self.frames.append(_Frame(sub, node, eoff))
                try:
                    self.load_members(sub, node, eoff, False)
                finally:
                    self.frames.pop()

    def load_string_member(self, node: Node, offset: int, t: TypeRef, info: MemberInfo, rec: Record):
        if t.kind == "array":
            for i in range(t.count):
                self.load_xstring(node, offset + 4 * i)
            return
        if t.kind != "pointer":
            raise ZoneError("string member is not a pointer")
        if t.to.kind == "pointer":
            # const char** with a count: pointer array of strings
            raw = self.ptr_value(node, offset)
            if raw == 0:
                self.set_ptr(node, offset, Ptr("null"))
                return
            if raw != FOLLOW:
                self.set_ptr(node, offset, self.resolve_ref(raw))
                return
            count = self.eval(info.count, info, rec) if info.count is not None else 1
            child = self.new_node(t.to, count, 4)
            self.load_into(child, 4 * count, t.to, count)
            self.set_ptr(node, offset, Ptr("follow", child))
            self.parents.append(child)
            for i in range(count):
                self.load_xstring(child, 4 * i)
            self.parents.pop()
            return
        self.load_xstring(node, offset)

    def load_pointer(self, rec: Record, f: Field, info: Optional[MemberInfo], node: Node, offset: int, pointee: TypeRef, index):
        p = self.p
        raw = self.ptr_value(node, offset)
        if raw == 0:
            self.set_ptr(node, offset, Ptr("null"))
            return

        if pointee.kind == "record" and p.is_asset(pointee.name):
            self.load_asset_ptr(node, offset, pointee.name)
            return

        count_expr = None
        if info is not None:
            count_expr = info.index_counts.get(index) if index else None
            if count_expr is None:
                count_expr = info.count
        count = self.eval(count_expr, info, rec) if count_expr is not None else 1

        if info is not None and info.delayed is not None:
            # Streamed after all assets (console image pixels): allocate a placeholder now.
            block_name, alignment = info.delayed
            child = Node(pointee, count, BLOCK_BY_NAME[block_name])
            child.extra["align"] = alignment
            child.extra["delayed"] = True
            child.extra["origin"] = ("member", rec.name, f.name)
            if self.parents:
                self.parents[-1].children.append(child)
            self.delayed.append(child)
            self.set_ptr(node, offset, Ptr("follow", child))
            return

        member_block = BLOCK_BY_NAME[info.block] if info is not None and info.block else None
        # OAT pushes every block but the default normal one. Console zones also need an explicit
        # VIRTUAL push from inside the temp block (360 texture headers), which OAT cannot express.
        pushed = member_block is not None and (member_block != BLOCK_VIRTUAL or self.block != BLOCK_VIRTUAL)
        if pushed:
            self.push(member_block)

        reusable = info is not None and info.reusable
        in_temp = member_block == BLOCK_TEMP
        try:
            if reusable and in_temp and raw not in (FOLLOW, INSERT):
                self.set_ptr(node, offset, self.resolve_alias(raw))
                return
            if reusable and not in_temp and raw != FOLLOW:
                self.set_ptr(node, offset, self.resolve_ref(raw))
                return

            if pointee.kind == "pointer":
                self.load_pointer_array(node, offset, pointee, count, reusable, raw)
                node.relocs[offset].node.extra["origin"] = ("ptrarray", rec.name, f.name)
                return

            alignment = pointee.align
            if info is not None and info.allocalign is not None:
                alignment = info.allocalign.eval(None)
            elif pointee.kind == "record":
                alignment = p.type_alloc_align(pointee.name, pointee.align)

            child = self.new_node(pointee, count, alignment)
            child.extra["origin"] = ("member", rec.name, f.name)
            if pushed:
                child.push_before = member_block
            ptr = Ptr("insert" if (in_temp and raw == INSERT) else "follow", child)
            child.extra["ptr"] = ptr
            if ptr.kind == "insert":
                child.insert = True
                ptr.insert_addr = self.insert_slot()
                self.slots[ptr.insert_addr] = (ptr, 1)
            self.set_ptr(node, offset, ptr)

            runtime = self.block not in p.streamed_blocks
            if pointee.kind == "record" and not p.record_is_leaf(pointee.name) and not runtime:
                prec = p.record(pointee.name)
                if count == 1 and p.dynamic_member(prec.name) is not None:
                    self.load_struct(child, prec, stream_start=True)
                else:
                    self.load_into(child, count * pointee.size, pointee, count)
                    for i in range(count):
                        self.load_struct(child, prec, False, i * pointee.size)
            else:
                self.load_into(child, count * pointee.size, pointee, count)
        finally:
            if pushed:
                self.pop()

    def load_pointer_array(self, node: Node, offset: int, pointee: TypeRef, count: int, reusable: bool, raw: int):
        p = self.p
        child = self.new_node(pointee, count, 4)
        self.set_ptr(node, offset, Ptr("follow", child))
        self.load_into(child, 4 * count, pointee, count)
        target = pointee.to
        self.parents.append(child)
        try:
            for i in range(count):
                eraw = self.ptr_value(child, 4 * i)
                if eraw == 0:
                    self.set_ptr(child, 4 * i, Ptr("null"))
                    continue
                if target.kind == "record" and p.is_asset(target.name):
                    self.load_asset_ptr(child, 4 * i, target.name)
                    continue
                if reusable and eraw != FOLLOW:
                    self.set_ptr(child, 4 * i, self.resolve_ref(eraw))
                    continue
                alignment = p.type_alloc_align(target.name, target.align) if target.kind == "record" else target.align
                elem = self.new_node(target, 1, alignment)
                elem.extra["origin"] = ("ptrelem", target.name if target.kind == "record" else "")
                self.set_ptr(child, 4 * i, Ptr("follow", elem))
                if target.kind == "record" and not p.record_is_leaf(target.name):
                    self.load_struct(elem, p.record(target.name), stream_start=True)
                else:
                    self.load_into(elem, target.size, target, 1)
        finally:
            self.parents.pop()


def asset_name(p: Platform, node: Node) -> str:
    """Best effort name of an asset header node."""
    rec = p.record(node.type.name)
    for candidate in ("name", "szInternalName", "aliasName", "fontName"):
        f = find_field(rec, candidate)
        if f is not None and f.type.kind == "pointer":
            ptr = node.relocs.get(f.offset)
            target = ptr.target() if ptr is not None else None
            if target is not None and target.string:
                return bytes(target.data[:-1]).decode("latin-1")
    for f in rec.fields:
        if f.type.kind == "record":
            sub = p.record(f.type.name)
            nf = find_field(sub, "name")
            if nf is not None and nf.type.kind == "pointer":
                ptr = node.relocs.get(f.offset + nf.offset)
                target = ptr.target() if ptr is not None else None
                if target is not None and target.string:
                    return bytes(target.data[:-1]).decode("latin-1")
    return ""


# ---------------------------------------------------------------------------
# Writer


class Writer:
    """Serialises a :class:`Zone` for a platform (block layout, alignment and pointer encoding)."""

    def __init__(self, platform: Platform):
        self.p = platform
        self.out = bytearray()
        self.offsets = [0] * BLOCK_COUNT
        self.temp_max = 0
        self.u32 = platform.u32
        self.delayed: List[Node] = []

    def align(self, block: int, alignment: int):
        if alignment > 1:
            self.offsets[block] = (self.offsets[block] + alignment - 1) & ~(alignment - 1)

    def pointer_value(self, ptr: Ptr) -> int:
        kind = ptr.kind
        if kind == "null":
            return 0
        if kind == "follow":
            return FOLLOW
        if kind == "insert":
            return INSERT
        if kind == "ref":
            n = ptr.node
            if n.extra.get("new_offset") is None:
                raise ZoneError(f"reference to unwritten node {n!r}")
            return zone_addr(n.block, n.extra["new_offset"] + ptr.index * n.elem_size + ptr.inner) + 1
        if kind == "alias":
            slot = ptr.slot
            addr = slot.insert_addr if ptr.index == 1 else slot.addr
            if addr is None:
                raise ZoneError(f"alias to unwritten slot {slot!r}")
            return addr + 1
        raise ZoneError(kind)

    def emit(self, node: Node):
        if node.extra.get("delayed"):
            self.delayed.append(node)
            return
        block = node.block
        saved_temp = None
        if node.push_before == BLOCK_TEMP:
            saved_temp = self.offsets[BLOCK_TEMP]

        self.align(block, node.extra.get("align", 1))
        node.extra["new_offset"] = self.offsets[block]

        if node.insert:
            ptr = node.extra["ptr"]
            self.align(BLOCK_VIRTUAL, 4)
            ptr.insert_addr = zone_addr(BLOCK_VIRTUAL, self.offsets[BLOCK_VIRTUAL])
            self.offsets[BLOCK_VIRTUAL] += 4

        data = node.data
        if node.relocs:
            normal = block in (BLOCK_VIRTUAL, BLOCK_LARGE, BLOCK_PHYSICAL)
            for off, ptr in node.relocs.items():
                ptr.addr = zone_addr(block, node.extra["new_offset"] + off) if normal else None

        start = len(self.out)
        streamed = block in self.p.streamed_blocks
        if streamed:
            self.out += data
        self.offsets[block] += len(data) + node.runtime_size
        if block == BLOCK_TEMP:
            self.temp_max = max(self.temp_max, self.offsets[BLOCK_TEMP])

        for child in node.children:
            self.emit(child)

        # Pointers are encoded once the children are written: a pointer can reference data that
        # is loaded while processing this node's members (e.g. techniques sharing technique 0).
        if streamed and node.relocs:
            for off, ptr in node.relocs.items():
                self.u32.pack_into(self.out, start + off, self.pointer_value(ptr))

        if saved_temp is not None:
            self.offsets[BLOCK_TEMP] = saved_temp

    def write(self, zone: Zone) -> bytes:
        p = self.p
        root: Node = zone.extra_root
        # the asset list header: counts are refreshed from the zone contents
        string_count = zone.script_node.count if zone.script_node is not None else 0
        asset_count = zone.assets_node.count // 2 if zone.assets_node is not None else 0
        header = struct.pack(
            p.endian + "4I",
            string_count,
            FOLLOW if zone.script_node is not None else 0,
            asset_count,
            FOLLOW if zone.assets_node is not None else 0,
        )
        self.out += header
        for child in root.children:
            self.emit(child)

        # Delayed data (360 image pixels) follows the assets and is not part of the zone size.
        size = len(self.out)
        for node in self.delayed:
            self.align(node.block, node.extra.get("align", 1))
            node.extra["new_offset"] = self.offsets[node.block]
            self.out += node.data
            self.offsets[node.block] += len(node.data)

        block_sizes = list(self.offsets)
        # The PC linker reserves room for the 16 byte asset list header in the temp block.
        block_sizes[BLOCK_TEMP] = self.temp_max + self.p.temp_padding
        head = struct.pack(p.endian + "9I", size, zone.external_size, *block_sizes)
        return head + bytes(self.out)
