# t4ff: World at War PC to Xbox 360 fastfile converter

`t4ff` converts PC Call of Duty: World at War usermaps (custom zombie maps) into
fastfiles that CoD Xe loads on the Xbox 360 (`_codxe/usermaps/<map>/`), and
keeps them small enough for the console's memory:

- **Every asset type of a map is converted**: models, animations, the world
  (GfxWorld, collision, paths), effects, weapons, materials, sounds, menus,
  scripts, string tables and localized strings.
- **Textures** are rebuilt as tiled Xenos textures with their mip chains.
  Uncompressed textures are DXT compressed. An optional memory budget drops the
  top mip levels of the largest textures first.
- **Sounds** are encoded to XMA: loaded sounds into the fastfile (XMA1, as the
  console's in-memory sounds are), streamed sounds to `sounds/*.xma` files.
  Both can be downsampled or downmixed to save memory.
- **Technique sets (shaders)** cannot be converted from PC data. They are copied
  from Xbox 360 fastfiles you provide (`--console-zone`), together with any stock
  image, sound or other asset the PC map expects from the game.
- **One fastfile per map**, as in CoD Xenon's converted maps: `mod.ff` (the
  console has no mod zone) and `<map>_patch.ff` are merged into `<map>.ff`.
  Scripts of later zones win (`_patch` over the map, `mod.ff` over both), and
  assets several zones define (including textures nested in materials) are
  loaded once. `<map>_load.ff` (loading screen) is optional (`--load-zone`).

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
    --console-zone "D:/360/WaW/zone/english/nazi_zombie_factory.ff" \
    --console-zone "D:/360/WaW/zone/english/common.ff" \
    --iwd "C:/Program Files (x86)/Activision/Call of Duty - World at War/main" \
    --texture-budget 64 --sound-rate 32000

# Then copy out/_codxe into the World at War game folder (merge with the existing _codxe folder).
```

Useful options:

| Option | Effect |
| --- | --- |
| `--console-zone PATH` | Xbox 360 fastfile (or folder of them) to copy console only assets from: technique sets, and stock images, sounds, models... the PC map expects from the game. Repeatable. |
| `--iwd PATH` | Extra `.iwd` files or folders to look up `images/*.iwi` and sounds (e.g. the PC game's `main` folder for stock images a map embeds). |
| `--texture-budget MIB` | Texture memory budget for the map and its mod together, including textures copied from console fastfiles. Largest textures lose their top mip level first. |
| `--max-texture-size N` | Cap texture dimensions. |
| `--no-mips` | Drop all mip levels (about 25% less memory, but textures shimmer at a distance). |
| `--no-compress` | Keep uncompressed textures uncompressed. |
| `--sound-rate HZ`, `--mono-sounds` | Downsample (24000, 32000, 44100 or 48000) or downmix loaded (in memory) sounds. |
| `--stream-rate HZ`, `--mono-streams` | Resample or downmix streamed sounds. |
| `--xma-quality N` | xma2encode quality (1-100, default 60). |
| `--no-mod`, `--no-patch` | Do not merge `mod.ff` / `<map>_patch.ff` into the map fastfile. |
| `--load-zone` | Also write `<map>_load.ff`, the loading screen zone. Experimental: CoD Xenon's converted maps have none. |
| `--allow-unverified` | Also convert asset types whose console layout was not verified. Expect crashes. |
| `--max-loaded-sounds N` | Loaded (in memory) sounds the map may have, default 1500. The console holds 1600, the game's own included; a map with more stops with "Exceeded limit of 1600 'loaded_sound' assets". Identical sounds are shared, then the longest ones become streamed sounds played from the map's `sounds` folder. 0: no limit. |
| `--jobs N` | Sounds encoded at a time and threads compressing the fastfile (default: one per processor). |

Other commands:

```sh
python -m t4ff info <fastfile> [--list]   # blocks, asset counts, asset names (PC or console)
python -m t4ff roundtrip <fastfile>...    # read + rewrite, checks the result is byte identical
```

### Testing on the console

**How CoD Xe finds the map.** CoD Xe does not keep a list of usermaps: when the
game opens the fastfile of a zone, CoD Xe serves
`_codxe/usermaps/<zone>/<zone>.ff` instead if it exists (and the sounds of the
active map from its `sounds` folder). So the folder and the fastfile must carry
the map's name exactly; `t4ff` names them after the PC map fastfile.

**How it appears in the menu.** The map list is part of CoD Xenon's
`_codxe/zone/patch_ui.ff` (one button per map that runs `devmap <map>`) and
`patch.ff` (map names and descriptions). It lists the six maps of their
release. A map converted by `t4ff` with one of these names (e.g.
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
3. Back up their `_codxe/usermaps/nazi_zombie_aztec/nazi_zombie_aztec.ff`,
   replace it with yours and start Aztec from the menu.
4. Then convert again with stock Xbox 360 fastfiles of your game as console
   fastfiles (and `xma2encode.exe`) instead, which is how other maps are
   converted.

If the game stops while loading, run `python -m t4ff info` on the fastfile and
report the output together with the step that failed.

### Memory

The tool prints the memory each fastfile needs once loaded. CoD Xenon's own
conversion of `nazi_zombie_aztec` needs about 157 MiB (64 MiB of textures, 35 MiB
of in-memory sounds). Textures and loaded sounds are the parts you can shrink:
`--texture-budget`, `--max-texture-size`, `--sound-rate 32000` and
`--mono-sounds` trade quality for memory.

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
