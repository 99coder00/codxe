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
  ("'0' is not a valid value for dvar 'bg_shock_viewKickPeriod'").

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
