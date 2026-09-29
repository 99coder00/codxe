"""Shader arguments of copied technique sets, in the sections the console game reads them from.

A pass of a technique lists its shader arguments in three sections, one after the other: per
primitive, per object and stable (``perPrimArgCount``, ``perObjArgCount``, ``stableArgCount``). The
renderer sets the stable ones when it sets up the pass, and the per object ones before each object
it draws with it. Some of the textures the engine supplies (code samplers) are only valid per
object: the dynamic shadow texture (code sampler 0x12, which the sun lit techniques sample) is set
by the functions that draw models and world surfaces for the lit pass, right before their per
object arguments, and nowhere else. A pass that has it among its stable arguments reads whatever
the last object drawn left there: nothing when no lit object was drawn before it, and the game
stops with "Tried to use '(null)' when it isn't valid. Material='mc/berlin_window_browirglas',
tech='lp_sun_b0c0d0n0s0_dtex_sm3', techType=10" (The Simpsons, near a window of the house; the
texture name table of the console build has no name for it, hence '(null)').

The console game's own technique sets put each code argument in the same section every time (all
the zones of its disc: 21 zones, about 4000 techniques); the tables below are theirs. The technique
sets of CoD Xenon's maps put the dynamic shadow texture among the stable arguments, in every sun
lit technique of every one of their maps; no other argument is in another section than the game's.
Technique sets are copied from those fastfiles (see library.py), so the arguments of each pass are
moved to the section the game uses, where the game's own technique sets would have them: its
sections are ordered by argument type, code arguments of a type by register.
"""

from __future__ import annotations

from typing import Dict, List, Optional, Tuple

from .commands import find_field
from .zone import Node, Platform, Zone

PER_PRIM, PER_OBJECT, STABLE = 0, 1, 2

MTL_ARG_CODE_VERTEX_CONST = 3
MTL_ARG_CODE_PIXEL_SAMPLER = 4
MTL_ARG_CODE_PIXEL_CONST = 5


def _table(per_prim=(), per_object=(), stable=()) -> Dict[int, int]:
    table = {i: STABLE for i in stable}
    table.update({i: PER_OBJECT for i in per_object})
    table.update({i: PER_PRIM for i in per_prim})
    return table


# The section of each code argument (by argument type and code constant or texture index) in the
# console game's technique sets. Indices the game's technique sets never use are left where they are.
SECTIONS: Dict[int, Dict[int, int]] = {
    MTL_ARG_CODE_VERTEX_CONST: _table(
        per_prim=(0x3B, 0x3D, 0x64, 0x6B, 0x6C, 0x76, 0x77, 0x78, 0x7F, 0x87),
        per_object=(0x35, 0x36, 0x37, 0x39, 0x3A, 0x3C, 0x6F, 0x70, 0x73, 0x7B, 0x7C, 0x83),
        stable=(
            0x05, 0x06, 0x07, 0x0A, 0x12, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x24, 0x2A, 0x3E,
            0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x47, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54,
            0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x61, 0x62, 0x63, 0x8B,
        ),
    ),
    MTL_ARG_CODE_PIXEL_SAMPLER: _table(
        per_object=(0x09, 0x11, 0x12, 0x1A, 0x1B, 0x1C, 0x1D, 0x1F, 0x20, 0x21, 0x22, 0x23),
        stable=(0x01, 0x03, 0x06, 0x07, 0x08, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x13, 0x14, 0x16, 0x17, 0x18),
    ),
    MTL_ARG_CODE_PIXEL_CONST: _table(
        stable=(
            0x00, 0x01, 0x02, 0x03, 0x04, 0x08, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x11, 0x12, 0x13, 0x14,
            0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x23, 0x24, 0x25,
            0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x44, 0x45, 0x48,
            0x49, 0x4A, 0x4B, 0x4C, 0x60, 0x63, 0x65, 0x66, 0x67, 0x68,
        ),
    ),
}  # fmt: skip

_COUNT_FIELDS = ("perPrimArgCount", "perObjArgCount", "stableArgCount")


def argument_section(p: Platform, arg: bytes) -> Optional[int]:
    """The section the console game has the shader argument ``arg`` in (None: any)."""
    arg_type = p.u16.unpack_from(arg, 0)[0]
    table = SECTIONS.get(arg_type)
    if table is None:
        return None
    # u.codeSampler (a texture index) or u.codeConst.index
    index = p.u32.unpack_from(arg, 4)[0] if arg_type == MTL_ARG_CODE_PIXEL_SAMPLER else p.u16.unpack_from(arg, 4)[0]
    return table.get(index)


def sort_sections(p: Platform, args: List[bytes], counts: Tuple[int, int, int]) -> Optional[Tuple[List[bytes], Tuple[int, int, int]]]:
    """The arguments of a pass (``counts`` per section) with the code arguments moved to the sections
    the game uses, or None when they already are."""
    sections: List[List[bytes]] = [[], [], []]
    moved = []
    start = 0
    for section, count in enumerate(counts):
        for arg in args[start : start + count]:
            want = argument_section(p, arg)
            if want is not None and want != section:
                moved.append((want, arg))
            else:
                sections[section].append(arg)
        start += count
    if not moved:
        return None

    def key(arg: bytes):
        return p.u16.unpack_from(arg, 0)[0], p.u16.unpack_from(arg, 2)[0]  # type, then register

    for section, arg in moved:
        items = sections[section]
        position = next((i for i, other in enumerate(items) if key(other) > key(arg)), len(items))
        items.insert(position, arg)
    return [a for items in sections for a in items], tuple(len(items) for items in sections)


def fix_argument_sections(p: Platform, zone: Zone, log=print) -> int:
    """Move the shader arguments of the zone's technique sets to the sections the game uses (see
    above). Returns the number of passes changed."""
    tech_rec = p.record("MaterialTechnique")
    pass_rec = p.record("MaterialPass")
    pass_count_off = find_field(tech_rec, "passCount").offset
    pass_array_off = find_field(tech_rec, "passArray").offset
    count_offs = [find_field(pass_rec, f).offset for f in _COUNT_FIELDS]
    args_off = find_field(pass_rec, "args").offset
    arg_size = p.record("MaterialShaderArgument").size

    # passes by the arguments they read (an argument array could be shared by passes): the
    # arguments, where they start in it, and (technique, pass offset) of each pass
    passes: Dict[Tuple[int, int], Tuple[Node, int, List[Tuple[Node, int]]]] = {}
    names: Dict[Tuple[int, int], str] = {}
    for node in zone.extra_root.walk():
        if node.string or node.type.name != "MaterialTechnique":
            continue
        for k in range(p.u16.unpack_from(node.data, pass_count_off)[0]):
            po = pass_array_off + k * pass_rec.size
            ptr = node.relocs.get(po + args_off)
            target = ptr.target() if ptr is not None else None
            if target is None:
                continue
            start = ptr.index * target.elem_size + ptr.inner if ptr.kind == "ref" else 0
            key = (id(target), start)
            passes.setdefault(key, (target, start, []))[2].append((node, po))
            names.setdefault(key, _technique_name(p, node))

    changed = 0
    examples = []
    for key, (args_node, start, users) in passes.items():
        layouts = {tuple(node.data[po + off] for off in count_offs) for node, po in users}
        if len(layouts) != 1:
            log(f"warning: technique '{names[key]}': passes sharing arguments count them differently, left as they are")
            continue
        counts = layouts.pop()
        total = sum(counts)
        if start + total * arg_size > len(args_node.data):
            continue
        args = [bytes(args_node.data[start + i * arg_size : start + (i + 1) * arg_size]) for i in range(total)]
        result = sort_sections(p, args, counts)
        if result is None:
            continue
        new_args, new_counts = result
        args_node.data[start : start + total * arg_size] = b"".join(new_args)
        for node, po in users:
            for off, count in zip(count_offs, new_counts):
                node.data[po + off] = count
        changed += len(users)
        if len(examples) < 3 and names[key] not in examples:
            examples.append(names[key])
    if changed:
        log(f"technique sets: shader arguments of {changed} passes moved to the sections the game reads them from (e.g. {', '.join(examples)})")
    return changed


def _technique_name(p: Platform, node: Node) -> str:
    ptr = node.relocs.get(find_field(p.record("MaterialTechnique"), "name").offset)
    target = ptr.target() if ptr is not None else None
    return bytes(target.data[:-1]).decode("latin-1") if target is not None and target.string else "?"
