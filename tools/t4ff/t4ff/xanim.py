"""PC -> Xbox 360 animation (XAnimParts) data conversion.

PC animations have 10 part types, console animations 12 (7 rotation, 4
translation, all):

    PC                     console
    0 NO_QUAT              0 NO_QUAT
    1 HALF_QUAT            1 HALF_QUAT             frames: 1 packed short
    2 FULL_QUAT            2 FULL_QUAT             frames: 1 packed int (randomDataInt)
                           3 FULL_QUAT_PRECISE     frames: 3 packed shorts
    3 HALF_QUAT_NO_SIZE    4 HALF_QUAT_NO_SIZE     1 packed short
    4 FULL_QUAT_NO_SIZE    5 FULL_QUAT_NO_SIZE     1 packed int (dataInt)
                           6 FULL_QUAT_NO_SIZE_PRECISE  3 packed shorts
    5 SMALL_TRANS          7 SMALL_TRANS
    6 TRANS                8 TRANS
    7 TRANS_NO_SIZE        9 TRANS_NO_SIZE
    8 NO_TRANS             10 NO_TRANS
    9 ALL                  11 ALL

Console quaternions use a "smallest components" packing: the sign and index of
the largest component, then the other components divided by it, in cyclic order
after it. Viewmodel animations (assetType 1) and the main skeleton bones keep the
precise 48-bit packing, every other full quaternion uses 32 bits.

Bones are sorted by their console part type (stable), which renumbers the bone
indices of the translation tracks; translation data is unchanged.

The data layout follows OpenAssetTools' FlatXAnimReader. Verified against the
602 animations of CoD Xenon's converted nazi_zombie_aztec: identical structure,
and 99.8% of the quaternions bit identical (the rest differ by one unit of the
last place).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np

PC_QUAT_TYPES = ("none", "half", "full", "halfns", "fullns")
PC_TRANS_TYPES = ("small", "full", "nosize", "none")
X360_QUAT_TYPES = ("none", "half", "fullc", "fullp", "halfns", "fullnsc", "fullnsp")
X360_TRANS_TYPES = PC_TRANS_TYPES
PC_PART_TYPE_COUNT = 10
X360_PART_TYPE_COUNT = 12

ASSET_TYPE_VIEWMODEL = 1

# Bones whose full quaternions keep the precise 48-bit packing in non viewmodel animations.
PRECISE_BONES = frozenset(
    (
        "j_mainroot",
        "j_hip_le",
        "j_hip_ri",
        "j_knee_le",
        "j_knee_ri",
        "j_ankle_le",
        "j_ankle_ri",
        "j_spinelower",
        "j_spineupper",
        "j_spine4",
        "j_clavicle_le",
        "j_clavicle_ri",
        "j_shoulder_le",
        "j_shoulder_ri",
        "j_elbow_le",
        "j_elbow_ri",
        "j_wrist_le",
        "j_wrist_ri",
    )
)


class XAnimError(Exception):
    pass


# ---------------------------------------------------------------------------
# Quaternion packing


def pack_quats(quats: np.ndarray, widths: Sequence[int]) -> np.ndarray:
    """Pack (n, 4) int16 quaternions (x, y, z, w) into console integers.

    ``widths`` are the bit widths of the three stored components, low bits first:
    (9, 10, 10) for 32-bit and (15, 15, 15) for 48-bit quaternions. Above them are the
    index of the largest component (3 - component) and its sign.
    """
    q = np.asarray(quats, dtype=np.int64).reshape(-1, 4)
    n = len(q)
    rows = np.arange(n)
    comp = np.argmax(np.abs(q), axis=1)
    largest = q[rows, comp].astype(np.float64)
    largest = np.where(largest == 0, 1.0, largest)
    out = np.zeros(n, dtype=np.uint64)
    shift = 0
    for k, w in enumerate(widths):
        other = q[rows, (comp + 1 + k) % 4].astype(np.float64)
        scale = (1 << (w - 1)) - 1
        v = np.clip(np.floor(other / largest * scale + 0.5), -(1 << (w - 1)), (1 << (w - 1)) - 1).astype(np.int64)
        out |= (v & ((1 << w) - 1)).astype(np.uint64) << np.uint64(shift)
        shift += w
    out |= (3 - comp).astype(np.uint64) << np.uint64(shift)
    out |= (q[rows, comp] < 0).astype(np.uint64) << np.uint64(shift + 2)
    out[~q.any(axis=1)] = 0
    return out


def pack_quats32(quats: np.ndarray) -> np.ndarray:
    return pack_quats(quats, (9, 10, 10)).astype(np.uint32)


def pack_quats48(quats: np.ndarray) -> np.ndarray:
    """(n, 3) uint16 words, most significant first."""
    v = pack_quats(quats, (15, 15, 15))
    return np.stack([(v >> np.uint64(32)) & np.uint64(0xFFFF), (v >> np.uint64(16)) & np.uint64(0xFFFF), v & np.uint64(0xFFFF)], axis=1).astype(np.uint16)


def pack_half_quats(quats: np.ndarray) -> np.ndarray:
    """Pack (n, 2) int16 (z, w) rotations into 16 bits: sign, z-is-largest, 14-bit ratio."""
    q = np.asarray(quats, dtype=np.int64).reshape(-1, 2)
    z, w = q[:, 0], q[:, 1]
    z_largest = np.abs(z) >= np.abs(w)
    largest = np.where(z_largest, z, w).astype(np.float64)
    other = np.where(z_largest, w, z).astype(np.float64)
    ratio = np.divide(other, largest, out=np.zeros_like(other), where=largest != 0)
    v = np.clip(np.floor(ratio * 8191 + 0.5), -8192, 8191).astype(np.int64)
    out = (v & 0x3FFF) | (z_largest.astype(np.int64) << 14) | ((largest < 0).astype(np.int64) << 15)
    out[(z == 0) & (w == 0)] = 0
    return out.astype(np.uint16)


# ---------------------------------------------------------------------------
# Flat data model


@dataclass
class Indices:
    """Frame indices of one track, re-emitted exactly as stored."""

    kind: str  # 'none', 'byte' (dataByte), 'short' (dataShort) or 'pool' (indices + dataShort checkpoints)
    values: List[int] = field(default_factory=list)
    checkpoints: List[int] = field(default_factory=list)


@dataclass
class QuatTrack:
    kind: str
    stored_size: int = 0  # frame count - 1, for tracks with frames
    indices: Indices = field(default_factory=lambda: Indices("none"))
    frames: Optional[np.ndarray] = None  # (n, 4) or (n, 2) int16


@dataclass
class TransTrack:
    kind: str
    bone: int
    stored_size: int = 0
    mins: List[int] = field(default_factory=list)  # raw float bits
    size: List[int] = field(default_factory=list)
    indices: Indices = field(default_factory=lambda: Indices("none"))
    frames: List[int] = field(default_factory=list)  # bytes or shorts
    constant: List[int] = field(default_factory=list)  # raw float bits


class _Cursor:
    def __init__(self, arrays: Dict[str, np.ndarray]):
        self.arrays = {k: v.tolist() for k, v in arrays.items()}
        self.pos = {k: 0 for k in arrays}

    def pop(self, key: str, n: int = 1) -> List[int]:
        i = self.pos[key]
        values = self.arrays[key][i : i + n]
        if len(values) < n:
            raise XAnimError(f"{key} exhausted (need {n} at {i} of {len(self.arrays[key])})")
        self.pos[key] = i + n
        return values

    def remaining(self) -> Dict[str, int]:
        return {k: len(v) - self.pos[k] for k, v in self.arrays.items() if len(v) - self.pos[k]}


def _read_indices(cur: _Cursor, stored: int, byte_indices: bool) -> Indices:
    n = stored + 1
    if byte_indices:
        return Indices("byte", cur.pop("dataByte", n))
    if stored >= 64:
        return Indices("pool", cur.pop("indices", n), cur.pop("dataShort", (n - 2) // 256 + 2))
    return Indices("short", cur.pop("dataShort", n))


def read_pc(bone_counts: Sequence[int], arrays: Dict[str, np.ndarray], numframes: int):
    """Decode the PC flat animation data. ``arrays`` holds dataByte, dataShort, ... as numpy arrays."""
    byte_indices = numframes < 256
    if byte_indices:
        # byte frame indices live in dataByte; the byte index pool is carried over as is
        arrays = {k: v for k, v in arrays.items() if k != "indices"}
    cur = _Cursor(arrays)
    quats: List[QuatTrack] = []
    for kind, count in zip(PC_QUAT_TYPES, bone_counts[:5]):
        for _ in range(count):
            track = QuatTrack(kind)
            if kind in ("half", "full"):
                track.stored_size = cur.pop("dataShort")[0] & 0xFFFF
                track.indices = _read_indices(cur, track.stored_size, byte_indices)
                width = 2 if kind == "half" else 4
                track.frames = np.array(cur.pop("randomDataShort", (track.stored_size + 1) * width), dtype=np.int16).reshape(-1, width)
            elif kind == "halfns":
                track.frames = np.array(cur.pop("dataShort", 2), dtype=np.int16).reshape(1, 2)
            elif kind == "fullns":
                track.frames = np.array(cur.pop("dataShort", 4), dtype=np.int16).reshape(1, 4)
            quats.append(track)

    trans: List[TransTrack] = []
    for kind, count in zip(PC_TRANS_TYPES, bone_counts[5:9]):
        for _ in range(count):
            track = TransTrack(kind, cur.pop("dataByte")[0])
            if kind in ("small", "full"):
                track.stored_size = cur.pop("dataShort")[0] & 0xFFFF
                track.mins = cur.pop("dataInt", 3)
                track.size = cur.pop("dataInt", 3)
                track.indices = _read_indices(cur, track.stored_size, byte_indices)
                n = (track.stored_size + 1) * 3
                track.frames = cur.pop("randomDataByte", n) if kind == "small" else cur.pop("randomDataShort", n)
            elif kind == "nosize":
                track.constant = cur.pop("dataInt", 3)
            trans.append(track)

    left = cur.remaining()
    if left:
        raise XAnimError(f"unread animation data: {left}")
    return quats, trans


# ---------------------------------------------------------------------------
# Console encoding


@dataclass
class ConsoleAnim:
    bone_order: List[int]  # console bone i = PC bone bone_order[i]
    bone_counts: List[int]  # 12 console part type counts
    data_byte: bytes
    data_short: np.ndarray  # int16 / uint16 values
    data_int: np.ndarray  # uint32 values
    random_data_short: np.ndarray
    random_data_byte: bytes
    random_data_int: np.ndarray
    indices: np.ndarray  # uint16 pool (short indices) or empty


def _console_kind(track: QuatTrack, precise: bool) -> str:
    if track.kind == "full":
        return "fullp" if precise else "fullc"
    if track.kind == "fullns":
        return "fullnsp" if precise else "fullnsc"
    return track.kind


class _Writer:
    def __init__(self):
        self.data_byte: List[int] = []
        self.data_short: List[int] = []
        self.data_int: List[int] = []
        self.random_short: List[int] = []
        self.random_byte: List[int] = []
        self.random_int: List[int] = []
        self.indices: List[int] = []

    def put_indices(self, idx: Indices):
        if idx.kind == "byte":
            self.data_byte += idx.values
        elif idx.kind == "short":
            self.data_short += idx.values
        elif idx.kind == "pool":
            self.indices += idx.values
            self.data_short += idx.checkpoints


def _u16(values: np.ndarray) -> List[int]:
    return np.asarray(values, dtype=np.int64).astype(np.uint16).astype(np.int16).tolist()


def to_console(bone_names: Sequence[str], quats: List[QuatTrack], trans: List[TransTrack], asset_type: int) -> ConsoleAnim:
    viewmodel = asset_type == ASSET_TYPE_VIEWMODEL
    kinds = [_console_kind(t, viewmodel or bone_names[i] in PRECISE_BONES) for i, t in enumerate(quats)]
    order = [i for kind in X360_QUAT_TYPES for i, k in enumerate(kinds) if k == kind]
    new_index = {old: new for new, old in enumerate(order)}

    w = _Writer()
    for i in order:
        track, kind = quats[i], kinds[i]
        if kind in ("half", "fullc", "fullp"):
            w.data_short.append(track.stored_size)
            w.put_indices(track.indices)
            if kind == "half":
                w.random_short += _u16(pack_half_quats(track.frames))
            elif kind == "fullc":
                w.random_int += pack_quats32(track.frames).tolist()
            else:
                w.random_short += _u16(pack_quats48(track.frames).reshape(-1))
        elif kind == "halfns":
            w.data_short += _u16(pack_half_quats(track.frames))
        elif kind == "fullnsc":
            w.data_int += pack_quats32(track.frames).tolist()
        elif kind == "fullnsp":
            w.data_short += _u16(pack_quats48(track.frames).reshape(-1))

    trans_counts = []
    for kind in X360_TRANS_TYPES:
        tracks = sorted((t for t in trans if t.kind == kind), key=lambda t: new_index[t.bone])
        trans_counts.append(len(tracks))
        for t in tracks:
            w.data_byte.append(new_index[t.bone])
            if kind in ("small", "full"):
                w.data_short.append(t.stored_size)
                w.data_int += t.mins + t.size
                w.put_indices(t.indices)
                if kind == "small":
                    w.random_byte += t.frames
                else:
                    w.random_short += t.frames
            elif kind == "nosize":
                w.data_int += t.constant

    counts = [kinds.count(k) for k in X360_QUAT_TYPES] + trans_counts + [len(quats)]
    return ConsoleAnim(
        bone_order=order,
        bone_counts=counts,
        data_byte=bytes(w.data_byte),
        data_short=np.array(w.data_short, dtype=np.int64).astype(np.uint16),
        data_int=np.array(w.data_int, dtype=np.int64).astype(np.uint32),
        random_data_short=np.array(w.random_short, dtype=np.int64).astype(np.uint16),
        random_data_byte=bytes(w.random_byte),
        random_data_int=np.array(w.random_int, dtype=np.int64).astype(np.uint32),
        indices=np.array(w.indices, dtype=np.int64).astype(np.uint16),
    )


# ---------------------------------------------------------------------------
# Zone nodes

# stream order of the XAnimParts members
MEMBER_ORDER = (
    "name",
    "names",
    "notify",
    "deltaPart",
    "dataByte",
    "dataShort",
    "dataInt",
    "randomDataShort",
    "randomDataByte",
    "randomDataInt",
    "indices",
)

_ARRAYS = {
    # member: (numpy dtype, scalar type name)
    "names": ("u2", "ushort"),
    "dataByte": ("u1", "uchar"),
    "dataShort": ("i2", "short"),
    "dataInt": ("i4", "int"),
    "randomDataShort": ("i2", "short"),
    "randomDataByte": ("u1", "uchar"),
    "randomDataInt": ("i4", "int"),
    "indices": ("u2", "ushort"),
}


_STRUCT_CODES = {1: "B", 2: "H", 4: "I"}


def _member_of(node) -> Optional[str]:
    origin = node.extra.get("origin")
    if not origin or origin[0] != "member":
        return None
    return "indices" if origin[1] == "XAnimIndices" else origin[2]


def convert_parts(conv, src, dst, name: str):
    """Rewrite the generically converted XAnimParts ``dst`` (and its data arrays) for the console."""
    from .commands import find_field
    from .layout import TypeRef
    from .zone import BLOCK_VIRTUAL, Node, Ptr

    sp, dp = conv.src, conv.dst
    srec, drec = sp.record("XAnimParts"), dp.record("XAnimParts")

    def get(fname):
        f = find_field(srec, fname)
        return struct.unpack_from(sp.endian + _STRUCT_CODES[f.type.size], src.data, f.offset)[0]

    def put(fname, value):
        f = find_field(drec, fname)
        struct.pack_into(dp.endian + _STRUCT_CODES[f.type.size], dst.data, f.offset, value)

    numframes = get("numframes")
    asset_type = get("assetType")
    bc = find_field(srec, "boneCount")
    bone_counts = list(src.data[bc.offset : bc.offset + PC_PART_TYPE_COUNT])

    src_children = {_member_of(c): c for c in src.children}
    arrays = {}
    for member in ("dataByte", "dataShort", "dataInt", "randomDataShort", "randomDataByte", "indices"):
        child = src_children.get(member)
        dtype = "<" + _ARRAYS[member][0]
        arrays[member] = np.frombuffer(bytes(child.data), dtype=dtype) if child is not None else np.zeros(0, dtype=dtype)
    names_child = src_children.get("names")
    name_ids = np.frombuffer(bytes(names_child.data), dtype="<u2").tolist() if names_child is not None else []
    strings = conv.zone.script_strings
    bone_names = [strings[i] if i < len(strings) and strings[i] is not None else "" for i in name_ids]
    if len(bone_names) != bone_counts[9]:
        raise XAnimError(f"xanim '{name}': {len(bone_names)} bone names for {bone_counts[9]} bones")

    quats, trans = read_pc(bone_counts, arrays, numframes)
    anim = to_console(bone_names, quats, trans, asset_type)

    dbc = find_field(drec, "boneCount")
    dst.data[dbc.offset : dbc.offset + X360_PART_TYPE_COUNT] = bytes(anim.bone_counts)
    put("dataByteCount", len(anim.data_byte))
    put("dataShortCount", len(anim.data_short))
    put("dataIntCount", len(anim.data_int))
    put("randomDataByteCount", len(anim.random_data_byte))
    put("randomDataIntCount", len(anim.random_data_int))
    put("randomDataShortCount", len(anim.random_data_short))

    new_arrays = {
        "names": np.array([name_ids[i] for i in anim.bone_order], dtype=np.uint16),
        "dataByte": np.frombuffer(anim.data_byte, dtype=np.uint8),
        "dataShort": anim.data_short,
        "dataInt": anim.data_int,
        "randomDataShort": anim.random_data_short,
        "randomDataByte": np.frombuffer(anim.random_data_byte, dtype=np.uint8),
        "randomDataInt": anim.random_data_int,
    }
    if numframes >= 256:
        new_arrays["indices"] = anim.indices
        put("indexCount", len(anim.indices))

    dst_children = {_member_of(c): c for c in dst.children}
    for member, values in new_arrays.items():
        dtype, scalar = _ARRAYS[member]
        data = np.asarray(values).astype(dp.endian + dtype).tobytes()
        count = len(values)
        f = find_field(drec, member)
        ptr = dst.relocs.get(f.offset)
        child = dst_children.get(member)
        if not count:
            if child is not None:
                dst.children.remove(child)
            if ptr is not None:
                ptr.kind, ptr.node = "null", None
            continue
        size = np.dtype(dtype).itemsize
        if child is None:
            t = TypeRef("scalar", scalar, size, size)
            child = Node(t, count, BLOCK_VIRTUAL)
            child.extra["align"] = size
            child.extra["origin"] = ("member", "XAnimIndices", "_2") if member == "indices" else ("member", "XAnimParts", member)
            rank = MEMBER_ORDER.index(member)
            pos = next((i for i, c in enumerate(dst.children) if MEMBER_ORDER.index(_member_of(c) or "name") > rank), len(dst.children))
            dst.children.insert(pos, child)
            new_ptr = Ptr("follow", child)
            new_ptr.owner = dst
            new_ptr.offset = f.offset
            dst.relocs[f.offset] = new_ptr
        child.count = count
        child.data = bytearray(data)
        child.segments = [(child.type, count, len(data), False)]
