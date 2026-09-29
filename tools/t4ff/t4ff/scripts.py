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
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

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


_STRING_LITERAL = re.compile(rb'"([^"\\\n]{1,64})"')


def script_strings(p: Platform, zone: Zone) -> Set[str]:
    """The string literals of the zone's scripts, lowercase: the menus they open or precache among
    them (OpenMenu( "loadout" ), game["menu"] = "tom_music_player_002c")."""
    strings = set()
    for name, node in _rawfiles(p, zone):
        if not name.startswith(",") and name.lower().endswith(SCRIPT_EXTENSIONS) and _buffer(node) is not None:
            strings.update(m.group(1).decode("latin-1").lower() for m in _STRING_LITERAL.finditer(rawfile_text(node)))
    return strings


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


# The console's SetCursorHint lists the valid hint types when a script gives another one, and its list
# runs past the end of their table (into the float 1.0 after it): the game crashes reading a string
# at 0x3F800000 (Xenia freezes). Dead Sand's Nebelwerfer battery sets "HINT_NONE", which is none;
# the icon-less hint is "HINT_NOICON".
VALID_CURSOR_HINTS = ("HINT_INHERIT", "HINT_NOICON", "HINT_SEAT", "HINT_ACTIVATE", "HINT_HEALTH", "HINT_FRIENDLY", "HINT_SPECTATOR")
_CURSOR_HINT = re.compile(rb'(\bsetcursorhint\s*\(\s*)"([^"]*)"', re.I)


def valid_cursor_hints(p: Platform, zone: Zone, log=print) -> List[str]:
    """Scripts give SetCursorHint valid hint types only, "HINT_NOICON" for others (see above).
    Returns the names of the scripts changed."""
    changed = []
    for name, node in _rawfiles(p, zone):
        if name.startswith(",") or not name.lower().endswith(".gsc") or _buffer(node) is None:
            continue
        text = rawfile_text(node)
        if b"setcursorhint" not in text.lower():
            continue

        def fix(m):
            if m.group(2).decode("latin-1").upper() in VALID_CURSOR_HINTS:
                return m.group(0)
            return m.group(1) + b'"HINT_NOICON"'

        new = _CURSOR_HINT.sub(fix, text)
        if new != text:
            set_rawfile_text(p, node, new)
            changed.append(normalize(name))
    if changed:
        log(f"scripts: hint types the console has not are HINT_NOICON (its SetCursorHint crashes on others) ({', '.join(changed)})")
    return changed


# The console refuses precaches once the level script has waited ("precacheItem must be called
# before any wait statements in the level script"), and a model not precached cannot be set ("model
# 'katyusha_rocket' not precached"). Zombie maps' level scripts call the zombie mode's main(), which
# waits for the players, then their own setup: Dead Sand's precaches its rocket barrage there, so
# its Nebelwerfer fires no rocket. The precaches of the functions the level script's main() calls
# are made at its start instead (theirs become comments), and so are those of the zone's models
# scripts set by name that no script precaches (Dead Sand's rockets set "katyusha_rocket", which
# nothing precaches on the console).
_PRECACHE = re.compile(rb'\b(precache(?:item|model|shader|shellshock|rumble|menu|string))\s*\(\s*(&?"[^"\n]*")\s*\)\s*;', re.I)
_PRECACHE_MARK = b"// t4ff: the precaches of the setup below, before the level script's first wait"


def _blank_comments(text: bytes) -> bytes:
    """``text`` with its comments made spaces (the rest keeps its offsets)."""
    return _COMMENTS.sub(lambda m: " " * len(m.group(0)), text.decode("latin-1")).encode("latin-1")


def _function_body(text: bytes, name: str) -> Optional[Tuple[int, int]]:
    """(start, end) of the body (inside the braces) of the function ``name`` defined in ``text``."""
    source = _blank_comments(text)
    m = re.search(rb"(?m)^[ \t]*" + re.escape(name.encode("latin-1")) + rb"[ \t]*\([^)]*\)\s*\{", source, re.I)
    if m is None:
        return None
    depth, i = 1, m.end()
    in_string = False
    while i < len(source) and depth:
        c = source[i : i + 1]
        if in_string:
            if c == b"\\":
                i += 1
            elif c == b'"':
                in_string = False
        elif c == b'"':
            in_string = True
        elif c == b"{":
            depth += 1
        elif c == b"}":
            depth -= 1
        i += 1
    return (m.end(), i - 1) if depth == 0 else None


_CALLED = re.compile(rb"(?:\b([A-Za-z_]\w*(?:[\\/]\w+)+)\s*::\s*)?\b([A-Za-z_]\w*)\s*\(")


_SET_MODEL = re.compile(rb'\bsetmodel\s*\(\s*"([^"\n]+)"\s*\)', re.I)


def _model_names(p: Platform, zone: Zone) -> List[str]:
    return [asset_name(p, n) for n in zone.extra_root.walk() if n.type.name == "XModel" and (n.extra.get("origin") or ("",))[0] == "asset"]


def precache_before_waits(p: Platform, zone: Zone, level_script: str, log=print) -> List[str]:
    """The precaches of the functions the level script's main() calls, and of the zone's models
    scripts set by name that nothing precaches, at its start (see above). Returns the precache calls
    added."""
    nodes = {normalize(name): node for name, node in _rawfiles(p, zone) if not name.startswith(",") and _buffer(node) is not None}
    level_script = level_script.lower()
    if level_script not in nodes:
        return []
    level_text = rawfile_text(nodes[level_script])
    if _PRECACHE_MARK in level_text:
        return []
    body = _function_body(level_text, "main")
    if body is None:
        return []
    source = _blank_comments(level_text)
    # the scripts a bare function name may come from: the level script and those it includes
    includes = [level_script] + sorted(r for r in script_references(level_script, b"\n".join(re.findall(rb"#include\s+[\w\\/]+\s*;", source))))
    wanted: List[bytes] = []
    seen_calls = set()
    moved: Dict[str, List[Tuple[int, int]]] = {}  # the calls made at the start instead, by script
    for m in _CALLED.finditer(source[body[0] : body[1]]):
        path, function = m.group(1), m.group(2).decode("latin-1")
        if function.lower() in ("main", "if", "while", "for", "switch", "return", "wait", "thread"):
            continue
        candidates = [normalize(path.decode("latin-1")) + ".gsc"] if path else includes
        for script in candidates:
            if (script, function.lower()) in seen_calls or script not in nodes:
                continue
            seen_calls.add((script, function.lower()))
            text = _blank_comments(rawfile_text(nodes[script]))
            found = _function_body(text, function)
            if found is None:
                continue
            for call in _PRECACHE.finditer(text, found[0], found[1]):
                if call.group(0) not in wanted:
                    wanted.append(call.group(0))
                moved.setdefault(script, []).append(call.span())
            break
    main_precaches = {c.group(0).lower() for c in _PRECACHE.finditer(source[body[0] : body[1]])}
    wanted = [c for c in wanted if c.lower() not in main_precaches]
    # the setup's own calls, after the wait, would stop it ("precacheItem must be called before any
    # wait statements"): they are comments
    for script, spans in moved.items():
        text = rawfile_text(nodes[script])
        for start, end in sorted(set(spans), reverse=True):
            text = text[:start] + b"/* t4ff: made first in the level script: " + text[start:end] + b" */" + text[end:]
        set_rawfile_text(p, nodes[script], text)
    level_text = rawfile_text(nodes[level_script])
    body = _function_body(level_text, "main")
    # models the scripts set by name that nothing precaches
    loaded = {name.lower() for name in _model_names(p, zone)}
    precached, set_by_name = set(), []
    for name, node in nodes.items():
        if not name.endswith(".gsc"):
            continue
        text = _blank_comments(rawfile_text(node))
        precached.update(c.group(2).strip(b'"&').lower() for c in _PRECACHE.finditer(text) if c.group(1).lower() == b"precachemodel")
        for m in _SET_MODEL.finditer(text):
            if m.group(1) not in set_by_name:
                set_by_name.append(m.group(1))
    for model in set_by_name:
        call = b'PrecacheModel( "' + model + b'" );'
        if model.lower() not in precached and model.decode("latin-1").lower() in loaded and call not in wanted:
            wanted.append(call)
    if not wanted:
        return []
    newline = b"\r\n" if b"\r\n" in level_text else b"\n"
    insert = newline + b"\t" + _PRECACHE_MARK + b"".join(newline + b"\t" + c for c in wanted) + newline
    set_rawfile_text(p, nodes[level_script], level_text[: body[0]] + insert + level_text[body[0] :])
    added = [c.decode("latin-1") for c in wanted]
    log(f"scripts: {len(added)} precaches of the setup {level_script} runs are made before its first wait too, as the console needs ({', '.join(added[:4])})")
    return added


# Mods choose their options in their own front end menus, which set dvars the map's scripts read:
# PhilMod's main menu sets its difficulty (philmod_gamemode 2, "Default") before the map loads. The
# console shows the game's menus, the dvar is never set and the map reads 0 (PhilMod's "Easy"). A dvar
# the map's scripts read that the map's menus set to one value only gets that value at the start of
# the level script, when nothing set it (the game's own dvars always have a value, so they keep it).
_MENU_SETDVAR = re.compile(r'"setdvar"\s+"(\w+)"\s+(?:"([^"]*)"|([^\s";]+))(\s*\()?', re.I)
# the game's own settings, which its menus set from the player's choices (cg_blood 0 is the PC's
# "disable mature content"): never the mod's options
_ENGINE_DVAR = re.compile(r"^(?:cg|ui|r|g|sv|cl|com|con|snd|bg|player|scr|xblive|party|dw|fs|net|sys|ai|compass|hud|in|vid|"
                          r"developer|onlinegame|systemlink|splitscreen|credits)(?:_|$)", re.I)  # fmt: skip
_GETDVAR = re.compile(rb'\bgetdvar(?:int|float)?\s*\(\s*"(\w+)"\s*\)', re.I)
_MENU_DVAR_MARK = b"// t4ff: the options the map's own menus set on PC, which the console does not show"


def script_dvars(p: Platform, zone: Zone) -> Set[str]:
    """The dvars the zone's scripts read by name (GetDvar( "name" ), lowercase)."""
    read = set()
    for name, node in _rawfiles(p, zone):
        if not name.startswith(",") and name.lower().endswith(SCRIPT_EXTENSIONS) and _buffer(node) is not None:
            read.update(m.group(1).decode("latin-1").lower() for m in _GETDVAR.finditer(_blank_comments(rawfile_text(node))))
    return read


def menu_dvar_values(zone: Zone) -> Dict[str, Set[str]]:
    """{dvar: values} the menus' scripts (every string of ``zone``) set with literal values."""
    values: Dict[str, Set[str]] = {}
    for node in zone.extra_root.walk():
        if not node.string:
            continue
        text = bytes(node.data).rstrip(b"\0").decode("latin-1")
        if "setdvar" not in text.lower():
            continue
        for m in _MENU_SETDVAR.finditer(text):
            if m.group(4):
                continue  # an expression ("dvarString" ( "other" ))
            value = m.group(2) if m.group(2) is not None else m.group(3)
            values.setdefault(m.group(1).lower(), set()).add(value)
    return values


def menu_dvar_defaults(p: Platform, zone: Zone, menu_values: Dict[str, Set[str]], level_script: str, log=print) -> Dict[str, str]:
    """The dvars the map's scripts read that its menus (``menu_values``, from menu_dvar_values before
    the front end menus go) set to one value get it at the start of the level script's main() when
    unset (see above). Returns {dvar: value}."""
    nodes = {normalize(name): node for name, node in _rawfiles(p, zone) if not name.startswith(",") and _buffer(node) is not None}
    level_script = level_script.lower()
    if level_script not in nodes or not menu_values:
        return {}
    level_text = rawfile_text(nodes[level_script])
    if _MENU_DVAR_MARK in level_text:
        return {}
    read = set()
    for name, node in nodes.items():
        if name.endswith(SCRIPT_EXTENSIONS):
            read.update(m.group(1).decode("latin-1").lower() for m in _GETDVAR.finditer(_blank_comments(rawfile_text(node))))
    defaults = {dvar: next(iter(values)) for dvar, values in sorted(menu_values.items())
                if dvar in read and len(values) == 1 and not _ENGINE_DVAR.match(dvar)}  # fmt: skip
    body = _function_body(level_text, "main")
    if not defaults or body is None:
        return {}
    newline = b"\r\n" if b"\r\n" in level_text else b"\n"
    lines = [b"\t" + _MENU_DVAR_MARK]
    for dvar, value in defaults.items():
        lines.append(f'\tif( GetDvar( "{dvar}" ) == "" )'.encode("latin-1"))
        lines.append(f'\t\tSetDvar( "{dvar}", "{value}" );'.encode("latin-1"))
    insert = newline + newline.join(lines) + newline
    set_rawfile_text(p, nodes[level_script], level_text[: body[0]] + insert + level_text[body[0] :])
    log(f"scripts: dvars the map's own menus set on PC get their value when unset ({', '.join(f'{d} {v}' for d, v in defaults.items())})")
    return defaults


# The map's options asked in game (menu.py, add_options_menus): the level script precaches their
# menus and, on a fresh start, asks each one as the player connects. A script opening a menu replaces
# the one open, so the level script's own start menus (PhilMod's weapon choice, which opens as the
# player connects too) wait for the answers: it opens menus through t4ff_open_menu(), which waits
# while the options are asked. A changed choice restarts the level with it (the restart asks
# nothing, and the map's menus open as usual); B keeps the choice.
_OPTIONS_MARK = b"// t4ff: the map's options"
_OPEN_MENU = re.compile(rb"(?<![\w:])OpenMenu(?=\s*\()", re.I)
_OPTIONS_FUNCTIONS = rb"""
// t4ff: the map's options, which the menus of its mod choose on PC, asked as it starts
t4ff_options()
{
	if( GetDvar( "t4ff_options_restart" ) == "1" )
	{
		SetDvar( "t4ff_options_restart", "0" );
		return;
	}
	players = GetPlayers();
	while( players.size == 0 )
	{
		wait( 0.05 );
		players = GetPlayers();
	}
	player = players[0];
	changed = false;
%(questions)s	if( changed )
	{
		SetDvar( "t4ff_options_restart", "1" );
		player OpenMenu( "%(restart)s" );
		return;
	}
	level.t4ff_asking = false;
}

t4ff_ask( player, menu_name, dvar )
{
	player OpenMenu( menu_name );
	for( ;; )
	{
		player waittill( "menuresponse", menu, response );
		if( menu == menu_name )
			break;
	}
	player CloseMenu( menu_name );
	if( response == "keep" || response == GetDvar( dvar ) )
		return false;
	SetDvar( dvar, response );
	return true;
}

// t4ff: the map's scripts open their menus once the options are asked
t4ff_open_menu( menu_name )
{
	while( IsDefined( level.t4ff_asking ) && level.t4ff_asking )
		wait( 0.05 );
	self OpenMenu( menu_name );
}
"""


def options_script(p: Platform, zone: Zone, level_script: str, options: Sequence[dict], menus: Sequence[str], log=print) -> bool:
    """The level script asks the map's options in game, and the map's scripts open their menus once
    they are asked (see above): ``menus`` are the options' menus, in their order, and the restart
    menu last (menu.add_options_menus)."""
    nodes = {normalize(name): node for name, node in _rawfiles(p, zone) if not name.startswith(",") and _buffer(node) is not None}
    level_script = level_script.lower()
    if level_script not in nodes or len(menus) != len(options) + 1:
        return False
    text = rawfile_text(nodes[level_script])
    body = _function_body(text, "main")
    if _OPTIONS_MARK in text or body is None:
        return False
    # the menus the level script opens wait for the options (a call of its own: a call from other
    # scripts into the level script is one more of the compiler's script references, whose number is
    # limited, "MAX_PRECACHE_ENTRIES exceeded": PhilMod's scripts are close to it)
    waiting = len(_OPEN_MENU.findall(_blank_comments(text)))
    text = _OPEN_MENU.sub(b"t4ff_open_menu", text)
    body = _function_body(text, "main")
    newline = b"\r\n" if b"\r\n" in text else b"\n"
    start = [b"\t" + _OPTIONS_MARK + b": asked as the level starts (t4ff_options)"]
    start += [f'\tPrecacheMenu( "{menu}" );'.encode("latin-1") for menu in menus]
    start.append(b'\tlevel.t4ff_asking = GetDvar( "t4ff_options_restart" ) != "1";')
    start.append(b"\tlevel thread t4ff_options();")
    questions = b"".join(f'\tif( t4ff_ask( player, "{menu}", "{option["dvar"]}" ) )\n\t\tchanged = true;\n'.encode("latin-1")
                         for menu, option in zip(menus, options))  # fmt: skip
    functions = _OPTIONS_FUNCTIONS % {b"questions": questions, b"restart": menus[-1].encode("latin-1")}
    text = text[: body[0]] + newline + newline.join(start) + newline + text[body[0] :]
    if not text.endswith(b"\n"):
        text += newline
    set_rawfile_text(p, nodes[level_script], text + functions.replace(b"\n", newline))
    log(f"scripts: {level_script} asks the map's options as it starts ({', '.join(o['label'] for o in options)}); "
        f"the {waiting} menus it opens wait for them")
    return True


# Treyarch's first zombie scripts (Nacht, and maps made with the first mod tools) hurry the last
# zombies of a round with speed_up_zombies(), which gives every axis AI the zombies' sprint: Dead
# Sand's SS soldiers then run as zombies. Only zombies are hurried.
_SPEED_UP = re.compile(rb"(zombie_stragglers\s*=\s*GetAiArray\s*\(\s*\"axis\"\s*\)\s*;\s*for\s*\([^)]*zombie_stragglers\.size[^)]*\)\s*\{)", re.I)
_SPEED_UP_FIX = b"// t4ff: only zombies"


def speed_up_zombies_only(p: Platform, zone: Zone, log=print) -> List[str]:
    """speed_up_zombies() hurries zombies only (see above). Returns the names of the scripts changed."""
    changed = []
    for name, node in _rawfiles(p, zone):
        if name.startswith(",") or not name.lower().endswith(".gsc") or _buffer(node) is None:
            continue
        text = rawfile_text(node)
        if b"zombie_stragglers" not in text or _SPEED_UP_FIX in text:
            continue
        newline = b"\r\n" if b"\r\n" in text else b"\n"
        fix = (newline + b"\t\t" + _SPEED_UP_FIX + newline
               + b"\t\tif( !IsDefined( zombie_stragglers[i].is_zombie ) || !zombie_stragglers[i].is_zombie )" + newline
               + b"\t\t\tcontinue;" + newline)
        new, count = _SPEED_UP.subn(lambda m: m.group(1) + fix, text)
        if count:
            set_rawfile_text(p, node, new)
            changed.append(normalize(name))
    if changed:
        log(f"scripts: speed_up_zombies() hurries zombies only, not the map's other AI ({', '.join(changed)})")
    return changed


# Treyarch's zombie modes give the zombies their idles by replacing the idles of the game's stand and
# crouch poses (init_animscripts()): every AI idles as a zombie then, and Dead Sand's soldiers (its
# commissars, marines and SS) stand with their arms out when they stop. On maps with soldiers the
# zombie idles get poses of their own ("zombie_stand", "zombie_crouch"), and a zombie's stop script
# plays them (the animscripts let a script run first as each AI stops, self.exception["stop_immediate"]:
# for zombies it is stop.gsc's own loop, with their idles, which killanimscript ends like the rest).
_ZOMBIE_IDLE = re.compile(rb'anim\.idleAnimArray\s*\[\s*"(?:stand|crouch)"\s*\]\s*\[\s*\d+\s*\]\s*\[\s*\d+\s*\]\s*=\s*%\s*ai_zombie_idle', re.I)
_IDLE_POSE = re.compile(rb'(anim\.idleAnim(?:Array|Weights)\s*\[\s*)"(stand|crouch)"', re.I)
_GENERIC_HUMAN = re.compile(rb'#using_animtree\s*\(\s*"generic_human"\s*\)', re.I)
_FUNCTION_HEADER = re.compile(rb"(?m)^[ \t]*([A-Za-z_]\w*)[ \t]*\([^)]*\)\s*\{")
_ZOMBIE_IDLES_MARK = b"t4ff_zombie_idles"
_ZOMBIE_IDLE_FUNCTIONS = rb"""
// t4ff: the zombies' idles are theirs only (the map has soldiers, who idled as zombies)
t4ff_zombie_idles()
{
	for( ;; )
	{
		ai = GetAiArray();
		for( i = 0; i < ai.size; i++ )
		{
			if( IsDefined( ai[i].is_zombie ) && ai[i].is_zombie && !IsDefined( ai[i].t4ff_idles ) && IsDefined( ai[i].exception ) )
			{
				ai[i].t4ff_idles = true;
				ai[i].exception[ "stop_immediate" ] = ::t4ff_zombie_stop;
			}
		}
		wait( 0.05 );
	}
}

// t4ff: animscripts\stop::main() for zombies, with their idles (killanimscript ends it)
t4ff_zombie_stop()
{
	animscripts\utility::initialize( "stop" );
	for( ;; )
	{
		pose = "stand";
		if( self animscripts\stop::getDesiredIdlePose() == "crouch" )
			pose = "crouch";
		if( self.a.pose != pose )
			self clearAnim( %root, 0.3 );
		self animscripts\SetPoseMovement::SetPoseMovement( pose, "stop" );
		sets = anim.idleAnimArray[ "zombie_" + pose ];
		idleSet = RandomInt( sets.size );
		idleAnim = animscripts\utility::anim_array( sets[ idleSet ], anim.idleAnimWeights[ "zombie_" + pose ][ idleSet ] );
		transTime = 0.2;
		if( GetTime() == self.a.scriptStartTime )
			transTime = 0.5;
		self setFlaggedAnimKnobAllRestart( "idle", idleAnim, %body, 1, transTime, self.animplaybackrate );
		self animscripts\shared::DoNoteTracks( "idle" );
	}
}
"""


def _actor_classnames(p: Platform, zone: Zone) -> Set[str]:
    """The classnames of the actors (AI spawners) of the zone's map entities."""
    names = set()
    for node in zone.extra_root.walk():
        if node.type.name != "MapEnts" or (node.extra.get("origin") or ("",))[0] != "asset":
            continue
        f = find_field(p.record("MapEnts"), "entityString")
        ptr = node.relocs.get(f.offset) if f is not None else None
        target = ptr.target() if ptr is not None else None
        if target is not None:
            text = bytes(target.data).rstrip(b"\0").decode("latin-1")
            names.update(m.lower() for m in re.findall(r'"classname"\s+"(actor_[^"]*)"', text))
    return names


def zombie_idles_for_zombies(p: Platform, zone: Zone, log=print) -> List[str]:
    """On maps with soldiers, only zombies idle as zombies (see above). Returns the names of the
    scripts changed."""
    if not any("zombie" not in name for name in _actor_classnames(p, zone)):
        return []
    changed = []
    for name, node in _rawfiles(p, zone):
        if name.startswith(",") or not name.lower().endswith(".gsc") or _buffer(node) is None:
            continue
        text = rawfile_text(node)
        if _ZOMBIE_IDLES_MARK in text or not _GENERIC_HUMAN.search(text):
            continue
        source = _blank_comments(text)
        m = _ZOMBIE_IDLE.search(source)
        if m is None:
            continue
        # the function setting the zombie idles
        headers = [h for h in _FUNCTION_HEADER.finditer(source) if h.start() < m.start()]
        body = _function_body(text, headers[-1].group(1).decode("latin-1")) if headers else None
        if body is None or not body[0] <= m.start() < body[1]:
            continue
        newline = b"\r\n" if b"\r\n" in text else b"\n"
        function = _IDLE_POSE.sub(rb'\1"zombie_\2"', text[body[0] : body[1]])
        function = newline + b"\tlevel thread t4ff_zombie_idles();" + function
        text = text[: body[0]] + function + text[body[1] :]
        if not text.endswith(b"\n"):
            text += newline
        set_rawfile_text(p, node, text + _ZOMBIE_IDLE_FUNCTIONS.replace(b"\n", newline))
        changed.append(normalize(name))
    if changed:
        log(f"scripts: the map has soldiers, and only its zombies idle as zombies ({', '.join(changed)})")
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
        # not renamed: the map's scripts folder has it (usermap_scripts), older CoD Xe builds run the game's
        if name.startswith(_ENGINE_RUN_SCRIPTS):
            log(f"scripts: {name} of the mod runs from the map's scripts folder: the engine runs it by name, and the game's own zones have one")
            continue
        game_callers = sorted(caller for caller, refs in calls.items() if name in refs and runs_game_copy[caller] and caller != name)
        if game_callers:
            log(f"scripts: {name} of the mod runs from the map's scripts folder: the game's own zones have one, which their {game_callers[0]} calls too")
            continue
        new = _free_name(name[: -len(".gsc")] + MOD_SCRIPT_SUFFIX, ".gsc", set(nodes) | set(calls), game)
        if not _rename_rawfile(p, zone, nodes[name], new):
            log(f"scripts: {name} of the mod runs from the map's scripts folder: its name is shared with other data of the zone")
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


# The mod's versions of scripts the game's zones have too, which keep_mod_scripts cannot rename (the
# game's own scripts call them too, or the engine runs them by name: PhilMod's _load, _gameskill,
# _laststand, _callbackglobal, animscripts/death, its client scripts...), go to the map's folder
# (usermaps/<map>/scripts/<name>), from which CoD Xe loads a script in place of the zones' one, as
# the PC loads a mod's loose scripts. Older CoD Xe builds run the game's.
USERMAP_SCRIPTS = "scripts"


def usermap_scripts(p: Platform, zone: Zone, mod_scripts: Set[str], console_library, renamed: Dict[str, str]) -> Dict[str, bytes]:
    """{name: text} of the mod's scripts (``mod_scripts``) the game's own zones have too and that
    differ, but the renamed ones (see above)."""
    if console_library is None:
        return {}
    scripts = {}
    for name, node in _rawfiles(p, zone):
        key = normalize(name)
        if name.startswith(",") or key not in mod_scripts or key in renamed or not key.endswith(SCRIPT_EXTENSIONS) or _buffer(node) is None:
            continue
        found = console_library.find_in_game_zones("RawFile", key)
        text = rawfile_text(node)
        if found is not None and _SPACE.sub(b"", rawfile_text(found[1])) != _SPACE.sub(b"", text):
            scripts[key] = text
    return scripts


def write_usermap_scripts(scripts: Dict[str, bytes], out_dir: str, log=print) -> List[str]:
    """Write ``scripts`` to ``out_dir``/scripts, replacing what an earlier conversion wrote there."""
    import os
    import shutil

    folder = os.path.join(out_dir, USERMAP_SCRIPTS)
    if os.path.isdir(folder):
        shutil.rmtree(folder)
    for name, text in sorted(scripts.items()):
        path = os.path.join(folder, *name.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(text)
    if scripts:
        log(f"scripts: {len(scripts)} of the mod's versions of the game's scripts go to {USERMAP_SCRIPTS}/, which CoD Xe loads in place of "
            f"the game's ({', '.join(sorted(scripts)[:6])}{', ...' if len(scripts) > 6 else ''})")
    return sorted(scripts)


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


def missing_scripts_zone(p: Platform, zone: Zone, sources: List, console_library=None, log=print,
                         roots: Sequence[str] = (), from_map: Optional[Set[str]] = None) -> Optional[Zone]:
    """A zone with the scripts ``zone``'s scripts use but no zone of the map has: from the map's own
    files (``sources[0]``, images.IwdLibrary), the console library, then other PC files
    (``sources[1:]``). ``roots``: scripts the engine loads by name for the map (its level script
    and client script), which no script names: taken from the map's own files when no zone has
    them (PhilMod's maps keep every script in an .iwd). A script the game's zones have too comes
    from the map's own files when they have it, as on PC; its name goes into ``from_map`` (for
    keep_mod_scripts)."""
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
    by_name: List[str] = []
    for ref in roots:
        ref = normalize(ref)
        data = sources[0].read(ref) if sources else None
        if defined.get(ref) is not None or data is None:
            continue
        node = make_rawfile(p, template, ref, data.rstrip(b"\0"))
        by_name.append(ref)
        defined[ref] = node
        added.append(("rawfile", asset_name(p, node), node))
        queue.append((ref, rawfile_text(node)))
    while queue:
        user, text = queue.pop()
        for ref in sorted(script_references(user, text)):
            if ref in defined or ref in unknown:
                continue
            in_game = console_library is not None and console_library.in_game_zones("RawFile", ref)
            if in_game and (not sources or sources[0].read(ref) is None):
                defined[ref] = None  # the game's own zones have it, the map has no copy of its own
                continue
            node, origin = finder.find(ref)
            if node is None:
                unknown[ref] = user
                continue
            if in_game and from_map is not None:
                from_map.add(ref)
            origins[ref] = origin
            defined[ref] = node
            added.append(("rawfile", asset_name(p, node), node))
            queue.append((ref, rawfile_text(node)))
    for ref, source in sorted(engine.items()):
        log(f"scripts: added {ref} (the game loads it for zombie maps, the map has none) from the {source}")
    for ref in by_name:
        log(f"scripts: added {ref} (the game loads it by name for the map, no zone of it has it) from the map's files")
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
