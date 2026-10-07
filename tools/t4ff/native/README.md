# t4ff native

t4ff rewritten in C++ as a Windows program, in steps. The Python t4ff (`tools/t4ff/t4ff`) stays the
reference: every step is checked by comparing the native output with the Python output, byte for
byte, on real fastfiles.

Licensed under the GPL-3.0 ([LICENSE](LICENSE)): the structure layouts and zone code commands
compiled into the program come from OpenAssetTools (GPL-3.0). Bundled libraries keep their own
licenses: zlib (`third_party/zlib`, zlib license), Dear ImGui (`third_party/imgui`, MIT) and minimp3
(`third_party/minimp3`, CC0).

## Building

Needs Visual Studio 2022 with the C++ desktop workload (its bundled CMake works). From this folder:

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The programs are `build/Release/t4ff.exe` (the window) and `build/Release/t4ff-cli.exe` (the command
line), and they need nothing else to run. The structure layouts and commands are in `data/`,
compiled into both as resources. After changing
OpenAssetTools' definitions or t4ff's `defs/`, regenerate them from `tools/t4ff` (with the Python
t4ff's requirements installed):

```bash
python native/tools/gen_schema.py
```

## The window

`t4ff.exe` is the converter window (the Python's `python -m t4ff gui`; `t4ff-cli gui` opens it too).
Usermap folders or map fastfiles given on its command line, or dropped on it, join its list.
- **Simple mode:** the usermaps to convert, the Xbox 360 fastfiles (best: CoD Xenon's `_codxe\t4`
  folder) and the output folder. Convert converts the waiting maps of the list one after another.
  Each map's row shows its state, the memory it needs, the time it took, and opens its folder. When
  the list is done, a banner says to copy `<output>\_codxe` to the console.
- **Advanced mode** adds every option of `convert` (memory target, texture budget, streaming, sounds,
  the map's fastfiles, extra `.iwd` files) and, for the selected map, its name in the map lists, its
  loading picture and its command line.
- **CoD Xe's settings** (advanced mode, off by default): after each map converted, the window writes
  CoD Xe's `codxe.json` in the output's `_codxe\t4` folder (`_codxe` without the t4 layout), which CoD
  Xe reads when the game starts: its debug switches (`log_console`, `thread_watch`, `dump_rawfile`,
  `dump_map_ents`), the active mod, and a `startup_command` that starts the map converted last as soon
  as the game is up (`devmap <map>` or `map <map>`, as the headless tests did). The file's other
  settings, their order and its line ends stay; an invalid file is left as it is, with a warning.
  Write now writes them without converting (to turn the startup command off again, for one).
- **Tools:** inspect a fastfile (`info`), update the Custom Maps menu (`menu`), rewrite streamed sounds
  (`streams`), set up the encoder and FFmpeg (`setup`).
- What is dropped goes where it belongs: usermaps to the list, Xbox 360 fastfiles and CoD Xenon's
  folder to the fastfiles, `.iwd` files to the extra ones, a picture to the selected map.
- Dark and light themes (Windows' setting by default), scaled with the monitor's DPI. The settings are
  kept in `%APPDATA%\t4ff\window.json`; the first time, they come from the Python window's `gui.json`.

Each command runs in a process of its own, as the Python window runs `python -m t4ff`:
`t4ff.exe --worker <t4ff-cli's command line>` runs exactly what `t4ff-cli` runs. Its output fills the
log and its `@progress` lines (`--progress-lines`) the bar. The window stays responsive, a
conversion's memory goes with its process, a crash fails one map rather than the window, and Stop
ends the process and the encoders it started at once (they are in one job object). The worker opts
out of power throttling too: it has no window of its own.

The window builds the command line as the Python window's `convert_args` does, then adds the options
the Python window lacks. `tools/gui_args_check.py` compares the two for saved settings of the Python
window (from `tools/t4ff`):

```bash
python native/tools/gui_args_check.py
```

`t4ff.exe` has developer options for checking it without a mouse: `--dev-settings <json>` (instead of
`window.json`), `--dev-import <gui.json>`, `--dev-simple` / `--dev-advanced`, `--dev-theme dark|light`,
`--dev-size <w> <h>`, `--dev-scale <s>`, `--dev-select <n>`, `--dev-options-scroll <0 to 1>` (the
advanced options scrolled down), `--dev-run` (convert the list) and
`--dev-screenshot <png>`: the window, hidden, is drawn into a PNG once nothing runs any more (or after
`--dev-screenshot-after <seconds>`), then it quits. `--dev-print-args` prints the command line of each
map given.

## Using it

`t4ff-cli` takes the Python t4ff's command line: what `python -m t4ff` takes, it takes too, parsed
the same way (an option can be shortened to any unique prefix, `--option=value`, `-o value`):

```text
t4ff-cli [--no-install] [--progress-lines] info <fastfile>... [--list]
t4ff-cli roundtrip <fastfile>...
t4ff-cli convert <pc fastfile or usermap folder> -o <output folder> [options]
t4ff-cli menu <_codxe\t4 folder> [--rows N] [--no-streams] [--menu-zone P]...
t4ff-cli streams <folder>...
t4ff-cli setup [--xma2encode P] [--no-test]
t4ff-cli gui
```

`convert` takes the Python's options (`t4ff-cli convert -h` lists them): `--iwd`, `--console-zone`,
`--texture-budget`, `--memory-target`, `--stream-textures`, `--deep-stream`, the sound options,
`--no-mod`, `--no-patch`, `--no-t4-layout`, `--no-load-zone`, `--name`, `--loading-image` and the rest.
It finds the map's files as the Python does (`find_usermap`: the map, its patch, `mod.ff` here or in
`mods\<map>`, the mod's language zones, the `.iwd` files), and writes
`<output>\_codxe\t4\usermaps\<map>`. Options of its own, for checks against the Python (no Python
option starts with `--dev`):
- `--dev-zone P` and `--dev-dump P`: the map's zone, uncompressed, and the dump of its node tree;
- `--dev-timings`: the time of each phase;
- `--dev-no-zone-cache`, `--dev-no-sound-cache`, `--dev-ffmpeg P`.

`menu` turns CoD Xenon's 0.2.0 `patch_ui.ff` into t4ff's Custom Maps list, or takes a menu zone with CoD
Xe's own list (`--menu-zone`), and gives the maps their names, descriptions, `map.json` and pictures.
`setup` finds FFmpeg and xma2encode.exe, installs an encoder found in a `.zip` (the Downloads folder,
or `--xma2encode`) into the `bin` folder next to `t4ff-cli.exe`, and test-encodes a tone. `convert`
does that install itself unless `--no-install` is given. `gui` opens the window, `t4ff.exe` next to
`t4ff-cli.exe`.

The developer commands (`t4ff-cli dev` lists them):

```text
t4ff-cli roundtrip [--compress] [--jobs N] <file or folder>...  read and write back, compare (with its options or folders)
t4ff-cli dump <fastfile> <out.txt>                            the node tree as text
t4ff-cli rewrite <fastfile> <out.zone>                        the zone as written back
t4ff-cli bench [--jobs N] [--keep] <file or folder>...        reading speed and memory
t4ff-cli textures [--jobs N] <out.txt> <iwd or folder>...     texture battery, as CRCs
t4ff-cli texbench [--jobs N] <iwd or folder>...               texture conversion speed
t4ff-cli cache [--clear] [--zone-cache-dir DIR]               the zone and sound caches' sizes, or delete them
t4ff-cli regex <patterns.txt> <file>...                       regex engine check, as CRCs
t4ff-cli convert --out <zone> [options] <PC fastfile>...      convert, every input given explicitly
```

The developer `convert` (chosen by `--out`) takes every input explicitly: `--map-iwd`, `--map-files`,
`--load-ff`, `--map-name`, `--out-dir` (or `--sounds-dir`), the conversion's options under their Python
names, `--reference-techsets`, and `--dump`, `--ff`, `--no-zone-cache`, `--no-sound-cache`, `--ffmpeg`.

The program opts out of Windows 11's power throttling (EcoQoS), which otherwise runs a console
program that is not in front on slower cores at lower clocks: compressing The Simpsons' fastfile took
4.6 s that way, 1.2 s without.

`convert` runs the Python's `_convert_map` and the rest of its `convert` command, in their order:
1. converts the map's streamed sounds (with an encoder and an output folder);
2. gives the PC zones' scripts the map's loose ones, and notes the mod's scripts;
3. converts the stock streamed sounds the map's aliases use, from the PC game's files;
4. encodes the loaded sounds, converts each PC fastfile and merges them in order;
5. adds the scripts the map's scripts use but no zone has, and the assets the game looks up by name;
6. gives the mod's versions of the game's scripts names of their own, keeps or drops the mod's
   menus and videos, gives PC menus controller buttons and navigation, and fixes the scripts for the
   console;
7. fixes the technique sets' argument sections, removes the references nothing uses, keeps the
   loaded sounds within their limits and syncs the alias types;
8. writes the map's options and scripts folder, and with `--stream-textures` streams the textures
   into its `images.pak`;
9. measures the memory the zone takes and, over the target, converts again (steps 2 to 8) with less:
   the stock upgrades and the mip tails first, then the streamed textures' sizes, then a smaller
   texture budget (up to five conversions, as the Python's `convert` command);
10. writes its zone and its other files (above).

xma2encode is a separate program, run on every processor. The same input gives the same output (four
parallel encodes of one sound were identical), so encodings are kept in
`%LOCALAPPDATA%\t4ff\sound_cache`, by the SHA-256 of the WAV the encoder gets, its quality and its file.
A repeat conversion skips them.

`roundtrip` and `bench` take `--zone-cache` (the default folder) or `--zone-cache-dir DIR`. With
either, zones come from the zone cache (below).

Folders are searched for `.ff` files (`textures` and `texbench`: for `.iwd` files and loose
`images/*.iwi`). The work runs on every processor (or `--jobs N`).

## Memory: node views and the zone cache

A node's bytes are a view into the zone it was read from: a node is always one run of the stream,
since the reader refuses anything else. A node copies its bytes only when a conversion changes them
(`Bytes`, copy on write). The zone keeps its bytes alive (`Zone::source`), and a zone whose nodes
view another's keeps that one too (`Zone::keep_alive`).

The zone's bytes are in one of two places:
- **Memory.** Address space is reserved and committed as the zone is inflated: no buffer grows by
  copying, and nothing is committed past the end.
- **The zone cache** (`%LOCALAPPDATA%\t4ff\zone_cache`). A fastfile is inflated once, streamed
  straight to a file a few MiB at a time, then mapped. A cache file is named by the fastfile's path
  and remade when the fastfile's size or date changes.

The console library's zones use the cache. They are mostly texture and sound data that only an
asset copy reads, so those pages are never read from disk, and the system can drop the pages that
are. Repeat runs skip zlib altogether. `t4ff-cli cache` gives the cache's size and `--clear` empties
it.

Loading the 49 zones of a conversion's console library (4.3 GiB of zones, 2.3 million nodes):

| | committed memory at the peak | wall time |
| --- | --- | --- |
| copies (step 1) | 9.5 GiB | 3.1 s |
| views, zones in memory | 5.8 GiB | 1.7 s |
| views, zone cache | 1.6 GiB (the nodes' bookkeeping) | 0.58 s (1.8 s while making the cache) |

## Checking it against the Python t4ff

`tools/py_reference.py` gives the Python side of `dump` and `rewrite`, in the same format. Run it
from `tools/t4ff`:

```bash
python native/tools/py_reference.py dump <fastfile> py.txt
```

Then compare it with `t4ff-cli dump <fastfile> cpp.txt` (`cmp`/`fc /b`). The two dumps are identical
when the reader is right: every node, its block, offset, segments, origin and pointers.

`roundtrip` compares the written zone with the original. PC zones and t4ff's own console
conversions come back identical. Console zones made by other tools (the disc, CoD Xenon) differ
only in their header's size fields: the console linker counts delayed data and runtime blocks
differently. The Python writer differs the same way, and the report says when the body is the same.
`--compress` also compresses the zone again as t4ff does (1 MiB chunks, as pigz does) and compares
the whole fastfile, which is identical for fastfiles t4ff wrote.

`textures` converts every IWI of the archives given in a fixed battery and writes CRCs of
everything:
- the parsed levels;
- console textures with four option sets: the defaults; normal map with two levels dropped and no
  mip tail; at most 64 texels and uncompressed, then `with_mips`; twenty levels dropped, which
  decodes, downscales and encodes DXT again;
- the headers, the untiled levels and the size estimates.

`py_reference.py textures` writes the same report, and `tools/compare_reports.py` compares two
reports and names the fields that differ.

The script and menu passes find their matches with Python's `re`, so the C++ has a regex engine
with its semantics (`src/core/pyre.*`): a backtracking matcher whose state is on the heap. A bytes
pattern has ASCII `\w`, `\s`, `\d` and `\b`, as Python's do. A str pattern runs on the latin-1 text
the Python decodes, with Unicode's classes and case folding of those 256 characters. Checking it:

```bash
python native/tools/regex_check.py patterns pats.txt
python native/tools/regex_check.py corpus corpus/ <PC fastfile>...
python native/tools/regex_check.py run pats.txt corpus/*.txt > py.txt
t4ff-cli regex pats.txt corpus/*.txt > cpp.txt
```

`patterns` writes every pattern of the passes (taken from the modules, and those their functions
build), plus a few that exercise the engine itself. `corpus` writes the raw files and strings of
fastfiles. `run` writes a CRC per pattern and file: `finditer` over the file, and `match`, `fullmatch`
and positioned searches over each line. The two outputs must be identical.

The DXT encoder works in double precision exactly as the numpy one does. numpy 2.x's `einsum` adds
three products as `(p0 + p2) + p1` (its two lane SIMD loop), so the C++ does too. Another numpy
could change the Python output, but not the C++ output.

## State

| Step | What | State |
| --- | --- | --- |
| 1 | Foundation: fastfiles, layouts, zone code commands, zone reader and writer | done |
| 2 | Textures: IWI, wavelet, DXT, Xenos tiling | done |
| 3 | Conversion core: assets, technique sets, console library, merge, xanims | done |
| 4 | Sounds: decoders, XMA encoding in parallel, cache | done |
| 5 | Scripts, menus, loading screens | done |
| 6 | Streaming and the memory planner | done |
| 7 | Command line parity with the Python t4ff | done |
| 8 | The GUI (Dear ImGui): simple and advanced modes, queue, results | done |

Step 1, checked on 2026-10-05:

- **PC:** 36 of 36 usermap fastfiles round-trip identical: Kino Rezurrection, Kino Der Toten, The
  Simpsons, Mini-Labor, NukeCraft, Matrix, Dead Sand and Super Mario 64. With CoD Xenon's 4 console
  game zones (`Downloads/zone`, identical too), that's 1.76 GiB of zones in 3.2 s.
- **Console:** 88 of 88 readable fastfiles on the disc folder and its CoD Xenon maps give the
  Python writer's bytes. 29 signed multiplayer fastfiles (`IWff0100`) can't be read by either
  version.
- **Node trees:** the dumps of `ber1_load`, `intro_pac`, CoD Xenon's `mario.ff` (458,187 lines)
  and Kino Rezurrection's `mod.ff` (227,634 lines) are identical to the Python's.
- **Compression:** Kino Rezurrection's console conversion (`d.ff`, 211 MiB) compresses again to
  the identical fastfile.
- **Speed:** on one thread, zones read 7-9 times faster than the Python (`mario.ff`: 0.20 s vs
  1.55 s) and write 3-4 times faster. zlib's inflate is now the largest cost of one file. The 49
  console zones of a conversion's library (4.3 GiB) load in 3.1 s on 20 threads.

Step 2, checked on 2026-10-05:

- **Kino Rezurrection:** the battery's 7,355 lines over its 1,471 IWIs are identical to the
  Python's. The IWIs are DXT1/3/5, uncompressed, and 26 wavelet ones from Black Ops.
- **Stock:** the PC game's 8,822 IWIs give 44,110 identical lines, including its 20 R8G8B8 cube maps.
- **Speed:** converting Kino Rezurrection's textures with the default options (582.4 MiB of
  console textures) takes 37.9 s in Python, 3.0 s on one thread and 0.46 s on 20 threads.

Step 3, checked on 2026-10-06. `py_reference.py convert` runs the same steps with the Python t4ff;
the converted zone and the dump of its node tree must be identical. Each map is converted from all
its fastfiles (map, patch, mod, and the mod's language zones) and its `.iwd` files, with:
- a console library of CoD Xenon's 23 maps and its 4 game zones;
- the PC game's stock textures;
- a texture budget of 100 MiB, which makes the planner drop levels and rebuild library textures.

| Map | Zone | Python | C++ |
| --- | --- | --- | --- |
| The Simpsons | 162.7 MiB | 127.7 s | 5.2 s |
| Mini-Labor | 192.4 MiB | 130.1 s | 5.8 s |
| NukeCraft | 199.7 MiB | 95.4 s | 4.8 s |
| Super Mario 64 | 209.5 MiB | 85.2 s | 7.6 s |
| Kino Der Toten (UGX) | 197.8 MiB | 88.6 s | 7.6 s |
| Kino Rezurrection | 234.5 MiB | 147.8 s | 11.9 s |

All six are identical. So is Matrix, with and without the console library (66,869 nodes).
`tools/compare_maps.py` runs these comparisons; `--cpp-only` checks the C++ again against the Python
outputs of an earlier run.
Textures are built on every processor before the zones are converted (`prebuild_textures`), and the
images read the same way (`prefetch_image_sources`): the same function of the same data, so the bytes
are the same.

Step 4, checked on 2026-10-06 with `compare_maps.py --sounds`: the same maps and options as step 3,
with xma2encode. Each side writes its own sounds folder; the zones, the dumps and every `.xma` file
must be identical.

| Map | Sound files | Python | C++, cache empty | C++, cache full |
| --- | --- | --- | --- | --- |
| The Simpsons | 1,881 | 142.3 s | 27.1 s | |
| Mini-Labor | 3,043 | 173.2 s | 39.5 s | |
| NukeCraft | 1,748 | 153.4 s | 24.4 s | |
| Super Mario 64 | 2,128 | 152.1 s | 24.9 s | |
| Kino Der Toten (UGX) | 346 | 123.9 s | 16.3 s | |
| Kino Rezurrection | 1,707 | 184.4 s | 34.8 s | 23.5 s |

All six are identical, and so is Matrix (1,878 sound files). The checks cover:
- loaded sounds: XMA1 repacking, seek tables and format blocks, resampled to the XMA1 rates;
- streamed sounds: the map's and the stock ones, in 4 KiB XMA2 blocks;
- the loaded sound limit: identical sounds shared, the longest streamed;
- the alias types.

The stock streams come from the PC game's files and are in no cache, so "cache empty" still reused
those the earlier maps had encoded.

Two details matter for the identical bytes:
- **Resampling** is numpy's float64 expression term for term. numpy's float64 `sin` and `cos` are
  the C runtime's (`ucrtbase.dll`) on processors without AVX-512, so the resampler calls those
  (`audio.cpp`, `Trig`).
- **Child processes** (xma2encode, FFmpeg) inherit their three standard handles only. Inherited
  handles of files other threads had open made parallel encodes fail ("being used by another
  process").

Step 5, checked on 2026-10-06. `py_reference.py convert` now runs t4ff's own `_convert_map` (without
streaming), then what its `convert` command writes after the zone. Each side writes the map's
folder (`<out dir>/<map>.<side>.dir`), and `compare_maps.py` compares every file in it, besides the
zone and the dump. The options are those of step 3, without sounds:

| Map | Files in the folder | Python | C++ |
| --- | --- | --- | --- |
| The Simpsons | 10 | 81.1 s | 7.1 s |
| Mini-Labor | 30 | 98.2 s | 8.0 s |
| NukeCraft | 14 | 100.8 s | 7.2 s |
| Super Mario 64 | 11 | 91.1 s | 8.0 s |
| Kino Der Toten (UGX) | 14 | 100.9 s | 8.2 s |
| Kino Rezurrection | 17 | 136.4 s | 10.4 s |
| Matrix | 12 | 82.7 s | 6.6 s |
| Dead Sand | 11 | 79.2 s | 6.6 s |

All eight are identical: zones, dumps, and every file. The times now include compressing the map's
fastfile and making its loading screen. With sounds (`--sounds`, cache full), Mini-Labor's 3,073
files and Kino Rezurrection's 1,725 are identical too: 33.6 s and 26.8 s against 183.9 s and 198.1 s. The passes these maps exercise:
- **Scripts:** the map's loose scripts, missing scripts and the zombie mode's client scripts, named
  assets (shellshocks, anim tree animations, footsteps, player animations), the mod's scripts renamed
  or put in the scripts folder.
- **Script fixes:** use key hints, modderHelp, script_struct spawns, cursor hints, precaches before
  the first wait, menu dvar defaults, speed_up_zombies, zombie idles, splitscreen fog, MG42 turrets.
- **Menus:** front end menus dropped, game menu lists and menus renamed, gamepad buttons, inert items,
  D-pad navigation, and the options (Mini-Labor's difficulty: `options.txt`, the in-game menus and the
  level script asking them).
- **The map's files:** the loading screen, copied from CoD Xenon's or made from a title card scaled
  through FFmpeg, then `preview.bin`, `map.json` and `preview.dds`.

The regex engine gives Python's matches for all 67 patterns over the 227 raw files and string sets
of four maps' patch and mod zones: 15,209 identical signatures.

Step 6, checked on 2026-10-06. The Python side is now t4ff's own `convert` command
(`py_reference.py cli` runs `python -m t4ff` with its arguments, saving the zone it writes and the
dump of its node tree), so the memory loop and everything after the zone are its own code. The C++
gets the same map through its explicit options. These runs use `compare_maps.py --budget auto` (the
automatic budget and a 212 MiB target, `t4ff convert`'s defaults) and `--extra` for the streaming
options:

| Map | Options | Conversions | Python | C++ |
| --- | --- | --- | --- | --- |
| Kino Rezurrection | automatic budget | 3: mip tail, then 82.7 MiB of textures | 273.7 s | 22.3 s |
| The Simpsons | automatic budget | 1 | 80.6 s | 7.3 s |
| Kino Der Toten (UGX) | `--stream-textures --deep-stream all` | 1 (683 streamed, 665 deep) | 123.3 s | 11.3 s |
| Kino Rezurrection | `--stream-textures --deep-stream all` | 3: upgrades and mip tail, then 26 MiB less of the 897 streamed | 395.7 s | 27.1 s |
| Mini-Labor | `--stream-textures` | 3: upgrades and mip tail, then 41.3 MiB less of the 661 streamed | 197.4 s | 20.1 s |
| Kino Der Toten, the disc in the library | `--stream-textures --deep-stream all` | 1 (21 console `.hi` files copied, 9 unstreamed) | 196.7 s | 14.9 s |

All are identical: the zones, the dumps, `images.pak` and every other file. So are the other six maps
on the automatic budget (Mini-Labor, NukeCraft, Super Mario 64, Kino Der Toten, Matrix and Dead
Sand; most convert twice): 6.9-17.9 s against 80.9-168.9 s. The checks cover:
- the pack (deep entries, eighth-size entries, the console's `.hi` files);
- mips made for textures saved without them;
- the PC versions of stock textures, streamed or whole;
- the boxes of models and world surfaces, and the world's tree;
- every branch of the memory loop.

The boxes need numpy's float64 arithmetic: its `norm` adds the three squares in order, without
fused multiplies, along an axis and for one vector alike. Model texture coordinates are float16.

Step 7, checked on 2026-10-06. `compare_maps.py` now gives both programs the same command line
(`convert <map> -o <root> ...`), the C++ adding `--dev-zone` and `--dev-dump`. The eight maps on the
automatic budget give identical zones, dumps and folders: 6.2-22.3 s against 134-473 s (the Python
runs throttled in the background, see "Using it").

The other commands give the Python's files and output:
- `menu` on CoD Xenon's 0.2.0 `patch_ui.ff` (the rewritten menu zone, its `.orig`, 13 descriptions,
  `preview.txt`, `preview.bin`, 14 `map.json` and `preview.dds`), run again from the `.orig` with
  `--rows 9`, and with CoD Xenon's 0.3.0 list given by `--menu-zone`;
- `streams` on two of CoD Xenon's maps' sound folders (121 files rewritten);
- `info --list` and `roundtrip` on a PC and a console zone, and `menu`'s error for a folder without a
  menu zone.

Step 8, checked on 2026-10-06:
- **Command lines:** `gui_args_check.py` gives the Python window's command lines for nine saved
  settings (including older versions of its settings file, which it migrates) and two inputs each:
  18 identical.
- **Conversions:** Matrix and Dead Sand, converted through the window (hidden, `--dev-run`), give
  step 7's folders. Mini-Labor with its sounds gives `t4ff-cli`'s files (3,043 `.xma`), the encoders
  running in the worker's job object.
- **Stop:** ending the window in the middle of a conversion leaves no process behind.
- **Screens:** both modes and both themes, at 100% and 125%, drawn by `--dev-screenshot`: empty,
  converting (the step, its count and the bar), done (memory, time, the banner), and the dialogs.
- **CoD Xe's settings:** written into a copy of the game's `codxe.json` (its keys, order and CRLF
  kept, `log_console` on, `startup_command "devmap matrix"`), turned off again (the startup command
  removed, the active mod set), into an output without one (a new file), and onto an invalid file
  (left untouched, a warning in the log).

## Layout

| Python | C++ |
| --- | --- |
| `layout.py` (libclang) | `tools/gen_schema.py` writes `data/layout_*.json`, read by `src/core/layout.*` |
| `commands.py` | `src/core/commands.*` |
| `zone.py` | `src/core/zone.*` |
| `fastfile.py` | `src/core/fastfile.*`, and `src/core/zone_cache.*` (zones in memory or mapped) |
| `platforms.py` | `src/core/platforms.*` |
| `images.py`, `stream.py`'s `with_mips` | `src/core/image.*` (and `zip.*`: `.iwd` archives) |
| `wavelet.py` | `src/core/wavelet.*` (codeword tables in `wavelet_tables.inc`) |
| `dxt.py` | `src/core/dxt.*` |
| `xenos.py` | `src/core/xenos.*`, formats in `texture_format.h` |
| `convert.py` | `src/convert/converter.*` and `record_map.*` (RecordMap, ScalarMap, _ArrayMap) |
| `assets.py` | `src/convert/assets.*` |
| `audio.py` | `src/audio/audio.*` (with `src/core/process.*`, `src/core/sha256.*`) |
| `soundbudget.py` | `src/audio/soundbudget.*` |
| `deps.py` | `src/app/deps.*` (`find_xma2encode` and `ffmpeg_exe` in `audio.cpp`): no Python packages to install |
| `library.py` | `src/convert/library.*` (zones read on every processor, from the zone cache) |
| `merge.py` | `src/convert/merge.*`; its menu and video passes in `src/convert/menus.*` |
| `techsets.py`, `xanim.py` | `src/convert/techsets.*`, `src/convert/xanim.*` |
| `scripts.py`, `named.py` | `src/convert/scripts.*` (the script text it adds: `script_templates.inc`, from `tools/gen_templates.py`) |
| `menu.py` | `src/convert/menus.*` (the conversion's passes), `usermaps_menu.*` (the `menu` command's list), `menu_editor.h` (MenuEditor, clone); `map.json` and `preview.dds` in `loadscreen.*` |
| `loadscreen.py` | `src/convert/loadscreen.*` (the title card's font: `loadscreen_tables.inc`) |
| `stream.py`, `memory.py` | `src/convert/stream.*` (`with_mips` in `src/core/image.*`); the memory loop in `src/app/convert.*` |
| Python's `re`, `str` | `src/core/pyre.*` (a regex engine with `re`'s semantics), `src/core/pystr.*` |
| `__main__.py` | `src/cli/t4ff_cli.*` (its command line), `src/app/convert.*` (`cmd_convert` from the map's fastfiles on, `_convert_map`), `src/app/usermap.*` (`find_usermap`); `src/cli/main.cpp` (the developer commands) |
| `progress.py` | `src/core/progress.*` (`--progress-lines` for the window, or a sink) |
| `gui.py` | `src/gui/*`: `app.*` (the window), `settings.*` (`Settings`, `convert_args`, `advice`), `worker.*` (the worker process), `codxe_config.*` (CoD Xe's `codxe.json`, the window's own), `theme.*`, `shell.*` (Windows' dialogs), `main.cpp` (Direct3D 11, the loop) |

`src/core/threads.*` runs work on every processor, with threads that have stacks as large as the main
thread's (64 MiB): zones are walked recursively.
