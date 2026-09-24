# t4ff: World at War PC to Xbox 360 fastfile converter

`t4ff` converts PC Call of Duty: World at War usermaps (custom zombie maps) into
fastfiles that CoD Xe loads on the Xbox 360 (`_codxe/usermaps/<map>/`), and
keeps them small enough for the console's memory:

- **Textures** are rebuilt as tiled Xenos textures with their mip chains.
  Uncompressed textures are DXT compressed. An optional memory budget drops the
  top mip levels of the largest textures first.
- **Streamed sounds** are encoded to XMA2 and stored in the `SDNS` container
  that the console build reads from `sounds/`.
- **mod.ff** is merged into `<map>.ff`, because the console has no mod zone.
  `<map>_patch.ff` and `<map>_load.ff` are converted separately.

> [!IMPORTANT]
> This is a work in progress. Asset types whose console format has not been
> recovered yet are replaced by *name references* (`,name`). The game then uses
> its own asset with that name when one exists; otherwise it falls back to a
> default asset. See [Status](#status).

## Requirements

- Python 3.9+
- `pip install -r requirements.txt` (numpy, libclang, and optionally imageio-ffmpeg)
- The `OpenAssetTools` folder of this repository. The PC structure definitions
  and zone streaming rules are read from it (`src/Common/Game/T4/T4_Assets.h`,
  `src/ZoneCode/Game/T4`).
- For sounds: `xma2encode.exe` from the Xbox 360 XDK (also shipped with the
  GDK and the XAudio2 desktop samples). No open source XMA encoder exists. Pass
  it with `--xma-encoder`, or set `XMA2ENCODE` (or `XEDK`). On Linux and macOS
  it runs through `wine`.

## Usage

```sh
cd tools/t4ff

# Convert a PC usermap folder (containing <map>.ff, mod.ff, *.iwd, ...)
python -m t4ff convert "C:/.../mods/nazi_zombie_aztec" -o out \
    --xma-encoder "C:/Program Files (x86)/Microsoft Xbox 360 SDK/bin/win32/xma2encode.exe" \
    --texture-budget 64

# Then copy out/_codxe into the World at War game folder (merge with the existing _codxe folder).
```

Useful options:

| Option | Effect |
| --- | --- |
| `--iwd PATH` | Extra `.iwd` files or folders to look up `images/*.iwi` and sounds (e.g. the PC game's `main` folder for stock images a map embeds). |
| `--texture-budget MIB` | Texture memory budget. Largest textures lose their top mip level first. |
| `--max-texture-size N` | Cap texture dimensions. |
| `--no-mips` | Drop all mip levels (about 25% less memory, but textures shimmer at a distance). |
| `--no-compress` | Keep uncompressed textures uncompressed. |
| `--stream-rate HZ`, `--mono-streams` | Resample or downmix streamed sounds. |
| `--allow-unverified` | Also convert asset types whose console layout was not verified. Expect crashes. |

Other commands:

```sh
python -m t4ff info <fastfile> [--list]   # blocks, asset counts, asset names (PC or console)
python -m t4ff roundtrip <fastfile>...    # read + rewrite, checks the result is byte identical
```

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
   the PC layout to the console layout (byte swapping and resizing).
   `t4ff/assets.py` holds the asset-specific rules.

### What was recovered about the console format

These facts were recovered by comparing PC fastfiles with the fastfiles
produced by CoD Xenon's converter:

- The container is the same `IWffu100` / version `0x183` header, but big
  endian with a zlib-compressed zone.
- **Images**: `GfxImage` is 40 bytes, followed by a 16-byte load def and a
  52-byte `D3DBaseTexture` header in the virtual block. The header is stored
  *little endian*. Pixel data is streamed after all assets into the large
  runtime block, and its size is `cardMemory`.
- **Materials / technique sets** have 51 technique slots (the PC tool/debug
  techniques are gone). Technique sets are referenced by name; the console has
  its own shaders.
- **Shaders** (console only) are split into a cached part and a 32-byte-aligned
  physical part. A pass holds one vertex shader per vertex format.
- **Menus** keep per-client state for 4 splitscreen players (`[4]` arrays).
- **Animations** have 12 part types (6 rotation, 5 translation, all).
- **Streamed sounds** are XMA2 packets in an `SDNS` container (see
  `t4ff/audio.py`). Its sample count is the XMA frame count × 512.

The CoD Xenon fastfiles `patch.ff` and `patch_ui.ff` read and write back
byte-identically. The PC loadscreen texture converts to exactly the bytes CoD
Xenon produced.

## Status

| Asset type | Status |
| --- | --- |
| rawfile, stringtable, localize, menu, menulist | converted (layouts verified) |
| material | converted (technique slots remapped) |
| techset | name reference to the console technique set |
| image | converted from the zone or from `.iwi` files. Stock images without pixel data become references. Cube maps become references for now. |
| weapon | layout verified; converted when its dependencies are |
| xanim | layout verified, PC → console part type mapping still unknown (reference) |
| xmodel, gfxworld, clipmap, comworld, gameworld_sp, fx, sound, loaded_sound, lightdef, physpreset, destructibledef, ... | not verified yet (reference unless `--allow-unverified`) |
| streamed sounds | converted to XMA2 (needs `xma2encode`) |
| loaded (in zone) sounds | console format not recovered yet |

Finishing the remaining types requires a PC map fastfile together with its
CoD Xenon converted console version (e.g. `nazi_zombie_aztec.ff` for both).
Their layouts can then be derived and checked the same way. Verify new console
layouts with:

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
