"""Converts usermaps with t4ff-cli convert and with t4ff's own convert command (py_reference.py cli),
both given the same command line, and compares the zones, the dumps of their node trees and every
file of the maps' folders (<out dir>/<map>.<side>.root/_codxe/t4/usermaps/<map>).

    python native/tools/compare_maps.py <out dir> [--budget MiB|auto] [--console-zone P]... [--iwd P] [--sounds] [--cpp-only]
                                        [--extra "OPTIONS"] <map>...

A map is its folder or one of its fastfiles, as `python -m t4ff convert` takes it. --cpp-only
converts with t4ff-cli only, comparing with the Python outputs of an earlier run in <out dir>.
"""

import argparse
import filecmp
import os
import shlex
import shutil
import subprocess
import sys
import time

T4FF = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, T4FF)
from t4ff.__main__ import find_usermap  # noqa: E402

CLI = os.path.join(T4FF, "native", "build", "Release", "t4ff-cli.exe")
REF = os.path.join(T4FF, "native", "tools", "py_reference.py")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir")
    ap.add_argument("--budget", default="100")
    ap.add_argument("--console-zone", action="append", default=[])
    ap.add_argument("--iwd")
    ap.add_argument("--cpp-only", action="store_true")
    ap.add_argument("--sounds", action="store_true", help="convert the sounds too (both sides' outputs are compared, their .xma files among them)")
    ap.add_argument("--xma-encoder")
    ap.add_argument("--extra", default="", help="more options for both sides (e.g. \"--stream-textures --deep-stream all\")")
    ap.add_argument("maps", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    identical = 0
    for folder in a.maps:
        name = find_usermap(os.path.abspath(folder))[0]
        # the options both sides take
        common = []
        for zone in a.console_zone:
            common += ["--console-zone", zone]
        if a.iwd:
            common += ["--iwd", a.iwd]
        if a.xma_encoder:
            common += ["--xma-encoder", a.xma_encoder]
        common += ["--texture-budget", a.budget] + ([] if a.sounds else ["--no-sounds"]) + shlex.split(a.extra)
        base = os.path.join(a.out_dir, name)
        # both sides take t4ff convert's command line, each writing the map's folder under <map>.<side>.root;
        # the C++ also saves the zone and its dump (the Python's are saved by py_reference.py cli)
        sides = [("cpp", [CLI])] + ([] if a.cpp_only else [("py", [sys.executable, REF, "cli", f"{base}.py.zone", f"{base}.py.txt"])])
        times = {}
        for side, program in sides:
            start = time.time()
            root = f"{base}.{side}.root"
            if os.path.isdir(root):
                shutil.rmtree(root)
            cmd = program + ["--no-install", "convert", os.path.abspath(folder), "-o", root] + common
            if side == "cpp":
                cmd += ["--dev-zone", f"{base}.cpp.zone", "--dev-dump", f"{base}.cpp.txt"]
            with open(f"{base}.{side}.log", "w", encoding="utf-8", errors="replace") as log:
                code = subprocess.call(cmd, cwd=T4FF, stdout=log, stderr=subprocess.STDOUT)
            times[side] = f"{side} exit {code}, {time.time() - start:.1f} s"

        def same(ext):
            return all(os.path.exists(f"{base}.{s}.{ext}") for s in ("cpp", "py")) and filecmp.cmp(f"{base}.cpp.{ext}", f"{base}.py.{ext}", shallow=False)

        zone, dump = same("zone"), same("txt")
        # the output folders: sounds, options.txt, scripts/...
        trees = []
        for root in (os.path.join(f"{base}.{side}.root", "_codxe", "t4", "usermaps", name) for side in ("cpp", "py")):
            trees.append({os.path.relpath(os.path.join(d, f), root).lower(): os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs})
        common = set(trees[0]) & set(trees[1])
        differ = [k for k in sorted(common) if not filecmp.cmp(trees[0][k], trees[1][k], shallow=False)]
        only = sorted(set(trees[0]) ^ set(trees[1]))
        sounds = f", files {len(common) - len(differ)}/{len(set(trees[0]) | set(trees[1]))} identical"
        if differ or only:
            sounds += f" (e.g. {(differ + only)[0]})"
        zone = zone and not differ and not only
        identical += zone and dump
        size = os.path.getsize(f"{base}.cpp.zone") / 1048576 if os.path.exists(f"{base}.cpp.zone") else 0
        print(f"{name}: zone {'IDENTICAL' if zone else 'DIFFERENT'}, dump {'IDENTICAL' if dump else 'DIFFERENT'}{sounds} ({size:.1f} MiB; "
              f"{'; '.join(times.values())})", flush=True)
    return 0 if identical == len(a.maps) else 1


if __name__ == "__main__":
    sys.exit(main())
