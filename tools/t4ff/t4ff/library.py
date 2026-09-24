"""Copying console assets from other Xbox 360 fastfiles.

Some assets cannot be converted from PC data: technique sets carry compiled
shaders, and stock images or sounds may not be available as PC files. They are
copied from Xbox 360 fastfiles instead (stock zones from the console game or
maps converted by CoD Xenon), given with ``--console-zone``.

An asset of a library zone can share data with anything loaded before it in
that zone (offset pointers into earlier allocations, aliases to earlier pointer
slots). A copy stands alone: shared data is copied inline where the asset refers
to it (a following pointer instead of an offset pointer), and nested assets
loaded by earlier assets (e.g. pixel shaders) are copied at their first use.
Within the output zone, data copied once is shared again by the later copies.

The children of a node are in load order and each one is loaded through one
following pointer of its parent, in the order the reader met those pointers
(``Node.relocs`` preserves it), which gives the position of every copied child.
"""

from __future__ import annotations

import os
from typing import Dict, List, Optional, Tuple

from .fastfile import read_fastfile
from .merge import node_script_string_offsets
from .zone import ASSET_RECORDS, Node, Platform, Ptr, Reader, Zone, asset_name


class LibraryError(Exception):
    pass


class ConsoleLibrary:
    """Assets of Xbox 360 fastfiles, looked up by type and name (loaded on first use)."""

    def __init__(self, platform: Platform, paths: List[str], log=print):
        self.p = platform
        self.paths = [p for p in paths if p]
        self.log = log
        self._zones: Optional[List[Zone]] = None
        self._index: Dict[Tuple[str, str], Tuple[Zone, Node]] = {}

    def _load(self):
        if self._zones is not None:
            return
        self._zones = []
        for path in self.paths:
            files = [path]
            if os.path.isdir(path):
                files = sorted(os.path.join(root, f) for root, _, fs in os.walk(path) for f in fs if f.lower().endswith(".ff"))
            for f in files:
                try:
                    endian, _, data = read_fastfile(f)
                    if endian != ">":
                        self.log(f"warning: {f}: not an Xbox 360 fastfile, ignored")
                        continue
                    zone = Reader(self.p, data).load()
                except Exception as e:  # a library zone that cannot be read is skipped
                    self.log(f"warning: {f}: cannot be read ({e}), ignored")
                    continue
                self._zones.append(zone)
                count = 0
                for node in zone.extra_root.walk():
                    origin = node.extra.get("origin")
                    if not origin or origin[0] != "asset":
                        continue
                    name = asset_name(self.p, node)
                    if name and not name.startswith(","):
                        count += self._index.setdefault((origin[1], name.lower()), (zone, node)) == (zone, node)
                self.log(f"console library: {os.path.basename(f)}: {count} assets")

    def find(self, rec_name: str, name: str) -> Optional[Tuple[Zone, Node]]:
        if not self.paths:
            return None
        self._load()
        return self._index.get((rec_name, name.lstrip(",").lower()))


def _ptr(kind: str, owner: Node, offset: int, node: Node = None, index: int = 0, inner: int = 0, slot: Ptr = None) -> Ptr:
    ptr = Ptr(kind, node, index, inner, slot)
    ptr.owner = owner
    ptr.offset = offset
    return ptr


class Cloner:
    """Copies library assets into one output zone."""

    def __init__(self, platform: Platform, strings: List[Optional[str]]):
        self.p = platform
        self.strings = strings  # script strings of the output zone (extended as needed)
        self.string_index = {s: i for i, s in enumerate(strings) if s is not None}
        self.copies: Dict[int, Node] = {}  # id(library node) -> copy
        self.slots: Dict[int, Ptr] = {}  # id(library pointer) -> the pointer of the copy
        self.assets: Dict[Tuple[str, str], Ptr] = {}  # nested assets copied: (record, name) -> loading pointer

    def register_asset(self, rec_name: str, name: str, ptr: Ptr):
        """``ptr`` loads the asset ``name`` in the output zone."""
        self.assets.setdefault((rec_name, name.lstrip(",").lower()), ptr)

    def copy_asset(self, zone: Zone, node: Node) -> Node:
        state = (dict(self.copies), dict(self.slots), dict(self.assets))
        try:
            return self._copy(zone, node)
        except LibraryError:
            self.copies, self.slots, self.assets = state
            raise

    # -- internals ------------------------------------------------------------

    def _script_string(self, zone: Zone, index: int) -> int:
        text = zone.script_strings[index] if index < len(zone.script_strings) else None
        if text is None:
            return 0
        if text not in self.string_index:
            self.string_index[text] = len(self.strings)
            self.strings.append(text)
        return self.string_index[text]

    def _copy(self, zone: Zone, src: Node) -> Node:
        new = Node(src.type, src.count, src.block)
        new.data = bytearray(src.data)
        new.push_before = src.push_before
        new.push_after = src.push_after
        new.string = src.string
        new.asset = src.asset
        new.runtime_size = src.runtime_size
        new.segments = list(src.segments)
        for key in ("align", "origin", "delayed"):
            if key in src.extra:
                new.extra[key] = src.extra[key]
        self.copies[id(src)] = new

        if new.data and not new.string:
            for off in node_script_string_offsets(self.p, src):
                if off + 2 <= len(new.data):
                    value = self.p.u16.unpack_from(new.data, off)[0]
                    self.p.u16.pack_into(new.data, off, self._script_string(zone, value))

        for off, ptr in src.relocs.items():
            new.relocs[off] = self._copy_ptr(zone, new, off, ptr)
        return new

    def _add_child(self, owner: Node, offset: int, kind: str, child: Node) -> Ptr:
        ptr = _ptr(kind, owner, offset, child)
        owner.children.append(child)
        if kind == "insert":
            child.insert = True
            child.extra["ptr"] = ptr
        return ptr

    def _copy_ptr(self, zone: Zone, owner: Node, offset: int, ptr: Ptr) -> Ptr:
        if ptr.kind == "null":
            return _ptr("null", owner, offset)

        if ptr.kind in ("follow", "insert"):
            target = ptr.node
            key = self._asset_key(target)
            if key is not None and key in self.assets:
                # a nested asset already copied into this zone: point to its slot
                first = self.assets[key]
                self.slots[id(ptr)] = first
                return _ptr("alias", owner, offset, slot=first, index=1 if first.kind == "insert" else 0)
            kind = "insert" if key is not None else ptr.kind
            new = self._add_child(owner, offset, kind, self._copy(zone, target))
            self.slots[id(ptr)] = new
            if key is not None:
                self.assets[key] = new
            return new

        if ptr.kind == "ref":
            target = ptr.node
            copy = self.copies.get(id(target))
            if copy is not None:
                return _ptr("ref", owner, offset, copy, ptr.index, ptr.inner)
            # shared with data outside the copied asset: copy that data here
            if ptr.index or ptr.inner:
                if target.count > 1 or ptr.inner:
                    raise LibraryError(f"offset pointer into the middle of {target!r}")
            child = self._copy(zone, target)
            return self._add_child(owner, offset, "follow", child)

        if ptr.kind == "alias":
            slot = ptr.slot
            mine = self.slots.get(id(slot))
            if mine is not None:
                # without an insert slot, the pointer slot itself holds the asset once loaded
                index = ptr.index if mine.kind == "insert" else 0
                return _ptr("alias", owner, offset, slot=mine, index=index)
            target = slot.target() if slot.kind == "alias" else slot.node
            if target is None:
                raise LibraryError("alias to an empty pointer slot")
            key = self._asset_key(target)
            if key is not None and key in self.assets:
                first = self.assets[key]
                self.slots[id(slot)] = first
                return _ptr("alias", owner, offset, slot=first, index=1 if first.kind == "insert" else 0)
            # the pointed asset (or data) was loaded before the copied asset: load it here
            copied = self.copies.get(id(target))
            if copied is not None:
                if key is not None:
                    raise LibraryError(f"no pointer loads the copied asset {key[1]}")
                return _ptr("ref", owner, offset, copied, 0, 0)
            kind = "insert" if key is not None else "follow"
            new = self._add_child(owner, offset, kind, self._copy(zone, target))
            self.slots[id(slot)] = new
            if key is not None:
                self.assets[key] = new
            return new

        raise LibraryError(f"unsupported pointer {ptr!r}")

    def _asset_key(self, node: Node) -> Optional[Tuple[str, str]]:
        origin = node.extra.get("origin")
        if not origin or origin[0] != "asset":
            return None
        name = asset_name(self.p, node)
        return (origin[1], name.lower()) if name else None


def library_record(asset_type: str) -> str:
    return ASSET_RECORDS[asset_type]
