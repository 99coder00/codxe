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

    duplicates = 0
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
                    duplicates += 1
            seen.add(key)
            merged_assets.append(ZoneAsset(asset.type, ptr, asset.name))
            asset_relocs.append(ptr)
            if ptr is not None and id(ptr) in child_for_ptr:
                asset_children.append(child_for_ptr[id(ptr)])

    if duplicates:
        log(f"merge: {duplicates} assets defined by several zones are kept once")

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
    dedupe_nested_assets(p, zone, log)
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


def prune_references(p: Platform, zone: Zone, types=("techset",), log=print) -> int:
    """Remove top level name references of ``types`` that nothing in the zone uses.

    PC zones list technique sets the console does not have (e.g. the high quality shadow map
    variants materials only use on PC); left in, the console would look them up by name.
    """
    node = zone.assets_node
    if node is None:
        return 0
    used = set()
    for n in zone.extra_root.walk():
        for ptr in n.relocs.values():
            if ptr.kind == "alias" and ptr.slot is not None:
                used.add(id(ptr.slot))
    keep = []
    removed = 0
    for i, asset in enumerate(zone.assets):
        ptr = node.relocs.get(8 * i + 4)
        target = ptr.node if ptr is not None and ptr.kind in ("follow", "insert") else None
        if (
            asset.type in types
            and target is not None
            and _is_reference(p, target)
            and id(ptr) not in used
        ):
            removed += 1
            continue
        keep.append((asset, ptr, target))
    if not removed:
        return 0
    data = bytearray(8 * len(keep))
    relocs = {}
    children = []
    assets = []
    positions = {id(a): i for i, a in enumerate(zone.assets)}
    for i, (asset, ptr, target) in enumerate(keep):
        j = positions[id(asset)]
        data[8 * i : 8 * i + 4] = node.data[8 * j : 8 * j + 4]
        if ptr is not None:
            ptr.offset = 8 * i + 4
            relocs[8 * i + 4] = ptr
        if target is not None:
            children.append(target)
        assets.append(asset)
    node.data = data
    node.relocs = relocs
    node.children = children
    node.count = 2 * len(keep)
    node.segments = [(node.type, node.count, len(data), False)]
    zone.assets = assets
    log(f"removed {removed} unused technique set references")
    return removed


def _is_reference(p: Platform, node: Node) -> bool:
    from .zone import asset_name

    try:
        return asset_name(p, node).startswith(",")
    except Exception:
        return False


_NORMAL_BLOCKS = (4, 5, 6)  # virtual, large, physical: blocks whose pointer slots can be aliased


def dedupe_nested_assets(p: Platform, zone: Zone, log=print) -> int:
    """Load every named asset once: later nested copies (e.g. the images of mod materials that the
    map already has) point to the first one instead."""
    from .zone import asset_name

    incoming: Dict[int, List[int]] = {}
    alias_by_slot: Dict[int, List[Ptr]] = {}
    for n in zone.extra_root.walk():
        for ptr in n.relocs.values():
            if ptr.kind == "ref" and ptr.node is not None:
                incoming.setdefault(id(ptr.node), []).append(id(n))
            elif ptr.kind == "alias" and ptr.slot is not None:
                alias_by_slot.setdefault(id(ptr.slot), []).append(ptr)
                if ptr.slot.owner is not None:
                    incoming.setdefault(id(ptr.slot.owner), []).append(id(n))

    first: Dict[Tuple[str, str], Ptr] = {}
    removed = 0
    # depth first in stream order: (node, pointer that loads it, parent)
    stack = [(zone.extra_root, None, None)]
    while stack:
        node, ptr, parent = stack.pop()
        origin = node.extra.get("origin")
        if ptr is not None and ptr.kind in ("follow", "insert") and origin and origin[0] == "asset":
            try:
                name = asset_name(p, node)
            except Exception:
                name = ""
            if name and not name.startswith(","):
                key = (origin[1], name.lower())
                kept = first.get(key)
                if kept is None:
                    if ptr.kind == "insert" or (parent is not None and parent.block in _NORMAL_BLOCKS):
                        first[key] = ptr
                else:
                    inside = {id(x) for x in node.walk()}
                    if not any(src not in inside for target in inside for src in incoming.get(target, ())):
                        for alias in alias_by_slot.get(id(ptr), ()):
                            if alias.index == 1:
                                alias.slot = kept
                                alias.index = 1 if kept.kind == "insert" else 0
                        ptr.kind, ptr.node, ptr.slot, ptr.index = "alias", None, kept, 1 if kept.kind == "insert" else 0
                        parent.children = [c for c in parent.children if c is not node]
                        removed += 1
                        continue
        loaders = {id(q.node): q for q in node.relocs.values() if q.kind in ("follow", "insert") and q.node is not None}
        for child in reversed(node.children):
            stack.append((child, loaders.get(id(child)), node))
    if removed:
        log(f"merge: {removed} nested assets loaded by an earlier asset are shared")
    return removed
