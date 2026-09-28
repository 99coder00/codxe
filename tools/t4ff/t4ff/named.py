"""Assets the game looks up by name while a map starts.

Most assets of a map are loaded because others point to them. Some the game looks up by name
instead, and it reports those no zone has ("Could not load xanim", "couldn't open
'shock/zombie_death.shock'"). PC maps lack some of them too, and so do CoD Xenon's conversions of
those maps:

- the player body animations the player animation script (``mp/playeranim.script`` of
  ``common.ff``) lists: PC Aztec has no satchel ones (``pb_hold_run_satchel``, ...), CoD Xenon's
  ``zm_tranzit`` and ``zm_terminus`` have them;
- the shellshock files the scripts use (``shock/<name>.shock``): the one played when a player dies,
  ``zombie_death`` (``level.player_killed_shellshock``), is in no zone of PC Aztec; several of CoD
  Xenon's maps have it. Without it the game rejects its settings
  ("'0' is not a valid value for dvar 'bg_shock_viewKickPeriod'");
- the animations of the anim trees the scripts use (``animtrees/<name>.atr``, ``#using_animtree``):
  the PC game's own zones have the dogs' (``german_shepherd_run``, its window jumps, ...), so a PC
  map's zones name none of them, and neither do the console's; CoD Xenon's ``zm_tranzit`` has them.
  Without them the dogs of the dog rounds cannot run, jump through windows nor feel pain.

They are taken from the map's files or the console fastfiles given, when they have them and the
game's own zones (``common.ff`` among the console fastfiles) do not.
"""

from __future__ import annotations

import re
from typing import List, Optional, Set, Tuple

from .scripts import SCRIPT_EXTENSIONS, RawfileFinder, _rawfiles, rawfile_text, zone_of_assets
from .zone import Node, Platform, Zone, asset_name

PLAYER_ANIM_SCRIPT = "mp/playeranim.script"

_LITERAL = re.compile(r'"(\w+)"')
_ANIM_LINE = re.compile(r"^\s*(?:both|legs|torso|turret)\s+(\w+)", re.M)
_COMMENTS = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
_USING_ANIMTREE = re.compile(r'#using_animtree\s*\(\s*"(\w+)"\s*\)')
_ATR_TOKEN = re.compile(r"[{}]|[\w.]+(?:\s*:\s*[\w ]+)?")


def _without_block(source: str, name: str) -> str:
    """``source`` without its top level block ``name { ... }``."""
    m = re.search(rf"^\s*{name}\s*{{", source, re.M | re.I)
    if m is None:
        return source
    depth, i = 1, m.end()
    while i < len(source) and depth:
        depth += {"{": 1, "}": -1}.get(source[i], 0)
        i += 1
    return source[: m.start()] + source[i:]


def player_animations(text: bytes) -> List[str]:
    """The animations a player animation script plays as players move, in order. Those of its
    ``scriptevent`` block (campaign vehicle rides, 2 MiB of animations) are left out: zombie
    maps do not play them."""
    source = _without_block(_COMMENTS.sub("", text.decode("latin-1")), "scriptevent")
    return list(dict.fromkeys(m.group(1).lower() for m in _ANIM_LINE.finditer(source)))


def anim_tree_animations(text: bytes) -> List[str]:
    """The animations an anim tree (``animtrees/<name>.atr``) names, in order: its leaves; a name
    followed by a block is a blend node (``german_shepherd_look_2 : additive { ... }``)."""
    tokens = [t for t in _ATR_TOKEN.findall(_COMMENTS.sub("", text.decode("latin-1")))]
    names = []
    for i, token in enumerate(tokens):
        if token in "{}":
            continue
        if i + 1 < len(tokens) and tokens[i + 1] == "{":
            continue  # a blend node
        names.append(token.split(":")[0].strip().lower())
    return list(dict.fromkeys(names))


def anim_trees_used(scripts: List[Tuple[str, bytes]]) -> List[str]:
    """``animtrees/<name>.atr`` of every ``#using_animtree`` of the scripts."""
    names = set()
    for _, text in scripts:
        names.update(m.group(1).lower() for m in _USING_ANIMTREE.finditer(_COMMENTS.sub("", text.decode("latin-1"))))
    return [f"animtrees/{n}.atr" for n in sorted(names)]


def shellshock_candidates(scripts: List[Tuple[str, bytes]]) -> List[str]:
    """``shock/<name>.shock`` for every string of the scripts: a shellshock can be named anywhere
    (``level.player_killed_shellshock = "zombie_death"``) before it is played."""
    names = set()
    for _, text in scripts:
        names.update(m.group(1).lower() for m in _LITERAL.finditer(_COMMENTS.sub("", text.decode("latin-1"))))
    return [f"shock/{n}.shock" for n in sorted(names)]


def named_assets_zone(p: Platform, zone: Zone, sources: List, console_library=None, log=print) -> Optional[Zone]:
    """A zone with the assets the game looks up by name that neither ``zone`` nor the game's own
    zones have, from the map's files (``sources[0]``), the console library, then other PC files."""
    defined: Set[Tuple[str, str]] = set()
    scripts: List[Tuple[str, bytes]] = []
    template = None
    for node in zone.extra_root.walk():
        origin = node.extra.get("origin")
        if origin and origin[0] == "asset":
            name = asset_name(p, node) or ""
            defined.add((origin[1], name.lstrip(",").lower()))
    for name, node in _rawfiles(p, zone):
        if name.startswith(","):
            continue
        template = template or node
        if name.lower().endswith(SCRIPT_EXTENSIONS):
            scripts.append((name, rawfile_text(node)))
    if template is None:
        return None

    def wanted(rec: str, name: str) -> bool:
        if (rec, name.lower()) in defined:
            return False
        return console_library is None or not console_library.in_game_zones(rec, name)

    finder = RawfileFinder(p, template, sources, console_library)
    added: List[Tuple[str, str, Node]] = []

    shocks = []
    for ref in shellshock_candidates(scripts):
        if not wanted("RawFile", ref):
            continue
        node, _ = finder.find(ref)
        if node is not None:
            added.append(("rawfile", ref, node))
            shocks.append(ref)
    if shocks:
        log(f"shellshocks: added {', '.join(shocks)} (used by the map's scripts, in none of its zones)")

    # the animations of the anim trees: the map's own trees, else those the game's zones have
    if console_library is not None:
        from .library import LibraryError

        trees = {name.lower(): rawfile_text(node) for name, node in _rawfiles(p, zone) if not name.startswith(",") and name.lower().endswith(".atr")}
        for tree in anim_trees_used(scripts):
            if tree not in trees:
                found = console_library.find_in_game_zones("RawFile", tree)
                if found is not None:
                    trees[tree] = rawfile_text(found[1])
        taken, missing = {}, []
        for tree, text in sorted(trees.items()):
            for anim in anim_tree_animations(text):
                if anim in taken or not wanted("XAnimParts", anim):
                    continue
                found = console_library.find("XAnimParts", anim)
                if found is None:
                    missing.append(anim)
                    continue
                try:
                    node = finder.cloner.copy_asset(*found)
                except LibraryError:
                    missing.append(anim)
                    continue
                added.append(("xanim", anim, node))
                defined.add(("XAnimParts", anim))
                taken[anim] = tree
        for tree in sorted(set(taken.values())):
            anims = [a for a, t in taken.items() if t == tree]
            log(f"animations: added {len(anims)} of {tree} (the game loads them by name, none of the map's zones nor the game's have them: {', '.join(anims[:4])}{', ...' if len(anims) > 4 else ''})")
        if missing:
            log(f"animations: {len(missing)} the map's anim trees name are in none of the console fastfiles given (e.g. {', '.join(missing[:4])}): the game reports them (\"Could not load xanim\") and plays none")

    # the player animation script: the map's own, else the game's (common.ff among the console
    # fastfiles; not another map's, which may have changed it)
    script = next((rawfile_text(node) for name, node in _rawfiles(p, zone) if name.lower() == PLAYER_ANIM_SCRIPT), None)
    if script is None and console_library is not None:
        found = console_library.find_in_game_zones("RawFile", PLAYER_ANIM_SCRIPT)
        script = rawfile_text(found[1]) if found is not None else None
    if script is not None and console_library is not None:
        from .library import LibraryError

        anims = []
        for anim in player_animations(script):
            if not wanted("XAnimParts", anim):
                continue
            found = console_library.find("XAnimParts", anim)
            if found is None:
                continue
            try:
                node = finder.cloner.copy_asset(*found)
            except LibraryError:
                continue
            added.append(("xanim", anim, node))
            anims.append(anim)
        if anims:
            log(f"player animations: added {len(anims)} the player animation script plays, in none of the map's zones nor the game's ({', '.join(anims[:4])}{', ...' if len(anims) > 4 else ''})")
    return zone_of_assets(p, [(t, asset_name(p, n), n) for t, _, n in added], finder.strings) if added else None
