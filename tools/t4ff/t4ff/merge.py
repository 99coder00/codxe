"""Merging several zones into one (a usermap's <map>_patch.ff and mod.ff into <map>.ff for the console).

Script strings of every zone are merged into one table and all script string
fields are remapped. Assets defined by more than one zone are kept once; later
duplicates become name references resolved by the game. For scripts, string
tables and localized strings the last zone's version is kept (it overrides the
earlier ones on PC); for other assets the first one.
"""

from __future__ import annotations

import re
import struct
from typing import Dict, List, Optional, Set, Tuple

from .commands import find_field
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


# Self-contained asset types for which the last zone's version wins (mod.ff and <map>_patch.ff
# replace scripts of the map on PC)
LATER_WINS = ("rawfile", "stringtable", "localize")


def _content(node: Node) -> bytes:
    return b"".join(bytes(n.data) for n in node.walk())


def _is_reference(p: Platform, node: Optional[Node]) -> bool:
    from .zone import asset_name

    if node is None or (node.extra.get("origin") or ("",))[0] != "asset":
        return False
    try:
        return asset_name(p, node).startswith(",")
    except Exception:
        return False


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
    overridden = 0
    defined: Dict[Tuple[str, str], Tuple[Zone, Ptr, int]] = {}  # first definition: zone, pointer, child index
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
            if not asset.name.startswith(",") and ptr is not None and ptr.kind in ("follow", "insert") and _is_reference(p, ptr.node):
                # a name reference (e.g. left by an earlier merge) under the plain name
                asset = ZoneAsset(asset.type, ptr, "," + asset.name)
            duplicate = key in seen and not asset.name.startswith(",")
            if ptr is not None and ptr.kind in ("follow", "insert") and duplicate:
                target = ptr.node
                if not _has_incoming_refs(zone, target):
                    earlier = defined.get(key)
                    if asset.type in LATER_WINS and earlier is not None and _content(earlier[1].node) != _content(target):
                        # a later zone overrides the script / table (as mod.ff and <map>_patch.ff do on PC):
                        # its version takes the place of the earlier one
                        earlier_zone, earlier_ptr, earlier_child = earlier
                        if not _has_incoming_refs(earlier_zone, earlier_ptr.node):
                            old = earlier_ptr.node
                            earlier_ptr.node = target
                            if earlier_ptr.kind == "insert":
                                target.insert = True
                                target.extra["ptr"] = earlier_ptr
                            asset_children[earlier_child] = target
                            target = old
                            overridden += 1
                    ref = asset_hooks.build_reference(_Conv(p), asset.type, target, "," + asset.name)
                    ptr.node = ref
                    child_for_ptr[id(ptr)] = ref
                    duplicates += 1
                    asset = ZoneAsset(asset.type, ptr, "," + asset.name)
            elif ptr is not None and ptr.kind in ("follow", "insert") and not asset.name.startswith(",") and key not in defined:
                defined[key] = (zone, ptr, len(asset_children))
            seen.add(key)
            merged_assets.append(ZoneAsset(asset.type, ptr, asset.name))
            asset_relocs.append(ptr)
            if ptr is not None and id(ptr) in child_for_ptr:
                asset_children.append(child_for_ptr[id(ptr)])

    if duplicates:
        log(f"merge: {duplicates} assets defined by several zones are kept once ({overridden} scripts/tables taken from the later zone)")

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
    _set_asset_list(zone, keep)
    log(f"removed {removed} unused technique set references")
    return removed


def _set_asset_list(zone: Zone, keep: List[Tuple[ZoneAsset, Optional[Ptr], Optional[Node]]]):
    """Make the zone's asset list ``keep`` ((asset, its pointer, the node it loads)), a subset of it."""
    node = zone.assets_node
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


def scripted_menu_names(p: Platform, zone: Zone) -> set:
    """Lowercase names of the menus the zone's scripts open, close or precache."""
    import re

    from .scripts import _rawfiles, rawfile_text

    names = set()
    for name, raw in _rawfiles(p, zone):
        if name.lower().endswith((".gsc", ".csc")):
            text = rawfile_text(raw).decode("latin-1")
            names.update(m.lower() for m in re.findall(r'(?i)(?:openmenu|precachemenu|closemenu)\s*\(\s*"([^"]+)"', text))
    return names


def _menu_name_string(menu: Node) -> Optional[Node]:
    ptr = menu.relocs.get(0)  # menuDef_t.window.name
    string = ptr.target() if ptr is not None else None
    return string if string is not None and string.string else None


def menu_names(node: Node) -> List[str]:
    """Names of the menus a menu list (or any node) loads, references (``,name``) without their comma."""
    names, seen = [], set()
    for n in node.walk():
        # the console's lists point to their menus' asset slots (the menus are assets of their own)
        for menu in [n] + [ptr.target() for ptr in n.relocs.values()]:
            if menu is None or menu.type.name != "menuDef_t" or id(menu) in seen:
                continue
            seen.add(id(menu))
            string = _menu_name_string(menu)
            if string is not None:
                names.append(bytes(string.data).split(b"\0")[0].decode("latin-1").lstrip(","))
    return names


PAUSE_MENU = "pausedmenu"
CONSOLE_OPTIONS_MENU = "ingameoptions"
_OPEN = re.compile(r'"open"\s+"([^"]+)"', re.I)


def bind_pause_menu(p: Platform, zone: Zone, ingame_menus, log=print) -> Tuple[set, List[str]]:
    """Keep the mod's pause menu as the map's own when it offers more than the console's.

    The console opens the pause menu by name, and the map's ``pausedmenu`` takes the place of the
    game's while the map is loaded (UGX Mod's showed on Kino), so a map can have its own. The game's
    in-game menus (``ingame_menus``: those of its ``ui/ingame.txt``) are the only ones the pause menu
    can open, besides the script menus the map precaches: a PC pause menu's Options opened
    ``options_new_pc`` and only closed it. The mod's pause menu stays when it opens menus the game's
    has not and the zone has (UGX's Challenges: ``menu_challenges``, then ``popup_tier``): its options
    menus become the console's ``ingameoptions``, and those menus join a menu list the scripts
    precache, which the game loads with the in-game menus. Otherwise the console's pause menu stays
    (see :func:`rename_game_menus`). Returns (ids of the menus that keep their names, names of the
    menus that joined a script menu list).
    """
    ingame = {name.lower() for name in ingame_menus}
    menus = {}  # name -> [(menu node, the pointer that loads it, index of its list)]
    for i, asset in enumerate(zone.assets):
        if asset.type != "menulist" or asset.node is None:
            continue
        for n in asset.node.walk():
            for ptr in n.relocs.values():
                menu = ptr.node if ptr.kind in ("follow", "insert") else None
                if menu is not None and menu.type.name == "menuDef_t":
                    string = _menu_name_string(menu)
                    if string is not None:
                        name = bytes(string.data).split(b"\0")[0].decode("latin-1").lower()
                        menus.setdefault(name, []).append((menu, ptr, i))
    if PAUSE_MENU not in menus or not ingame:
        return set(), []
    # the mod's in-game list's pause menu, else the first one (UGX also has one in its ui/hud.txt)
    pause, _, _ = next((m for m in menus[PAUSE_MENU] if "ingame" in zone.assets[m[2]].name.lower()), menus[PAUSE_MENU][0])

    def opened(menu: Node) -> List[str]:
        return [m.lower() for s in menu.walk() if s.string for m in _OPEN.findall(bytes(s.data).decode("latin-1"))]

    extras, todo = [], [m for m in opened(pause) if m not in ingame and "options" not in m]
    while todo:
        name = todo.pop(0)
        if name in extras or name in ingame or name == PAUSE_MENU:
            continue
        if name not in menus:
            log(f"menus: the mod's pause menu opens '{name}', which the map has not: the console's pause menu stays")
            return set(), []
        extras.append(name)
        todo += opened(menus[name][0][0])
    if not extras:
        return set(), []
    scripted = scripted_menu_names(p, zone)
    owners = [menus[name][0][2] for name in extras]
    hosts = [i for i, a in enumerate(zone.assets)
             if a.type == "menulist" and a.node is not None and i > max(owners)
             and a.name.lower().startswith("ui/scriptmenus/") and a.name.lower()[len("ui/scriptmenus/"):].rsplit(".", 1)[0] in scripted]  # fmt: skip
    if not hosts:
        log(f"menus: the mod's pause menu opens {', '.join(extras)}, but no menu list the scripts precache comes after them: the console's pause menu stays")
        return set(), []
    host = zone.assets[hosts[0]]

    # its options menus: the console's
    for s in pause.walk():
        if s.string:
            text = bytes(s.data).decode("latin-1")
            new = _OPEN.sub(lambda m: f'"open" "{CONSOLE_OPTIONS_MENU}"' if "options" in m.group(1).lower() and m.group(1).lower() not in ingame else m.group(0), text)
            if new != text:
                s.data = bytearray(new.encode("latin-1"))
                s.count = len(s.data)
                s.segments = [(s.type, s.count, s.count, False)]
    # the menus it opens: in the precached list too (pointing to the copies the zone loads)
    rec = p.record("MenuList")
    array_ptr = host.node.relocs.get(find_field(rec, "menus").offset)
    array = array_ptr.node if array_ptr is not None else None
    if array is None:
        return set(), []
    for name in extras:
        offset = len(array.data)
        array.data += b"\0\0\0\0"
        array.count += 1
        ptr = Ptr("alias", None)
        ptr.owner, ptr.offset, ptr.slot, ptr.index = array, offset, menus[name][0][1], 0
        array.relocs[offset] = ptr
        # PC hints name the keyboard: the controller's button closes them
        for s in menus[name][0][0].walk():
            if s.string and b"Press ESC" in s.data:
                s.data = bytearray(bytes(s.data).replace(b"Press ESC", b"Press B"))
                s.count = len(s.data)
                s.segments = [(s.type, s.count, s.count, False)]
    array.segments = [(array.segments[0][0], array.count, len(array.data), False)]
    struct.pack_into(p.endian + "i", host.node.data, find_field(rec, "menuCount").offset, array.count)
    log(f"menus: the mod's pause menu stays the map's own (its {', '.join(extras)}), its options are the console's; "
        f"{', '.join(extras)} load with {host.name}, which the scripts precache")
    kept = {id(pause)} | {id(menus[name][0][0]) for name in extras}
    return kept, extras


def drop_frontend_menus(p: Platform, zone: Zone, is_stock_menu, log=print, is_stock_list=None, keep_menus=()) -> List[str]:
    """Remove the menu lists made of the mod's versions of the console's own menus.

    PC mods restyle the main menu and the lobbies with their own versions of the stock menus
    (``ui/main.menu``, ``ui/xboxlive_lobby.menu``), loaded with the mod. On the console the map's
    zone is loaded in game, where they would only take memory and replace the console's menus
    of the same names. A list goes when most of its menus are the console's and the map's scripts
    open or precache none of them: script menus (a music box...) stay. So does a list of the name of
    one of the game's own (``is_stock_list``, the console's ``common.ff`` has ``ui/ingame.txt`` and
    ``ui/hud.txt``), which takes the place of the game's: UGX Mod's ``ui/ingame.txt`` (88 menus,
    mostly PC ones) had the PC's pause menu, whose Options and Challenges open menus the console has
    not in game ("Could not find menu 'options_new_pc'") and only closed it.
    """
    from .zone import asset_name

    node = zone.assets_node
    if node is None:
        return []
    scripted = scripted_menu_names(p, zone)
    candidates = {}
    game_lists = {}  # the mod's copies of the game's own lists: index -> list node
    for i, asset in enumerate(zone.assets):
        ptr = node.relocs.get(8 * i + 4)
        target = ptr.node if ptr is not None and ptr.kind in ("follow", "insert") else None
        if asset.type != "menulist" or target is None:
            continue
        menus = [(asset_name(p, n) or "").lstrip(",").lower() for n in target.walk() if n.type.name == "menuDef_t"]
        stock = sum(1 for m in menus if is_stock_menu(m))
        game_list = is_stock_list is not None and is_stock_list(asset.name.lstrip(","))
        # a menu file the scripts load by name (precacheMenu("x"): ui/scriptmenus/x.menu) stays
        base = asset.name.lstrip(",").lower().rsplit("/", 1)[-1].rsplit(".", 1)[0]
        if base in scripted or scripted.intersection(menus):
            continue
        # a list without menus of its own points into others' (the PC linker loads a menu shared by
        # menu files once): UGX's options menu files into its ui/ingame.txt, which they kept
        if not menus or 2 * stock > len(menus) or game_list:
            candidates[i] = {id(n) for n in target.walk()}
            if game_list:
                game_lists[i] = target
    if not candidates:
        rename_game_menus(zone, is_stock_menu, scripted, log, keep_menus)
        return []
    # a list something that stays points into stays too (the lists often share strings and items,
    # e.g. the lobby points into the main menu): repeat until no more lists are kept
    list_of = {member: i for i, members in candidates.items() for member in members}
    pointers = []  # (list pointed into, list of the pointer or None)
    for n in zone.extra_root.walk():
        for ptr in n.relocs.values():
            target = ptr.node if ptr.kind == "ref" else ptr.slot.owner if ptr.kind == "alias" and ptr.slot is not None else None
            if target is not None and id(target) in list_of and list_of.get(id(n)) != list_of[id(target)]:
                pointers.append((list_of[id(target)], list_of.get(id(n))))
    while True:
        kept = {i for i, source in pointers if i in candidates and source not in candidates}
        if not kept:
            break
        for i in kept:
            del candidates[i]
    # a copy of a game's list something else needs (UGX's vote menus share strings and items with
    # its ui/ingame.txt) stays under a name of its own: the game loads its lists by name, and the
    # console's own pause menu and HUD then stay the game's
    renamed = []
    pinned = {i for i in game_lists if i not in candidates}
    names = {id(game_lists[i].relocs[0].node): i for i in pinned if 0 in game_lists[i].relocs and game_lists[i].relocs[0].node is not None}
    shared = set()  # name strings something else points to too (the linker stores a string once)
    for n in zone.extra_root.walk() if names else ():
        for off, ptr in n.relocs.items():
            string = ptr.target()
            if string is not None and id(string) in names and not (off == 0 and n is game_lists[names[id(string)]]):
                shared.add(names[id(string)])
    for i in sorted(pinned):
        name_ptr = game_lists[i].relocs.get(0)  # MenuList.name
        old = zone.assets[i].name.lstrip(",")
        if name_ptr is None or name_ptr.node is None or not name_ptr.node.string or i in shared:
            log(f"warning: menus: the mod's {old} stays and takes the place of the game's (its name is shared)")
            continue
        stem, dot, ext = old.rpartition(".")
        new = f"{stem}_mod.{ext}" if dot else f"{old}_mod"
        string = name_ptr.node
        string.data = bytearray(new.encode("latin-1") + b"\0")
        string.count = len(string.data)
        string.segments = [(string.type, string.count, string.count, False)]
        zone.assets[i].name = new
        renamed.append(f"{old} as {new}")
    if renamed:
        log(f"menus: the mod's versions of the game's own menu lists stay under names of their own ({', '.join(renamed)}): "
            f"its menus point into them, and the console's own lists (its pause menu, its HUD) stay the game's")
    dropped = [zone.assets[i].name for i in sorted(candidates)]
    if candidates:
        keep = []
        for i, asset in enumerate(zone.assets):
            if i in candidates:
                continue
            ptr = node.relocs.get(8 * i + 4)
            keep.append((asset, ptr, ptr.node if ptr is not None and ptr.kind in ("follow", "insert") else None))
        _set_asset_list(zone, keep)
        log(f"menus: left out {', '.join(dropped)}, the mod's versions of menus the console has (its main menu, lobbies, pause menu...)")
    rename_game_menus(zone, is_stock_menu, scripted, log, keep_menus)
    return dropped


def drop_unused_videos(p: Platform, zone: Zone, log=print) -> List[str]:
    """Remove the Bink videos (``*.bik`` raw files) that no menu or script left in the zone names.

    PC mods put their main menu's background video in the mod's fastfile (Kino Rezurrection's
    ``bik/kino_menu.bik``, 9.8 MiB, played by its ``ui/main.menu`` with ``ui_cinematic kino_menu``).
    On the console the main menu is the game's (:func:`drop_frontend_menus`), and every block of the
    map's zone takes the game's main memory (see memory.py)."""
    from .scripts import _rawfiles, rawfile_text
    from .zone import asset_name

    node = zone.assets_node
    if node is None:
        return []
    videos = [i for i, asset in enumerate(zone.assets) if asset.type == "rawfile" and asset.name.lower().endswith(".bik")]
    if not videos:
        return []
    texts = [rawfile_text(raw).lower() for name, raw in _rawfiles(p, zone) if not name.lower().endswith(".bik")]
    for i, asset in enumerate(zone.assets):
        ptr = node.relocs.get(8 * i + 4)
        target = ptr.node if ptr is not None and ptr.kind in ("follow", "insert") else None
        if asset.type in ("menu", "menulist") and target is not None:
            texts.extend(bytes(n.data).lower() for n in target.walk() if n.string)
    unused = set()
    for i in videos:
        stem = zone.assets[i].name.lstrip(",").replace("\\", "/").rsplit("/", 1)[-1][: -len(".bik")].lower().encode("latin-1")
        if not any(stem in text for text in texts):
            unused.add(i)
    if not unused:
        return []
    keep = []
    for i, asset in enumerate(zone.assets):
        if i in unused:
            continue
        ptr = node.relocs.get(8 * i + 4)
        keep.append((asset, ptr, ptr.node if ptr is not None and ptr.kind in ("follow", "insert") else None))
    dropped = [zone.assets[i].name.lstrip(",") for i in sorted(unused)]
    _set_asset_list(zone, keep)
    log(f"videos: left out {', '.join(dropped)}, which no menu or script of the map plays (the mod's main menu's)")
    return dropped


def rename_game_menus(zone: Zone, is_stock_menu, keep_names=(), log=print, keep_menus=()) -> List[str]:
    """Give the mod's menus that have the name of one of the console's their own (``<name>_mod``).

    The console opens menus by name, and a menu the map's zone loads takes the place of the game's
    of the same name, whichever list it is in: UGX Mod's PC ``pausedmenu`` (in its ``ui/ingame.txt``
    and ``ui/hud.txt``, which stay because its vote menus and weapons point into them) replaced the
    console's even with its lists renamed, and its Options and Challenges open PC menus the console's
    in-game list has not (``options_new_pc``, ``menu_challenges``), so they only closed it. Menus the
    scripts open by name (``keep_names``) and references (``,name``) keep theirs; so does a menu whose
    name string something outside the renamed menus uses too (the PC linker stores a string once).
    """
    keep_names = {name.lower() for name in keep_names}
    menus = {}  # id(menu) -> (menu, its name string, its name)
    seen = set()
    for asset in zone.assets:
        if asset.type != "menulist" or asset.node is None:
            continue
        for n in asset.node.walk():
            if n.type.name != "menuDef_t" or id(n) in seen or id(n) in keep_menus:
                continue
            seen.add(id(n))
            ptr = n.relocs.get(0)  # menuDef_t.window.name
            string = ptr.target() if ptr is not None else None
            if string is None or not string.string:
                continue
            name = bytes(string.data).split(b"\0")[0].decode("latin-1")
            if name and not name.startswith(",") and name.lower() not in keep_names and is_stock_menu(name):
                menus[id(n)] = (n, string, name)
    if not menus:
        return []
    owner = {id(m): key for key, (menu, _, _) in menus.items() for m in menu.walk()}
    strings = {id(string): string for _, string, _ in menus.values()}
    shared = set()
    for n in zone.extra_root.walk():
        for ptr in n.relocs.values():
            target = ptr.target()
            if target is not None and id(target) in strings and id(n) not in owner:
                shared.add(id(target))
    renamed, kept = [], []
    for menu, string, name in menus.values():
        if id(string) in shared:
            kept.append(name)
            continue
        if not bytes(string.data).startswith(f"{name}_mod\0".encode("latin-1")):  # menus sharing one name string
            string.data = bytearray(f"{name}_mod".encode("latin-1") + b"\0")
            string.count = len(string.data)
            string.segments = [(string.type, string.count, string.count, False)]
        renamed.append(name)
    if renamed:
        names = sorted(set(renamed))
        log(f"menus: {len(names)} of the mod's menus have the name of one of the console's and would take its place "
            f"({', '.join(names[:8])}{', ...' if len(names) > 8 else ''}): they are named <name>_mod, the console's own stay")
    if kept:
        log(f"warning: menus: {', '.join(sorted(set(kept)))} keep the console's names (their name strings are shared) and take the place of its own")
    return renamed


def _is_reference(p: Platform, node: Node) -> bool:
    from .zone import asset_name

    try:
        return asset_name(p, node).startswith(",")
    except Exception:
        return False


_NORMAL_BLOCKS = (4, 5, 6)  # virtual, large, physical: blocks whose pointer slots can be aliased


def dedupe_nested_assets(p: Platform, zone: Zone, log=print, key_of=None, what: str = "nested assets loaded by an earlier asset") -> int:
    """Load every named asset once: later nested copies (e.g. the images of mod materials that the
    map already has) point to the first one instead.

    ``key_of(record, name, node)`` says which assets are the same (default: same type and name);
    None leaves an asset alone.
    """
    from .zone import asset_name

    if key_of is None:

        def key_of(record, name, node):
            return (record, name.lower())

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
            key = key_of(origin[1], name, node) if name and not name.startswith(",") else None
            if key is not None:
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
        log(f"merge: {removed} {what} are shared")
    return removed
