"""Generate the structure layouts and zone code commands the native t4ff embeds.

Run from tools/t4ff (it uses the Python t4ff, with libclang when its layout cache is stale):

    python native/tools/gen_schema.py

Writes native/data/layout_pc.json, layout_x360.json (the exact MSVC x86 layout of every T4
structure, from OpenAssetTools' T4_Assets.h and t4ff's console overrides) and commands_pc.txt,
commands_x360.txt (OpenAssetTools' T4 zone code commands with their includes inlined, then
t4ff's fixes and console commands, in the order the Python t4ff parses them). The native build
compiles them into the executable, so it needs no Python, libclang or OpenAssetTools checkout.
Run it again whenever those files change.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
T4FF = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, T4FF)

from t4ff.commands import OAT_T4_COMMANDS  # noqa: E402
from t4ff.layout import load_layout  # noqa: E402
from t4ff.platforms import PC_FIXES, X360_COMMANDS  # noqa: E402

DATA = os.path.join(HERE, "..", "data")


def flatten(path: str) -> str:
    """A command file with its #include lines replaced by the included files (recursively)."""
    base = os.path.dirname(path)
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f.read().splitlines():
            m = re.match(r'\s*#include\s+"([^"]+)"', line)
            if m:
                out.append(flatten(os.path.join(base, m.group(1))))
            else:
                out.append(line)
    return "\n".join(out)


def main():
    os.makedirs(DATA, exist_ok=True)
    for platform in ("pc", "x360"):
        layout = load_layout(platform)
        with open(os.path.join(DATA, f"layout_{platform}.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(layout.to_json(), f, separators=(",", ":"))
        parts = [flatten(OAT_T4_COMMANDS), flatten(PC_FIXES)]
        if platform == "x360":
            parts.append(flatten(X360_COMMANDS))
        with open(os.path.join(DATA, f"commands_{platform}.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(parts) + "\n")
    print("wrote", ", ".join(sorted(os.listdir(DATA))))


if __name__ == "__main__":
    main()
