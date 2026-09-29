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
   and those they use in turn;
3. :func:`keep_mod_scripts`: scripts of the mod that the game's own zones have too get names of
   their own, so that the console runs the mod's, as the PC does.
"""

from __future__ import annotations

import re
from typing import Dict, Iterable, List, Optional, Set, Tuple

from .commands import find_field
from .layout import TypeRef
from .zone import Node, Platform, Ptr, Zone, ZoneAsset, asset_name

SCRIPT_EXTENSIONS = (".gsc", ".csc")

# Client scripts the game loads by name for a zombie map, which no script names: it calls
# clientscripts/_callbacks::sound_notify and loads clientscripts/_zombie_mode. The console's own
# _callbacks.csc has no sound_notify and none of its zones used by usermaps has _zombie_mode.csc, so
# every map of CoD Xenon's carries both; PC maps made with the first mod tools (Dead Sand) have
# neither, as the PC game's own zones had them ("Could not find script 'clientscripts/_zombie_mode'").
ZOMBIE_ENGINE_SCRIPTS = ("clientscripts/_callbacks.csc", "clientscripts/_zombie_mode.csc")

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


def set_rawfile_text(p: Platform, node: Node, text: bytes):
    """Give the RawFile asset ``node`` the content ``text``."""
    buffer = _buffer(node)
    buffer.data = bytearray(text + b"\0")
    buffer.count = len(buffer.data)
    buffer.segments = [(buffer.segments[0][0] if buffer.segments else buffer.type, buffer.count, buffer.count, False)]
    p.u32.pack_into(node.data, find_field(p.record("RawFile"), "len").offset, len(text))


def override_scripts(p: Platform, zones: List[Zone], loose, log=print) -> int:
    """Give the scripts of ``zones`` (PC) the content of the map's loose scripts (``loose``: an
    images.IwdLibrary of the map's .iwd files and folder)."""
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
            set_rawfile_text(p, node, text)
            replaced.add(normalize(name))
    if replaced:
        log(f"scripts: {len(replaced)} taken from the map's own files, as the PC game does ({', '.join(sorted(replaced))})")
    return len(replaced)


# A PC hint names the use key, F by default ("Press F To Play A Song"); in a hint string the game
# shows the key bound to +activate for &&1, as the stock hints do: the console's use button.
_USE_KEY_HINT = re.compile(rb'"(Press|Hold) F (?=[Tt]o )')


def use_key_hints(p: Platform, zone: Zone, log=print) -> List[str]:
    """Hints of the scripts naming the PC's use key show the console's use button (see above).
    Only scripts that set hint strings are changed. Returns their names."""
    changed = []
    for name, node in _rawfiles(p, zone):
        if name.startswith(",") or not name.lower().endswith(".gsc") or _buffer(node) is None:
            continue
        text = rawfile_text(node)
        if b"sethintstring" not in text.lower():
            continue
        new, count = _USE_KEY_HINT.subn(rb'"\1 &&1 ', text)
        if count:
            set_rawfile_text(p, node, new)
            changed.append(normalize(name))
    if changed:
        log(f"scripts: hints naming the PC's use key (F) show the console's use button ({', '.join(changed)})")
    return changed


# The DLC2 / DLC3 modding kits (Sparks') check a map's entities with modderHelp(entity, message), and
# their setup code stops when one is missing ("return; // Kill Thread"). But modderHelp only says
# so with the developer dvar on: in the game as played it says nothing is missing, and the setup
# goes on without the entities. The retail game runs a script on past runtime errors, so a loop
# over a missing entity never ends: The Simpsons has no zipline, and the zipline's setup loops
# forever (the console kills the thread, "potential infinite loop in script"; Xenia crashes).
_MODDER_HELP = re.compile(rb"(?m)^(modderHelp[ \t]*\([ \t]*(\w+)[ \t]*,[ \t]*(\w+)[ \t]*\)[ \t]*\r?\n[ \t]*\{)")
_MODDER_HELP_FIX = b"// t4ff: a missing entity stops the setup without developer too"


def fix_modder_help(p: Platform, zone: Zone, log=print) -> List[str]:
    """modderHelp says a missing entity is missing also without developer (see above). Returns the
    names of the scripts changed."""
    changed = []
    for name, node in _rawfiles(p, zone):
        if name.startswith(",") or not name.lower().endswith(".gsc") or _buffer(node) is None:
            continue
        text = rawfile_text(node)
        if b"modderHelp" not in text or _MODDER_HELP_FIX in text:
            continue
        newline = b"\r\n" if b"\r\n" in text else b"\n"

        def fix(m):
            entity, msg = m.group(2), m.group(3)
            return (m.group(1) + newline + b"\t" + _MODDER_HELP_FIX + newline
                    + b"\tif( !isDefined( " + entity + b" ) && isDefined( " + msg + b' ) && getDvarInt( "developer" ) < 1 )' + newline
                    + b"\t\treturn true;" + newline)

        new, count = _MODDER_HELP.subn(fix, text)
        if count:
            set_rawfile_text(p, node, new)
            changed.append(normalize(name))
    if changed:
        log(f"scripts: the modding kit's check for missing entities works without developer too, so setups stop there ({', '.join(changed)})")
    return changed


# The PC game spawns script_struct entities (spawn("script_struct", origin)): The Simpsons' rocket
# barrage links one to each rocket to play its explosion sounds. The console's refuses ("script_struct
# cannot be spawned dynamically", "unable to spawn "script_struct" entity"), which ends the thread:
# the rockets fly nowhere and hurt no one. A script_origin is the entity for that (linkTo, playSound).
_SPAWN_STRUCT = re.compile(rb'(\bspawn\s*\(\s*)"script_struct"', re.I)


def spawn_script_origins(p: Platform, zone: Zone, log=print) -> List[str]:
    """Scripts spawning script_struct entities spawn script_origin ones (see above). Returns the
    names of the scripts changed."""
    changed = []
    for name, node in _rawfiles(p, zone):
        if name.startswith(",") or not name.lower().endswith(SCRIPT_EXTENSIONS) or _buffer(node) is None:
            continue
        text = rawfile_text(node)
        if b"script_struct" not in text:
            continue
        new, count = _SPAWN_STRUCT.subn(rb'\1"script_origin"', text)
        if count:
            set_rawfile_text(p, node, new)
            changed.append(normalize(name))
    if changed:
        log(f"scripts: script_struct entities, which the console cannot spawn, are spawned as script_origin ({', '.join(changed)})")
    return changed


# The game runs the first script of a name it loads, and the console loads its own zones (common.ff,
# patch.ff, ...) before the map: a script both have is the game's there. On PC, with the map's mod
# active, the mod's scripts (its mod.ff, its .iwd and loose files) win over the game's instead. The
# console's patch.ff has Der Riese's maps/_zombiemode_zone_manager.gsc, and the DLC3 modding kit
# ships a changed one in the mod: when no enabled zone has a player in it, the kit's makes the map's
# first zone active, Der Riese's its "receiver_zone", which other maps have not. With the game's, a
# player of The Simpsons in the room with the TV (a zone its doors never enable) leaves no zone
# active, so no spawner: every round ends as it starts, five at a time, and dogs stay where they
# spawn until the failsafe kills them.
# So the mod's copy gets a name of its own, and the map's scripts call it by that name. Not when a
# script of the game's that the map runs calls it too (the game's maps/_load.gsc calls
# maps/_laststand.gsc): both copies would run, each with its own idea of the state they share. And
# not for scripts the engine runs by name (animscripts, client scripts, the callbacks).
MOD_SCRIPT_SUFFIX = "_mod"
_ENGINE_RUN_SCRIPTS = ("animscripts/", "clientscripts/", "character/", "aitype/", "mptype/", "xmodelalias/",
                       "maps/_callbacksetup.gsc", "maps/_callbackglobal.gsc")  # fmt: skip
_SPACE = re.compile(rb"\s+")


def keep_mod_scripts(p: Platform, zone: Zone, mod_scripts: Set[str], console_library, level_script: str, log=print) -> Dict[str, str]:
    """Scripts of the mod (``mod_scripts``: normalized names) that the game's own zones among
    ``console_library`` have too, and that differ, get a name of their own, and the map's scripts
    that include or call them use it (see above). ``level_script``: the map's level script
    (``maps/<map>.gsc``). Returns {old name: new name}."""
    if console_library is None:
        return {}
    nodes = {normalize(name): node for name, node in _rawfiles(p, zone) if not name.startswith(",") and _buffer(node) is not None}
    texts = {name: rawfile_text(node) for name, node in nodes.items() if name.endswith(".gsc")}
    game_texts: Dict[str, Optional[bytes]] = {}

    def game(name: str) -> Optional[bytes]:
        if name not in game_texts:
            found = console_library.find_in_game_zones("RawFile", name)
            game_texts[name] = rawfile_text(found[1]) if found is not None else None
        return game_texts[name]

    shadowed = [
        name for name in sorted(texts)
        if name in mod_scripts and game(name) is not None and _SPACE.sub(b"", game(name)) != _SPACE.sub(b"", texts[name])
    ]  # fmt: skip
    if not shadowed:
        return {}

    # the scripts the map runs, as the console has them: the game's copy of those both have
    runs_game_copy: Dict[str, bool] = {}
    calls: Dict[str, Set[str]] = {}
    queue = [level_script.lower(), "maps/_callbacksetup.gsc"]
    while queue:
        name = queue.pop()
        if name in calls:
            continue
        text = game(name)
        runs_game_copy[name] = text is not None
        if text is None:
            text = texts.get(name)
        calls[name] = script_references(name, text) if text is not None else set()
        queue.extend(calls[name] - set(calls))

    renamed: Dict[str, str] = {}
    for name in shadowed:
        if name not in calls:
            continue  # nothing the map runs uses it
        if name.startswith(_ENGINE_RUN_SCRIPTS):
            log(f"warning: {name} of the mod is not the one the console runs: the engine runs it by name, and the game's own zones have one")
            continue
        game_callers = sorted(caller for caller, refs in calls.items() if name in refs and runs_game_copy[caller] and caller != name)
        if game_callers:
            log(f"warning: {name} of the mod is not the one the console runs: the game's own zones have one, which their {game_callers[0]} calls too")
            continue
        new = _free_name(name[: -len(".gsc")] + MOD_SCRIPT_SUFFIX, ".gsc", set(nodes) | set(calls), game)
        if not _rename_rawfile(p, zone, nodes[name], new):
            log(f"warning: {name} of the mod is not the one the console runs: its name is shared with other data of the zone")
            continue
        renamed[name] = new
    if not renamed:
        return {}
    for name, node in nodes.items():
        if not name.endswith(".gsc"):
            continue
        text = new_text = rawfile_text(node)
        for old, new in renamed.items():
            path = new[: -len(".gsc")].replace("/", "\\").encode("latin-1")
            new_text = _script_path(old).sub(lambda m, path=path: path, new_text)
        if new_text != text:
            set_rawfile_text(p, node, new_text)
    for old, new in renamed.items():
        log(f"scripts: {old} of the mod runs as {new}: the game's own zones have one too, which the console would run instead (the PC runs the mod's)")
    return renamed


def _script_path(name: str):
    """A regex for the script ``name`` (normalized) where scripts include or call it."""
    parts = name[: -len(".gsc")].split("/")
    return re.compile(rb"(?i)(?<![\w\\/])" + rb"[\\/]".join(re.escape(part.encode("latin-1")) for part in parts) + rb"(?=\s*(?:::|;))")


def _free_name(stem: str, ext: str, taken: Set[str], game) -> str:
    name, i = stem + ext, 2
    while name in taken or game(name) is not None:
        name, i = f"{stem}{i}{ext}", i + 1
    return name


def _rename_rawfile(p: Platform, zone: Zone, node: Node, name: str) -> bool:
    """Give the RawFile asset ``node`` the name ``name`` (False when its name string is shared)."""
    off = find_field(p.record("RawFile"), "name").offset
    old = node.relocs.get(off)
    old_string = old.target() if old is not None else None
    if old_string is not None:
        for other in zone.extra_root.walk():
            for ptr in other.relocs.values():
                if ptr is not old and ptr.target() is old_string:
                    return False
    string = Node(TypeRef("scalar", "char", 1, 1), 0, old_string.block if old_string is not None else _buffer(node).block)
    string.string = True
    string.data = bytearray(name.encode("latin-1") + b"\0")
    string.count = len(string.data)
    string.segments = [(string.type, string.count, string.count, False)]
    string.extra["align"] = 1
    index = next((i for i, child in enumerate(node.children) if child is old_string), None)
    if index is not None:
        node.children[index] = string
    else:
        node.children.insert(0, string)  # the name is loaded before the buffer
    node.relocs[off] = _pointer("follow", node, off, string)
    for asset in zone.assets:
        if asset.node is node:
            asset.name = name
    return True


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


def zone_of_assets(p: Platform, assets: List[Tuple[str, str, Node]], strings: Optional[List[Optional[str]]] = None) -> Zone:
    """A zone loading ``assets`` ((asset type, name, header node)), to merge into another.
    ``strings``: the script strings the assets use (e.g. those of the Cloner that copied them)."""
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
    zone = Zone(p.name, list(strings) if strings else [None], zone_assets, [], 0, 0, None, node)
    zone.extra_root = root
    return zone


def missing_scripts_zone(p: Platform, zone: Zone, sources: List, console_library=None, log=print) -> Optional[Zone]:
    """A zone with the scripts ``zone``'s scripts use but no zone of the map has: from the map's own
    files (``sources[0]``, images.IwdLibrary), the console library, then other PC files
    (``sources[1:]``)."""
    defined: Dict[str, Optional[Node]] = {}
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
    finder = RawfileFinder(p, template, sources, console_library)
    queue = list(texts.items())
    engine: Dict[str, str] = {}  # scripts the game loads by name, and where they came from
    if is_zombie_map(texts):
        for ref in ZOMBIE_ENGINE_SCRIPTS:
            if defined.get(ref) is not None:
                continue  # the map's own
            node, origin = finder.find(ref)  # even when the game's own zones have one: not this one
            if node is None:
                unknown[ref] = "the game"
                continue
            engine[ref] = origin
            defined[ref] = node
            added.append(("rawfile", asset_name(p, node), node))
            queue.append((ref, rawfile_text(node)))
    while queue:
        user, text = queue.pop()
        for ref in sorted(script_references(user, text)):
            if ref in defined or ref in unknown:
                continue
            if console_library is not None and console_library.in_game_zones("RawFile", ref):
                defined[ref] = None  # the game's own zones have it
                continue
            node, origin = finder.find(ref)
            if node is None:
                unknown[ref] = user
                continue
            origins[ref] = origin
            defined[ref] = node
            added.append(("rawfile", asset_name(p, node), node))
            queue.append((ref, rawfile_text(node)))
    for ref, source in sorted(engine.items()):
        log(f"scripts: added {ref} (the game loads it for zombie maps, the map has none) from the {source}")
    for ref, source in sorted(origins.items()):
        log(f"scripts: added {ref} (used by the map's scripts, not in its fastfiles) from the {source}")
    for ref in ZOMBIE_ENGINE_SCRIPTS:
        if unknown.get(ref) == "the game":
            del unknown[ref]
            log(f"warning: {ref}, which the game loads for zombie maps, is in none of the map's files nor the console fastfiles given: give a map converted by CoD Xenon among them (--console-zone)")
    if unknown:
        log(
            f"scripts: {len(unknown)} scripts the map uses are left to the game's own zones (e.g. {', '.join(sorted(unknown)[:6])}). "
            "Should the console stop with \"Could not find script\", add the Xbox 360 fastfile that has it."
        )
    return zone_of_assets(p, added, finder.strings) if added else None


def is_zombie_map(texts: Dict[str, bytes]) -> bool:
    """Whether the scripts (normalized name -> text) are those of a zombie map."""
    return any(name.startswith("maps/_zombiemode") or b"_zombiemode" in text.lower() for name, text in texts.items())


class RawfileFinder:
    """Raw files (scripts, shellshock files, ...) a map lacks, from the map's own files
    (``sources[0]``, images.IwdLibrary), the console library, then other PC files (``sources[1:]``:
    the game folder, the mod tools' ``raw`` folder)."""

    def __init__(self, p: Platform, template: Node, sources: List, console_library=None):
        from .library import Cloner

        self.p = p
        self.template = template  # a RawFile of the zone, the layout of new ones
        self.sources = sources
        self.console_library = console_library
        self.strings: List[Optional[str]] = [None]
        self.cloner = Cloner(p, self.strings) if console_library is not None else None

    def find(self, ref: str) -> Tuple[Optional[Node], str]:
        """(a RawFile asset ``ref``, where it was found), (None, "") when nowhere."""
        from .library import LibraryError

        data = self.sources[0].read(ref) if self.sources else None
        if data is not None:
            return make_rawfile(self.p, self.template, ref, data.rstrip(b"\0")), "map's files"
        if self.console_library is not None:
            found = self.console_library.find("RawFile", ref)
            if found is not None:
                try:
                    return self.cloner.copy_asset(*found), "Xbox 360 fastfiles"
                except LibraryError:
                    pass
        for source in self.sources[1:]:
            data = source.read(ref) or source.read("raw/" + ref)
            if data is not None:
                return make_rawfile(self.p, self.template, ref, data.rstrip(b"\0")), "PC files"
        return None, ""
