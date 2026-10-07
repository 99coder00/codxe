"""Checks that t4ff.exe (the native window) gives the Python window's command lines.

    python native/tools/gui_args_check.py [t4ff.exe]

For settings saved by the Python window (gui.json, several variants), the native window imports them
(--dev-import) and prints the command line of a conversion (--dev-print-args); the Python window's
gui.convert_args gives its own. They must be identical: the native window then converts as the
Python window does, through t4ff-cli's command line, which is checked against the Python t4ff itself.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
T4FF = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, T4FF)

from t4ff import gui  # noqa: E402

EXE = os.path.join(HERE, "..", "build", "Release", "t4ff.exe")

CASES = [
    {},
    {"output": r"C:\out dir", "console_zones": [r"C:\x\_codxe\t4", r"D:\zone"], "iwds": [r"C:\Games\WaW\main"]},
    {"output": "out", "texture_budget": "64", "memory_target": 200, "max_texture_size": 1024, "sound_rate": 32000, "stream_rate": 24000},
    {"output": "o", "texture_budget": 0, "xma_quality": 75, "max_loaded_sounds": 900, "mono_sounds": True, "mono_streams": True},
    {"output": "o", "no_mips": True, "no_compress": True, "no_mod": True, "no_patch": True, "no_sounds": True, "t4_layout": False, "load_zone": False},
    {"output": "o", "texture_budget": "96.5", "memory_target": 212.5, "max_loaded_sounds": 0},
    {"output": "o", "texture_budget": "  AUTO "},
    # saved by older versions of the Python window: its migrations
    {"version": 1, "output": "o", "t4_layout": False, "texture_budget": "48", "load_zone": False, "memory_target": 190},
    {"version": 5, "output": "o", "memory_target": 180, "texture_budget": "80"},
]


def main():
    exe = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else EXE)
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for i, case in enumerate(CASES):
            data = {"version": 6, "xma_encoder": ""}
            data.update(case)
            path = os.path.join(tmp, f"gui{i}.json")
            with open(path, "w", encoding="utf-8") as f:
                json.dump(data, f)
            for input_path in (r"C:\maps\nazi_zombie_x", r"C:\Program Files\maps\my map\my_map.ff"):
                settings = gui.Settings.load(path)
                settings.input = input_path
                expected = gui.convert_args(settings)
                out = subprocess.run(
                    [exe, "--dev-settings", os.path.join(tmp, f"none{i}.json"), "--dev-import", path, "--dev-print-args", input_path],
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    check=True,
                ).stdout
                got = out.split("--\n")[0].splitlines()
                same = got == expected
                failed += not same
                print(f"case {i} ({input_path}): {'identical' if same else 'DIFFERENT'}")
                if not same:
                    print("  python:", expected)
                    print("  native:", got)
    print("all identical" if not failed else f"{failed} different")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
