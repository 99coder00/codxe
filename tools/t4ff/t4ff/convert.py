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

from . import progress
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

# Asset types a map uses by name reference when the game's own zones have them.
GAME_REFERENCE_TYPES = ("lightdef",)


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


def pack_dec3n(v: np.ndarray) -> np.ndarray:
    """(n, 3) floats in [-1, 1] -> signed 10:10:10 packed unsigned ints."""
    q = np.clip(np.floor(np.asarray(v, dtype=np.float64) * 511.0 + 0.5), -511, 511).astype(np.int64) & 0x3FF
    return (q[:, 0] | (q[:, 1] << 10) | (q[:, 2] << 20)).astype(np.uint32)


def _pack_unit_vec(src: np.ndarray) -> np.ndarray:
    """PC PackedUnitVec (3 biased bytes and a scale byte) -> console 10:10:10 signed normalized.

    The vector is renormalized before packing (bit identical to CoD Xenon's world vertices).
    """
    b = src.astype(np.float64)
    v = (b[:, :3] - 127.0) * ((b[:, 3:4] + 192.0) / 32385.0)
    length = np.linalg.norm(v, axis=1, keepdims=True)
    v = np.divide(v, length, out=np.zeros_like(v), where=length > 0)
    packed = pack_dec3n(v).astype(">u4")
    return packed.view(np.uint8).reshape(-1, 4)


# Records whose console encoding differs from a field by field copy: name -> converter of
# (count, src size) uint8 arrays to (count, dst size) uint8 arrays.
RECORD_CONVERTERS: Dict[str, Callable[[np.ndarray], np.ndarray]] = {
    "PackedUnitVec": _pack_unit_vec,
}


def _field_offset(platform, rec: str, path: str) -> int:
    from .commands import find_field

    offset = 0
    for i, part in enumerate(path.split(".")):
        f = find_field(platform.record(rec), part)
        offset += f.offset
        rec = f.type.name
    return offset


def _fix_gfx_surface(src: np.ndarray, dst: np.ndarray):
    """The console keeps a copy of the surface bounds right after the triangle info."""
    from .platforms import x360

    p = x360()
    b = _field_offset(p, "GfxSurface", "bounds")
    c = _field_offset(p, "GfxSurface", "boundsCopy")
    dst[:, c : c + 24] = dst[:, b : b + 24]


def _fix_static_model_inst(src: np.ndarray, dst: np.ndarray):
    """PC placement (origin, 3x3 axis, scale) -> console origin, DEC3N packed axis, scale."""
    from .platforms import pc, x360

    ps, pd = pc(), x360()
    so = _field_offset(ps, "GfxStaticModelDrawInst", "placement")
    n = len(src)
    placement = np.ascontiguousarray(src[:, so : so + 52]).view("<f4").reshape(n, 13)
    o = _field_offset(pd, "GfxStaticModelDrawInst", "origin")
    dst[:, o : o + 12] = placement[:, 0:3].astype(">f4").view(np.uint8).reshape(n, 12)
    a = _field_offset(pd, "GfxStaticModelDrawInst", "axis")
    axis = np.stack([pack_dec3n(placement[:, 3 + 3 * i : 6 + 3 * i]) for i in range(3)], axis=1)
    dst[:, a : a + 12] = axis.astype(">u4").view(np.uint8).reshape(n, 12)
    sc = _field_offset(pd, "GfxStaticModelDrawInst", "scale")
    dst[:, sc : sc + 4] = placement[:, 12:13].astype(">f4").view(np.uint8).reshape(n, 4)
    # the flags are byte flags (0x80 on PC is 0x80000000 on console)
    fs = _field_offset(ps, "GfxStaticModelDrawInst", "flags")
    fd = _field_offset(pd, "GfxStaticModelDrawInst", "flags")
    dst[:, fd : fd + 4] = src[:, fs : fs + 4]


def _fix_aabb_tree(src: np.ndarray, dst: np.ndarray):
    """Leaf trees have no children offset on console (the PC linker leaves a stale value)."""
    from .platforms import x360

    p = x360()
    cc = _field_offset(p, "GfxAabbTree", "childCount")
    co = _field_offset(p, "GfxAabbTree", "childrenOffset")
    leaf = (dst[:, cc] == 0) & (dst[:, cc + 1] == 0)
    dst[leaf, co : co + 4] = 0


# Records that need values computed after the field by field mapping: name -> fixup(src rows, dst rows)
RECORD_FIXUPS: Dict[str, Callable[[np.ndarray, np.ndarray], None]] = {
    "GfxSurface": _fix_gfx_surface,
    "GfxStaticModelDrawInst": _fix_static_model_inst,
    "GfxAabbTree": _fix_aabb_tree,
}


# PC asset types stored under another type on console. Single player console maps use the PVS
# clip map type (as in CoD Xenon's converted maps).
X360_ASSET_TYPE = {"clipmap": "clipmap_pvs"}


# Runtime fields left zeroed on console (the PC linker leaves garbage in them).
ZERO_FIELDS = {
    ("XSurface", "zoneHandle"),
    ("GfxSurface", "pad"),
}


def _scalar_bytes(t: TypeRef, records) -> int:
    """Bytes of non pointer scalar data in a type."""
    if t.kind in ("scalar", "enum"):
        return t.size
    if t.kind == "array":
        return t.count * _scalar_bytes(t.elem, records)
    if t.kind == "record" and t.name in records:
        rec = records[t.name]
        sizes = [_scalar_bytes(f.type, records) for f in rec.fields if f.name]
        return (max(sizes) if rec.is_union else sum(sizes)) if sizes else 0
    return 0


def _elem_size(t: TypeRef) -> int:
    while t.kind == "array":
        t = t.elem
    return t.size


# Byte arrays that the console stores as one 32-bit value (byte order reversed).
SWAP32_FIELDS = {
    ("FxElemVisualState", "color"),
}


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
        self.custom: List[Tuple[int, int, int, Callable]] = []  # (src, dst, src size, converter)
        self.fixups: List[Tuple[int, int, int, int, Callable]] = []  # (src, dst, src size, dst size, fixup)
        self.offsets: Dict[int, int] = {}  # src offset -> dst offset of every mapped field
        self.pointers: Dict[int, int] = {}  # src offset -> dst offset of pointer fields kept on console
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
        if name in RECORD_FIXUPS and not partial:
            self.fixups.append((0, 0, src_rec.size, dst_rec.size, RECORD_FIXUPS[name]))
        self._merge()

    # -- building ------------------------------------------------------------

    def _map_record(self, src: Record, dst: Record, so: int, do: int, src_limit=None, dst_limit=None):
        records_src = self.conv.src.layout.records
        records_dst = self.conv.dst.layout.records

        if dst.is_union:
            # Pointer members all live at the union offset.
            self.offsets.setdefault(so, do)
            if any(f.type.kind == "pointer" for f in dst.fields) and any(f.type.kind == "pointer" for f in src.fields):
                self.pointers.setdefault(so, do)
            candidates = [f for f in dst.fields if f.name and src.field(f.name) is not None]
            if not candidates:
                return
            # Map the member carrying the most plain data (e.g. a leaf count over child pointers):
            # pointer slots are rewritten by the zone writer anyway. Pointers of every member are kept.
            chosen = max(candidates, key=lambda f: (f.type.kind != "pointer", _scalar_bytes(f.type, records_dst), _elem_size(f.type), f.type.size))
            for f in candidates:
                self._collect_pointers(src.field(f.name).type, f.type, so, do, records_src, records_dst)
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
            if (dst.name, f.name) in ZERO_FIELDS:
                continue
            if (dst.name, f.name) in SWAP32_FIELDS and sf.type.size == f.type.size and f.type.size % 4 == 0:
                self.offsets.setdefault(so + sf.offset, do + f.offset)
                self.ops.append(_ScalarOp(so + sf.offset, "<u4", do + f.offset, ">u4", f.type.size // 4))
                continue
            if f.bit_width:
                key = f.offset
                if key in seen_storage:
                    continue
                seen_storage.add(key)
            self._map_field(sf.type, f.type, so + sf.offset, do + f.offset, records_src, records_dst)

    def _collect_pointers(self, st: TypeRef, dt: TypeRef, so: int, do: int, rs, rd):
        if dt.kind == "pointer" and st.kind == "pointer":
            self.pointers.setdefault(so, do)
        elif dt.kind == "array" and st.kind == "array":
            for i in range(min(st.count, dt.count)):
                self._collect_pointers(st.elem, dt.elem, so + i * st.elem.size, do + i * dt.elem.size, rs, rd)
        elif dt.kind == "record" and st.kind == "record":
            src, dst = rs[st.name], rd[dt.name]
            for f in dst.fields:
                sf = src.field(f.name) if f.name else None
                if sf is not None:
                    self._collect_pointers(sf.type, f.type, so + sf.offset, do + f.offset, rs, rd)

    def _map_field(self, st: TypeRef, dt: TypeRef, so: int, do: int, rs, rd):
        self.offsets.setdefault(so, do)
        if dt.kind == "record":
            if dt.name in RECORD_CONVERTERS and st.kind == "record" and st.name == dt.name:
                self.custom.append((so, do, st.size, RECORD_CONVERTERS[dt.name]))
                return
            if st.kind in ("pointer", "scalar"):
                # a PC runtime object pointer (e.g. IDirect3DVertexBuffer9*) or placeholder that is an
                # embedded structure on console: left zeroed here, filled by a fixup when needed
                return
            if st.kind != "record":
                raise ConvertError(f"{self.name}: type mismatch {st!r} -> {dt!r}")
            self._map_record(rs[st.name], rd[dt.name], so, do)
            if dt.name in RECORD_FIXUPS:
                self.fixups.append((so, do, st.size, dt.size, RECORD_FIXUPS[dt.name]))
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
            if st.kind == "pointer":
                self.pointers[so] = do
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
        for so, do, size, fn in self.custom:
            out = fn(np.ascontiguousarray(src[:, so : so + size]))
            dst[:, do : do + out.shape[1]] = out
        for so, do, ssize, dsize, fn in self.fixups:
            fn(src[:, so : so + ssize], dst[:, do : do + dsize])
        return dst.tobytes()

    def keeps_pointer(self, off: int) -> bool:
        return off in self.pointers

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

    def keeps_pointer(self, off: int) -> bool:
        return True


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
    compress_textures: bool = True
    # the map's own .iwd files / folders (images/<name>.iwi of its textures)
    iwd_paths: List[str] = field(default_factory=list)
    # the PC game's own files (--iwd, e.g. its main folder): their images are stock textures, used
    # only when no console version of them is at hand (the game's zones, the console library)
    stock_paths: List[str] = field(default_factory=list)
    reference_missing_images: bool = True
    # sounds: xma2encode wrapper (audio.XmaEncoder) for loaded sounds, their rate cap / downmix,
    # and the output folder holding the converted streamed sounds (sounds/<dir>/<name>.xma)
    xma_encoder: Optional[object] = None
    sound_rate: int = 0
    mono_sounds: bool = False
    sounds_dir: Optional[str] = None
    # Xbox 360 fastfiles (stock or converted) that console only assets are copied from
    console_zones: List[str] = field(default_factory=list)
    # the map being converted: a console fastfile of the same name among them is read first
    map_name: str = ""
    log: Callable[[str], None] = print
    # parallel jobs for sound encoding (0: one per processor)
    jobs: int = 0
    # technique sets only referenced by name (zones unloaded while the game runs)
    reference_techsets: bool = False
    # encoded loaded sounds (or the error encoding them), shared by the zones converted together
    sound_cache: Dict[Tuple[str, int], object] = field(default_factory=dict, repr=False, compare=False)


@dataclass
class ConvertStats:
    converted: Dict[str, int] = field(default_factory=dict)
    referenced: Dict[str, int] = field(default_factory=dict)
    copied: Dict[str, int] = field(default_factory=dict)  # from the console library
    texture_bytes: int = 0
    sound_bytes: int = 0
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
        self.stock_library = None
        if self.options.iwd_paths or self.options.stock_paths:
            from .images import IwdLibrary

            self.library = IwdLibrary(self.options.iwd_paths) if self.options.iwd_paths else None
            self.stock_library = IwdLibrary(self.options.stock_paths) if self.options.stock_paths else None
        # console assets copied from other Xbox 360 fastfiles (shared by the zones of one run)
        self.script_strings = list(zone.script_strings)
        self.console_library = None
        self.cloner = None
        if self.options.console_zones:
            from .library import Cloner, ConsoleLibrary

            self.console_library = _shared_library(dst, tuple(self.options.console_zones), self.options.log, self.options.map_name)
            self.cloner = Cloner(dst, self.script_strings, self._replace_library_asset)
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
            # keep typedef alignment (e.g. UShortVec: unsigned short[3] aligned to 4)
            return TypeRef("array", t.name, elem.size * t.count, max(elem.align, t.align), elem=elem, count=t.count)
        if t.kind == "pointer":
            return TypeRef("pointer", "", 4, 4, to=self.dst_type(t.to) if t.to is not None and t.to.kind == "record" else t.to)
        return t

    # -- verification ---------------------------------------------------------

    def unverified_records(self, node: Node) -> Set[str]:
        """Record types used by ``node``'s subtree that were not verified on console."""
        missing = set()
        stack = [node]
        while stack:
            n = stack.pop()
            # nested assets are converted (or referenced) on their own; members the console
            # structure does not have are dropped
            for child in n.children:
                origin = child.extra.get("origin") or ("",)
                if origin[0] == "asset":
                    continue
                if origin[0] == "member" and not self._dst_has_member(origin[1], origin[2]):
                    continue
                stack.append(child)
            for t, _, _, _ in n.segments:
                while t.kind == "array":
                    t = t.elem
                if t.kind == "record" and t.name not in self.verified:
                    missing.add(t.name)
        return missing

    def _dst_has_member(self, rec: str, member: str) -> bool:
        from .commands import find_field

        if rec not in self.dst.layout.records:
            return True
        return find_field(self.dst.record(rec), member) is not None

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
                if info.delayed is not None and node.extra.get("delayed"):
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

        def keeps_pointer(off: int, segments=segments) -> bool:
            for s0, s1, d0, m, sst, dst_stride in segments:
                if s0 <= off < s1:
                    return m.keeps_pointer((off - s0) % sst if sst else 0)
            return True

        self.node_map[id(node)] = new
        self.offset_maps[id(node)] = translate

        dropped = set()
        for off, ptr in node.relocs.items():
            if not keeps_pointer(off):
                # the console structure has no such pointer (e.g. model collision triangles)
                if ptr.kind in ("follow", "insert") and ptr.node is not None:
                    dropped.add(id(ptr.node))
                continue
            new_off = translate(off)
            new.relocs[new_off] = ptr
            self.ptrs.append((ptr, new))
            ptr.offset = new_off

        for child in node.children:
            if id(child) not in dropped:
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
        reduced = asset_type == "image" and self.image_drop_levels.get(name.lstrip(","), 0)
        stock_techset = asset_type == "techset" and self.options.reference_techsets
        if name.startswith(",") and not reduced and not stock_techset:
            # the PC zone expects this asset from another zone: the console library may have it
            copy = self.from_library(asset_type, name, node)
            if copy is not None:
                self.node_map[id(node)] = copy
                self.offset_maps[id(node)] = lambda off: off
                return copy
        if asset_type in GAME_REFERENCE_TYPES and not name.startswith(",") and self.console_library is not None:
            if self.console_library.in_game_zones(ASSET_RECORDS[asset_type], name):
                # the game's own version, as CoD Xenon's DerBerg does: the PC map's copy of a stock
                # light definition has the PC linker's lookup index, not the console's
                return self.reference_asset(asset_type, node, name)
        hook = self.hooks.get(asset_type)
        if hook is not None:
            result = hook(self, asset_type, node, name)
            if result is not None:
                self.node_map[id(node)] = result
                self.offset_maps.setdefault(id(node), lambda off: off)
                if not result.extra.get("library") and not any(
                    isinstance(p, Ptr) and p.kind == "follow" and p.node.string and bytes(p.node.data).startswith(b",")
                    for p in result.relocs.values()
                ):
                    self.stats.count(self.stats.converted, asset_type)
                    self.register_converted(asset_type, name, node)
                return result

        missing = self.unverified_records(node)
        if missing and not self.options.allow_unverified and not name.startswith(","):
            self.warn(f"{asset_type} '{name}': console layout of {', '.join(sorted(missing))} not verified, emitting a reference")
            return self.reference_asset(asset_type, node, name)

        self.stats.count(self.stats.converted, asset_type)
        self.register_converted(asset_type, name, node)
        return self.convert_node(node)

    def _replace_library_asset(self, rec_name: str, name: str, library_node: Node) -> Optional[Node]:
        """Nested assets of library copies: textures reduced by the budget are rebuilt."""
        if rec_name == "GfxImage" and self.image_drop_levels.get(name):
            from .assets import rebuild_console_image

            return rebuild_console_image(self, name, library_node)
        return None

    def register_converted(self, asset_type: str, name: str, node: Node):
        """Library copies use converted assets of the same name instead of copying them again."""
        if self.cloner is None or not name or name.startswith(",") or asset_type not in ASSET_RECORDS:
            return
        loader = node.extra.get("ptr")
        if loader is None or loader.kind not in ("follow", "insert"):
            return
        owner = loader.owner
        if loader.kind == "follow" and (owner is None or owner.block not in (BLOCK_VIRTUAL, 5, 6)):
            return  # the pointer slot is not addressable
        self.cloner.register_asset(ASSET_RECORDS[asset_type], name, loader)

    def from_library(self, asset_type: str, name: str, src_node: Optional[Node] = None) -> Optional[Node]:
        """A copy of the console asset ``name`` from the console library, if it has one."""
        if self.console_library is None or asset_type not in ASSET_RECORDS:
            return None
        if self.console_library.in_game_zones(ASSET_RECORDS[asset_type], name):
            return None  # the game's own zones load it: a reference does (and costs no memory)
        found = self.console_library.find(ASSET_RECORDS[asset_type], name)
        if found is None:
            return None
        from .library import LibraryError

        try:
            copy = self.cloner.copy_asset(*found)
        except LibraryError as e:
            self.warn(f"{asset_type} '{name.lstrip(',')}': cannot be copied from the console library ({e})")
            return None
        self.stats.count(self.stats.copied, asset_type)
        copy.extra["library"] = True
        if asset_type == "image":
            self.stats.texture_bytes += sum(len(n.data) for n in copy.walk() if n.extra.get("delayed"))
        elif asset_type == "loaded_sound":
            self.stats.sound_bytes += sum(len(n.data) for n in copy.walk() if (n.extra.get("origin") or ("", "", ""))[1:] == ("snd_asset", "data"))
        loader = src_node.extra.get("ptr") if src_node is not None else None
        if loader is not None and loader.kind in ("follow", "insert"):
            # later copies that use this asset alias the pointer that loads it
            self.cloner.register_asset(ASSET_RECORDS[asset_type], name, loader)
        if src_node is not None:
            from .assets import map_name_string

            map_name_string(self, src_node, asset_type, copy)
        return copy

    def reference_asset(self, asset_type: str, node: Node, name: str) -> Node:
        """Replace an asset by a name only reference (resolved by the game at load time)."""
        from . import assets

        copy = self.from_library(asset_type, name, node)
        if copy is not None:
            self.node_map[id(node)] = copy
            self.offset_maps[id(node)] = lambda off: off
            return copy
        self.stats.count(self.stats.referenced, asset_type)
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

        if not getattr(self, "textures_planned", False):
            from .assets import plan_textures

            plan_textures(self, root)

        from .assets import encode_loaded_sounds

        encode_loaded_sounds(self)

        for child in root.children:
            if child is assets_node:
                new_root.children.append(self.convert_assets_node(child))
            else:
                new_root.children.append(self.convert_node(child))

        self.fix_pointers()
        from .assets import apply_string_edits

        apply_string_edits(self, new_root)

        new_zone = Zone(
            self.dst.name,
            self.script_strings,
            [],
            [],
            0,
            0,
            self.node_map.get(id(script_node)) if script_node is not None else None,
            self.node_map.get(id(assets_node)) if assets_node is not None else None,
        )
        new_zone.extra_root = new_root
        new_zone.assets = [ZoneAsset(X360_ASSET_TYPE.get(a.type, a.type), a.ptr, a.name) for a in zone.assets]
        return new_zone

    def convert_assets_node(self, node: Node) -> Node:
        new = Node(node.type, node.count, node.block)
        new.extra["align"] = 4
        new.extra["origin"] = None
        new.segments = [(TypeRef("scalar", "uint", 4, 4), node.count, len(node.data), False)]
        data = bytearray(len(node.data))
        for i, asset in enumerate(self.zone.assets):
            type_index = self.dst.asset_type_index[X360_ASSET_TYPE.get(asset.type, asset.type)]
            self.dst.u32.pack_into(data, 8 * i, type_index)
        new.data = data
        self.node_map[id(node)] = new
        self.offset_maps[id(node)] = lambda off: off
        for off, ptr in node.relocs.items():
            new.relocs[off] = ptr
            self.ptrs.append((ptr, new))
        label = getattr(self, "progress_label", None) or "Converting assets"
        for index, child in enumerate(node.children):
            progress.step(label, index, len(node.children))
            new.children.append(self.convert_child(child))
        progress.step(label, len(node.children), len(node.children))
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

    def keeps_pointer(self, off: int) -> bool:
        return self.elem.keeps_pointer(off % self.elem.src_size)


_LIBRARIES: Dict[tuple, object] = {}


def _shared_library(platform: Platform, paths: tuple, log, first: str = ""):
    from .library import ConsoleLibrary

    key = (platform.name, paths, first)
    if key not in _LIBRARIES:
        _LIBRARIES[key] = ConsoleLibrary(platform, list(paths), log, first)
    return _LIBRARIES[key]


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
