"""The memory a converted map may use on the console.

CoD Xenon's 13 maps of their 0.2.0 release load with 148 to 220 MiB (their memory once loaded,
as ``t4ff`` reports it for its own fastfiles): textures up to 99 MiB (``nazi_zombie_sc_dh2``),
loaded sounds up to 35 MiB, the rest (world, models, animations...) up to 116 MiB
(``nazi_zombie_leviathan``). CoD Xe does not change the game's memory, so these are the
known good values.

By default (``--texture-budget auto``) a map's textures get the memory the target leaves: a map
whose textures fit keeps them all at full quality, a bigger one has its largest textures lose
their top mip levels until it fits.
"""

from __future__ import annotations

import struct
from typing import List, Optional

from .zone import Zone

MIB = 1024 * 1024

# the total a map may use: 6 of CoD Xenon's 13 maps need more (up to 219.5 MiB)
MEMORY_TARGET_MIB = 200
# textures: CoD Xenon's maps have up to 99.3 MiB
TEXTURE_CAP_MIB = 96
# below this the textures of a map would look too blurry to be worth it: the target is exceeded
MIN_TEXTURE_BUDGET_MIB = 24
# room left under the target when the budget is recomputed (the planned sizes are estimates)
MARGIN_MIB = 1


def block_sizes(out: bytes) -> List[int]:
    """The block sizes of a written (uncompressed) zone, from its header."""
    return list(struct.unpack_from(">7I", out, 8))


def texture_bytes(zone: Zone) -> int:
    """Memory of the textures of a console zone (their pixel data)."""
    return sum(len(n.data) for n in zone.extra_root.walk() if n.extra.get("delayed"))


def next_texture_budget(total: int, target: int, textures: int, budget: int) -> Optional[int]:
    """The texture budget for another conversion when ``total`` (converted with ``budget``, which
    planned ``textures`` of texture memory) is over ``target``; None when it fits or the textures
    cannot shrink further."""
    if total <= target:
        return None
    new = min(budget, textures) - (total - target) - MARGIN_MIB * MIB
    floor = MIN_TEXTURE_BUDGET_MIB * MIB
    if new < floor:
        new = floor
    return new if new < budget else None
