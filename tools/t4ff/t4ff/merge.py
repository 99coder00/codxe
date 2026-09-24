"""Merging several zones into one (e.g. a usermap's mod.ff into <map>.ff for the console).

Script strings of every zone are merged into one table and all script string
fields are remapped. Assets defined by more than one zone are kept once (first
zone wins); later duplicates become name references resolved by the game.
"""

from __future__ import annotations

from typing import Dict, List, Optional, Set, Tuple

from .layout import TypeRef
from .zone import BLOCK_VIRTUAL, Node, Platform, Ptr, Zone, ZoneAsset

_SS_CACHE: Dict[Tuple[str, str, bool], List[int]] = {}


def record_script_string_offsets(p: Platform, name: str, partial: bool = False) -> List[int]:
    """Offsets of every script string (u16) inside one instance of record ``name``."""
    key = (p.name, name, partial)
    if key in _SS_CACHE:
        return _SS_CACHE[key]
    _SS_CACHE[key] = []
    rec = p.record(name)
    limit = None
    if partial:
        dyn = p.dynamic_member(name)
        limit = dyn.offset if dyn is not None else None
    result = []
    for f in rec.fields:
        if not f.name or (limit is not None and f.offset >= limit):
            continue
        infos = p.member_infos(name, f.name)
        t = f.type
        if any(i.scriptstring for i in infos.values()) and t.kind != "pointer":
            count = t.count if t.kind == "array" else 1
            result.extend(f.offset + 2 * i for i in range(count))
            continue
        # embedded records and arrays of records
        count = 1
        while t.kind == "array":
            count *= t.count
            t = t.elem
        if t.kind == "record" and not rec.is_union:
            inner = record_script_string_offsets(p, t.name)
            for i in range(count):
                result.extend(f.offset + i * t.size + o for o in inner)
    _SS_CACHE[key] = result
    return result


def node_script_string_offsets(p: Platform, node: Node) -> List[int]:
    origin = node.extra.get("origin")
    if origin and origin[0] == "member":
        infos = p.member_infos(origin[1], origin[2])
        if any(i.scriptstring for i in infos.values()):
            return list(range(0, len(node.data) - 1, 2))
    result = []
    pos = 0
    for t, count, size, partial in node.segments:
        if t.kind == "record" and count:
            stride = size // count
            inner = record_script_string_offsets(p, t.name, partial)
            if inner:
                for i in range(count):
                    result.extend(pos + i * stride + o for o in inner)
        pos += size
    return result


def remap_script_strings(p: Platform, root: Node, mapping: List[int]):
    for node in root.walk():
        if not node.data:
            continue
        for off in node_script_string_offsets(p, node):
            if off + 2 > len(node.data):
                continue
            value = p.u16.unpack_from(node.data, off)[0]
            if value < len(mapping):
                p.u16.pack_into(node.data, off, mapping[value])


def merge_zones(p: Platform, zones: List[Zone], log=print) -> Zone:
    from . import assets as asset_hooks

    first = zones[0]
    strings: List[Optional[str]] = list(first.script_strings)
    if not strings:
        strings = [None]
    index = {s: i for i, s in enumerate(strings) if s is not None}

    merged_assets: List[ZoneAsset] = []
    seen: Set[Tuple[str, str]] = set()
    asset_children: List[Node] = []
    asset_relocs: List[Ptr] = []

    for zone_index, zone in enumerate(zones):
        mapping = []
        for s in zone.script_strings:
            if s is None:
                mapping.append(0)
                continue
            if s not in index:
                index[s] = len(strings)
                strings.append(s)
            mapping.append(index[s])
        if zone_index and zone.assets_node is not None:
            remap_script_strings(p, zone.assets_node, mapping)

        if zone.assets_node is None:
            continue
        node = zone.assets_node
        children = iter(node.children)
        # children of the asset list node are the followed asset headers in asset order
        followed = {id(ptr): ptr for ptr in node.relocs.values() if ptr.kind in ("follow", "insert")}
        child_for_ptr = {}
        for off in sorted(node.relocs):
            ptr = node.relocs[off]
            if ptr.kind in ("follow", "insert"):
                child_for_ptr[id(ptr)] = next(children)
        for i, asset in enumerate(zone.assets):
            ptr = node.relocs.get(8 * i + 4)
            key = (asset.type, asset.name.lstrip(","))
            duplicate = key in seen and not asset.name.startswith(",")
            if ptr is not None and ptr.kind in ("follow", "insert") and duplicate:
                target = ptr.node
                if not _has_incoming_refs(zone, target):
                    ref = asset_hooks.build_reference(_Conv(p), asset.type, target, "," + asset.name)
                    ptr.node = ref
                    child_for_ptr[id(ptr)] = ref
                    log(f"merge: duplicate {asset.type} '{asset.name}' replaced by a reference")
            seen.add(key)
            merged_assets.append(ZoneAsset(asset.type, ptr, asset.name))
            asset_relocs.append(ptr)
            if ptr is not None and id(ptr) in child_for_ptr:
                asset_children.append(child_for_ptr[id(ptr)])

    # script string table
    script_node = Node(TypeRef("pointer", "", 4, 4, to=TypeRef("scalar", "char", 1, 1)), len(strings), BLOCK_VIRTUAL)
    script_node.extra["align"] = 4
    script_node.data = bytearray(4 * len(strings))
    script_node.segments.append((script_node.type, len(strings), len(script_node.data), False))
    for i, s in enumerate(strings):
        if s is None:
            ptr = Ptr("null")
        else:
            child = asset_hooks.string_node(s)
            script_node.children.append(child)
            ptr = Ptr("follow", child)
        ptr.owner = script_node
        ptr.offset = 4 * i
        script_node.relocs[4 * i] = ptr

    # asset list
    assets_node = Node(TypeRef("scalar", "uint", 4, 4), 2 * len(merged_assets), BLOCK_VIRTUAL)
    assets_node.extra["align"] = 4
    assets_node.data = bytearray(8 * len(merged_assets))
    assets_node.segments.append((assets_node.type, 2 * len(merged_assets), len(assets_node.data), False))
    for i, (asset, ptr) in enumerate(zip(merged_assets, asset_relocs)):
        p.u32.pack_into(assets_node.data, 8 * i, p.asset_type_index[asset.type])
        if ptr is None:
            ptr = Ptr("null")
        ptr.owner = assets_node
        ptr.offset = 8 * i + 4
        assets_node.relocs[8 * i + 4] = ptr
    assets_node.children = asset_children

    root = Node(TypeRef("scalar", "uint", 4, 4), 4, -1)
    root.data = bytearray(16)
    root.children = [script_node, assets_node]
    zone = Zone(p.name, strings, merged_assets, [], 0, 0, script_node, assets_node)
    zone.extra_root = root
    return zone


class _Conv:
    """Minimal converter facade for building reference assets."""

    def __init__(self, p: Platform):
        self.dst = p


def _has_incoming_refs(zone: Zone, target: Node) -> bool:
    inside = {id(n) for n in target.walk()}
    for node in zone.extra_root.walk():
        if id(node) in inside:
            continue
        for ptr in node.relocs.values():
            if ptr.kind == "ref" and id(ptr.node) in inside:
                return True
            if ptr.kind == "alias":
                slot = ptr.slot
                if slot is not None and slot.owner is not None and id(slot.owner) in inside:
                    return True
    return False
