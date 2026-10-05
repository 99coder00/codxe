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
from typing import Dict, List, Optional, Sequence, Set, Tuple

from . import progress
from .fastfile import read_fastfile
from .merge import node_script_string_offsets
from .zone import ASSET_RECORDS, Node, Platform, Ptr, Reader, Zone, asset_name


class LibraryError(Exception):
    pass


# Zones the game loads itself, before the map: what they have need not be in the map's fastfile.
GAME_ZONES = ("code_pre_gfx", "code_post_gfx", "common", "patch")


# Zones of the console's menus (the front end): their menus are the console's own too.
UI_ZONES = ("ui", "patch_ui")


def is_game_zone(path: str) -> bool:
    stem = os.path.splitext(os.path.basename(path))[0].lower()
    return stem in GAME_ZONES or (stem.startswith("localized_") and stem[len("localized_") :] in GAME_ZONES)


# Written next to the fastfiles of every map t4ff converts: they are not console data.
T4FF_MARKER = "t4ff.txt"


def made_by_t4ff(path: str) -> bool:
    """Whether the fastfile ``path`` is one of t4ff's own conversions."""
    return os.path.exists(os.path.join(os.path.dirname(os.path.abspath(path)), T4FF_MARKER))


def _inside(path: str, folder: str) -> bool:
    path, folder = (os.path.normcase(os.path.realpath(p)) for p in (path, folder))
    return path == folder or path.startswith(folder.rstrip(os.sep) + os.sep)


def library_files(paths: List[str], first: str = "", exclude: Sequence[str] = (), skipped: Optional[List[str]] = None) -> List[str]:
    """The fastfiles of ``paths`` (files, or folders searched in name order), each once, in the
    order they are read: the first that has an asset gives it. The fastfile named ``first`` (the
    map being converted: CoD Xenon's conversion of the same map, in a folder of their maps) comes
    first, so its own versions of assets other maps also have win.

    t4ff's own conversions (a usermaps folder holding them among CoD Xenon's maps) and the fastfiles
    in the folders of ``exclude`` (where this conversion writes) are left out, into ``skipped``:
    read as console data, an earlier conversion of the map would hand its old copies back."""
    files, seen = [], set()
    for path in paths:
        found = sorted(os.path.join(root, f) for root, _, fs in os.walk(path) for f in fs if f.lower().endswith(".ff")) if os.path.isdir(path) else [path]
        for f in found:
            key = os.path.normcase(os.path.realpath(f))
            if key in seen:
                continue
            seen.add(key)
            if made_by_t4ff(f) or any(folder and _inside(f, folder) for folder in exclude):
                if skipped is not None:
                    skipped.append(f)
                continue
            files.append(f)
    if first:
        files.sort(key=lambda f: os.path.splitext(os.path.basename(f))[0].lower() != first.lower())
    return files


class ConsoleLibrary:
    """Assets of Xbox 360 fastfiles, looked up by type and name (loaded on first use)."""

    def __init__(self, platform: Platform, paths: List[str], log=print, first: str = "", exclude: Sequence[str] = ()):
        self.p = platform
        self.paths = [p for p in paths if p]
        self.log = log
        self.first = first  # the name of the map being converted (see library_files)
        self.exclude = tuple(exclude)  # folders this conversion writes to (see library_files)
        self._zones: Optional[List[Zone]] = None
        self._index: Dict[Tuple[str, str], Tuple[Zone, Node]] = {}
        self._game: Dict[Tuple[str, str], Tuple[Zone, Node]] = {}  # assets of the game's own zones among them
        self._ui_menus: Set[str] = set()  # menus of the console's menu zones among them
        # technique sets of every zone by name (the first is the index's), and the names of the vertex
        # shaders of Treyarch's zones (those outside a usermaps folder): see techset_safe
        self._techsets: Dict[str, List[Tuple[Zone, Node]]] = {}
        self._treyarch_shaders: Set[str] = set()

    def _load(self):
        if self._zones is not None:
            return
        self._zones = []
        skipped: List[str] = []
        files = library_files(self.paths, self.first, self.exclude, skipped)
        # Maps' load zones (mario_load.ff) hold what is theirs only: their loading screen ($levelbriefing
        # and its picture, which a mod's menus name). A map given it showed Mario's once loaded
        # (Mini-Labor). The loading screen writer reads them itself.
        files = [f for f in files if not os.path.basename(f).lower().endswith("_load.ff")]
        if skipped:
            self.log(
                f"console library: {len(skipped)} fastfiles t4ff converted left out (e.g. {os.path.basename(skipped[0])}): "
                "give the console's and CoD Xenon's fastfiles, not converted maps"
            )
        for index, f in enumerate(files):
            progress.step("Reading Xbox 360 fastfiles", index, len(files))
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
            game = is_game_zone(f)
            ui = os.path.splitext(os.path.basename(f))[0].lower() in UI_ZONES
            treyarch = "usermaps" not in os.path.normcase(os.path.abspath(f)).split(os.sep)
            count = 0
            for node in zone.extra_root.walk():
                if treyarch and node.type.name == "MaterialVertexShader" and not node.string:
                    shader_name = node.relocs.get(0).target() if node.relocs.get(0) is not None else None
                    if shader_name is not None and shader_name.string:
                        self._treyarch_shaders.add(bytes(shader_name.data).rstrip(b"\0").decode("latin-1").lower())
                origin = node.extra.get("origin")
                if not origin or origin[0] != "asset":
                    continue
                name = asset_name(self.p, node)
                if origin[1] == "MaterialTechniqueSet" and name and not name.startswith(","):
                    self._techsets.setdefault(name.lower(), []).append((zone, node))
                if name and not name.startswith(","):
                    count += self._index.setdefault((origin[1], name.lower()), (zone, node)) == (zone, node)
                    if game:
                        self._game.setdefault((origin[1], name.lower()), (zone, node))
                if ui and name and origin[1] == "menuDef_t":
                    # a reference too: the console's ui.ff has that menu
                    self._ui_menus.add(name.lstrip(",").lower())
            self.log(f"console library: {os.path.basename(f)}: {count} assets")
        progress.step("Reading Xbox 360 fastfiles", len(files), len(files))

    def find(self, rec_name: str, name: str) -> Optional[Tuple[Zone, Node]]:
        if not self.paths:
            return None
        self._load()
        key = name.lstrip(",").lower()
        if rec_name == "MaterialTechniqueSet":
            # a copy every slot of which the game can draw with, when a zone has one (see techset_safe)
            return next((found for found in self._techsets.get(key, []) if self.techset_safe(found[1])), self._index.get((rec_name, key)))
        return self._index.get((rec_name, key))

    def techset_safe(self, node: Node) -> bool:
        """Whether every model and world vertex shader of a technique set (vertexShaderArray 1 to 15)
        is one of Treyarch's (a name its zones have). CoD Xenon compiled shaders the console's zones
        lack themselves, and those need a vertex declaration: the game sets none when a pass has the
        shader of the vertex type it draws (it expects Treyarch's, compiled for that type), and the
        D3D library then binds the shader to an empty declaration and spins forever. Kino
        Rezurrection's dry grass (mc/mtl_drygrass, mc_ambient_t0c0 of CoD Xenon's Leviathan) froze the
        game at its first frame. Their effect shaders (vertexShaderArray 0) draw."""
        import struct

        from .commands import find_field

        if not self._treyarch_shaders or os.environ.get("T4FF_ALLOW_UNSAFE_TECHSETS"):
            return True  # no zone of Treyarch's given: nothing to tell them apart with (or a test of CoD Xe's guard)
        name_ptr = node.relocs.get(0)
        set_name = bytes(name_ptr.target().data).rstrip(b"\0").decode("latin-1").lower() if name_ptr is not None and name_ptr.target() is not None else ""
        if "effect" in set_name.lstrip(",").split("_"):
            return True  # effects draw with generic vertices (vertexShaderArray 0): Kino Der Toten's do
        ts_rec, tech_rec, pass_rec = (self.p.record(r) for r in ("MaterialTechniqueSet", "MaterialTechnique", "MaterialPass"))
        techs = find_field(ts_rec, "techniques").offset
        passes, count = find_field(tech_rec, "passArray").offset, find_field(tech_rec, "passCount").offset
        shaders = find_field(pass_rec, "vertexShaderArray").offset
        for t in range(find_field(ts_rec, "techniques").type.count):
            ptr = node.relocs.get(techs + 4 * t)
            tech = ptr.target() if ptr is not None else None
            if tech is None:
                continue
            for k in range(struct.unpack_from(self.p.endian + "H", tech.data, count)[0]):
                for slot in range(1, 16):
                    shader_ptr = tech.relocs.get(passes + k * pass_rec.size + shaders + 4 * slot)
                    shader = shader_ptr.target() if shader_ptr is not None and shader_ptr.kind != "null" else None
                    name_ptr = shader.relocs.get(0) if shader is not None else None
                    name = name_ptr.target() if name_ptr is not None else None
                    if name is not None and bytes(name.data).rstrip(b"\0").decode("latin-1").lower() not in self._treyarch_shaders:
                        return False
        return True

    def names(self, rec_name: str) -> List[str]:
        """The (lowercase) names of the assets of a record among the library."""
        if not self.paths:
            return []
        self._load()
        return sorted(name for rec, name in self._index if rec == rec_name)

    def in_game_zones(self, rec_name: str, name: str) -> bool:
        """Whether a zone the game loads itself (e.g. ``common.ff``) among the library has the asset:
        the map can refer to it by name instead of carrying a copy."""
        if not self.paths:
            return False
        self._load()
        return (rec_name, name.lstrip(",").lower()) in self._game

    def is_stock_menu(self, name: str) -> bool:
        """Whether the console's own zones (the game's or its menu zones) have a menu ``name``."""
        if not self.paths:
            return False
        self._load()
        name = name.lstrip(",").lower()
        return name in self._ui_menus or ("menuDef_t", name) in self._game

    def game_rawfiles(self) -> List[Tuple[str, Node]]:
        """(name, node) of the raw files (scripts...) of the game's own zones among the library."""
        if not self.paths:
            return []
        self._load()
        return [(name, node) for (rec_name, name), (_, node) in self._game.items() if rec_name == "RawFile"]

    def find_in_game_zones(self, rec_name: str, name: str) -> Optional[Tuple[Zone, Node]]:
        """The asset as the game's own zones among the library have it (maps may carry changed
        copies of the game's raw files)."""
        if not self.paths:
            return None
        self._load()
        return self._game.get((rec_name, name.lstrip(",").lower()))


def _ptr(kind: str, owner: Node, offset: int, node: Node = None, index: int = 0, inner: int = 0, slot: Ptr = None) -> Ptr:
    ptr = Ptr(kind, node, index, inner, slot)
    ptr.owner = owner
    ptr.offset = offset
    return ptr


class Cloner:
    """Copies library assets into one output zone."""

    def __init__(self, platform: Platform, strings: List[Optional[str]], replace=None):
        self.p = platform
        # replace(record, name, library node) -> Node or None: a substitute for a nested asset
        self.replace = replace
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
            new = self._add_child(owner, offset, kind, self._copy_or_replace(zone, target, key))
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
            new = self._add_child(owner, offset, kind, self._copy_or_replace(zone, target, key))
            self.slots[id(slot)] = new
            if key is not None:
                self.assets[key] = new
            return new

        raise LibraryError(f"unsupported pointer {ptr!r}")

    def _copy_or_replace(self, zone: Zone, target: Node, key) -> Node:
        if key is not None and self.replace is not None:
            substitute = self.replace(key[0], key[1], target)
            if substitute is not None:
                self.copies[id(target)] = substitute
                return substitute
        return self._copy(zone, target)

    def _asset_key(self, node: Node) -> Optional[Tuple[str, str]]:
        origin = node.extra.get("origin")
        if not origin or origin[0] != "asset":
            return None
        name = asset_name(self.p, node)
        return (origin[1], name.lower()) if name else None


def library_record(asset_type: str) -> str:
    return ASSET_RECORDS[asset_type]
