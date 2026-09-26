# t4ff: World at War PC to Xbox 360 fastfile converter

`t4ff` converts PC Call of Duty: World at War usermaps (custom zombie maps) into
fastfiles that CoD Xe loads on the Xbox 360 (`_codxe/t4/usermaps/<map>/`), and
keeps them small enough for the console's memory:

- **Every asset type of a map is converted**: models, animations, the world
  (GfxWorld, collision, paths), effects, weapons, materials, sounds, menus,
  scripts, string tables and localized strings.
- **Textures** are rebuilt as tiled Xenos textures with their mip chains.
  Uncompressed textures are DXT compressed. Each map keeps as much texture
  quality as fits in memory: the texture budget is what a memory target based on
  CoD Xenon's working maps leaves, and only when a map is over it do its largest
  textures lose top mip levels. Stock textures use the console's own versions.
- **Sounds** are encoded to XMA: loaded sounds into the fastfile (XMA1, as the
  console's in-memory sounds are), streamed sounds to `sounds/*.xma` files.
  Both can be downsampled or downmixed to save memory.
- **Technique sets (shaders)** cannot be converted from PC data. They are copied
  from Xbox 360 fastfiles you provide (`--console-zone`), together with any stock
  image, sound or other asset the PC map expects from the game. What the game's
  own zones (`common.ff`, ...) already load stays a name reference when those
  zones are among the fastfiles given.
- **Assets the game looks up by name** that a PC map lacks (player body
  animations, shellshock files) are added from the Xbox 360 fastfiles given.
- **One fastfile per map**, as in CoD Xenon's converted maps: `mod.ff` (the
  console has no mod zone) and `<map>_patch.ff` are merged into `<map>.ff`.
  Scripts of later zones win (`_patch` over the map, `mod.ff` over both), and
  assets several zones define (including textures nested in materials) are
  loaded once.
- **A loading screen** (`<map>_load.ff`) made like CoD Xenon's, with the map's
  own picture, one you give, or a title card with the map's name.

Every written fastfile is read back with the console loading rules before the
tool reports success.

## Requirements

- Python 3.9+ (from python.org on Windows; the window needs its tkinter).
- The `OpenAssetTools` folder of this repository. The PC structure definitions
  and zone streaming rules are read from it (`src/Common/Game/T4/T4_Assets.h`,
  `src/ZoneCode/Game/T4`).
- Python packages (numpy, libclang, imageio-ffmpeg with FFmpeg): installed
  automatically when missing.
- For sounds: `xma2encode.exe`. No open source XMA encoder exists, and this one
  is part of Microsoft's licensed Xbox developer kits (Xbox 360 XDK, Xbox One
  XDK, or the Microsoft GDK with Xbox extensions), so it cannot be downloaded
  automatically. Once you have it, `t4ff` finds and installs it by itself (see
  below). On Linux and macOS it runs through `wine`.
- For technique sets: Xbox 360 World at War fastfiles, from your own copy of the
  game (for example `common.ff` and the stock zombie maps) or maps already
  converted by CoD Xenon. Several can be given; the first one that has an asset wins.

### Installing dependencies

```sh
python -m t4ff setup                           # install what is missing and check it
python -m t4ff setup --xma2encode <exe, folder or .zip>
```

The window does the same when it opens (it asks before installing packages)
and with **Set up dependencies**. `convert`, `info` and `roundtrip` also install
missing Python packages before running (`--no-install` turns this off).

`xma2encode.exe` is looked up in `tools/t4ff/bin`, the `XMA2ENCODE` variable,
the folders the Xbox developer kits install to (`XEDK`, `DurangoXDK`,
`GXDKLatest`/`GameDK`, `Program Files (x86)\Microsoft Xbox 360 SDK`, ...), your
Downloads and Desktop folders, and `.zip` files in Downloads. An installed kit's
encoder (for example `C:\Program Files (x86)\Microsoft Xbox 360 SDK\bin\win32\xma2encode.exe`)
is used where it is; one inside a `.zip` is extracted (with the DLLs next to it)
into `tools/t4ff/bin`, where every later run finds it. Setup tests it by
encoding a short tone. If that fails, it prints what the encoder says about its
options: please include that output when reporting the problem.

## Usage

### Window

Double click `t4ff_gui.pyw` (Windows), or run `python -m t4ff gui`. Choose the
PC usermap folder, an output folder, `xma2encode.exe` and the Xbox 360
fastfiles to take shaders from, set the memory options and press **Convert**.
The conversion runs as a separate process, so the window stays usable. The
line under the buttons shows the current step with its count (for example
`Converting mod.ff (file 3 of 3): 1200/2400 (50%)`), **Stop** ends it. The log
shows the conversion and, at the end, the memory the map needs; settings are
remembered for the next run. **Inspect fastfile...** lists the content of any
PC or Xbox 360 fastfile.

The window uses tkinter, which the python.org installers include (on Linux:
`sudo apt install python3-tk`).

### Command line

```sh
cd tools/t4ff

# Convert a PC usermap folder (containing <map>.ff, mod.ff, *.iwd, ...)
python -m t4ff convert "C:/.../mods/nazi_zombie_aztec" -o out \
    --xma-encoder "C:/Program Files (x86)/Microsoft Xbox 360 SDK/bin/win32/xma2encode.exe" \
    --console-zone "D:/codxe-t4-fastfiles-v0.2.0/_codxe/t4" \
    --iwd "C:/Program Files (x86)/Activision/Call of Duty - World at War/main"

# Then copy out/_codxe into the World at War game folder (merge with the existing _codxe folder).
```

Useful options:

| Option | Effect |
| --- | --- |
| `--console-zone PATH` | Xbox 360 fastfile (or folder of them) to copy console only assets from: technique sets, and stock images, sounds, models... the PC map expects from the game. Repeatable; the first one that has an asset wins. See [Console fastfiles](#console-fastfiles). |
| `--iwd PATH` | The PC game's own files (e.g. its `main` folder): stock textures a map uses and no console fastfile has are converted from them. Stock textures the console fastfiles have keep the console's version (Treyarch sized them for the console), which leaves the memory to the map's own textures. |
| `--texture-budget MIB` | Texture memory for the map and its mod together, `0` for no limit. Default `auto`: what `--memory-target` leaves, at most 96 MiB. Largest textures lose their top mip level first; the world's lightmaps keep theirs. |
| `--memory-target MIB` | Memory the map may use once loaded, for the automatic texture budget (default 200: CoD Xenon's 13 maps use 148 to 220). The map is converted, measured and, when over, converted again with less texture memory. |
| `--loaded-sound-memory MIB` | Memory of the loaded sounds (default 32; CoD Xenon's maps have up to 35). Beyond it the longest become streamed sounds, which keep their quality. 0: no limit. |
| `--max-texture-size N` | Cap texture dimensions. |
| `--no-mips` | Drop all mip levels (about 25% less memory, but textures shimmer at a distance). |
| `--no-compress` | Keep uncompressed textures uncompressed. |
| `--sound-rate HZ`, `--mono-sounds` | Downsample (24000, 32000, 44100 or 48000) or downmix loaded (in memory) sounds. |
| `--stream-rate HZ`, `--mono-streams` | Resample or downmix streamed sounds. |
| `--xma-quality N` | xma2encode quality (1-100, default 60). |
| `--no-mod`, `--no-patch` | Do not merge `mod.ff` / `<map>_patch.ff` into the map fastfile. |
| `--no-load-zone` | Do not write `<map>_load.ff`, the loading screen (see [Loading screen](#loading-screen)). |
| `--name TEXT` | The map's name in the map list and on its title card (default: the `longname` of its `.arena` file, else from its file name). Written to `description.txt` in the map's folder. |
| `--loading-image PATH` | Picture for the loading screen (`.png`, `.jpg`, `.bmp`, `.tga`, `.dds`, `.webp` or `.iwi`, any size: scaled to 1280x720). |
| `--no-t4-layout` | Write `_codxe/usermaps/<map>` instead of `_codxe/t4/usermaps/<map>`. CoD Xe reads `_codxe\t4` when it exists (its newer layout, used by CoD Xenon's 0.2.0 maps) and then ignores `_codxe\usermaps`, so this is only for a console without a `_codxe\t4` folder. |
| `--allow-unverified` | Also convert asset types whose console layout was not verified. Expect crashes. |
| `--max-loaded-sounds N` | Loaded (in memory) sounds the map may have, default 1500. The console holds 1600, the game's own included; a map with more stops with "Exceeded limit of 1600 'loaded_sound' assets". Identical sounds are shared, then the longest ones become streamed sounds played from the map's `sounds` folder. 0: no limit. |
| `--jobs N` | Sounds encoded at a time and threads compressing the fastfile (default: one per processor). |

Other commands:

```sh
python -m t4ff info <fastfile> [--list]   # blocks, asset counts, asset names (PC or console)
python -m t4ff menu <game>/_codxe/t4       # the map list shows every map of usermaps (see below)
python -m t4ff roundtrip <fastfile>...    # read + rewrite, checks the result is byte identical
```

### Testing on the console

**How CoD Xe finds the map.** CoD Xe does not keep a list of usermaps: when the
game opens the fastfile of a zone, CoD Xe serves
`_codxe/t4/usermaps/<zone>/<zone>.ff` instead if it exists (and the sounds of the
active map from its `sounds` folder). So the folder and the fastfile must carry
the map's name exactly; `t4ff` names them after the PC map fastfile.

**How it appears in the menu.** See [Map list in the menu](#map-list-in-the-menu)
for a list of every map of the `usermaps` folder. Otherwise, the map list is part of CoD Xenon's
`_codxe/t4/zone/patch_ui.ff` (one button per map that runs `devmap <map>`) and
`patch.ff` (map names and descriptions). It lists the maps of their release. A map converted by `t4ff` with one of these names (e.g.
`nazi_zombie_aztec`) is started from that button. Any other map is started from
the CoD Xe console: plug a USB keyboard into the console, press the console key
(`~`) and type `devmap <map>`.

**A first test.** Aztec is the best first test, because CoD Xenon's working
conversion of the same map is there to compare with:

1. Install CoD Xenon's fastfiles package and check that their Aztec loads. This
   confirms CoD Xe, the title update and their `patch_ui.ff` / `patch.ff`.
2. Convert the PC Aztec folder. For this first run you can give their
   `nazi_zombie_aztec.ff` as a console fastfile: shaders, stock textures and
   loaded sounds then come from a version known to work, and the test isolates
   what `t4ff` converts (models, animations, world, scripts, effects, weapons).
3. Back up their `_codxe/t4/usermaps/nazi_zombie_aztec/nazi_zombie_aztec.ff`,
   replace it with yours and start Aztec from the menu.
4. Then convert again with stock Xbox 360 fastfiles of your game as console
   fastfiles (and `xma2encode.exe`) instead, which is how other maps are
   converted.

If the game stops while loading, run `python -m t4ff info` on the fastfile and
report the output together with the step that failed.

### Scripts

On PC, with the map's mod active, the game reads scripts from the mod's own
files (its `.iwd` files and loose files) before its fastfiles, and scripts can
use those of the game's own zones. The console reads scripts from fastfiles
only. So the map's loose scripts replace those of its fastfiles (PC Aztec ships
newer `_zombiemode.gsc`, `_zombiemode_spawner.gsc` and `_loadout.gsc` in its
`.iwd`), and scripts the map's scripts include or call but its fastfiles lack
are added: from the map's files, the Xbox 360 fastfiles given, or the PC game
folder (also its `raw` folder). PC Aztec calls
`maps\_zombiemode_weapons_sumpf` from Shi No Numa, which the console does not
load for a usermap ("Could not find script"); it comes from CoD Xenon's Aztec
when that is given with `--console-zone`. Scripts found nowhere are left to the
game's own zones, and the log lists them.

### Console fastfiles

The Xbox 360 fastfiles given with `--console-zone` (the window's "Xbox 360
fastfiles" list) are where technique sets and other console only assets come
from, so the more a map shares with them, the more of it converts. The best set
is CoD Xenon's whole 0.2.0 package: add the `_codxe/t4` folder of the extracted
download (all its maps and its `zone` folder), not the folder the game reads,
which your own conversions go into. The fastfiles of a folder are read in name
order and the first one that has an asset gives it, except that CoD Xenon's
conversion of the map being converted (same name) is read first: its versions
of what other maps also have (scripts, sounds...) win. Reading all of it takes a
few minutes and about 4.5 GiB of memory.

`common.ff`, `code_post_gfx.ff` and `patch.ff` among them are recognized as the
game's own zones, loaded before any map, which makes the conversion better:

- what the map expects from the game and those zones have (technique sets,
  stock images, sounds...) stays a name reference instead of a copy (less
  memory, fewer asset slots used);
- scripts they have are not copied into the map;
- assets the game looks up by name while the map starts, which the PC map lacks
  too, are added from the other fastfiles: the player body animations the
  player animation script of `common.ff` lists (PC Aztec and CoD Xenon's Aztec
  have no satchel ones, "Could not load xanim pb_hold_run_satchel", CoD
  Xenon's `zm_tranzit` has them; the campaign vehicle rides of its
  `scriptevent` block are left out), and the shellshock files the scripts name
  (`shock/zombie_death.shock`: the zombie scripts play it when a player dies,
  but most maps lack it, and the game then prints "'0' is not a valid value for
  dvar 'bg_shock_viewKickPeriod'"; CoD Xenon's `nazi_zombie_derberg` has it).

Some errors in the console log come from the PC map itself and are harmless:
PC Aztec's zombie type names a `walther` sidearm zombies never draw, and
`collision_geo_32x32x128` is precached but never used.

### Map list in the menu

CoD Xenon's Nazi Zombies menu lists the four stock maps and a fixed set of
converted maps, as many as the screen holds. With the CoD Xe build that lists
the usermaps folder (`src/game/t4/sp/components/usermaps.cpp`, see
[docs/t4.md](/docs/t4.md#usermaps-list)), run once:

```sh
python -m t4ff menu "<game>/_codxe/t4"      # or Update game menu... in the window
```

It keeps CoD Xenon's `zone/patch_ui.ff` as `patch_ui.ff.orig` and rewrites the
menu's map list: the stock maps stay, then 13 rows show the maps of the
`usermaps` folder, sorted by name. The D-pad scrolls past the first and last
rows, LB / RB move a page, the line under the rows tells where you are
("14-26 / 40"), and the right side shows the focused map's name, description and
picture. New maps show up the next time the menu opens, without running it
again.

A map's name and description come from `description.txt` in its folder (first
line, next lines): `convert` writes the name (`--name`, the "Map name" field),
`menu` writes those of CoD Xenon's maps from their menu. The picture of CoD
Xenon's maps is theirs, in the menu zone (`preview.txt` names it); for the other
maps it is `preview.bin`, their loading screen at 512x288, which `convert`
writes next to `<map>_load.ff` and `menu` writes for the maps already installed
with one. CoD Xe copies it into a picture slot the menu zone has for it, so
maps converted before need `menu` run once more, for the slot.

### Loading screen

While a map loads, the game shows the material `$levelbriefing` of the map's
load zone, and a checkerboard when there is none. The load zone is made from one
of CoD Xenon's (found among the console fastfiles: all of their 0.2.0 maps have
the same one, with a 1280x720 picture) with, in this order:

1. the picture given with `--loading-image`;
2. CoD Xenon's own loading screen, when they converted the same map;
3. the map's own PC loading screen (`images/loadscreen_<map>.iwi`);
4. a title card with the map's name (from its `.arena` file), for maps without
   one: Zombie Woods (2008) has none.

Without any of CoD Xenon's load zones among the console fastfiles, the map's PC
`_load.ff` is converted when it has one.

### Memory

The tool prints the memory the map needs once loaded. CoD Xenon's 13 maps of
their 0.2.0 release need 148 to 220 MiB (textures up to 99 MiB, loaded sounds up
to 35 MiB); CoD Xe does not change the game's memory, so these are the known
good values. By default each map gets what fits: its textures get what the
`--memory-target` (200 MiB) leaves after everything else, at most 96 MiB, and
keep full quality when that is enough (Zombie Woods, Aztec: about 130 MiB with
every texture at full size). A map over the target is converted again with
less texture memory, its largest textures losing top mip levels first; the
world's lightmaps keep theirs. Loaded sounds beyond `--loaded-sound-memory`
(32 MiB) are streamed, the longest first. `--max-texture-size`,
`--sound-rate 32000` and `--mono-sounds` trade more quality for memory.

Besides memory, the console has a fixed number of slots per asset type, the
game's own assets included. Loaded sounds are the tight one: 1600 slots, which
the PC Aztec (1576 loaded sounds) overflows once the game's own are loaded.
`--max-loaded-sounds` (default 1500, under the 1532 of CoD Xenon's Aztec, which
loads) shares identical sounds and streams the longest ones from the map's
`sounds` folder to stay within it. CoD Xe enlarges the menu and effect pools;
the other types of a converted map stay close to CoD Xenon's counts.

## How it works

1. **Layouts.** `t4ff/layout.py` computes the exact MSVC x86 layout of every
   T4 structure with libclang from OpenAssetTools' `T4_Assets.h`. Console
   layouts come from the same header, with the structures that differ on the
   360 replaced by `t4ff/defs/x360_structs.h`.
2. **Streaming rules.** `t4ff/commands.py` parses OpenAssetTools' zone code
   commands (string and count directives, conditions, blocks, reorders, ...).
   Console differences are in `t4ff/defs/x360_commands.txt`.
3. **Zones.** `t4ff/zone.py` interprets those rules exactly like the game's
   `DB_Load*` functions. The result is a tree of allocations with their pointers
   (following, insert, offset and alias pointers), which is written back for any
   platform with recomputed block offsets.
4. **Conversion.** `t4ff/convert.py` maps every structure field by field from
   the PC layout to the console layout (byte swapping, resizing, dropping members
   the console does not have), with per-structure encodings and fixups.
   `t4ff/assets.py` holds the asset-specific rules, `t4ff/xanim.py` the
   animation data, `t4ff/audio.py` the sounds and `t4ff/library.py` the copies
   from console fastfiles.

### What was recovered about the console format

These facts were recovered by comparing the PC `nazi_zombie_aztec` usermap with
CoD Xenon's conversion of it, asset by asset:

- The container is the same `IWffu100` / version `0x183` header, but big
  endian with a zlib-compressed zone.
- **Images**: `GfxImage` is 40 bytes, followed by a 16-byte load def and a
  52-byte `D3DBaseTexture` header in the virtual block. The header is stored
  *little endian*. Pixel data is streamed after all assets into the large
  runtime block, and its size is `cardMemory`.
- **Materials / technique sets** have 51 technique slots: the console has no
  instanced lit techniques (PC 0x24-0x2A) and no instanced debug bump map
  technique (PC 0x3A), the others keep their order. State bits use the PC
  encoding; the table only keeps the entries of the console technique set's
  techniques. Technique sets carry console shaders (a cached part and a
  32-byte-aligned physical part; a pass holds one vertex shader per vertex
  format), so they come from console fastfiles.
- **Animations** have 12 part types: 7 rotation types (the full quaternion
  types exist in a 32-bit and a precise 48-bit variant), 4 translation types
  and all. Quaternions are packed as "smallest components": sign and index of
  the largest component, the others divided by it (half quaternions: 16 bits;
  full: 9+10+10 or 15+15+15 bits). Viewmodel animations and the main skeleton
  bones use the precise variant. Bones are sorted by part type.
- **Models and the world**: normals and tangents are 10:10:10 signed
  normalized (renormalized from the PC's byte packing), static model rotations
  too. Models get per-surface high mip bounds, lose their collision triangles
  and keep D3D buffer headers zeroed. World surfaces keep a copy of their
  bounds; light grid row headers and vertex layer colors are swapped as the
  console reads them.
- **Effects** store colors as 32-bit values.
- **Sounds**: loaded sounds are XMA1 (the XMA2 frames of `xma2encode` with
  XMA1 packet headers) with a seek table (decoded samples at the start of every
  packet) and an XAudio format block (loop region, source format, duration in
  milliseconds). Both formats end the frames of a packet the same way: the last
  bit of a frame is 0 when no other frame starts in its packet, and decoding
  goes on at the frame offset of the next packet's header (this skips the
  padding `xma2encode` puts at the end of every 64 KiB block). The loop region
  starts at the first frame, skipping 3 subframes of 128 samples, and ends at
  the bit offset of the frame holding decoded sample `length + 383`, with the
  subframe of that sample (all of CoD Xenon's 1489 loaded sounds follow this).
  Many PC loaded sounds are xWMA (WMA 2 with a `dpds` chunk) under a `WAVE`
  header without the codec options WMA needs. FFmpeg's xWMA defaults decode the
  44.1 kHz ones; the 22.05 and 32 kHz ones carry a fake 96 kbps bit rate and
  decode with their real one (20 kbps), at 32 kHz with 3 block sizes instead of
  4 (codec options 0x17). The decoded length is checked against the `dpds`
  chunk. Streamed sounds are XMA2 in an `SDNS` container
  (sample count = XMA frames × 512); their names drop the extension and carry a
  hash (`h = h * 0x1003F + c` from 5381 over `dir\name` in lower case). Sounds
  of the map are served by CoD Xe from `sounds\`.
- The clip map is stored under the PVS clip map asset type.
- **Menus** keep per-client state for 4 splitscreen players (`[4]` arrays).

The CoD Xenon fastfiles read and write back byte-identically. For
`nazi_zombie_aztec`, the converted GfxWorld matches CoD Xenon's except for
how index arrays are shared, 307 of 614 animations are byte identical (the others
differ in the last bit of a few quaternions), 585 of 648 materials have identical
state bits, and the loadscreen texture converts to the exact bytes CoD Xenon
produced. Differences that remain are where CoD Xenon used stock console data
(more precise model normals, texture streaming bounds) or source data.

Known deliberate difference: CoD Xenon writes the 16-bit frame indices of delta
animation parts of long animations (256 frames and more) little endian; `t4ff`
writes them big endian like every other 16-bit value the console reads.

## Status

| Asset type | Status |
| --- | --- |
| rawfile, stringtable, localize, menu, menulist, weapon, physpreset | converted |
| xanim, xmodel, fx, gfxworld, clipmap, comworld, gameworld_sp, map_ents | converted |
| material | converted (technique slots remapped, state bits filtered by the console technique set) |
| techset | copied from `--console-zone` fastfiles, name reference otherwise |
| image | converted from the zone or from `.iwi` files; stock images without pixel data are copied from console fastfiles or referenced. Cube maps become references for now. |
| sound (aliases) | converted, streamed names follow the console conventions |
| loaded sounds | encoded to XMA1 (needs `xma2encode`), otherwise copied from console fastfiles or referenced |
| streamed sounds | encoded to XMA2 `sounds/*.xma` (needs `xma2encode`) |

Verify new console layouts with:

```sh
python dev/verify_samples.py path/to/console/*.ff   # updates t4ff/defs/x360_verified.txt
```

## Tests

```sh
python -m unittest discover -s tests
T4FF_SAMPLES=/path/to/samples python -m unittest discover -s tests   # also sample based tests
```

## License

The tool reads OpenAssetTools' structure definitions and zone code commands at
run time. OpenAssetTools is GPL-3.0 licensed.
