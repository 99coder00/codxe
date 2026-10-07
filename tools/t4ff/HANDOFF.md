# t4ff and CoD Xe T4: handoff notes

State of the work as of 2026-10-06 (the sections below date from 2026-09-28 on unless marked), for
whoever (person or Claude session) picks it up next. The [README](README.md) explains what t4ff does and
how to use it; this file is about where the work stands, the rules it follows, what is open, what
was already tried, and how things were checked.

## Start here

To continue in a new Claude Code session (for example one running on the tester's own PC, which
can run t4ff on the real files and read the Xenia and Watson logs directly), give it this:

> Read `tools/t4ff/HANDOFF.md` on branch `claude/charming-ptolemy-rahr6b` of this repository, starting
> with its "State on 2026-10-06" and "State on 2026-10-04" sections, and continue from there. Follow
> its working rules.

Everything below is in the repository. Nothing from the earlier cloud sessions (their scratch
files, sample downloads, analysis scripts) carried over: the samples come from the files listed
under [Environment](#environment), and the checks are described under
[Investigation toolbox](#investigation-toolbox).

## State on 2026-10-06: the native port is done (read this first)

**Where it stands.** All eight steps of the C++ port are done, committed and pushed on
`claude/charming-ptolemy-rahr6b` (the last is e5e1526, "step 8: the window"). The working tree holds
nothing else uncommitted but the turret diagnostics, which are not meant to ship (the table under
"State on 2026-10-03"). The programs are `tools/t4ff/native/build/Release/t4ff.exe` (the window) and
`t4ff-cli.exe` (the Python t4ff's command line), built from `tools/t4ff/native`:

```sh
"C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
"C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

How the native output is checked against the Python (from `tools/t4ff`; native README, "Checking it
against the Python t4ff"):
- `compare_maps.py`: both programs convert each map with the same command line; zones, node dumps and
  every file of the map's folder must be identical (`--sounds` for the `.xma` files, `--extra=...`
  for more options). Step 7's run of the eight test maps, about 35 minutes (nearly all of it the
  Python), with `D` the Downloads folder:

```sh
python native/tools/compare_maps.py "<out dir>" --budget auto \
    --console-zone "$D/Compressed/Call of Duty - World at War (axekin.com).iso/WaW/_codxe/t4/usermaps" \
    --console-zone "$D/zone" --iwd "$D/Compressed/Call of Duty World at War B252004~AG/Call of Duty World at War/main" \
    "$D/nazi_zombie_simp" "$D/Mini-Labor v1.2" "$D/NukeCraft" "$D/Compressed/SuperMario64" \
    "$D/kinodertoten_updated/kinodertoten.ff" "$D/Kino Rezurrection 1.06" "$D/Matrix" "$D/nazi_zombie_dead_sand"
```

- `python native/tools/gui_args_check.py`: the window's command lines against the Python window's.
- `native/tools/regex_check.py` (its three commands are in the native README): the regex engine
  against Python's `re`.

**Open after step 8:**
1. **The window has not been used with a mouse yet.** It was checked by drawing itself into pictures
   (`t4ff.exe --dev-screenshot`, see step 8 below), and its conversions by `--dev-run`. Not tried:
   Windows' file dialogs, dragging from Explorer, moving it to a monitor of another DPI, and
   `t4ff-cli gui` actually starting it (only its error, when `t4ff.exe` is missing). The user's first
   session with it is the test.
2. **Map names outside ASCII:** a few places build paths from narrow strings, so a map whose name has
   other characters may get wrong paths. Every test map is ASCII; fix before such a map comes.
3. **Python and native both live.** Until the user says the native program replaces the Python t4ff,
   a change to the conversion goes into both, checked with `compare_maps.py`. Nothing is packaged for
   release (the two `.exe` files run as they are; `setup` puts the encoder in `bin` next to them).
4. Speed and memory ideas are under "Things noted for later steps" below; the console items
   (real hardware, the Simpsons, Dead Sand, Mini-Labor) are in the older sections and Next steps.

The step by step record follows.

**The user's request:** t4ff "cant stay in python due to efficiency issues". It is being rewritten in
C++ as a Windows program, `tools/t4ff/native` ([README](native/README.md)), in eight steps. The user
chose Dear ImGui for the GUI and the GPL-3.0 for the program (OpenAssetTools' layouts and commands
are compiled into it), and approved downloading zlib 1.3.1, Dear ImGui 1.91.9 and minimp3 (vendored
in `native/third_party`).

**The Python t4ff stays the reference.** Each native step is checked by comparing its output with the
Python output byte for byte on real fastfiles (`native/tools/py_reference.py` gives the Python side).
The native one has caught up (step 7 gave it the Python's command line), so a change to the
conversion now goes into both (Open after step 8, item 3).

**Step 1 (foundation) is done:** fastfiles, structure layouts (generated into `native/data` by
`native/tools/gen_schema.py`), zone code commands, and the zone reader and writer.
- Every readable fastfile at hand gives the Python writer's bytes: 40 PC and CoD Xenon zones, plus
  88 from the disc folder with its CoD Xenon maps.
- Node dumps are identical to the Python's on four zones, up to 458,187 lines.
- Kino Rezurrection's console `d.ff` recompresses to the identical fastfile.
- On one thread, zones read 7-9 times faster than the Python. Every processor reads its own zone:
  the 49 console zones of a conversion's library (4.3 GiB) load in 3.1 s.

**Step 2 (textures) is done:** IWI reading (`.iwd` archives too), wavelet decoding, DXT decoding
and encoding, Xenos tiling and mip chains, the console texture builder and `with_mips`
(`images.py`, `wavelet.py`, `dxt.py`, `xenos.py`).
- `t4ff-cli textures` runs a battery of conversions over every IWI given. Its report is identical to
  `py_reference.py textures`' on Kino Rezurrection (1,471 IWIs, wavelet ones among them) and on
  the PC game's 8,822 (cube maps among them).
- The DXT encoder matches numpy's float arithmetic operation for operation; see the native README
  for the one numpy detail it depends on.
- Kino Rezurrection's textures convert in 0.46 s on 20 threads (3.0 s on one), against 37.9 s in
  Python.
- `_image_from_load_def` and `_decode_console_image` (`assets.py`) read zone nodes, so they come
  with step 3.

**Step 3 (the conversion core) is done:** `convert.py`, `assets.py`, `library.py`, `techsets.py`,
`xanim.py`, and `merge.py`'s merging, reference pruning and nested asset sharing (native README).
- `t4ff-cli convert` converts and merges a map's fastfiles as `_convert_map` does up to there.
  `py_reference.py convert` does the same with the Python, and `native/tools/compare_maps.py`
  compares both.
- Six maps (Simpsons, Mini-Labor, NukeCraft, Mario, Kino Der Toten, Kino Rezurrection) and Matrix
  give identical zones and node dumps. The runs used the console library, the stock textures and a
  100 MiB texture budget.
- They take 5-12 s against the Python's 85-148 s.
- Left for later steps:
  - loaded sound encoding (step 4: without an encoder they are references, as the Python makes them);
  - `named.py` and `merge.py`'s menu and video passes (step 5: they need the script helpers).

**Step 4 (sounds) is done:** `audio.py`, `soundbudget.py`, and the loaded sound parts of
`assets.py` (native README).
- `t4ff-cli convert` now converts the map's streamed sounds and the stock ones its aliases use. It
  encodes the loaded sounds on every processor, keeps them within the loaded sound limit, and syncs
  the alias types.
- The six maps and Matrix give identical zones, dumps and `.xma` files. The C++ takes 16-40 s against
  the Python's 124-184 s, or 23.5 s for Kino Rezurrection once the sounds are cached.
- Encoded sounds go to `%LOCALAPPDATA%\t4ff\sound_cache`, by the SHA-256 of the encoder's input
  (xma2encode was checked to be deterministic).
- For identical PCM, the resampler calls `ucrtbase.dll`'s `sin` and `cos`, which numpy uses.
- Child processes inherit their standard handles only; parallel encodes failed without that.

**Step 5 (scripts, menus, loading screens) is done:** `scripts.py`, `named.py`, `menu.py`'s
conversion passes, `loadscreen.py`, and `merge.py`'s menu and video passes (native README).
- `t4ff-cli convert` now runs all of `_convert_map` but streaming, then writes the map's folder as
  `cmd_convert` does: the fastfile, `t4ff.txt`, `description.txt`, `<map>_load.ff`, `preview.bin`,
  `map.json`, `preview.dds`, `options.txt` and `scripts/`.
- `py_reference.py convert` calls the real `_convert_map`. `compare_maps.py` compares each side's
  whole output folder.
- Eight maps give identical zones, dumps and files (the six, Matrix and Dead Sand), in 6.6-10.4 s
  against 79-136 s.
- The passes use Python's `re`, so `src/core/pyre.*` is a regex engine with its semantics, checked
  against Python on every raw file of four maps (`native/tools/regex_check.py`, `t4ff-cli regex`).
- The script text the passes add is generated from scripts.py (`native/tools/gen_templates.py`),
  tabs and all.
- After conversion the PC zones' pointers lead to console nodes, in both versions. `menu_options`
  reads the PC menus through them. The port mirrors this: don't fix it on one side only.
- `menu.py`'s `menu` command (the Custom Maps list) comes with step 7.

**Step 6 (streaming and the memory plan) is done:** `stream.py`, `memory.py` and `cmd_convert`'s
memory loop (native README).
- `t4ff-cli convert` takes `--texture-budget auto` (now its default), `--memory-target`,
  `--stream-textures`, `--upgrade-budget`, `--deep-stream`, `--stream-growth`, `--keep-quarter` and
  `--keep-mip-tail`. Like the Python, it converts again (up to five times) when the map is over its
  target. It reruns the whole conversion each time, as the Python does: 7-9 s an attempt.
- The oracle is now t4ff's own command line: `py_reference.py cli` runs `python -m t4ff` and saves
  the zone it writes. `compare_maps.py` takes `--budget auto` and `--extra "OPTIONS"`.
- Identical: the automatic budget on Kino Rezurrection (three conversions) and The Simpsons;
  streaming on Kino Der Toten (deep, also with the disc's highmip folder), Kino Rezurrection (deep,
  three conversions: upgrades and mip tail, then eighth sizes) and Mini-Labor; `images.pak`
  included. 11-27 s against 81-396 s.
- The streaming boxes depend on numpy's float64 order: `norm` adds the squares in order, without fused
  multiplies.

**Step 7 (command line parity) is done:** `t4ff-cli` takes `python -m t4ff`'s command line
(`src/cli/t4ff_cli.*`, native README "Using it").
- Covered: `info`, `roundtrip`, `convert <map> -o <output>` (with `find_usermap`), `menu`, `streams`,
  `setup`, `--no-install` and `--progress-lines`. Parsing is argparse's: unique prefixes,
  `--opt=value`, `--no-` pairs, and its error messages and exit codes.
- `gui` opens the window (step 8). The developer commands stay; `convert --out` is the developer
  form.
- The conversion moved out of `main.cpp` into `src/app/convert.*`, its messages through a log
  callback and its progress through `src/core/progress.*`.
- `menu.py`'s Custom Maps list (`make_dynamic`) is `src/convert/usermaps_menu.*`. The `menu` command
  on CoD Xenon's 0.2.0 `patch_ui.ff`, a rerun from its `.orig`, and CoD Xenon's 0.3.0 list
  (`--menu-zone`) give the Python's files and output.
- `info`, `roundtrip`, `streams` and the `menu` error print exactly what the Python prints.
- `compare_maps.py` now gives both sides the same `convert` command line (the C++ adds
  `--dev-zone` / `--dev-dump`).
- `setup` installs an encoder from a `.zip` into `bin` next to `t4ff-cli.exe`.
- `t4ff-cli` opts out of Windows 11's power throttling (EcoQoS). A background console was getting
  slower cores: the fastfile's compression took 4.6 s instead of 1.2 s.

**Step 8 (the window) is done:** `t4ff.exe`, Dear ImGui on Direct3D 11 (`src/gui/*`, native README
"The window"). All eight steps are done.
- A queue of usermaps (dropped on the window or added), the Xbox 360 fastfiles, the output folder.
  Convert converts the waiting maps one after another; each row shows the state, the memory the map
  needs, the time, and opens the map's folder. A banner says to copy `_codxe` to the console.
- Simple mode: those three only. Advanced mode adds every option of `convert` and, for the selected
  map, its name in the map lists, its loading picture and its command line.
- Tools: `info`, `menu`, `streams`, `setup`. Dark and light themes (Windows' setting by default), the
  monitor's DPI. Settings in `%APPDATA%\t4ff\window.json`, taken from the Python window's `gui.json`
  the first time.
- **Each command runs in a process of its own,** `t4ff.exe --worker <t4ff-cli's command line>`, as the
  Python window runs `python -m t4ff` (not in the window's process, as planned before). The window
  stays responsive, a conversion's memory goes with its process, a crash fails one map, not the
  window, and Stop ends the process and the encoders it started at once (a job object).
- The window builds the command line as the Python window's `convert_args` does, plus the options it
  lacks. `native/tools/gui_args_check.py` compares the two for nine saved settings (old versions too):
  identical. Maps converted through the window (Matrix, Dead Sand; Mini-Labor with its 3,043 sounds)
  give step 7's and `t4ff-cli`'s files.
- `t4ff.exe`'s `--dev-*` options draw the hidden window into a PNG (`--dev-screenshot`), after a run of
  the queue (`--dev-run`) if asked: how the window was checked without a mouse.

**Memory (fixed before step 3).** Nodes now view their zone's bytes instead of copying them, and
zones can come from a zone cache: inflated once into `%LOCALAPPDATA%\t4ff\zone_cache`, then
mapped (native README, "Memory").
- Loading the console library's 49 zones dropped from 9.5 GiB committed at the peak to 1.6 GiB, and
  from 3.1 s to 0.58 s.
- Round-trips, node dumps and the texture battery were checked again and are unchanged.
- The library in step 3 uses the cache, with a setting to turn it off.

Things noted for later steps:
- What memory is left is node bookkeeping, about 700 bytes a node (Ptr objects, relocs, segment
  vectors). It could be packed further if a bigger library needs it.
- Speed: a faster inflate (libdeflate, zlib-ng) would help when a zone is not cached yet.

## State on 2026-10-04

**The user's goal now: strictly console compatible.** Every map is converted for a console's memory
(the default `--memory-target 212`); Xenia-only builds (Xenia's patch enlarges the game's memory
pool) are for experiments. Test with `dev/xenia/xenia_run.py --console-memory`: it turns that patch
off for the run (and puts the user's patch file back), so Xenia has a console's 414 MB pool. Checked:
Kino Rezurrection at 210.4 MiB leaves 10.2 MB free in Xenia that way, as the 220.7 MiB model says.

**Kino Rezurrection 1.06** (map `d`, a 533 MB PC map that needs T4M R49 there; files in
`Downloads\Kino Rezurrection 1.06`) is converted, installed (`usermaps\d`) and plays: the helicopter
intro, the landing, a tour of 15 places with zombies, at a console's memory. Converted with the Kino
Der Toten command (below) on its folder; about 25 minutes (three conversions in the memory loop).

What it needed:

1. **T4M** (why the PC needs it): bigger asset pools and memory (the console's pools hold it; the
   memory is below), and client scripts getting every view model notetrack, not only `tesla_` ones
   (the Thundergun's effects wait for theirs). CoD Xe nops that test (TU7 0x82141FB8, `main.cpp`).
2. **Wavelet IWIs** (formats 6-10, Black Ops weapon ports): decoded (`t4ff/wavelet.py`, from the
   public OpenAssetTools' decoder); A8L8 normal maps made DXN.
3. **Anim trees**: the map's own `#using_animtree` trees and the player animation script's
   `multiplayer.atr` are taken from its files (the game's lack its animations).
4. **The freeze at the first frame** (not CoD Xe's old "press A" wait: upstream #243 sets
   `ui_autoContinue`): technique sets CoD Xenon compiled (`mc_ambient_t0c0`, the dry grass) have model
   vertex shaders with vertex fetches, and the game binds those with no vertex declaration (Treyarch's
   are compiled for the layout); D3D's bind (TU7 8237D8B0) then reads the element count at address
   0x18 and loops. t4ff now replaces technique sets no Treyarch zone has the shaders of with the
   closest Treyarch one (`library.techset_safe`, `techsets.closest_techset`; `T4FF_ALLOW_UNSAFE_TECHSETS`
   keeps them, for tests). CoD Xe binds an empty declaration instead of none (`main.cpp`): a guard
   for maps converted before.
5. **Memory**: the game allocates every block of a map's zone from its main memory (TU7 0x821677A0),
   not only the textures: about 286.7 MiB free in Xenia, 220.7 on a console (`t4ff/memory.py`).
   Kino Rezurrection takes 139 MiB without textures (models 58, animations 22, the world 21, loaded
   sounds 20; no model is unused), so its textures get 71 MiB at a console's 212.

**Texture quality at a console's memory** (all in `t4ff/stream.py`, `assets.py`, `__main__.py`):

- Textures saved without mips (the IWI flag) get a box filtered chain and stream (`with_mips`; 41 of
  Kino Rezurrection's 80, 18.1 MiB whole in the fastfile before, 2.7 now).
- Over the target the memory loop gives way in order: the PC versions of stock textures, then the
  packed mip tails (levels of 16 texels or less: `--keep-mip-tail`, 17 MiB here), then the planner.
- The planner (`choose_drops`) counts, once a conversion has streamed, what each texture keeps in the
  fastfile (the report's `steps`), and cuts relative to its own count (`texture_cut`: its estimate is
  some MiB off the written zone's). Deep streamed textures first keep an eighth of their size
  instead of a quarter (tier 0, `--keep-quarter` to stop it), then the texture whose next top level
  saves the most loses it; 2D images and stock textures streamed as the PC game's go last (cutting
  the console copy loses the upgrade: 58 textures ended 64x64 before that).
- **Three levels** (`PAK_EIGHTH`, pack version 3: older CoD Xe builds refuse it): the fastfile keeps
  an eighth, a texture of its own; CoD Xe applies the pack's half (x4, +2) and whole (x8, +3) and
  takes the pitch from the width (`TexturePitch`: an eighth's rows are padded, so its pitch shifted
  would be twice the whole texture's). Checked with marked levels (`mark_pak.py --eighth`): 758
  eighths in 51 of 60 tour screenshots, no garbage; unmarked views normal.
- Fixed in passing: streaming boxes of deep textures were sized from a texture twice too small
  (`_MaterialTexels` took 4x the fastfile copy's size: right for the disc's halves, not quarters),
  so their top levels loaded from twice the disc linker's distance. They use the true size now.

Result (conversion 3; conversion 4, installed, is the same with half the box growth): 210.4 MiB; 899 textures stream (880 deep, 758 of them keeping an
eighth); every texture that streamed in the 278 MiB Xenia build keeps its full PC size; of those kept
whole 147 lost a level (mostly 256x256 decals), 8 more (zombie limbs 512 to 128, smoke). A first
console build with the old planner reduced 1328 of 1530 textures. 388-460 reads a tour, none failed,
nothing evicted (no extra pool: 10 MB free).

**The stream buffer starved (2026-10-05; the user saw wall buy chalk and map diagrams "compressed
beyond recognition")**: those were the fastfile's eighths (a 512x256 chalk: 64x32) shown right in
front of them; their top levels were never read. Three causes, in CoD Xe's streamer and t4ff's boxes:

1. The 64 MB buffer was full of applied base levels (priority 5: never evicted). With the disc
   linker's boxes, this map's large textures want 53-61 MB of base and half levels at once from the
   world alone (counted from the converted zone's world boxes holding the view at six tour places).
2. A load finding no room was proposed again every frame (the closest; inside their boxes all are at
   distance 0, the first the tree walk meets wins): no other image loaded (head-of-line blocking).
3. So images near the view kept their fastfile copy while others far inside their large boxes kept
   their base level.

Fixed: CoD Xe (`streaming.cpp`, "When the memory is full of base levels"): a load finding no room
waits 30 frames; images within 300 units still showing their fastfile copy are proposed before any
base level, the nearest first (`Propose`'s keys; the loader still gets the true distance): the user
found the Stakeout's chalk still blocky, 61 units in front of it and 3 units outside its box, behind
the base levels of every large texture the view was inside the box of; while memory is short a
first load does not bring the base level along; an image within 100 units that finds no room for
its half size has one place of its size given back (the applied base levels
there go to their half size for 900 frames, then are evictable). t4ff: for a console's memory the
boxes grow half the disc's distance (`--stream-growth`, `CONSOLE_STREAM_GROWTH`; the game renders
1024x600, 80 degrees: a top level shows alone up to about 600 / texels per unit). Measured on a
marked tour (every pack entry colored): screen covered by streamed levels 20% to 43% (full levels
17.4% to 18.2%, half sizes 2.3% to 24.4%); reads 129 to 347 MB in 150 s (164 MB read again: the
tour teleports). The chalks and the chalk map now load (half, or full when close). The extra pool's
margin went from 16 MB to 1 MB: a logged tour showed the game allocates nothing from PMem while a map
plays (all of it while it loads), and the pool goes back before anything else in PMem; a console's
map leaves about 10 MB, now an 8 MB pool. With the nearest-first order: full levels 18.9%, half sizes
28.0% of the screen (the build before all this: 17.4% and 2.3%). Diagnostics:
`STREAM_LOG_FULL 1` logs what holds the memory when a load finds no room and what gives way.

**CoD Xe plugin installed**: `codxe.xex` = `codxe.xex.fair` (md5 bff5d321): the fairer streamer above,
eighths, the notetrack patch, the vertex declaration guard (`codxe.xex.eighth`, md5 ae21d5a2: before the
streamer fixes). Backups next to it: `.notetracks`
(before the guard and eighths), `.declguard`, `.pre-notetracks`, `.pre-streaming`. The guard checked:
Kino Rezurrection converted with `T4FF_ALLOW_UNSAFE_TECHSETS=1` (the grass on CoD Xenon's set) plays
its intro into round 1 with it, three shaders logged once each ("bound without a vertex
declaration"); before, it froze at the first frame. Leviathan's grass clumps could not be reached
for a test (outside its playable area: it moves the player back).

**Open, in order:**

1. Real hardware: everything was checked in Xenia (with a console's memory pool now). The user's
   RGH console is the next test of Kino Rezurrection.
2. What the eighths cost: past 300 units of a texture's box it shows an eighth, not a quarter (a
   band where the GPU would sample a quarter is softer). Applying the half size texture farther
   (a "far" use band) would help; applied blocks cannot be evicted, so it needs care with the 64 MB.
3. Committed and pushed on 2026-10-05: t4ff as 2734211 on this branch; CoD Xe as 0d9e555 on
   `merge-upstream-r351` (pushed to origin, not merged here: it carries the upstream r351 merge, which
   the maintainer brings into this branch). The turret diagnostics in this tree stay uncommitted.
4. The older items below (2026-10-03's list).

## State on 2026-10-03

**Where the code is: two working trees, both with uncommitted work (commit only when asked).**

| Tree | Branch | What is uncommitted |
| --- | --- | --- |
| `Downloads\codxe` (this repository) | `claude/charming-ptolemy-rahr6b` | all of t4ff's work since 2026-10-01: `t4ff/stream.py` (new), `memory.py`, `merge.py`, `scripts.py`, `named.py`, `assets.py`, `library.py`, `menu.py`, `gui.py`, `__main__.py`, tests, README, this file, `dev/xenia/` (new); also turret diagnostics in `src/game/t4/sp` (`turret_view.*`, `main.cpp`, `codxe.vcxproj`) that are not meant to ship |
| `Downloads\codxe-build` (a worktree) | `merge-upstream-r351` | CoD Xe: `src/game/t4/sp/components/streaming.cpp`/`.h` (new), `fastfiles.cpp`/`.h` (images.pak, highmip redirect, `dump_executable`), `gsc.cpp` (`ExecuteCommand`), `main.cpp`, `symbols.h`, `codxe.vcxproj` |

Build CoD Xe in the worktree: `echo | cmd //c 'tools\build-codxe.bat'` (VS2010 + XDK), then copy
`build\Release\bin\codxe.xex` over `Downloads\Compressed\xenia_canary_windows\plugins\4156081C\codxe.xex`
(installed now; `codxe.xex.pre-streaming` there is the build before the streaming work).

**What the last sessions built** (2026-10-02/03; details in [Memory and texture streaming](#memory-and-texture-streaming-2026-10-02)):
texture streaming for converted maps that goes past what WaW shipped, after Black Ops 1's methods,
built a step at a time and each step checked in Xenia (color-marked texture levels, logs, screenshots):

1. **World streaming tree** (`GfxWorld.streamInfo`) and the disc linker's box formula: static models and
   world surfaces stream at all (before, only entities did).
2. **`images.pak`**: one pack per map instead of a `highmip` folder; CoD Xe serves it.
3. **Two levels at once** (`--deep-stream all`): the fastfile keeps a quarter size texture.
4. **Extra stream pool**: CoD Xe runs the streamer's slots itself and gives it the main memory the map
   leaves (Black Ops' `extraRStreamBuffer`; Kino: 56 MB more than the game's 64 MB).
5. **PC originals** (`--upgrade-budget`): every stock texture the console had smaller is the PC game's.
6. **Levels in steps**: half size first, base level inside the texture's box.

**Kino Der Toten** (installed in the game folder, `usermaps\kinodertoten`, with its 395 MB `images.pak`;
needs the new CoD Xe build) is the test map: 128.4 MiB of main memory, every texture with a PC source at
its PC size (77% identical data, 22.5% normal maps repacked to DXN, 2 textures re-encoded; 29 are
console only), 645 textures streamed (627 in steps). Converted from `tools/t4ff` with (Git Bash; about
6 minutes; the disc's campaign zones give its campaign assets):

```bash
W="C:/Users/Hunter/Downloads/Compressed/Call of Duty - World at War (axekin.com).iso/WaW"
Z=(); for f in ber1 ber2 ber3 ber3b mak nazi_zombie_prototype oki2 oki3 pby_fly pel1 pel1a pel1b pel2 see1 see2 sniper; do Z+=(--console-zone "$W/$f.ff"); done
python -m t4ff convert "C:/Users/Hunter/Downloads/kinodertoten_updated/kinodertoten.ff" -o "$W"   --iwd "C:/Users/Hunter/Downloads/Compressed/Call of Duty World at War B252004~AG/Call of Duty World at War"   --xma-encoder "C:/Program Files (x86)/Microsoft Xbox 360 SDK/bin/win32/xma2encode.exe" --xma-quality 67   --max-loaded-sounds 1500 --stream-textures --deep-stream all   --console-zone "C:/Users/Hunter/Downloads/Compressed/codxe-t4-fastfiles-v0.2.0" "${Z[@]}"
```

**Testing** without anyone at the controller: [`dev/xenia/README.md`](dev/xenia/README.md) (test mod,
`codxe.json` settings, tour scripts, screenshots, `mark_pak.py`). Always put the user's `codxe.json`
back (`mod_menu`; a copy is `dev/xenia/codxe.json.user-backup`).

**Reverse engineering**: IDA databases in `Downloads\ida_dbs` (`waw_disc_default_xex.i64`: the disc
executable, for logic; `waw_tu7_dump.i64`: Title Update 7, which Xenia runs, for addresses;
`bo1_sp_default_xex.i64`: Black Ops 1). TU7 addresses differ from the disc's (see Title Update 7
below). Black Ops 1's 360 fastfiles: `Downloads\bo1_tools\t5ff.py` decrypts all 48 of the disc's
(the key is Activision's: kept out of the public repositories).

**Open, in order of the user's interest:**

1. The CoD Xe dev asked whether the **packed mip tail** drop was tried: not yet. Measured on Kino: 11.9
   MiB less (936 textures; Xenos packs levels 16x16 and down into one 4 KiB aligned tail). Offered as a
   t4ff option to test in Xenia; Kino does not need it (66 MiB under its target), big maps may.
2. The user's **500+ MB map** (an .exe installer the user downloads): the real test of the extra pool.
3. **Real hardware**: everything streaming was checked in Xenia only (disc/HDD speed, GPU timing).
4. **Freeze Gun** (`freezegun_sp` or a variant) does not fire and disables the knife (user report, not
   looked at).
5. Committing (when asked): t4ff on this branch; CoD Xe on `merge-upstream-r351` (the turret diagnostics
   in this tree stay out).
6. The older open problems below (The Simpsons, Dead Sand, Mini-Labor console tests).

## The goal

Make PC Call of Duty: World at War custom zombie maps (usermaps) run on the Xbox 360 through
CoD Xe, in Xenia Canary and on a real RGH console, by converting them with
[`t4ff`](README.md). Every fix should help future conversions too, not only the map at hand:
prefer general rules (a script pattern, an asset class) to one-off patches of a single map. The
CoD Xe mod `mod_menu` stays enabled in all tests.

## Working rules

- **Branch.** Work on `claude/charming-ptolemy-rahr6b` and push with
  `git push -u origin claude/charming-ptolemy-rahr6b`. If a push is rejected, fetch and merge
  (the maintainer merges upstream into the branch themselves); do not rebase or force push.
- **Pull requests** only when asked.
- **Commit messages** end with the attribution lines of the session's instructions. Never put a
  model name or identifier in commits, code, comments or docs.
- **Private code.** A developer of an Xbox 360 fork of OpenAssetTools shared a private copy of it
  on one condition: "feel free to do whatever but not share the code please, you can release as
  many fastfiles / maps as you want though". This repository is public. So:
  - never commit, push, paste or paraphrase code from that fork into this repository;
  - keep any copy next to the repository (`.gitignore` ignores `/OpenAssetTools-xenon*/` and
    `/OpenAssetTools-private*/`), never inside it;
  - use it only as a reference, and check what it says against real data before relying on it:
    it is not always right (its code texture enum, for one, is doubtful, see below);
  - patches written for that fork (the streamed sound layout fix was one) are handed to its
    developer privately, never committed here.
- **Verify before claiming.** Several guesses in this work turned out wrong. Compare against real
  console data (CoD Xenon's converted maps, the game's disc files) and state what is proven and
  what is only likely. The console `nazi_zombie_aztec.ff` used as a reference is CoD Xenon's
  conversion of a community map, not a Treyarch zone.
- **Tests.** `python -m unittest discover -s tests` in `tools/t4ff`, with `T4FF_SAMPLES` set for
  the sample based tests (layout below). Run them before every push; 108 as of 2026-10-04 (21 skip
  without samples), all passing on Windows.

## Environment

What the maintainer has (paths on their Windows PC; adapt):

| Thing | Use |
| --- | --- |
| PC World at War install (`Call of Duty World at War` folder with `main` and `zone`) | `--iwd` (stock textures, stock streamed sounds); its `zone\english\*.ff` are the PC game's own fastfiles, not read by t4ff yet (see Next steps) |
| CoD Xenon's fastfiles package 0.2.0 (`codxe-t4-fastfiles-v0.2.0`, 13 converted maps and a `zone` folder) | `--console-zone`: technique sets and every console asset t4ff copies |
| Xbox 360 SDK (`C:\Program Files (x86)\Microsoft Xbox 360 SDK\bin\win32\xma2encode.exe`) | `--xma-encoder`, the only XMA encoder |
| World at War 360 game folder for Xenia (`...\WaW\_codxe\t4` with `usermaps`, `zone`, `mods\mod_menu`, `codxe.json`) | where converted maps go; `python -m t4ff menu <that _codxe\t4>` rebuilds the map list |
| Xenia Canary | first test; its `xenia.log` holds the game's console when `log_console` is on |
| RGH Xbox 360 with xbWatson and XeXMenu's FTP server | real hardware test; xbWatson shows the game's debug output |
| Visual Studio 2010 + Xbox 360 SDK | building CoD Xe (`tools\build-codxe.bat`) |
| Visual Studio 2022 (C++ desktop workload, its bundled CMake) | building t4ff native (`tools/t4ff/native`) |
| FFmpeg (`C:\ffmpeg\bin`, on PATH) | pictures (loading screens) and compressed sounds, for both t4ffs |

The maintainer's usual conversion (placeholders for their paths):

```sh
python -m t4ff convert "<PC usermap folder>" -o "<output folder>" ^
    --xma-encoder "C:\Program Files (x86)\Microsoft Xbox 360 SDK\bin\win32\xma2encode.exe" ^
    --console-zone "<codxe-t4-fastfiles-v0.2.0>" ^
    --iwd "<PC World at War folder>" [--name "The Simpsons"] [--loading-image picture.png]
python -m t4ff menu "<game>\_codxe\t4"
```

The native `t4ff-cli.exe` takes the same command lines (`t4ff-cli convert ...`, `t4ff-cli menu ...`),
and the window `t4ff.exe` runs them for a list of maps.

`codxe.json` for testing: `"log_console": true` (the game's console and script errors go to the
debug output), and `"thread_watch": true` when a map hangs or crashes without an error (see
[docs/t4.md](/docs/t4.md#console-log)).

Sample files for the tests (`T4FF_SAMPLES` points to a folder with this layout):

```
pc/nazi_zombie_aztec.ff, nazi_zombie_aztec_patch.ff, nazi_zombie_aztec_load.ff, mod.ff, nazi_zombie_aztec.iwd
                                               (PC Aztec usermap, "nazi_zombie_aztec_V2.2")
x360/nazi_zombie_aztec.ff, patch.ff, patch_ui.ff   (CoD Xenon's: usermaps\nazi_zombie_aztec and zone\)
x360/sounds/para_egg.xma                           (a stream of CoD Xenon's Aztec)
x360/stock_sounds/*.xma                            (streams from the game's disc, any few)
v020/_codxe/t4/...                                 (CoD Xenon's 0.2.0 package as extracted: usermaps\mario,
                                                    usermaps\zm_tranzit, zone\patch.ff, zone\patch_ui.ff are used)
```

## What is done

In the order it was built; `git log` on the branch has the details.

**The converter (t4ff).** A reader and writer of T4 zones driven by OpenAssetTools' structure
definitions and zone code for both platforms, and a field by field PC to 360 converter for every
asset type a usermap has: models, animations (360 quaternion packing), the world (GfxWorld, clip
map, paths), effects, weapons, materials, menus, scripts, string tables, localized strings. The
console layouts were recovered by comparing PC Aztec with CoD Xenon's conversion; CoD Xenon's
fastfiles read and write back byte for byte. A window (`t4ff_gui.pyw`) and a CLI.

**Assets the console lacks.** Technique sets are copied from the console fastfiles given (they
cannot be converted), with stock images, sounds and models the PC map expects from the game.
Assets the game's own zones load stay references when those zones are given. The code arguments of
the copied passes are moved to the sections (per primitive, per object, stable) the game's own
technique sets have them in (`techsets.py`): CoD Xenon's read the dynamic shadow texture as a
stable argument (see Open problems, 1).

**Textures.** Tiled Xenos textures with mip chains, DXT compression, a per map texture budget from
a memory target. Normal maps become DXN. Cube maps (reflection probes, skies) are converted: the
probes of PC Aztec come out byte for byte as CoD Xenon's. Swizzles fixed for uncompressed
textures (`8_8_8_8` had red and blue swapped, `L8` a wrong alpha, the A8L8 format code named the
wrong swizzle); the values are those the SDK's D3DFORMAT constants hold.

**Sounds.** Loaded sounds are XMA1 (from `xma2encode`'s XMA2) with seek tables and loop regions
matching CoD Xenon's. Streamed sounds are written in the layout of the game's own disc streams:
4 KiB blocks and a table of decoded samples per block. That fixed voices and the music box cutting
off after a split second (confirmed in game), and `menu` rewrites older streams in place. Maps stay
within the 1600 loaded sound slots; looping sounds stay loaded.

**Scripts.** The map's loose scripts replace those of its fastfiles; scripts the map calls but
lacks are added (from its files, the console fastfiles, the PC files). Script fixes for the console:
- the modding kits' `modderHelp()` stops setups at missing entities without `developer` too (The
  Simpsons' zipline setup looped forever; Xenia crashed with "Overflowed stackpoints!");
- `spawn("script_struct", ...)` becomes `spawn("script_origin", ...)`: the console refuses to spawn
  script_struct (The Simpsons' rocket barrage died at every rocket);
- hints naming the PC's use key show the console's button; PC script menus (Tom_bmx's music box)
  get controller buttons;
- zombie maps get `clientscripts/_zombie_mode.csc` and the zombie `clientscripts/_callbacks.csc`
  (with `sound_notify`), which the game loads by name and maps from the first mod tools lack;
- scripts of the mod that the console's own zones have too (which the console would run instead)
  get names of their own when only the map's scripts call them (`keep_mod_scripts`): The Simpsons'
  zone manager (see Open problems, 2), Dead Sand's blockers (3);
- `SetCursorHint` gets valid hint types only (`valid_cursor_hints`; the console crashes on others:
  Dead Sand's Nebelwerfer froze Xenia);
- the setup's literal precaches, and those of the zone's models scripts set by name that nothing
  precaches, are made before the level script's first wait (`precache_before_waits`; Dead Sand's
  rockets);
- `speed_up_zombies()` hurries zombies only (`speed_up_zombies_only`; Dead Sand's SS sprinted);
- on maps with soldiers, only zombies idle as zombies (`zombie_idles_for_zombies`);
- in splitscreen the map keeps its own fog, not the yellow placeholder fog of the game's `_load.gsc`
  (`level.splitscreen_fog`), and its `SetVolFog` calls set the game's splitscreen fog there
  (`splitscreen_fog`; The Simpsons played washed in yellow, reported by CoD Xenon's developer);
- on zombie maps the MG42 turrets are held guns (`mounted_guns`): on the console the client draws
  a player's view on one far from the gun (Dead Sand's first room, reported by players; CoD Xenon's
  Airport and stock Nacht with one placed too; see Open problems, 3);
- the level and client scripts, loaded by name, come from the map's files when no zone has them,
  and a script the game has too comes from the map's files when it has its own
  (`missing_scripts_zone` roots; Mini-Labor, 7);
- options the mod's front end menus set get their value when unset (`menu_dvar_defaults`; 7).

**Assets the game looks up by name.** Player body animations of the player animation script,
shellshock files the scripts name, and the animations of the anim trees (`animtrees/*.atr`): the
dogs' come from the PC game's own zones, so PC maps with dog rounds had none on the console
(The Simpsons: 60 "Could not load xanim german_shepherd_*").

**Robustness.** Data the PC linker shares between assets (a string, sound names, models and
materials one weapon loads and later ones alias) is now loaded by the first user when the asset
that held it is replaced by a reference (`fix_pointers` / `adopt_orphans` in `convert.py`). That
fixed "reference to unconverted node" on Dead Sand. Maps converted by t4ff (marked with a
`t4ff.txt`) are never read back as console fastfiles.

**Loading screen and map list.** A load zone like CoD Xenon's with the map's picture, a given one or
a title card; map names from `.arena` (localized keys resolved). CoD Xe lists the whole `usermaps`
folder in a "Custom Maps" menu (`t4ff menu` patches CoD Xenon's `patch_ui.ff`), with pictures, 13
rows, paging, and scrolling (the cursor fix is in `src/game/t4/sp/components/usermaps.cpp`;
confirmed in Xenia with CoD Xe r422: up from the first row goes to the last, 7-19 of 19).

**CoD Xe (T4 SP).** `log_console` (console and script errors to the debug output), `thread_watch`
(where a stuck thread is), `startup_command` (a console command run once the main menu is up, e.g.
`devmap <map>`), a usermap's own versions of the game's scripts (`usermaps/<map>/scripts/`, loaded in
place of the zones' copies), a usermap's options in the Custom Maps menu (`options.txt`), the dynamic usermaps list, a mod menu entry for music boxes, VS2010
build script.

## Memory and texture streaming (2026-10-02)

- **The real memory limit.** The game allocates a map's large runtime (textures), large and physical
  zone blocks from its "main" physical memory, which has about 202.7 MiB free when a map loads
  (Kino in Xenia: "Need 58631667 more bytes of 'main' physical ram" with 258.6 MiB of textures;
  168.8 + 24.2 + 1.5 MiB loads and plays). The virtual block comes from elsewhere. `memory.py`'s
  target (194 MiB) is now that memory, not the total; CoD Xenon's "200 MiB total" was only their known
  good range. GUI settings version 4 drops saved targets of the old meaning.
- **Texture streaming** (`--stream-textures`, `t4ff/stream.py`). A `.hi` file is the full texture's
  tiled top level, no header, padded with zeros to 4 times the half size texture's (padded) base level:
  the streamer reads that much (verified on 1474 streamed images of four disc zones; the split itself on
  all 239 of `pby_fly.ff`). The fastfile keeps a half size texture whose layout is the full one's mips
  (`split` checks it byte for byte), `streaming` 1, `streamSlot` 0xFFFF. Images named like one of the
  game's zones' don't stream (requests are by name).
- **What loads a top level** (disc executable, `R_Stream*` around 0x82429B28): the streamer loads, one at
  a time, the closest image whose top level is missing, of the materials of
  - the entities drawn: `XModel.streamInfo.highMipBounds` (per LOD 0 surface, model space);
  - the world: `GfxWorld.streamInfo` is a tree (`aabbTrees`, 32 bytes a node: ushort first leaf
    reference, reference count, first child, child count; float mins[3], maxs[3]) walked from node 0,
    pruning nodes farther than `r_streamMaxDist` (600). Leaves list `leafRefs`: a world surface index
    (its box: `GfxSurface.boundsCopy`, at +16) or `~index` of a static model (its model's boxes).
    **Static models and the world stream only through the tree**: before it was written (2026-10-02
    morning) Kino's streamed props never loaded (14 files served in a test, all entities').
  The boxes are where the top level is needed: the disc's linker grows each triangle's box by 1931.2 /
  its texels per unit (world surfaces of nazi_zombie_prototype.ff: half exact, 90% within 75 units;
  model boxes vary with which image it measured), an empty box (131072, -131072) when nothing streams.
  t4ff writes all three (`write_stream_bounds`). The disc's tree holds every surface and static model
  with a streamed image (t4ff picks 2122 surfaces and 1085 static models of prototype; the disc 2122
  and 1096).
- **The streaming buffer**: 64 MB (`lis r3, 0x400`, "Streaming Texture Buffer", TU7 0x82445D04, render
  init), a buddy allocator of 16 regions of 4 MB down to 128 KiB slots, slot address = base + slot <<
  17. It is reserved at boot whatever the map: a map that streams nothing leaves it unused. Cinematics
  borrow its first 24 MB (0x82411208 on the disc). `r_streamSize` (MB, 0: no limit) caps the slots'
  addresses. Black Ops does better (`extraRStreamBuffer`, below).
- **PC stock textures**: with `--stream-textures` a stock texture the console has smaller (a console
  library copy) is the PC game's (`--iwd`) when its streamed half takes no more memory than the console
  copy (`stream_textures(stock_texture=...)`): memory neutral, the top level streams.
- Kino (2026-10-02, evening): 364 textures stream (127 PC stock), the world's tree has 858 surfaces and
  314 static models, 156.4 MiB of main physical memory; a scripted tour (teleports to 10 places) had 142
  `.hi` files served, none failed (`dev/xenia/t4ff_test_tour.gsc`).
- **`images.pak`** (2026-10-02, night): a map's streamed levels are one file in its folder, not a
  `highmip` folder (`stream.PakWriter`, version 2, big endian): a 32 byte header (`T4FFPAK1`, version,
  count, index offset, index size), the entries 4 KiB aligned (TU7's loader opens with
  `FILE_FLAG_NO_BUFFERING` and share mode 0), then the index (24 bytes an entry: name offset, name
  length, flags, offset, size, level 1 offset, 0) and the names. CoD Xe's `Sys_CreateFile` hook opens
  the pack (adding read sharing) and seeks to the entry; TU7's read (0x82397B70, `ReadFile` without
  an offset) reads from there. Checked on the Kino tour: 142 reads from the pack, none failed.
- **Two levels at once** (`--deep-stream`): the fastfile keeps a quarter size texture, the pack the
  whole texture (level 0 then its mips: the GPU wants the mips contiguous after level 1). CoD Xe
  (`streaming.cpp`) hooks TU7's loader 0x824457D0 (sets the image's `baseSize` to a quarter of the
  entry: the streamer allocates and reads 4 times `baseSize`), the swap queue's consumer 0x824536A8
  (queue: lock 0x84F6B748, count 0x84F6B764, entries 0x84F6B768 of image and slot address or 0) and
  the revert 0x824535F0 (also called when the streamer takes a slot back). For a deep image it
  saves the fetch constant and writes pitch x4, base = slot, size x4, max mip level +2, mip
  address = slot + level 1 offset; dropping restores the saved constant. The game's own swap (for
  reference): pitch x2, base = slot, size x2, max mip +1, mips = old base; its revert: base = mips,
  mips = base + `baseSize` (the runtime one: bytes of the fastfile copy's base level). Checked in
  Xenia with a color-marked entry (level 0 green, mips red: green walls, red at grazing angles; with
  `r_stream 0` the quarter texture, with `r_stream 1` the colors again; scripts cannot set internal
  dvars: `ExecuteCommand("r_stream 0")`). Kino with `all`: 356 of 364 deep, 119.8 MiB (was 156.3);
  the tour had 232 reads for 142 files (the 64 MB pool fills sooner: bigger entries), none failed.
- **The extra stream pool** (Black Ops' `extraRStreamBuffer`, CoD Xe `streaming.cpp`): CoD Xe now manages
  the streamer's slots itself (TU7: init 0x824655B0, alloc 0x824656A8, free 0x824658B0, assign 0x82465988,
  lock/unlock 0x82465B08/0x82465B38, per frame update 0x82465BE0, evict all 0x82465C78, record use
  0x82445568 (slot, frame, squared distance: bands 0 / 10000 / 90000), the loader for images.pak images;
  the game's state machine 0x82445AC0 rewritten as `RateBlock`; its frame counter 0x84F3B9A8, buffer
  0x84F3B9AC, loading image 0x84F3B9E0, stream lock 0x84F3B9B4) with the game's rules (priority 5
  applied, 4 dropped but kept, 1-3 loaded ahead, 0 free; an allocation evicts below its priority, never
  4 or 5, the least recently changed first) over ranges of slots: the game's buffer (slots 0-511) and,
  once a map of images.pak streams, the main memory it leaves less 16 MB (PMem side 1: TU7 begin
  0x822916A0, end 0x822916F8, free 0x82291718 (newest first, by name pointer), alloc 0x82291840,
  get free 0x82291BD8; the pool is 480 MB in TU7). It is given back before any other PMem begin or free
  (the load zone `<map>_load` is on side 1 too). Checked: the same tour gives the game's numbers (232
  reads, 328 deep loads, 272 drops); Kino gets 64 MB at B0070000 (slots 512-1023, 18.2 MB left); with
  the extra pool searched first 141 of 145 loads go there and render; `devmap` again gives it back
  (84175 KiB free) and takes it again. Kino's tour never fills the game's 64 MB (0 evictions): the
  232 reads of earlier runs came from the test's own `r_stream 0`/`1` (142 without).
- **PC originals for every stock texture** (2026-10-02, night): Kino still had 323 streamable stock textures
  below their PC size (the disc's campaign copies: `char_marine_new_c` 128x128 for the PC's 1024x1024). The
  rule was memory neutral; now a stock texture is the PC game's when it streams (deep) and the bytes it adds
  to the fastfile fit `--upgrade-budget` (96 MiB; the memory loop lowers it first), and the ones that cannot
  stream (too small for a slot, effects) are the PC game's whole in the fastfile within the same budget.
  Kino: 463 PC originals (408 streamed, 55 whole) for 9.4 MiB, 645 textures streamed (627 deep), the world's
  tree 1625 surfaces and 746 static models, 128.4 MiB; every texture with a PC source at its PC size
  (`$white`, `$black` aside). The tour now reads 321 files; the game's 64 MB fills and 408 loads go to the
  extra pool (56 MB), none failed. With `r_stream 0` the walls show the fastfile's quarter copies (blurred),
  with `r_stream 1` the PC originals (scratchpad `compare_wall.png`).
- **Levels in steps** (step 5, Black Ops' streamed parts): a deep pack entry also has the offset of level 2
  in its mip region (index field 6; t4ff's `deep_split`: the half size texture's base level, 0 unless
  4 KiB aligned). CoD Xe loads a deep image's half size texture first (the entry from `mipOffset` on: a
  self contained texture, a quarter of the memory; applied with pitch and size x2, max mip +1, mips at
  half + level 2 offset) and its base level (the entry up to `mipOffset`, a second block; applied x4, +2,
  its mips the half size block) once the view is inside its box: its hook on the game's material scanner
  (TU7 0x82445C00: material r3, squared distance f1, frame r5, the image being loaded r6; the candidate
  at 0x84F3B9D0/0x84F3B9D4) proposes it again for that; inside the box already when first loaded, both
  come in the same load. Out of reach an image goes back a step, then to the fastfile's copy; the base
  level is the first to go when memory is short (its half size takes it along). Queued swaps of an
  evicted image are purged; a swap naming a block that went is ignored. Checked in Xenia with marked
  levels (green level 0, red level 1, blue the rest): a test build that never loads base levels shows red
  walls, the real one green. Kino's tour: 712 reads, 153.8 MB (whole textures: 519 reads, 201.0 MB);
  re-read 28.5 MB instead of 75.6 MB; as sharp as whole textures 5 seconds after arriving. Test switches
  in streaming.cpp: STREAM_TEST_WHOLE, STREAM_TEST_HALF_ONLY, STREAM_LOG_SWAPS.
- **Packed mip tail** (the CoD Xe dev's idea): dropping the levels from 16x16 down (Xenos packs them in
  one tail, each texture's data 4 KiB aligned) would save 9.6 MiB on Kino (759 textures); far surfaces
  would sample a 32x32 level instead of a smaller one. Done 2026-10-04: dropped when a map is over its
  memory target (`--keep-mip-tail` keeps it), before any texture loses a level.
- **CoD Xe serves the map's `highmip` folder**: its `Sys_CreateFile` hook (0x823972F0) is, in Title
  Update 7, the CRT's CreateFileA; a request starting `D:\highmip` and ending `.hi` gets
  `usermaps\<map>\highmip\<file>` when that exists (`ResolveHighmipPath`, fastfiles.cpp; logs
  `highmip: <path>`). The dev's choice: the game's files stay untouched.
- **Title Update 7 vs the disc executable.** Xenia runs TU7 (`content\0000000000000000\4156081C\000B0000`);
  `Downloads\ida_dbs\waw_disc_default_xex.i64` is the disc's original executable, whose addresses differ (CoD Xe's
  TU7 symbols land mid-function there). Game logic found there holds; addresses for hooks must come
  from TU7: CoD Xe dumps the running image when a file `_codxe	4\dump_executable` exists
  (`dump_executable.bin`, 50.6 MB, then the copy stalls on the unmapped tail: kill Xenia, delete the
  marker), loaded in IDA as `idat -A -c -TBinary -pppc -b8200000 dump.bin` (`Downloads\ida_dbs\waw_tu7_dump.i64`).
  TU7's highmip path is `%s\highmip%s\%s.hi` (an optional subfolder), its loader 0x824457D0.
- **Black Ops (T5, 360 disc in Downloads)**: its fastfiles are Salsa20 encrypted (`PHEEBs71`); textures
  stream in mip parts from `images.pak` (2.3 GB) and `images_low.pak` (lowest mips), sounds from
  `snd.all.pak`; its zombie maps are 50-55 MB fastfiles. After a level loads it hands **all the free
  physical memory** to the texture streamer (`extraRStreamBuffer`, sub_8259CA50 of its default.xex),
  released when the level goes. A CoD Xe hook could do the same for WaW (a second pool after the 64 MB
  one: the allocator's table is fixed at 16 regions, so its alloc and slot address functions would need
  replacing).
- **Next**: the steps of the user's plan are done (pak, two levels, extra pool, PC originals, levels in steps); open: the user's 500+ MB map, hardware tests, committing the CoD Xe changes, optionally the mip tail drop.

## Maps tested in game

| Map | State |
| --- | --- |
| Aztec (`nazi_zombie_aztec`) | converts and plays; the reference map |
| Zombie Woods (`nazi_zombie_wh`, 2008) | converts and plays; title card loading screen |
| The Simpsons (`simpsons`, 2010, mod heavy) | converts and plays; voices, music box, airstrike (tester), rounds and dog rounds (Xenia) work. The window crash (1) is fixed but awaits a console test; Moe's (2) not looked into |
| Dead Sand (`nazi_zombie_dead_sand`, 2009) | converts and plays; commissars, marines, SS, Nebelwerfer (Xenia). Objective picture (3) |
| Mini-Labor (`nazi_zombie_002c`, 2014, PhilMod) | converts and plays; weapon choice, doors, power, box, Pack-a-Punch, Perk-o-Matic (Xenia). PhilMod's core scripts from the map's scripts folder (needs the new CoD Xe; 7) |
| Kino Der Toten (`kinodertoten`, 2013, A-Grand, UGX Mod 1.0.3) | converted 2026-10-01 (with the disc's campaign zones as console fastfiles: 1377 of its anim tree's animations came from them), 199.0 MiB with UGX's `localized_common.ff` (its guns) merged; UGX's vote menus kept (CoD Xenon's 0.3.0 conversion of the same map forces Classic). Tested: loads, vote menu works with the D-pad (wraps since the 4th conversion), gungame has its guns (no knife and unlimited ammo are UGX's: `AllowMelee(false)` and `giveMaxAmmo` every 0.05 s). Pause menu Options/Challenges only closed it: UGX's PC `pausedmenu` (Options opens `options_new_pc`, Challenges `menu_challenges`) took the place of the console's. Renaming its lists (`*_mod.txt`) didn't help (tested): menus are found by name whatever list holds them, so the mod's menus of console names are renamed `<name>_mod` too (`merge.rename_game_menus`, 102 in Kino); 5th conversion: Options works, Challenges gone (the console's pause menu has none). 6th: UGX's pause menu stays Kino's own (`merge.bind_pause_menu`), Options opens the console's `ingameoptions`, `menu_challenges`/`popup_tier` load with the precached `ugxm_vote_host` list (IDA: script menus and `ui/ingame.txt` share the UI's menu context, `Menus_FindByName` takes the first, 120 menus at most); not tested yet. Firing either Thundergun froze the game (CoD Xenon's Kino too): UGX's `ugx_thundergun.csc` plays effects for local clients 3, 2, 1 (`playfx( i, ...)`), split screen slots the console has but nobody plays; t4ff's `local_client_effects` makes them the script's own local client. Found in Xenia with a test mod (`_codxe/t4/mods/t4ff_test`: `startup_command` "devmap kinodertoten", a script answering UGX's vote and firing through CoD Xe's new `ExecuteCommand("+attack")`); 11th conversion fires 8 shots without freezing. "Objectives: Disabled" is UGX's own setting for Kino (`set_gamemode("objectives", false)` in `ugxm_user_settings.gsc`), the same on PC. Mini-Labor had the same (PC `ui/hud.txt` with its `pausedmenu`, the testers' "options menu" report): its installed zone and zip are patched in place (76 menus; Overkill scripts kept). 13 missing textures (lens/spec composites, `m60_gold_col`) are missing from UGX's iwds on PC too. Installed now: `--stream-textures --deep-stream all` with every stock texture's PC original (645 streamed, 627 two levels, 128.4 MiB; its `images.pak`, 395 MB, ships with the map; needs t4ff's CoD Xe build) |
| The Matrix (`matrix`, 2017, Black Ops perks and wave gun) | stopped at load: "animation 'ai_zombie_crawl_microwave_death_walking_c' not defined in anim tree 'generic_human'" (the game's tree won; the map's is in its .iwd). Its anim trees now go to the scripts folder (needs the new CoD Xe); reconverted 2026-10-01, not tested in game yet |

## Open problems

### 1. The Simpsons: crash at the first window in the house (fixed, awaits a console test)

A tester got, near the first window of the house (after an airstrike, repairing a barrier, or
zombies coming in there; not every time):

```
Tried to use '(null)' when it isn't valid.
Material='mc/berlin_window_browirglas', tech='lp_sun_b0c0d0n0s0_dtex_sm3', techType=10
```

Cause, from the game's code (IDA database of the disc's `default.xex`, not TU7) and data:

- The error is the renderer's for an unset code texture (`sub_82432080` in the disc build; its name
  table has no name for the index, hence `(null)`). Both argument setters check it: the stable one
  (`sub_824376E8`, called when a pass is set up, `sub_82437A90`) and the per object one
  (`sub_82437138`). Code textures are `source + 0xF90 + 4 * index`.
- The dynamic shadow texture (code sampler `0x12`, sampled by the sun lit techniques) is written
  (`source + 0xFD8`) only by the 8 functions that draw models and world surfaces for the lit pass,
  right before their per object arguments.
- The game's own technique sets (21 SP zones of the disc, ~4000 techniques) always have `0x12` among
  the per object arguments of a pass; CoD Xenon's (all 17 of their maps, 352 passes, e.g.
  `mc_l_sm_b0c0d0n0s0`) among the stable ones, read before any object set it. No other code
  argument is in another section than the game's.
- The door (`prefab_berlin_asylum_dbldoor_dr`) is a script model (`door4` at -370 474 1517, next
  to the first window `pf329`); `techsets.py` now moves every copied pass's code arguments to the
  game's sections (48 passes in The Simpsons), after which CoD Xenon's pass is byte identical to the
  game's.

Xenia: the fixed map ran ~15 minutes around that window and door, zombies coming in through it,
without the error. The error was not reproduced on the old build (not tried long): the proof is
the code and data above; a console test after an airstrike and barrier repairs would settle it.

### 2. The Simpsons: round skips, AI and dogs (fixed); Moe's

Reported by a tester: rounds skip (5 at a time in the TV room, 1 or 2 outside), the TV room's
barriers are never used, the AI does not always find its way in, dogs sometimes do not spawn or die
right away, and Moe's cannot be reached. The developer of the private fork says he did not change
scripts for this (only for FNAF, which uses the UGX mod).

Cause, reproduced in Xenia with logging copies of the map's scripts (see the toolbox): the
console's `patch.ff` (the title update's) has Der Riese's `maps/_zombiemode_zone_manager.gsc`; the
DLC3 modding kit's changed one comes in The Simpsons' `mod.ff`. The game runs the first script of a
name it loads, so the console ran Der Riese's; the PC runs the mod's (mod zone and files win). When
no enabled zone has a player in it, the kit's makes `level.DLC3.initialZones[0]` active, Der
Riese's `receiver_zone`. The Simpsons' door into the TV room (`door5`) sets `enter_zone4`, which no
zone waits for (the map's zone setup connects `zone4` with `enter_zone3`: a mapper's slip, the same
on PC), so a player there left no zone active, no spawner (`round_spawning` returns without setting
`zombie_total`), and each round ended 11 seconds after it started; dog rounds had no dog locations,
the dogs stayed at their spawner (64 56 1512) until the 30 second failsafe killed them.
`keep_mod_scripts` (scripts.py) gives such a script of the mod a name of its own
(`..._mod.gsc`) and the map's scripts call it by that name. After the fix, the same scenario:
`initial_zone` active, 14 spawners, rounds of 60-80 seconds, a dog round with the dogs reaching the
player.

Ruled out on the way: path nodes and their links, the path visibility table and node tree
(identical to PC, as they are between PC Aztec and CoD Xenon's), zone volumes (`IsTouching` finds
the player in all 20), the traversal animations. The mod's `_laststand.gsc`, `_loadout.gsc` and
`_debug.gsc` stay the game's (the game's own `_load.gsc` or `_arcademode.gsc` calls them): what the
mapper changed in them is lost on the console, not looked into.

Next: Moe's (not looked into); a console test of rounds in the TV room and of dog rounds.

### 3. Dead Sand: crash, Nebelwerfer, soldiers (fixed); objective picture

About the map: made with the first mod tools (April 2009); its scripts spawn campaign AI (marines,
commissars, SS with panzerschrecks and MG42s) in some rounds and fire a rocket barrage (the
Nebelwerfer), so it also misses campaign animations, effects and HUD materials the console's zones
do not have (see 4). Its sky `flying_ft` is a stock cube map from the PC game's files, converted
only with `--iwd`. Fixed, each checked in Xenia (scripts.py):

- **Crash a few seconds in** ("Overflowed stackpoints!", the Server thread 65536 calls deep): the
  console ran `patch.ff`'s `maps/_zombiemode_blockers.gsc` instead of the mod's. With the mod's
  (`keep_mod_scripts`) it plays; A/B with only that changed. Which loop of the game's script ran
  away with Dead Sand's entities was not traced.
- **Freeze when the Nebelwerfer is bought**: its trigger sets `SetCursorHint("HINT_NONE")`; the
  console's `SetCursorHint` lists the valid types for an invalid one and reads past their table
  (a string pointer 0x3F800000, the float 1.0 after it). `valid_cursor_hints`.
- **No rockets**: `dead_sand_init()` precaches `rocket_barrage` after the zombie mode's wait
  ("precacheItem must be called before any wait statements in the level script"), and nothing
  precaches the rockets' `katyusha_rocket` ("model 'katyusha_rocket' not precached").
  `precache_before_waits`; the setup's own call becomes a comment (after the wait it errors).
- **SS sprinting like zombies**: `speed_up_zombies_only`.
- **Soldiers idling as zombies** (arms out; confirmed by a tester): `init_animscripts()` replaces
  `anim.idleAnimArray["stand"]`/`["crouch"]` for every AI. `stop.gsc` picks
  `idleAnimArray[pose][idleSet % size]` and `randomizeIdleSet()` resets `idleSet` on every stop,
  so a per zombie idle set does not hold; `zombie_idles_for_zombies` moves the zombie idles to
  `"zombie_stand"`/`"zombie_crouch"` and gives each zombie a `stop_immediate` exception (run at the
  start of `stop.gsc`'s `main()`, per AI) that plays them in a copy of its loop.
- **Only sky on the MG42 turrets** (players: "when you mount it you can't see anything"): not the
  conversion. A test script put the player on each turret (`UseBy`): the server has the eye at the
  turret's `tag_player`, but the console's `viewpos` (the game's console, CoD Xe, keyboard passed
  through with a controller plugged in) gave coordinates of 1e24. CoD Xenon's Airport (the view at
  the world's origin) and stock Nacht with one MG42 placed in its map entities (a patched copy, the
  view black) do the same; see1's placed MG42s (campaign) and The Simpsons' .30 cals work. Ruled out,
  A/B each: the model (the .30 cal's on the MG42 weapon, the campaign's on see1), overheating
  (`overheatWeapon` 0 on Nacht), the player's weapons, the AI, the zombie mode's depth of field
  (on see1), `cg_thirdPerson`/`cg_fov` (unchanged while on it). The client's turret view is
  `sub_8212A0E0` (the turret's `tag_player` from its client DObj, plus the weapon's `vProneOfs`),
  called from `sub_8212BB98`; why its pose is wrong in zombie mode was not found. `mounted_guns`
  makes the MG42 turrets of zombie maps held guns (see the README).

Not bugs of the conversion, found on the way (tracker script, see the toolbox):

- the allies die in batches at round start: `kill_some_friendlies()` kills about half at every
  round (worldspawn, `MOD_UNKNOWN`);
- the Nebelwerfer kills every axis AI of the map: its explosions check
  `Distance( target_pos.origin, enemy.origin )` with a position for `target_pos`. `.origin` of a
  vector is undefined (a probe logged it), the retail game skips the errors, and every AI gets the
  kill branch (SS spawned during a barrage died in the same frame; without it they fight for
  minutes). PC runs the same script engine; left as is.

Open: the objective picture in the pause menu is a checkerboard (a missing image, see 4 perhaps).
Harmless errors in its log: `_interactive_objects.gsc` calls `connectpaths` on script models.

### 4. Stock assets no console fastfile has

The PC game's own zones (`zone\english\common.ff`, ...) hold what PC maps take for granted; the
console's zones have much less. A map's zone names some of it (references), and the game looks the
rest up by name (anim trees, `precacheShader`, `precacheModel`, fx `loadfx`). t4ff copies such
assets from the console fastfiles given, which only works when one of CoD Xenon's maps happens to
have them. Missing in the logs: materials (The Simpsons: `zombie_electric_shock_overlay` and
`zombie_transporter_overlay`, which 11 of CoD Xenon's 13 maps have, and `water_droplet`,
`water_dynamic_spray`, which none has; Dead Sand: about 55, scorch marks, HUD and kill icons),
effects (Dead Sand: 21), animations (campaign AI: flamethrower, banzai, MG, vehicle crews), the
model `viewmodel_knife_bowie`.

Next, in this order:
1. add the materials scripts precache (`precacheShader`) and the overlays the game uses for zombie
   maps (`zombie_electric_shock_overlay`, `zombie_transporter_overlay`) to the named assets
   (`t4ff/named.py`), copied from the console fastfiles like the anim tree animations;
2. read the PC game's own fastfiles (in the folder given with `--iwd`) as a last source: index
   their assets once (cache the index), and convert the ones a map names that no console fastfile
   has with the same converter as the map's own. Materials still need a console technique set of
   the same name (from the console fastfiles or the game's zones).

### 5. Streamed sounds the map uses that no file given has

Logs show "Invalid file (incorrect length)" for `Stream\Music\Mission\zombie\mx_wave_1` and
`SFX\Levels\zombie\maps\giant\wind_loop`, and the conversion warns "streamed sounds: 3 the map uses
are not in its files nor in the PC game's files given (e.g. Stream/Music/Mission/zombie/game_over.wav)".
They play silence. Find where the PC game keeps them (another `.iwd`, a localized folder) or have
the map's alias use the game's own stream from the disc.

### 6. Smaller things

- Fatal error text is not logged (see 1).
- Model high mip bounds are the PC's placeholders; CoD Xenon has real bounds (used for texture
  streaming).
- The image load def is loaded with a follow pointer (-1) where the game's own zones use insert
  (-2); harmless so far.
- Volume maps are still references.
- The zone header's size: the game's own zones count the delayed image pixels in it, t4ff's do not
  (`Writer.write`); both load. Left as is.

### 7. Mini-Labor (PhilMod): fixed so far, and PhilMod's core scripts

A team map built on PhilMod (YaPh1l's framework: 10 perks through a Perk-o-Matic, 10 powerups,
ballistic knife, crossbow, traps, an objective ending, five difficulties). Fixed, checked in Xenia:

- **Its world was left out** ("console layout of GfxPortal not verified"): no console sample had
  portals. The disc's Nacht and Makin (`WaW/nazi_zombie_prototype.ff`, `mak.ff`) round trip byte
  for byte after the header and verified `GfxPortal` and 11 more layouts; `verify_samples.py`
  accepts header differences in size, temp and runtime only.
- **"Could not find script 'maps/nazi_zombie_002c'"**: every PhilMod script is in `_phil_mod.iwd`;
  the level and client scripts, loaded by name, are now roots of `missing_scripts_zone`.
- **The weapon choice (Auswahlomat) could not be accepted**: backgrounds took the focus (see the
  README); `decorate_inert_items` (menu.py). How the console moves the focus, from the disc's
  `default.xex`: the menu key handler (`sub_82261BB8`) sends up/left to the previous item unless
  (`sub_82263828`) the previous focusable item is on the same row (the focused item's vertical
  centre within its height) and to the left, left only when it is; down/right likewise with the
  next item (`sub_82263908`). Item_HandleKey is `sub_822619E8`.
- **Blurry menus**: the texture budget reduces 2D materials' images last (`_ui_images`, assets.py).
- **Easy instead of Default**, and no difficulty choice: PhilMod's main menu chooses it (a multiple
  choice item, `philmod_gamemode` 0 to 4). `menu_options` (menu.py) writes the map's `options.txt`
  from the PC menus; CoD Xe's Custom Maps menu shows the focused map's options (X / Y change them,
  usermaps.cpp) and its row sets them before `devmap` (the Custom Maps menu is built on CoD Xenon's
  `patch_ui.ff`, not released without its author's permission). The map also asks them in game
  (`add_options_menus`, menu.py; `options_script`, scripts.py): a copy of the game's
  `popmenu_difficulty` (common.ff) opens over the weapon choice; a changed choice sets the dvar and
  opens a menu running `fast_restart` (single player scripts have no `map_restart`: "unknown
  function"), and the restarted level asks nothing (`t4ff_options_restart`). Checked in Xenia:
  Insane chosen, the level restarts, PhilMod's game mode 3, the weapon choice follows. First opened
  2 seconds in, it replaced the weapon choice (a script's OpenMenu closes the open script menu):
  PhilMod waited for an ACCEPT that could not come, no gun, no round (tester). It now opens as the
  player connects and the level script's own OpenMenu calls wait for it (`t4ff_open_menu`). Waiting
  in PhilMod's `_callbackglobal` and music box too (calls into the level script) overflowed the
  script compiler's 1024 references ("MAX_PRECACHE_ENTRIES exceeded", `sub_82315AE8`): the level
  script's calls only. Tester: works.
  `menu_dvar_defaults` (scripts.py) still gives the menus' default when nothing set it.
- **Mario's loading screen once the map had loaded**: the mod's menus name `$levelbriefing`, and the
  console library found it in CoD Xenon's `mario_load.ff`; the library now leaves load zones out
  (library.py). Xenia also shows the previous map's loading picture when maps are loaded one after
  another in one session (same address and size in every load zone; its texture cache keeps it).
- **The weapon choice on a controller** (tester: the D-pad did nothing visible, A did not start
  the game): `controller_navigation` (menu.py) gives each button of the script menus D-pad and
  stick handlers (`setfocus` the nearest button that way; item handlers run before the default
  moves: `sub_8225C5B0`, the menu's own handlers first, then the focused item's), and choices
  confirmed by one button follow the focus (onFocus) and confirm on A. A first sent the choice and
  the confirmation: the level script's `waittill("menuresponse")` loop got the first only.
- **PhilMod's core scripts** (`_load`, `_gameskill`, `_laststand`, `_loadout`, `_callbackglobal`,
  `common_scripts/utility`, the death, melee and utility animscripts, two client scripts): the game's
  own scripts call them too or the engine runs them by name, so no renaming. t4ff writes them to
  `usermaps/<map>/scripts/`, and CoD Xe's script loader hook (`scr_parser.cpp`,
  `Scr_AddSourceBuffer`) reads that folder after the active mod's, so they run in place of the
  game's as on PC. Checked in Xenia: all 18 used load, no script error, `player_damageMultiplier`
  ends at PhilMod's 1 (the game's sets about 0.36), the systems above work.

Console testers (2026-10-01) report endless ammo and grenades ("easy is infinite ammo") and no
options menu. Endless ammo, likely (not reproduced): PhilMod's Unlimited Ammo power-up turns the
saved dvar `player_sustainammo` on and off after 30 seconds, and a game ending meanwhile left it on
for the next one; `reset_sustain_ammo` (scripts.py) turns it off as the level starts. Also possible:
CoD Xe's mod menu "Engine Infinite Ammo" (`sf_use_ignoreammo`), which the game's `maps/_cheat.gsc`
applies in every game on CoD Xe builds without the scripts folder (the game's `_load.gsc` runs
`_cheat::init()`, PhilMod's does not). Options: the Custom Maps menu's X / Y options need the new CoD
Xe build; the in-game difficulty popup worked in Xenia on an older build. Ask testers which build
they run. The distributed Mini-Labor has a harder Overkill patched into its fastfile (health 1000
+250 a round, every zombie sprints, one hit downs, the third with Juggernog): a reconversion drops it.

Not checked yet: the objective chain
(wrench, uranium, C4, the Endgame-O-Matic, `end_game`), the saw blades, the Amm-O-Matic, the
zipline. Its ending is Treyarch's game over; PhilMod's `maps/credits.gsc` only backs its main
menu's "About this map", which the console does not show.

## Next steps

0. Streaming (2026-10-03): see [State on 2026-10-03](#state-on-2026-10-03-read-this-first): the mip
   tail option, the 500+ MB map, hardware, the Freeze Gun.
1. The Simpsons on a console: the window crash (1) after an airstrike and barrier repairs, rounds
   in the TV room and dog rounds (2); then Moe's (2).
2. Dead Sand on a console (3), public testing; its objective picture.
3. Mini-Labor: public testing (with the CoD Xe build that loads usermap scripts); its objective chain (7).
4. Stock assets: named materials, then the PC game's fastfiles as a source (4).
   Technique sets: every map converted so far names 2-5 that no console zone given has (`mc_unlit`,
   `effect_zfeather_add_nofog_eyeoffset`...), left as references nothing loads. Most are in the
   disc's campaign zones (`ber1.ff`, `mak.ff`, `see1.ff`...): give the disc folder as a console zone.
   The rest could be assembled from the disc's individual shaders (a library of every shader of its
   zones, by name) instead of whole technique sets of the same name; not in t4ff yet.
   CoD Xe r351 (merged on `merge-upstream-r351`, not yet here) has its own custom maps list
   (`map.json`, `preview.dds`), which t4ff now writes too (README, map list).
5. Das Herrenhaus (Der Riese scripts, Black Ops perks, a boss, buildables, Harry's shield) as the
   next complex map.

## Investigation toolbox

t4ff's modules are enough to inspect any fastfile from Python (run from `tools/t4ff`):

```python
from t4ff.fastfile import read_fastfile
from t4ff.platforms import for_endian
from t4ff.zone import Reader, asset_name

endian, _, data = read_fastfile("simpsons.ff")        # PC or 360
p = for_endian(endian)
zone = Reader(p, data).load()                          # zone.extra_root: the tree of allocations
for node in zone.extra_root.walk():
    if (node.extra.get("origin") or ("",))[0] == "asset":
        print(node.type.name, asset_name(p, node))     # ',name' = a reference by name
```

A node's `data` is its bytes (`p.endian`), `relocs` its pointers by offset (`kind` follow, insert,
ref, alias; `ptr.target()` resolves them), `p.record(name)` the layout of a structure
(`t4ff.commands.find_field(record, field)` for offsets). `t4ff.scripts._rawfiles(p, zone)` and
`rawfile_text(node)` give the scripts; `zone.script_strings` resolves script string indices.

How the checks above were made, to repeat them:

- **Compare an asset with CoD Xenon's.** Load both zones, find the asset by name, walk its
  pointers recursively and compare the bytes of every node with the pointers masked. This found the
  door's differences and proved the probes (with `xenos.tile_mip_chain` on the PC probe data).
- **What uses an asset.** Walk every node's `relocs` and collect owners whose pointer targets it.
- **Collision.** Parse `clipMap_t` (`numNodes`/`nodes`, `leafbrushNodes`, `aabbTrees`, `brushes`
  with their sides and planes, `cmodels`) on both platforms and compare values, or test points
  against brushes (inside all planes and the bounds).
- **Path nodes.** `GameWorldSp.path`: 128-byte `pathnode_t`, strings as script string indices.
- **Logs.** The Xenia log and xbWatson carry the game's console with `log_console`; script
  errors are prefixed `[codxe][T4 SP] script error:`; "Could not load <type> "<name>"" lists
  assets looked up by name that no zone has.
- **Logging what scripts do, in game.** CoD Xe loads any script from the active mod's folder
  (`_codxe\t4\mods\mod_menu\maps\...`) instead of the zones' (the log says "GSCLoader: Loaded
  override script"). A copy of a map's script (from `rawfile_text` of the converted zone) with
  `setDvar("t4ff_dbg", ...)` lines added logs to the Xenia log, which records every dvar change
  ("dvar set t4ff_dbg ..."); the mod's `maps\_music.gsc` (`music_init`, which every SP map runs)
  can start a test script (teleports with `setOrigin`, `player.god = true`, killing zombies near the
  player as a player would). An override replaces the script for every map: remove them after.
  This found the round skips (zone states, spawner count and the failsafe kills per round).
- **Tracking AI in game.** A test script hooks every non-zombie spawner (`getSpawnerArray()`,
  `add_spawn_function`) and logs each soldier's spawn, damage, death (`waittill("death",
  attacker)` then `self.damagemod`, `self.damageweapon`) and, every 5 seconds, where the living ones
  are, their `a.script` and enemy; an entity gone without a death is a deletion. A lethal
  `DoDamage` sends no "damage" notify, and a script kill shows as worldspawn `MOD_UNKNOWN`. Found
  Dead Sand's friendly culling and Nebelwerfer (3). The console's compiler hung the level load (no
  error logged) on one such script until variables named `alive`, `mod`, `rec` and functions named
  `track`, `report` were renamed (which one was not narrowed down): with a hang right after
  "GSCLoader: Loaded override script", suspect the script.
- **Menus in a test.** With `startup_command` nothing pressed START at the title screen, so the
  game listens to the controller on the first slot, not the keyboard on the second: press START
  from the keyboard until the log shows "startup_command:". A test script can log what menus send
  (`self waittill("menuresponse", menu, response)`) and answer them itself
  (`player notify("menuresponse", "loadout", "accept")`).
- **The console's script compiler** hangs the level load without an error on an unknown function
  (`array()` is PC only): check each builtin and helper a test script uses against the console's
  scripts first.
- **Loading a map at boot.** `"startup_command": "devmap <map>"` in `codxe.json` loads it once the
  main menu is up (no menu navigation or typing in Xenia). `thread_watch` on hangs Dead Sand's load:
  keep it off unless looking for a stuck thread.
- **Driving Xenia.** Xenia takes its settings as arguments (`--keyboard_mode=1
  --keyboard_user_index=1 --log_file=...`): with a controller plugged in, the keyboard on the
  second slot presses START, which makes it the game's controller. Keys (the config's `[HID.Key]`):
  A `;`, B `'`, X `L` (use), START `X`, left stick W/A/S/D (the menus follow it; the D-pad needs
  Caps Lock), right stick the arrows; hold keys ~0.3 s and wait ~1.2 s between menu moves. A
  script sending keys (`keybd_event`) and grabbing the window (PIL `ImageGrab`) got from the title
  screen to a loaded map unattended; the test script's log lines tell it when to press use (to buy
  a door). Shift + ` opens the game's console in keyboard mode 2 (passthrough) only.
- **The game's code.** An IDA database of the disc's `default.xex` (not TU7: addresses differ from
  CoD Xe's) is next to it (`default.xex.id0` ...). `idat.exe -A -S"script.py args" default.xex.i64`
  on a copy runs IDAPython in batch (Hex-Rays for PPC is there): find a string's function, its
  callers, decompile, or scan instructions for a structure offset (how the dynamic shadow texture's
  writers were found).

Format facts learned in this work that are not in the README:

- **Console technique passes**: 16 vertex shaders indexed by vertex declaration
  (0 generic, 1 packed (models), 2 world, 3-13 world with more texture coordinates and normals,
  14 position/texcoord, 15 water), then one more vertex shader, the pixel shader, the argument
  counts, custom sampler flags (bit 0: reflection probe) and a precompiled vertex shader index
  (1 lit model, 2 unlit model). A technique set copied from another map only has the shaders of the
  vertex declarations that map used. `worldVertFormat` of the set picks the world declaration.
- **Code textures**: the PC and the private fork number them differently after `0xB`; do not trust
  either for the console without checking a technique's use. On the console `0x3` is model
  lighting, `0x7` the sun shadow map, `0x8` the spot shadow map, `0x11` the light attenuation
  cookie and `0x12` the dynamic shadow texture (the sun lit techniques' shadow; per object).
  `techsets.SECTIONS` lists the section of every code argument the game's technique sets use.
- **Scripts the game's zones shadow**: the console's `common.ff` has 24 scripts The Simpsons carries
  (the PC mod tools' copies of stock scripts in its `_patch.ff`, all different, all the game's on
  the console as on PC), `patch.ff` has the stock zombie maps' level scripts and Der Riese's zone
  manager.
- **Alias pointers** point to a pointer slot (index 1: the insert slot of an asset loaded inline);
  an asset the PC map loads inline in one asset is aliased by later ones. When the first one is
  replaced by a reference, the first alias in load order must load it (`adopt_orphans`).
- **Anim trees** (`.atr`): a name followed by a block is a blend node, other names are animations,
  which the game loads by name when a script uses the tree.
- **Scripts the engine loads by name for zombie maps**: `clientscripts/_zombie_mode.csc`, and
  `clientscripts/_callbacks.csc` must have `sound_notify`.

## Where things are

| Path | What |
| --- | --- |
| `tools/t4ff/t4ff/stream.py` | texture streaming: split, deep split, `images.pak` (`PakWriter`), PC originals, streaming boxes and the world's tree |
| `tools/t4ff/t4ff/memory.py` | the main memory measure (194 MiB target) |
| `tools/t4ff/dev/xenia/` | headless Xenia tests and measuring scripts (README there) |
| `codxe-build: src/game/t4/sp/components/streaming.cpp` | CoD Xe: the streamer's slots, extra pool, deep images in steps, the swap queue (TU7 addresses inside) |
| `codxe-build: src/game/t4/sp/components/fastfiles.cpp` | CoD Xe: usermap fastfiles, loose sounds, `images.pak` and `highmip` serving, `dump_executable` |
| `Downloads\ida_dbs`, `Downloads\bo1_tools` | IDA databases (WaW disc, WaW TU7, Black Ops 1); the Black Ops 1 fastfile decrypter |
| `tools/t4ff/t4ff/zone.py` | zone reader and writer, `Node`, `Ptr` |
| `tools/t4ff/native/` | the C++ port (GPL-3.0): `src/core` (zones, textures, regex...), `src/convert` (the passes), `src/audio`, `src/app` (the conversion, usermaps, deps), `src/cli` (t4ff-cli), `src/gui` (t4ff.exe), `data`, `tools` (the checks against the Python), README |
| `tools/t4ff/t4ff/convert.py` | the converter, pointer fixing, shared data adoption |
| `tools/t4ff/t4ff/assets.py` | per asset rules (images, materials, sounds, weapons...) |
| `tools/t4ff/t4ff/images.py`, `xenos.py`, `dxt.py` | textures: formats, tiling, cube maps, DXN |
| `tools/t4ff/t4ff/audio.py`, `soundbudget.py` | XMA, SDNS streams, loaded sound budget |
| `tools/t4ff/t4ff/scripts.py` | script overrides, missing scripts, script fixes |
| `tools/t4ff/t4ff/named.py` | assets the game looks up by name |
| `tools/t4ff/t4ff/library.py` | console fastfiles, copying assets from them |
| `tools/t4ff/t4ff/menu.py`, `loadscreen.py` | Custom Maps menu, loading screens |
| `tools/t4ff/t4ff/__main__.py`, `gui.py` | command line and window |
| `tools/t4ff/t4ff/defs/` | console structure and zone code differences |
| `tools/t4ff/tests/test_t4ff.py` | tests |
| `src/game/t4/sp/components/usermaps.cpp` | CoD Xe: usermaps list, map activation |
| `src/game/t4/sp/components/console.cpp` | CoD Xe: `log_console`, script errors |
| `src/game/t4/sp/components/thread_watch.cpp` | CoD Xe: `thread_watch` |
| `docs/t4.md` | CoD Xe T4 documentation (usermaps list, console log) |
