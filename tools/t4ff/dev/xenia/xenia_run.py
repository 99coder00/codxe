"""Run one headless Xenia test of a converted map and collect its log and screenshots.

    python xenia_run.py <test.gsc> --map d --out <folder> [--shot "t 0" --shot "t 27" ...] [--every 10]

Puts the test script in the test mod (``_codxe/t4/mods/t4ff_test/maps/t4ff_test.gsc``), points
``codxe.json`` at it with ``startup_command`` "devmap <map>", starts Xenia, follows ``xenia.log`` and
takes a screenshot (shot.ps1) whenever a ``t4ff_test`` step starts with one of the ``--shot`` texts
(and every ``--every`` seconds once the map runs). Stops at the step "done", when Xenia exits, or at
``--timeout``; then closes Xenia and puts the user's ``codxe.json`` back. ``--console-memory`` turns
Xenia's patch for the game's memory pool off for the run (a console's 414 MB instead of 480: what a
map must fit on a console), and puts the patch file back after. Paths are the maintainer's.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

XENIA = r"C:\Users\Hunter\Downloads\Compressed\xenia_canary_windows"
GAME = r"C:\Users\Hunter\Downloads\Compressed\Call of Duty - World at War (axekin.com).iso\WaW"
T4 = os.path.join(GAME, "_codxe", "t4")
HERE = os.path.dirname(os.path.abspath(__file__))
USER_CONFIG = os.path.join(HERE, "codxe.json.user-backup")
# Xenia's patch making the game's memory pool 480 MB (a console's is 414): off for --console-memory
GAME_PATCH = os.path.join(XENIA, "patches", "4156081C - Call of Duty - World at War (TU7).patch.toml")
USER_PATCH = os.path.join(HERE, "game-patch.toml.user-backup")
MEMORY_PATCH = "Memory allocator expansion"


def console_memory(on: bool):
    """Turn Xenia's memory patch off (on: True) for a run, keeping the user's file; put it back (False)."""
    if not on:
        if os.path.exists(USER_PATCH):
            shutil.copyfile(USER_PATCH, GAME_PATCH)
            os.remove(USER_PATCH)
        return
    console_memory(False)  # a run that stopped before putting it back
    with open(GAME_PATCH, "r", encoding="utf-8") as f:
        text = f.read()
    start = text.index(f'name = "{MEMORY_PATCH}"')
    end = text.find("[[patch]]", start)
    end = len(text) if end < 0 else end
    shutil.copyfile(GAME_PATCH, USER_PATCH)
    with open(GAME_PATCH, "w", encoding="utf-8") as f:
        f.write(text[:start] + re.sub(r"is_enabled\s*=\s*true", "is_enabled = false", text[start:end]) + text[end:])

INTERESTING = re.compile(
    r"t4ff_test|script error|Could not|ERROR|Error:|error:|Need \d+ more|Exceeded|exceeded|"
    r"Sys_Error|fatal|Fatal|\[Streaming\] (extra pool|slots)|Hunk|overflow|Overflow|not precached",
)


def kill_xenia():
    subprocess.run(["taskkill", "/F", "/IM", "xenia_canary.exe"], capture_output=True)
    for _ in range(60):
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq xenia_canary.exe"], capture_output=True, text=True).stdout
        if "xenia_canary.exe" not in out:
            return
        time.sleep(0.5)


def shot(path):
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", os.path.join(HERE, "shot.ps1"), "-Out", path],
                       capture_output=True, text=True)
    print("  shot:", (r.stdout or r.stderr).strip(), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("test")
    ap.add_argument("--map", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--shot", action="append", default=[], help="screenshot when a step starts with this text")
    ap.add_argument("--every", type=float, default=0, help="also a screenshot every N seconds after the first step")
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--extra", default="{}", help="more codxe.json settings (JSON)")
    ap.add_argument("--quiet", action="store_true", help="print test steps only")
    ap.add_argument("--xenia-arg", action="append", default=[], help="an argument for Xenia (e.g. --log_level=3)")
    ap.add_argument("--console-memory", action="store_true", help="the game's memory pool as a console's: Xenia's patch for it off for the run")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    shutil.copyfile(args.test, os.path.join(T4, "mods", "t4ff_test", "maps", "t4ff_test.gsc"))
    config = {"active_mod": "t4ff_test", "startup_command": f"devmap {args.map}", "dump_rawfile": False,
              "dump_map_ents": False, "log_console": True}
    config.update(json.loads(args.extra))
    with open(os.path.join(T4, "codxe.json"), "w") as f:
        json.dump(config, f, indent=2)

    kill_xenia()
    console_memory(args.console_memory)
    log = os.path.join(XENIA, "xenia.log")
    if os.path.exists(log):
        os.remove(log)
    proc = subprocess.Popen([os.path.join(XENIA, "xenia_canary.exe")] + args.xenia_arg + [os.path.join(GAME, "default.xex")], cwd=XENIA,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    start = time.time()
    pos = 0
    shots = 0
    first_step = None
    loaded = None  # when the map's fastfile was loaded
    last_every = 0
    done = False
    try:
        while time.time() - start < args.timeout and not done:
            time.sleep(0.5)
            if proc.poll() is not None:
                print("xenia exited", proc.returncode)
                break
            if not os.path.exists(log):
                continue
            with open(log, "r", encoding="utf-8", errors="replace") as f:
                f.seek(pos)
                chunk = f.read()
                pos = f.tell()
            for line in chunk.splitlines():
                if loaded is None and f"Loading fastfile '{args.map}'" in line:
                    loaded = time.time()
                m = re.search(r"dvar set t4ff_test (.*)", line)
                if m:
                    text = m.group(1).strip().strip('"')
                    print(f"[{time.time() - start:6.1f}] STEP {text}", flush=True)
                    if first_step is None:
                        first_step = time.time()
                    if any(text.startswith(s) for s in args.shot):
                        shots += 1
                        shot(os.path.join(args.out, f"{shots:02d}_{re.sub(r'[^A-Za-z0-9_]+', '_', text)[:40]}.png"))
                    if text == "done":
                        done = True
                elif not args.quiet and INTERESTING.search(line):
                    print(f"[{time.time() - start:6.1f}] {line.strip()[:300]}", flush=True)
            if args.every and (first_step or loaded) and time.time() - last_every >= args.every:
                last_every = time.time()
                shots += 1
                shot(os.path.join(args.out, f"{shots:02d}_every_{int(time.time() - (first_step or loaded))}s.png"))
        if done:
            time.sleep(1)
    finally:
        kill_xenia()
        console_memory(False)
        if os.path.exists(USER_CONFIG):
            shutil.copyfile(USER_CONFIG, os.path.join(T4, "codxe.json"))
        if os.path.exists(log):
            shutil.copyfile(log, os.path.join(args.out, "xenia.log"))
    print("finished", "done" if done else "not done", f"{time.time() - start:.0f}s")


if __name__ == "__main__":
    main()
