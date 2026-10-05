"""The memory a converted map may use on the console.

The game allocates every block of a map's zone when it loads it, the virtual one (models, animations,
the world, scripts) as well as the textures, from its "main" physical memory (Title Update 7's
0x821677A0 asks PMem for each block in turn), which has about 286.7 MiB free for them then. A
map that does not fit stops at the first block that does not: "Need <n> more bytes of 'main'
physical ram for alloc to succeed", n being what the blocks up to that one take beyond the free
memory. Measured in Xenia: Kino Der Toten's 278.5 MiB (168.8 MiB of textures, 83.9 virtual, 24.2
large, 1.5 physical) loads and plays; with 193.1 MiB of textures its large block was 15300623 bytes
short (286.7 MiB); Kino Rezurrection's 293.6 MiB (144.7 textures, 126.3 virtual, 20.1 large, 1.6
physical, 0.9 runtime) was 5494799 bytes short at its large block (286.7 MiB again). An earlier
reading counted the textures, large and physical blocks only (202.7 MiB): it held for Kino, whose
virtual block was about the same in every conversion.

Those numbers are Xenia's: its patch for the game ("Memory allocator expansion", on by default) makes
PMem_Init (Title Update 7 0x822915D2/0x822915E2) reserve 0x1E000000 bytes (480 MB) instead of the
game's 0x19E00000 (414 MB). A console has 66 MiB less, about 220.7 MiB for a map's zone, so the
default target is a console's: Kino Der Toten (212.8 MiB) loads on both; a map converted with
``--memory-target 278`` loads in Xenia only.

By default (``--texture-budget auto``) a map's textures get the memory the target leaves: a map whose
textures fit keeps them all at full quality. A bigger one gives way in this order, each step only
when the one before was not enough: the PC versions of stock textures (``--upgrade-budget``), the
textures' packed mip tails (``--keep-mip-tail``), then what the textures keep in the fastfile, counted
as the converted map streams them (assets.choose_drops): deep streamed textures keep an eighth of
their size instead of a quarter (``--keep-quarter``), then textures lose top levels, the one saving the
most first (the textures kept whole there, effects and small ones, before the streamed ones; 2D ones
and the PC versions of stock textures last). Kino Rezurrection (a console's 212 MiB, 139 MiB of it
not textures): 159 of 1530 textures a level smaller, 880 streamed in two or three levels.
"""

from __future__ import annotations

import struct
from typing import List, Optional

from .zone import Zone

MIB = 1024 * 1024

# the main memory a map's zone may use on a console (about 220.7 MiB free when it loads, see above),
# leaving room for what the game allocates there while it plays
MEMORY_TARGET_MIB = 212
# the blocks allocated from it: all of them (temp, runtime, large runtime, physical runtime, virtual,
# large, physical)
MEMORY_BLOCKS = tuple(range(7))
# what the game has free for a map's zone when it loads it (MiB): in Xenia (its patch makes the game's
# memory pool 66 MiB larger), and on a console
FREE_MIB = 286.7
XENIA_EXTRA_MIB = (0x1E000000 - 0x19E00000) / (1024 * 1024)
FREE_CONSOLE_MIB = FREE_MIB - XENIA_EXTRA_MIB
# below this the textures of a map would look too blurry to be worth it: the target is exceeded
MIN_TEXTURE_BUDGET_MIB = 24
# room left under the target when the budget is recomputed (the planned sizes are estimates)
MARGIN_MIB = 1
# the least memory a byte less of planned textures is taken to save (a deep streamed texture's)
MIN_EFFICIENCY = 1 / 16


def block_sizes(out: bytes) -> List[int]:
    """The block sizes of a written (uncompressed) zone, from its header."""
    return list(struct.unpack_from(">7I", out, 8))


def memory_bytes(sizes: List[int]) -> int:
    """The main memory a zone's blocks take (see above)."""
    return sum(sizes[i] for i in MEMORY_BLOCKS)


def texture_bytes(zone: Zone) -> int:
    """Memory of the textures of a console zone (their pixel data)."""
    return sum(len(n.data) for n in zone.extra_root.walk() if n.extra.get("delayed"))


def next_texture_budget(total: int, target: int, textures: int, budget: int, efficiency: float = 1.0) -> Optional[int]:
    """The texture budget for another conversion when ``total`` (converted with ``budget``, which
    planned ``textures`` of texture memory) is over ``target``; None when it fits or the textures
    cannot shrink further.

    ``efficiency``: the memory a byte less of planned textures saves. A streamed texture keeps a
    quarter of its size in the fastfile, a deep streamed one a sixteenth: Kino Rezurrection's largest
    textures are, and a planned budget 1.7 MiB smaller saved 0.1 MiB."""
    if total <= target:
        return None
    new = min(budget, textures) - int((total - target + MARGIN_MIB * MIB) / max(efficiency, MIN_EFFICIENCY))
    floor = MIN_TEXTURE_BUDGET_MIB * MIB
    if new < floor:
        new = floor
    return new if new < budget else None
