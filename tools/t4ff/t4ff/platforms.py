"""Platform descriptions (PC and Xbox 360)."""

from __future__ import annotations

import functools
import os

from .commands import CommandParser, load_commands
from .layout import TOOL_DIR, load_layout
from .zone import (
    BLOCK_LARGE,
    BLOCK_PHYSICAL,
    BLOCK_TEMP,
    BLOCK_VIRTUAL,
    PC_ASSET_TYPES,
    X360_ASSET_TYPES,
    Platform,
)

X360_COMMANDS = os.path.join(TOOL_DIR, "defs", "x360_commands.txt")
PC_FIXES = os.path.join(TOOL_DIR, "defs", "pc_commands.txt")


@functools.lru_cache(maxsize=None)
def pc() -> Platform:
    layout = load_layout("pc")
    parser = CommandParser(layout)
    from .commands import OAT_T4_COMMANDS

    parser.parse_file(OAT_T4_COMMANDS)
    parser.parse_file(PC_FIXES)
    return Platform("pc", "<", layout, parser.cmds, PC_ASSET_TYPES, [BLOCK_TEMP, BLOCK_VIRTUAL, BLOCK_LARGE, BLOCK_PHYSICAL])


@functools.lru_cache(maxsize=None)
def x360() -> Platform:
    layout = load_layout("x360")
    parser = CommandParser(layout, lenient=True)
    from .commands import OAT_T4_COMMANDS

    parser.parse_file(OAT_T4_COMMANDS)
    parser.parse_file(PC_FIXES)
    if os.path.exists(X360_COMMANDS):
        parser.parse_file(X360_COMMANDS)
    return Platform("x360", ">", layout, parser.cmds, X360_ASSET_TYPES, [BLOCK_TEMP, BLOCK_VIRTUAL, BLOCK_LARGE, BLOCK_PHYSICAL])


def for_endian(endian: str) -> Platform:
    return pc() if endian == "<" else x360()
