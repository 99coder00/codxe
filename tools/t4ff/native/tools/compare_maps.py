"""Converts usermaps with t4ff-cli convert and py_reference.py convert, and compares the zones and the
dumps of their node trees (step 3's check).

    python native/tools/compare_maps.py <out dir> [--budget MiB] [--console-zone P]... [--iwd P] [--cpp-only] <map>...

A map is its folder or one of its fastfiles, as `python -m t4ff convert` takes it. --cpp-only
converts with t4ff-cli only, comparing with the Python outputs of an earlier run in <out dir>.
"""

import argparse
import filecmp
import os
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
    ap.add_argument("--sounds", action="store_true", help="convert the sounds too (each side into <out dir>/<map>.<side>.sounds), compare the .xma files")
    ap.add_argument("--xma-encoder")
    ap.add_argument("maps", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    identical = 0
    for folder in a.maps:
        name, files, iwds = find_usermap(os.path.abspath(folder))
        paths = list(files.get("localized", [])) + [files["map"]] + [files[r] for r in ("patch", "mod") if r in files]
        args = []
        for iwd in iwds:
            args += ["--map-iwd", iwd]
        for zone in a.console_zone:
            args += ["--console-zone", zone]
        if a.iwd:
            args += ["--iwd", a.iwd]
        if a.xma_encoder:
            args += ["--xma-encoder", a.xma_encoder]
        args += ["--texture-budget", a.budget, "--map-name", name] + paths
        base = os.path.join(a.out_dir, name)
        sides = [("cpp", [CLI, "convert"])] + ([] if a.cpp_only else [("py", [sys.executable, REF, "convert"])])
        times = {}
        for side, cmd in sides:
            start = time.time()
            sound_args = ["--sounds-dir", f"{base}.{side}.sounds"] if a.sounds else ["--no-sounds"]
            with open(f"{base}.{side}.log", "w", encoding="utf-8", errors="replace") as log:
                code = subprocess.call(cmd + ["--out", f"{base}.{side}.zone", "--dump", f"{base}.{side}.txt"] + sound_args + args, cwd=T4FF, stdout=log,
                                       stderr=subprocess.STDOUT)
            times[side] = f"{side} exit {code}, {time.time() - start:.1f} s"

        def same(ext):
            return all(os.path.exists(f"{base}.{s}.{ext}") for s in ("cpp", "py")) and filecmp.cmp(f"{base}.cpp.{ext}", f"{base}.py.{ext}", shallow=False)

        zone, dump = same("zone"), same("txt")
        sounds = ""
        if a.sounds:
            trees = []
            for side in ("cpp", "py"):
                root = f"{base}.{side}.sounds"
                trees.append({os.path.relpath(os.path.join(d, f), root).lower(): os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs})
            common = set(trees[0]) & set(trees[1])
            differ = [k for k in sorted(common) if not filecmp.cmp(trees[0][k], trees[1][k], shallow=False)]
            only = sorted(set(trees[0]) ^ set(trees[1]))
            sounds = f", sounds {len(common) - len(differ)}/{len(set(trees[0]) | set(trees[1]))} identical"
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
