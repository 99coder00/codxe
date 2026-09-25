"""Scripts of a usermap on the console.

On PC, with the map's mod active, the game reads a script from the mod's own files (its .iwd files
or loose files) before looking in the fastfiles, so a map can ship newer scripts next to its
fastfiles. And the scripts can use scripts of the game's own zones: PC Aztec's ``_zombiemode.gsc``
calls ``maps\\_zombiemode_weapons_sumpf`` of Shi No Numa. The console reads scripts from fastfiles
only, and does not load Shi No Numa's zone for a usermap. So, as CoD Xenon's Aztec does:

1. :func:`override_scripts`: the scripts of the PC zones take the content of the map's loose
   scripts (CoD Xenon's ``_zombiemode.gsc`` is the one of the .iwd, not of the fastfiles);
2. :func:`missing_scripts_zone`: scripts the map's scripts include or call but no zone of the map
   has are taken from the map's files or from the Xbox 360 fastfiles given (``--console-zone``),
   and those they use in turn.
"""

from __future__ import annotations

import re
from typing import Dict, Iterable, List, Optional, Set, Tuple

from .commands import find_field
from .layout import TypeRef
from .zone import Node, Platform, Ptr, Zone, ZoneAsset, asset_name

SCRIPT_EXTENSIONS = (".gsc", ".csc")

_COMMENTS = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
_INCLUDE = re.compile(r"#include\s+([\w\\/]+)\s*;")
_CALL = re.compile(r"\b([A-Za-z_]\w*(?:[\\/]\w+)+)\s*::")


def normalize(name: str) -> str:
    return name.lstrip(",").replace("\\", "/").lower()


def script_references(name: str, text: bytes) -> Set[str]:
    """Scripts ``text`` includes or calls into (normalized names with the script's extension)."""
    ext = ".csc" if name.lower().endswith(".csc") else ".gsc"
    source = _COMMENTS.sub("", text.decode("latin-1"))
    refs = {m.group(1) for m in _INCLUDE.finditer(source)} | {m.group(1) for m in _CALL.finditer(source)}
    return {normalize(r) + ext for r in refs}


def _rawfiles(p: Platform, zone: Zone) -> Iterable[Tuple[str, Node]]:
    for node in zone.extra_root.walk():
        origin = node.extra.get("origin")
        if origin and origin[0] == "asset" and node.type.name == "RawFile":
            yield asset_name(p, node), node


def _buffer(node: Node) -> Optional[Node]:
    return next((c for c in node.children if (c.extra.get("origin") or ("", "", ""))[1:] == ("RawFile", "buffer")), None)


def rawfile_text(node: Node) -> bytes:
    buffer = _buffer(node)
    return bytes(buffer.data).rstrip(b"\0") if buffer is not None else b""


def override_scripts(p: Platform, zones: List[Zone], loose, log=print) -> int:
    """Give the scripts of ``zones`` (PC) the content of the map's loose scripts (``loose``: an
    images.IwdLibrary of the map's .iwd files and folder)."""
    length = find_field(p.record("RawFile"), "len").offset
    replaced = set()
    for zone in zones:
        for name, node in _rawfiles(p, zone):
            if name.startswith(",") or not name.lower().endswith(SCRIPT_EXTENSIONS):
                continue
            text = loose.read(normalize(name))
            buffer = _buffer(node)
            if text is None or buffer is None:
                continue
            text = text.rstrip(b"\0")
            if bytes(buffer.data).rstrip(b"\0") == text:
                continue
            buffer.data = bytearray(text + b"\0")
            buffer.count = len(buffer.data)
            buffer.segments = [(buffer.segments[0][0] if buffer.segments else buffer.type, buffer.count, buffer.count, False)]
            p.u32.pack_into(node.data, length, len(text))
            replaced.add(normalize(name))
    if replaced:
        log(f"scripts: {len(replaced)} taken from the map's own files, as the PC game does ({', '.join(sorted(replaced))})")
    return len(replaced)


def _pointer(kind: str, owner: Node, offset: int, node: Node) -> Ptr:
    ptr = Ptr(kind, node)
    ptr.owner = owner
    ptr.offset = offset
    return ptr


def make_rawfile(p: Platform, template: Node, name: str, text: bytes) -> Node:
    """A console RawFile asset ``name`` holding ``text``, laid out as ``template`` (another one)."""
    rec = p.record("RawFile")
    name_off = find_field(rec, "name").offset
    len_off = find_field(rec, "len").offset
    buffer_off = find_field(rec, "buffer").offset
    old_buffer = _buffer(template)
    node = Node(template.type, 1, template.block)
    node.push_before, node.push_after = template.push_before, template.push_after
    node.segments = list(template.segments)
    node.extra = {"align": template.extra.get("align", 4), "origin": ("asset", "RawFile")}
    node.data = bytearray(len(template.data))
    p.u32.pack_into(node.data, name_off, 0xFFFFFFFF)
    p.u32.pack_into(node.data, len_off, len(text))
    p.u32.pack_into(node.data, buffer_off, 0xFFFFFFFF)

    string = Node(TypeRef("scalar", "char", 1, 1), 0, old_buffer.block)
    string.string = True
    string.data = bytearray(name.encode("latin-1") + b"\0")
    string.count = len(string.data)
    string.segments = [(string.type, string.count, string.count, False)]
    string.extra["align"] = 1

    buffer = Node(old_buffer.type, len(text) + 1, old_buffer.block)
    buffer.data = bytearray(text + b"\0")
    buffer.segments = [(old_buffer.segments[0][0] if old_buffer.segments else old_buffer.type, buffer.count, buffer.count, False)]
    buffer.extra = {"align": old_buffer.extra.get("align", 1), "origin": ("member", "RawFile", "buffer")}

    node.relocs[name_off] = _pointer("follow", node, name_off, string)
    node.relocs[buffer_off] = _pointer("follow", node, buffer_off, buffer)
    buffer.extra["ptr"] = node.relocs[buffer_off]
    node.children = [string, buffer]
    return node


def zone_of_assets(p: Platform, assets: List[Tuple[str, str, Node]]) -> Zone:
    """A zone loading ``assets`` ((asset type, name, header node)), to merge into another."""
    from .zone import BLOCK_VIRTUAL

    count = len(assets)
    uint = TypeRef("scalar", "uint", 4, 4)
    node = Node(uint, 2 * count, BLOCK_VIRTUAL)
    node.data = bytearray()
    for asset_type, _, _ in assets:
        node.data += p.u32.pack(p.asset_type_index[asset_type]) + b"\xff\xff\xff\xff"
    node.segments = [(uint, 2 * count, len(node.data), False)]
    node.extra["align"] = 4
    zone_assets = []
    for i, (asset_type, name, header) in enumerate(assets):
        ptr = _pointer("follow", node, 8 * i + 4, header)
        node.relocs[8 * i + 4] = ptr
        node.children.append(header)
        header.extra["ptr"] = ptr
        zone_assets.append(ZoneAsset(asset_type, ptr, name))
    root = Node(uint, 4, -1)
    root.data = bytearray(16)
    root.children = [node]
    zone = Zone(p.name, [None], zone_assets, [], 0, 0, None, node)
    zone.extra_root = root
    return zone


def missing_scripts_zone(p: Platform, zone: Zone, sources: List, console_library=None, log=print) -> Optional[Zone]:
    """A zone with the scripts ``zone``'s scripts use but no zone of the map has: from the map's own
    files (``sources[0]``, images.IwdLibrary), the console library, then other PC files
    (``sources[1:]``)."""
    from .library import Cloner, LibraryError

    defined: Dict[str, Node] = {}
    texts: Dict[str, bytes] = {}
    template = None
    for name, node in _rawfiles(p, zone):
        key = normalize(name)
        if name.startswith(","):
            defined.setdefault(key, None)
            continue
        defined[key] = node
        template = template or node
        if key.endswith(SCRIPT_EXTENSIONS):
            texts[key] = rawfile_text(node)
    if template is None:
        return None

    added: List[Tuple[str, str, Node]] = []
    origins: Dict[str, str] = {}
    unknown: Dict[str, str] = {}
    cloner = Cloner(p, [None]) if console_library is not None else None
    queue = list(texts.items())
    while queue:
        user, text = queue.pop()
        for ref in sorted(script_references(user, text)):
            if ref in defined or ref in unknown:
                continue
            node = None
            data = sources[0].read(ref) if sources else None
            if data is not None:
                node = make_rawfile(p, template, ref, data.rstrip(b"\0"))
                origins[ref] = "map's files"
            if node is None and console_library is not None:
                found = console_library.find("RawFile", ref)
                if found is not None:
                    try:
                        node = cloner.copy_asset(*found)
                        origins[ref] = "Xbox 360 fastfiles"
                    except LibraryError:
                        node = None
            if node is None:
                # other PC files: the game folder, the mod tools' raw folder
                for source in sources[1:]:
                    data = source.read(ref) or source.read("raw/" + ref)
                    if data is not None:
                        node = make_rawfile(p, template, ref, data.rstrip(b"\0"))
                        origins[ref] = "PC files"
                        break
            if node is None:
                unknown[ref] = user
                continue
            defined[ref] = node
            added.append(("rawfile", asset_name(p, node), node))
            queue.append((ref, rawfile_text(node)))
    for ref, source in sorted(origins.items()):
        log(f"scripts: added {ref} (used by the map's scripts, not in its fastfiles) from the {source}")
    if unknown:
        log(
            f"scripts: {len(unknown)} scripts the map uses are left to the game's own zones (e.g. {', '.join(sorted(unknown)[:6])}). "
            "Should the console stop with \"Could not find script\", add the Xbox 360 fastfile that has it."
        )
    return zone_of_assets(p, added) if added else None
