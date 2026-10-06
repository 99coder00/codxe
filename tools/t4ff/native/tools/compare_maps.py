"""Converts usermaps with t4ff-cli convert and with t4ff's own convert command (py_reference.py cli),
and compares the zones, the dumps of their node trees and every file of the maps' folders
(<out dir>/<map>.cpp.dir, <out dir>/<map>.py.root/_codxe/t4/usermaps/<map>).

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
    ap.add_argument("--sounds", action="store_true", help="convert the sounds too (into each side's output folder, <out dir>/<map>.<side>.dir)")
    ap.add_argument("--xma-encoder")
    ap.add_argument("--extra", default="", help="more options for both sides (e.g. \"--stream-textures --deep-stream all\")")
    ap.add_argument("maps", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    identical = 0
    for folder in a.maps:
        name, files, iwds = find_usermap(os.path.abspath(folder))
        paths = list(files.get("localized", [])) + [files["map"]] + [files[r] for r in ("patch", "mod") if r in files]
        # the options both sides take
        common = []
        for zone in a.console_zone:
            common += ["--console-zone", zone]
        if a.iwd:
            common += ["--iwd", a.iwd]
        if a.xma_encoder:
            common += ["--xma-encoder", a.xma_encoder]
        common += ["--texture-budget", a.budget] + ([] if a.sounds else ["--no-sounds"]) + shlex.split(a.extra)
        # t4ff-cli: the map's files as t4ff convert finds them
        args = []
        for iwd in iwds:
            args += ["--map-iwd", iwd]
        # the map's own files: the folders of its fastfile and .iwd files, as t4ff convert reads them
        for folder_ in dict.fromkeys([os.path.dirname(os.path.abspath(files["map"]))] + [os.path.dirname(os.path.abspath(p)) for p in iwds]):
            args += ["--map-files", folder_]
        if "load" in files:
            args += ["--load-ff", files["load"]]
        args += ["--map-name", name] + common + paths
        base = os.path.join(a.out_dir, name)
        # the Python side is t4ff's own convert command, writing the map's folder under <map>.py.root
        py_root = f"{base}.py.root"
        py_dir = os.path.join(py_root, "_codxe", "t4", "usermaps", name)
        sides = [("cpp", f"{base}.cpp.dir")] + ([] if a.cpp_only else [("py", py_dir)])
        times = {}
        for side, out_dir in sides:
            start = time.time()
            # the map's output folder (options.txt, scripts/, sounds/...), emptied first
            for folder_ in (out_dir, py_root if side == "py" else out_dir):
                if os.path.isdir(folder_):
                    shutil.rmtree(folder_)
            if side == "cpp":
                os.makedirs(out_dir)
                cmd = [CLI, "convert", "--out", f"{base}.cpp.zone", "--dump", f"{base}.cpp.txt", "--out-dir", out_dir]
                cmd += (["--sounds-dir", out_dir] if a.sounds else []) + args
            else:
                cmd = [sys.executable, REF, "cli", f"{base}.py.zone", f"{base}.py.txt", "--no-install", "convert", os.path.abspath(folder), "-o", py_root]
                cmd += common
            with open(f"{base}.{side}.log", "w", encoding="utf-8", errors="replace") as log:
                code = subprocess.call(cmd, cwd=T4FF, stdout=log, stderr=subprocess.STDOUT)
            times[side] = f"{side} exit {code}, {time.time() - start:.1f} s"

        def same(ext):
            return all(os.path.exists(f"{base}.{s}.{ext}") for s in ("cpp", "py")) and filecmp.cmp(f"{base}.cpp.{ext}", f"{base}.py.{ext}", shallow=False)

        zone, dump = same("zone"), same("txt")
        # the output folders: sounds, options.txt, scripts/...
        trees = []
        for root in (f"{base}.cpp.dir", py_dir):
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
