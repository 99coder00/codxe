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

The program is `build/Release/t4ff-cli.exe`, and it needs nothing else to run. The structure layouts
and commands are in `data/`, compiled into the program as resources. After changing
OpenAssetTools' definitions or t4ff's `defs/`, regenerate them from `tools/t4ff` (with the Python
t4ff's requirements installed):

```bash
python native/tools/gen_schema.py
```

## Using it

```text
t4ff-cli info <fastfile>... [--list]                         platform, block sizes, assets
t4ff-cli roundtrip [--compress] [--jobs N] <file or folder>...  read and write back, compare
t4ff-cli dump <fastfile> <out.txt>                            the node tree as text
t4ff-cli rewrite <fastfile> <out.zone>                        the zone as written back
t4ff-cli bench [--jobs N] [--keep] <file or folder>...        reading speed and memory
t4ff-cli textures [--jobs N] <out.txt> <iwd or folder>...     texture battery, as CRCs
t4ff-cli texbench [--jobs N] <iwd or folder>...               texture conversion speed
t4ff-cli cache [--clear] [--zone-cache-dir DIR]               the zone cache's size, or delete it
```

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

The DXT encoder works in double precision exactly as the numpy one does. numpy 2.x's `einsum` adds
three products as `(p0 + p2) + p1` (its two lane SIMD loop), so the C++ does too. Another numpy
could change the Python output, but not the C++ output.

## State

| Step | What | State |
| --- | --- | --- |
| 1 | Foundation: fastfiles, layouts, zone code commands, zone reader and writer | done |
| 2 | Textures: IWI, wavelet, DXT, Xenos tiling | done |
| 3 | Conversion core: assets, technique sets, console library, merge, xanims | |
| 4 | Sounds: decoders, XMA encoding in parallel, cache | |
| 5 | Scripts, menus, loading screens | |
| 6 | Streaming and the memory planner | |
| 7 | Command line parity with the Python t4ff | |
| 8 | The GUI (Dear ImGui): simple and advanced modes, queue, results | |

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
| `__main__.py` | `src/cli/main.cpp` |

`src/core/threads.*` runs work on every processor, with threads that have stacks as large as the main
thread's (64 MiB): zones are walked recursively.
