"""Keeping a map within the console's loaded sound limit.

The console holds at most 1600 loaded sounds, those of the zones the game loads before the map
included; a map with more stops loading with "Exceeded limit of 1600 'loaded_sound' assets". CoD
Xenon's Aztec (1532 loaded sounds) loads, the whole PC Aztec (1576) does not. To stay within a
budget:

1. identical sounds (e.g. ``impacts/flesh/flesh_00`` and ``impacts//flesh/flesh_00``) are loaded
   once, the aliases share it (as nested assets shared between zones are);
2. then the longest sounds become streamed sounds, as CoD Xenon did with the tank sounds of Aztec:
   the alias plays ``sounds\\<dir>\\<name>.xma`` from the map folder (CoD Xe serves the map's
   ``sounds`` folder), which this writes, and the sound no longer takes memory in the fastfile.
"""

from __future__ import annotations

import hashlib
import os
from typing import Dict, List, Optional

from .commands import find_field
from .layout import TypeRef
from .merge import dedupe_nested_assets
from .zone import Node, Platform, Ptr, Zone, asset_name

LOADED_SOUND_LIMIT = 1600
# Room left for the loaded sounds of the zones loaded before the map (fewer than 68: CoD Xenon's
# Aztec loads with 1532, the PC one with 1576 does not).
DEFAULT_MAX_LOADED_SOUNDS = 1500

SAT_STREAMED = 2


def _sound_content(node: Node) -> bytes:
    """Digest of a loaded sound's data (its format block, seek table and XMA packets), name aside."""
    h = hashlib.sha1()
    for n in node.walk():
        if not n.string:
            h.update(bytes(n.data))
    return h.digest()


def _loaded_sounds(p: Platform, zone: Zone) -> List[Node]:
    return [n for n in zone.extra_root.walk() if (n.extra.get("origin") or ("",))[0] == "asset" and n.type.name == "LoadedSound"]


def _string_node(text: str, block: int) -> Node:
    node = Node(TypeRef("scalar", "char", 1, 1), 0, block)
    node.string = True
    node.data = bytearray(text.encode("latin-1") + b"\0")
    node.count = len(node.data)
    node.segments = [(node.type, node.count, node.count, False)]
    node.extra["align"] = 1
    return node


def _pointer(kind: str, owner: Node, offset: int, node: Optional[Node] = None) -> Ptr:
    ptr = Ptr(kind, node)
    ptr.owner = owner
    ptr.offset = offset
    return ptr


ALIAS_TYPE_SHIFT = 13  # snd_alias_t.flags bits 13-14: the type of the alias' sound file
ALIAS_TYPE_MASK = 3 << ALIAS_TYPE_SHIFT


def sync_alias_types(p: Platform, zone: Zone) -> int:
    """Set the sound type in the flags of every alias to the type of its sound file.

    The engine takes the type from the flags (bits 13-14; in every alias of PC Aztec and of CoD
    Xenon's 13 maps they equal SoundFile.type): an alias still flagged loaded whose sound file is
    streamed has its stream file name read as a loaded sound pointer when it plays. Returns the
    number of aliases changed.
    """
    rec = p.record("snd_alias_t")
    flags_off = find_field(rec, "flags").offset
    file_off = find_field(rec, "soundFile").offset
    changed = 0
    for node in zone.extra_root.walk():
        if node.type.name != "snd_alias_t":
            continue
        for i in range(node.count):
            base = i * rec.size
            ptr = node.relocs.get(base + file_off)
            sound_file = ptr.target() if ptr is not None and ptr.kind != "null" else None
            if sound_file is None or not sound_file.data or sound_file.data[0] not in (1, 2, 3):
                continue
            flags = p.u32.unpack_from(node.data, base + flags_off)[0]
            wanted = (flags & ~ALIAS_TYPE_MASK) | (sound_file.data[0] << ALIAS_TYPE_SHIFT)
            if wanted != flags:
                p.u32.pack_into(node.data, base + flags_off, wanted)
                changed += 1
    return changed


def limit_loaded_sounds(p: Platform, zone: Zone, limit: int, streams: Dict[str, object], sounds_dir: str, log=print) -> dict:
    """Bring the loaded sounds of ``zone`` down to ``limit``. ``streams`` gives the XMA2 stream
    (audio.XmaStream) of each converted loaded sound by name, for the ones to stream."""
    from .audio import write_sdns

    stats = {"shared": 0, "streamed": 0, "stream_bytes": 0}

    def same_sound(record, name, node):
        return ("LoadedSound", _sound_content(node)) if record == "LoadedSound" else None

    stats["shared"] = dedupe_nested_assets(p, zone, log=lambda msg: None, key_of=same_sound)
    sounds = _loaded_sounds(p, zone)
    count = len(sounds)
    if stats["shared"]:
        log(f"loaded sounds: {stats['shared']} identical sounds shared")
    if count <= limit:
        stats["count"] = count
        return stats

    rec = p.record("SoundFile")
    u = find_field(rec, "u").offset
    ss = p.record("StreamedSound")
    sfn = p.record("StreamFileName")
    fn = u + find_field(ss, "filename").offset
    hash_off = fn + find_field(sfn, "hash").offset
    dir_off = fn + find_field(sfn, "dir").offset
    name_off = fn + find_field(sfn, "name").offset
    prime_off = u + find_field(ss, "primeSnd").offset

    # who points where: the pointer loading each sound, aliases to it, pointers into it
    loader: Dict[int, Ptr] = {}
    aliases: Dict[int, List[Ptr]] = {}
    incoming: Dict[int, List[Node]] = {}
    for n in zone.extra_root.walk():
        for ptr in n.relocs.values():
            if ptr.kind in ("follow", "insert") and ptr.node is not None:
                loader[id(ptr.node)] = ptr
            elif ptr.kind == "alias" and ptr.slot is not None:
                aliases.setdefault(id(ptr.slot), []).append(ptr)
                if ptr.slot.owner is not None:
                    incoming.setdefault(id(ptr.slot.owner), []).append(n)
            elif ptr.kind == "ref" and ptr.node is not None:
                incoming.setdefault(id(ptr.node), []).append(n)

    def sound_files(sound: Node) -> Optional[List[Node]]:
        """The SoundFile entries playing ``sound``, None when something else refers to it."""
        first = loader.get(id(sound))
        if first is None or first.owner is None or first.owner.type.name != "SoundFile" or first.offset != u:
            return None
        inside = {id(x) for x in sound.walk()}
        if any(id(src) not in inside for target in sound.walk() for src in incoming.get(id(target), ())):
            return None
        owners = [first.owner]
        for alias in aliases.get(id(first), ()):
            if alias.owner is None or alias.owner.type.name != "SoundFile" or alias.offset != u:
                return None
            owners.append(alias.owner)
        return owners

    def duration(sound: Node) -> float:
        stream = streams.get(asset_name(p, sound).lstrip(",").lower())
        return stream.valid_samples / stream.rate if stream is not None and stream.rate else -1.0

    candidates = sorted((s for s in sounds if duration(s) > 0), key=duration, reverse=True)
    for sound in candidates:
        if count <= limit:
            break
        owners = sound_files(sound)
        if owners is None:
            continue
        name = asset_name(p, sound).lstrip(",")
        stream = streams[name.lower()]
        parts = name.replace("\\", "/").split("/")
        directory = "\\".join(["sounds"] + parts[:-1])
        stem = parts[-1]
        target = os.path.join(sounds_dir, "sounds", *parts[:-1], stem + ".xma")
        os.makedirs(os.path.dirname(target), exist_ok=True)
        data = write_sdns(stream)
        with open(target, "wb") as f:
            f.write(data)
        for sf in owners:
            old = sf.relocs.pop(u)
            if old.kind in ("follow", "insert"):
                sf.children = [c for c in sf.children if c is not old.node]
            sf.data[0] = SAT_STREAMED
            p.u32.pack_into(sf.data, hash_off, 0)  # 0: a file of the map (served from its sounds folder)
            p.u32.pack_into(sf.data, dir_off, 0xFFFFFFFF)
            p.u32.pack_into(sf.data, name_off, 0xFFFFFFFF)
            p.u32.pack_into(sf.data, prime_off, 0)
            for off, text in ((dir_off, directory), (name_off, stem)):
                node = _string_node(text, sf.block)
                sf.relocs[off] = _pointer("follow", sf, off, node)
                sf.children.append(node)
            sf.relocs[prime_off] = _pointer("null", sf, prime_off)
        count -= 1
        stats["streamed"] += 1
        stats["stream_bytes"] += len(data)
    # the aliases carry the type of their sound file too
    sync_alias_types(p, zone)
    stats["count"] = count
    log(
        f"loaded sounds: {stats['streamed']} of the longest are streamed from the map's sounds folder "
        f"({stats['stream_bytes'] / 1048576:.1f} MiB) to stay within {limit} loaded sounds"
        + ("" if count <= limit else f"; {count} remain, more than the limit: the map may not load")
    )
    return stats
