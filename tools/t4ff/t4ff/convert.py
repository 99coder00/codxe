"""PC -> Xbox 360 zone conversion.

The generic path converts every node of the PC zone tree into the 360 layout of
the same record: fields are matched by name, scalars are byte swapped (and
resized when needed), arrays are truncated or zero extended and pointers are
carried over. Asset types whose console format is known to differ get a
dedicated hook (images, materials, technique sets, ...).

Only record layouts that were verified against real console fastfiles are
converted by default. Assets that would need unverified layouts are replaced by
name references (``,name``) so the game resolves them against its own zones,
unless ``allow_unverified`` is set.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Set, Tuple

import numpy as np

from .commands import NEVER
from .layout import Record, SCALARS, TypeRef
from .zone import (
    ASSET_RECORDS,
    BLOCK_BY_NAME,
    BLOCK_TEMP,
    BLOCK_VIRTUAL,
    Node,
    Platform,
    Ptr,
    Zone,
    ZoneAsset,
    ZoneError,
)

DEFS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "defs")


class ConvertError(Exception):
    pass


# ---------------------------------------------------------------------------
# Scalar and record mapping


def _np_type(t: TypeRef, endian: str) -> Optional[str]:
    """numpy dtype string for a scalar type, None for raw bytes."""
    if t.kind == "enum":
        return endian + {1: "u1", 2: "u2", 4: "i4"}[t.size]
    if t.kind == "pointer":
        return endian + "u4"
    if t.kind != "scalar":
        return None
    code = SCALARS[t.name][0]
    if t.size == 1:
        return None  # single bytes are copied verbatim
    return endian + {"h": "i2", "H": "u2", "i": "i4", "I": "u4", "q": "i8", "Q": "u8", "f": "f4", "d": "f8"}[code]


@dataclass
class _ScalarOp:
    src: int
    src_type: str
    dst: int
    dst_type: str
    count: int


class RecordMap:
    """Maps the bytes of one record from the source layout to the destination layout."""

    def __init__(self, conv: "ZoneConverter", name: str, partial: bool):
        self.name = name
        src_rec = conv.src.record(name)
        dst_rec = conv.dst.record(name)
        self.src_size = src_rec.size
        self.dst_size = dst_rec.size
        self.ops: List[_ScalarOp] = []
        self.raw: List[Tuple[int, int, int]] = []  # (src, dst, size) verbatim copies
        self.offsets: Dict[int, int] = {}  # src offset -> dst offset of every mapped field
        self.conv = conv

        src_limit = dst_limit = None
        if partial:
            sdyn = conv.src.dynamic_member(name)
            ddyn = conv.dst.dynamic_member(name)
            src_limit = sdyn.offset if sdyn is not None else src_rec.size
            dst_limit = ddyn.offset if ddyn is not None else dst_rec.size
            self.src_size = src_limit
            self.dst_size = dst_limit

        self._map_record(src_rec, dst_rec, 0, 0, src_limit, dst_limit)
        self._merge()

    # -- building ------------------------------------------------------------

    def _map_record(self, src: Record, dst: Record, so: int, do: int, src_limit=None, dst_limit=None):
        records_src = self.conv.src.layout.records
        records_dst = self.conv.dst.layout.records

        if dst.is_union:
            # Pointer members all live at the union offset.
            self.offsets.setdefault(so, do)
            candidates = [f for f in dst.fields if f.name and src.field(f.name) is not None]
            if not candidates:
                return
            chosen = max(candidates, key=lambda f: (f.type.kind != "pointer", f.type.size))
            self._map_field(src.field(chosen.name).type, chosen.type, so, do, records_src, records_dst)
            return

        seen_storage = set()
        for f in dst.fields:
            if not f.name:
                continue
            sf = src.field(f.name)
            if sf is None:
                continue
            if src_limit is not None and sf.offset >= src_limit:
                continue
            if dst_limit is not None and f.offset >= dst_limit:
                continue
            if f.bit_width:
                key = f.offset
                if key in seen_storage:
                    continue
                seen_storage.add(key)
            self._map_field(sf.type, f.type, so + sf.offset, do + f.offset, records_src, records_dst)

    def _map_field(self, st: TypeRef, dt: TypeRef, so: int, do: int, rs, rd):
        self.offsets.setdefault(so, do)
        if dt.kind == "record":
            if st.kind != "record":
                raise ConvertError(f"{self.name}: type mismatch {st!r} -> {dt!r}")
            self._map_record(rs[st.name], rd[dt.name], so, do)
            return
        if dt.kind == "array":
            if st.kind != "array":
                # e.g. int -> int[4]: map into the first element
                self._map_field(st, dt.elem, so, do, rs, rd)
                return
            n = min(st.count, dt.count)
            se, de = st.elem, dt.elem
            if de.kind in ("scalar", "enum") and se.kind in ("scalar", "enum"):
                s_np = _np_type(se, "<")
                d_np = _np_type(de, ">")
                if s_np is None or d_np is None:
                    size = min(se.size, de.size)
                    if se.size == de.size:
                        self.raw.append((so, do, n * se.size))
                    else:
                        for i in range(n):
                            self.raw.append((so + i * se.size, do + i * de.size, size))
                else:
                    self.ops.append(_ScalarOp(so, s_np, do, d_np, n))
                for i in range(n):
                    self.offsets.setdefault(so + i * se.size, do + i * de.size)
                return
            for i in range(n):
                self._map_field(se, de, so + i * se.size, do + i * de.size, rs, rd)
            return
        if dt.kind == "pointer":
            return  # pointer values are written by the zone writer
        if dt.kind in ("scalar", "enum"):
            if st.kind not in ("scalar", "enum", "pointer"):
                raise ConvertError(f"{self.name}: type mismatch {st!r} -> {dt!r}")
            s_np = _np_type(st, "<")
            d_np = _np_type(dt, ">")
            if s_np is None and d_np is None:
                self.raw.append((so, do, 1))
            else:
                self.ops.append(_ScalarOp(so, s_np or "<u1", do, d_np or ">u1", 1))
            return
        if dt.kind == "void":
            return
        raise ConvertError(f"{self.name}: unsupported field type {dt!r}")

    def _merge(self):
        """Merge adjacent scalar runs of the same type."""
        ops = sorted(self.ops, key=lambda o: o.src)
        merged: List[_ScalarOp] = []
        for op in ops:
            if merged:
                last = merged[-1]
                ssz = np.dtype(last.src_type).itemsize
                dsz = np.dtype(last.dst_type).itemsize
                if (
                    last.src_type == op.src_type
                    and last.dst_type == op.dst_type
                    and last.src + last.count * ssz == op.src
                    and last.dst + last.count * dsz == op.dst
                ):
                    last.count += op.count
                    continue
            merged.append(_ScalarOp(op.src, op.src_type, op.dst, op.dst_type, op.count))
        self.ops = merged

    # -- conversion ----------------------------------------------------------

    def convert(self, data: bytes, count: int, src_stride: Optional[int] = None) -> bytes:
        src_stride = src_stride or self.src_size
        if count == 0:
            return b""
        src = np.frombuffer(bytes(data[: count * src_stride]), dtype=np.uint8).reshape(count, src_stride)
        dst = np.zeros((count, self.dst_size), dtype=np.uint8)
        for op in self.ops:
            sdt = np.dtype(op.src_type)
            ddt = np.dtype(op.dst_type)
            chunk = np.ascontiguousarray(src[:, op.src : op.src + sdt.itemsize * op.count]).view(sdt)
            dst[:, op.dst : op.dst + ddt.itemsize * op.count] = chunk.astype(ddt).view(np.uint8).reshape(count, -1)
        for so, do, size in self.raw:
            dst[:, do : do + size] = src[:, so : so + size]
        return dst.tobytes()

    def map_offset(self, off: int) -> int:
        if off in self.offsets:
            return self.offsets[off]
        # inside a field: find the closest preceding mapped offset
        best = max((o for o in self.offsets if o <= off), default=None)
        if best is None:
            raise ConvertError(f"{self.name}: cannot map offset {off}")
        return self.offsets[best] + (off - best)


class ScalarMap:
    """Maps runs of a scalar type (or verbatim bytes)."""

    def __init__(self, t: TypeRef):
        self.src_type = _np_type(t, "<")
        self.dst_type = _np_type(t, ">")
        self.size = max(t.size, 1)
        self.src_size = self.dst_size = self.size

    def convert(self, data: bytes, count: int, src_stride=None) -> bytes:
        if self.src_type is None:
            return bytes(data[: count * self.size])
        return np.frombuffer(bytes(data[: count * self.size]), dtype=self.src_type).astype(self.dst_type).tobytes()

    def map_offset(self, off: int) -> int:
        return off


# ---------------------------------------------------------------------------
# Options and statistics


@dataclass
class ConvertOptions:
    allow_unverified: bool = False
    # images: maximum width/height, 0 for no limit
    max_texture_size: int = 0
    # total texture memory budget in bytes, 0 for no limit
    texture_budget: int = 0
    keep_mips: bool = True
    iwd_paths: List[str] = field(default_factory=list)
    reference_missing_images: bool = True
    log: Callable[[str], None] = print


@dataclass
class ConvertStats:
    converted: Dict[str, int] = field(default_factory=dict)
    referenced: Dict[str, int] = field(default_factory=dict)
    texture_bytes: int = 0
    warnings: List[str] = field(default_factory=list)

    def count(self, table: Dict[str, int], key: str):
        table[key] = table.get(key, 0) + 1


def load_verified_records() -> Set[str]:
    path = os.path.join(DEFS_DIR, "x360_verified.txt")
    if not os.path.exists(path):
        return set()
    with open(path, "r", encoding="utf-8") as f:
        return {line.strip() for line in f if line.strip() and not line.startswith("#")}


# ---------------------------------------------------------------------------
# Zone conversion


class ZoneConverter:
    def __init__(self, zone: Zone, src: Platform, dst: Platform, options: Optional[ConvertOptions] = None):
        self.zone = zone
        self.src = src
        self.dst = dst
        self.options = options or ConvertOptions()
        self.stats = ConvertStats()
        self.verified = load_verified_records()
        self._maps: Dict[Tuple[str, bool], RecordMap] = {}
        self.node_map: Dict[int, Node] = {}  # id(src node) -> dst node
        self.offset_maps: Dict[int, Callable[[int], int]] = {}  # id(src node) -> offset translator
        self.ptrs: List[Tuple[Ptr, Node]] = []  # pointers to fix up (ptr, owning dst node)
        self.hooks: Dict[str, Callable] = {}
        self.image_drop_levels: Dict[str, int] = {}
        self.library = None
        if self.options.iwd_paths:
            from .images import IwdLibrary

            self.library = IwdLibrary(self.options.iwd_paths)
        from . import assets

        assets.register_hooks(self)

    def log(self, msg: str):
        self.options.log(msg)

    def warn(self, msg: str):
        self.stats.warnings.append(msg)
        self.log("warning: " + msg)

    # -- maps ---------------------------------------------------------------

    def record_map(self, name: str, partial: bool = False) -> RecordMap:
        key = (name, partial)
        if key not in self._maps:
            self._maps[key] = RecordMap(self, name, partial)
        return self._maps[key]

    def type_map(self, t: TypeRef, partial: bool = False):
        if t.kind == "record":
            return self.record_map(t.name, partial)
        if t.kind == "array":
            return _ArrayMap(self, t)
        if t.kind == "pointer":
            return ScalarMap(TypeRef("scalar", "uint", 4, 4))
        return ScalarMap(t)

    def dst_type(self, t: TypeRef) -> TypeRef:
        if t.kind == "record":
            rec = self.dst.record(t.name)
            return TypeRef("record", t.name, rec.size, rec.align)
        if t.kind == "array":
            elem = self.dst_type(t.elem)
            return TypeRef("array", "", elem.size * t.count, elem.align, elem=elem, count=t.count)
        if t.kind == "pointer":
            return TypeRef("pointer", "", 4, 4, to=self.dst_type(t.to) if t.to is not None and t.to.kind == "record" else t.to)
        return t

    # -- verification ---------------------------------------------------------

    def unverified_records(self, node: Node) -> Set[str]:
        """Record types used by ``node``'s subtree that were not verified on console."""
        missing = set()
        for n in node.walk():
            for t, _, _, _ in n.segments:
                while t.kind == "array":
                    t = t.elem
                if t.kind == "record" and t.name not in self.verified:
                    missing.add(t.name)
        return missing

    # -- nodes --------------------------------------------------------------

    def dst_alignment(self, node: Node, dst_type: TypeRef) -> int:
        origin = node.extra.get("origin")
        if node.string:
            return 1
        if origin is None:
            return node.extra.get("align", 1)
        kind = origin[0]
        if kind == "asset":
            rec = self.dst.record(origin[1])
            return self.dst.type_alloc_align(origin[1], rec.align)
        if kind == "ptrarray":
            return 4
        if kind == "ptrelem":
            if origin[1]:
                rec = self.dst.record(origin[1])
                return self.dst.type_alloc_align(origin[1], rec.align)
            return max(dst_type.align, 1)
        if kind == "member":
            infos = self.dst.member_infos(origin[1], origin[2])
            for info in infos.values():
                if info.delayed is not None:
                    return info.delayed[1]
                if info.allocalign is not None:
                    return info.allocalign.eval(None)
            if dst_type.kind == "record":
                return self.dst.type_alloc_align(dst_type.name, dst_type.align)
            return max(dst_type.align, 1)
        return node.extra.get("align", 1)

    def convert_node(self, node: Node) -> Node:
        dst_type = self.dst_type(node.type)
        new = Node(dst_type, node.count, node.block)
        new.push_before = node.push_before
        new.push_after = node.push_after
        new.insert = node.insert
        new.string = node.string
        new.asset = node.asset
        new.runtime_size = node.runtime_size
        new.extra["origin"] = node.extra.get("origin")
        if node.extra.get("delayed"):
            new.extra["delayed"] = True
        new.extra["align"] = self.dst_alignment(node, dst_type)

        # data, segment by segment
        segments = []  # (src start, src end, dst start, map, src stride, dst stride)
        src_pos = 0
        out = bytearray()
        for t, count, size, partial in node.segments:
            if node.runtime_size and not node.data:
                break
            m = self.type_map(t, partial)
            src_stride = m.src_size if count else 0
            if count and src_stride * count != size:
                src_stride = size // count
            chunk = m.convert(node.data[src_pos : src_pos + size], count, src_stride)
            segments.append((src_pos, src_pos + size, len(out), m, src_stride, (len(chunk) // count) if count else 0))
            out += chunk
            new.segments.append((self.dst_type(t), count, len(chunk), partial))
            src_pos += size
        new.data = out
        if node.runtime_size:
            new.runtime_size = self._runtime_size(node)

        def translate(off: int, segments=segments) -> int:
            for s0, s1, d0, m, sst, dst_stride in segments:
                if s0 <= off < s1 or (off == s1 and s1 == s0):
                    rel = off - s0
                    if sst == 0:
                        return d0
                    i, inner = divmod(rel, sst)
                    return d0 + i * dst_stride + m.map_offset(inner)
            if segments and off == segments[-1][1]:
                return len(out)
            if not segments:
                return off
            raise ConvertError(f"offset {off} outside of node {node!r}")

        self.node_map[id(node)] = new
        self.offset_maps[id(node)] = translate

        for off, ptr in node.relocs.items():
            new_off = translate(off)
            new.relocs[new_off] = ptr
            self.ptrs.append((ptr, new))
            ptr.offset = new_off

        for child in node.children:
            new.children.append(self.convert_child(child))
        return new

    def _runtime_size(self, node: Node) -> int:
        # runtime (non streamed) allocations: scale by the element size of the console type
        t = node.type
        dst = self.dst_type(t)
        if t.size and dst.size:
            return node.runtime_size // t.size * dst.size
        return node.runtime_size

    def convert_child(self, child: Node) -> Node:
        if child.asset is not None or (child.type.kind == "record" and child.extra.get("origin", ("",))[0] == "asset"):
            asset_type = child.asset or _asset_type_of(child.type.name)
            return self.convert_asset_node(asset_type, child)
        return self.convert_node(child)

    # -- assets -------------------------------------------------------------

    def convert_asset_node(self, asset_type: str, node: Node) -> Node:
        name = asset_display_name(self.src, node)
        hook = self.hooks.get(asset_type)
        if hook is not None:
            result = hook(self, asset_type, node, name)
            if result is not None:
                self.node_map[id(node)] = result
                self.offset_maps.setdefault(id(node), lambda off: off)
                if asset_type not in ("techset",) and not any(
                    isinstance(p, Ptr) and p.kind == "follow" and p.node.string and bytes(p.node.data).startswith(b",")
                    for p in result.relocs.values()
                ):
                    self.stats.count(self.stats.converted, asset_type)
                return result

        missing = self.unverified_records(node)
        if missing and not self.options.allow_unverified and not name.startswith(","):
            self.warn(f"{asset_type} '{name}': console layout of {', '.join(sorted(missing))} not verified, emitting a reference")
            self.stats.count(self.stats.referenced, asset_type)
            return self.reference_asset(asset_type, node, name)

        self.stats.count(self.stats.converted, asset_type)
        return self.convert_node(node)

    def reference_asset(self, asset_type: str, node: Node, name: str) -> Node:
        """Replace an asset by a name only reference (resolved by the game at load time)."""
        from . import assets

        ref_name = name if name.startswith(",") else "," + name
        new = assets.build_reference(self, asset_type, node, ref_name)
        self.node_map[id(node)] = new
        self.offset_maps[id(node)] = lambda off: off if off < len(new.data) else 0
        return new

    # -- zone ---------------------------------------------------------------

    def convert(self) -> Zone:
        zone = self.zone
        root = zone.extra_root
        new_root = Node(root.type, root.count, -1)
        new_root.data = bytearray(16)

        script_node = zone.script_node
        assets_node = zone.assets_node

        from .assets import plan_textures

        plan_textures(self, root)

        for child in root.children:
            if child is assets_node:
                new_root.children.append(self.convert_assets_node(child))
            else:
                new_root.children.append(self.convert_node(child))

        self.fix_pointers()

        new_zone = Zone(
            self.dst.name,
            list(zone.script_strings),
            [],
            [],
            0,
            0,
            self.node_map.get(id(script_node)) if script_node is not None else None,
            self.node_map.get(id(assets_node)) if assets_node is not None else None,
        )
        new_zone.extra_root = new_root
        new_zone.assets = [ZoneAsset(a.type, a.ptr, a.name) for a in zone.assets]
        return new_zone

    def convert_assets_node(self, node: Node) -> Node:
        new = Node(node.type, node.count, node.block)
        new.extra["align"] = 4
        new.extra["origin"] = None
        new.segments = [(TypeRef("scalar", "uint", 4, 4), node.count, len(node.data), False)]
        data = bytearray(len(node.data))
        for i, asset in enumerate(self.zone.assets):
            type_index = self.dst.asset_type_index[asset.type]
            self.dst.u32.pack_into(data, 8 * i, type_index)
        new.data = data
        self.node_map[id(node)] = new
        self.offset_maps[id(node)] = lambda off: off
        for off, ptr in node.relocs.items():
            new.relocs[off] = ptr
            self.ptrs.append((ptr, new))
        for child in node.children:
            new.children.append(self.convert_child(child))
        return new

    def fix_pointers(self):
        for ptr, owner in self.ptrs:
            ptr.owner = owner
            if ptr.kind in ("follow", "insert"):
                target = self.node_map.get(id(ptr.node))
                if target is None:
                    raise ConvertError(f"pointer to unconverted node {ptr.node!r}")
                ptr.node = target
                if ptr.kind == "insert":
                    target.extra["ptr"] = ptr
            elif ptr.kind == "ref":
                src_target = ptr.node
                target = self.node_map.get(id(src_target))
                if target is None:
                    raise ConvertError(f"reference to unconverted node {src_target!r}")
                src_off = ptr.index * src_target.elem_size + ptr.inner
                ptr.node = target
                ptr.index = 0
                ptr.inner = self.offset_maps[id(src_target)](src_off)
        # follow/insert pointers of converted asset hooks that were built directly
        for ptr, owner in self.ptrs:
            if ptr.kind == "insert" and ptr.node is not None:
                ptr.node.insert = True
                ptr.node.extra["ptr"] = ptr


class _ArrayMap:
    """Maps an embedded array type (used for after-partial array segments)."""

    def __init__(self, conv: ZoneConverter, t: TypeRef):
        self.elem = conv.type_map(t.elem)
        self.count = t.count
        self.src_size = self.elem.src_size * t.count
        self.dst_size = self.elem.dst_size * t.count

    def convert(self, data: bytes, count: int, src_stride=None) -> bytes:
        return self.elem.convert(data, count * self.count)

    def map_offset(self, off: int) -> int:
        i, inner = divmod(off, self.elem.src_size)
        return i * self.elem.dst_size + self.elem.map_offset(inner)


def _asset_type_of(rec_name: str) -> str:
    for t, r in ASSET_RECORDS.items():
        if r == rec_name:
            return t
    raise ConvertError(f"{rec_name} is not an asset")


def asset_display_name(p: Platform, node: Node) -> str:
    from .zone import asset_name

    try:
        return asset_name(p, node)
    except Exception:
        return ""
