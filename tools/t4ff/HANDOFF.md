# t4ff and CoD Xe T4: handoff notes

State of the work on branch `claude/charming-ptolemy-rahr6b` as of 2026-09-28, for whoever
(person or Claude session) picks it up next. The [README](README.md) explains what t4ff does and
how to use it; this file is about where the work stands, the rules it follows, what is open, what
was already tried, and how things were checked.

## Start here

To continue in a new Claude Code session (for example one running on the tester's own PC, which
can run t4ff on the real files and read the Xenia and Watson logs directly), give it this:

> Read `tools/t4ff/HANDOFF.md` on branch `claude/charming-ptolemy-rahr6b` of this repository and
> continue from its "Open problems" and "Next steps". Follow its working rules.

Everything below is in the repository. Nothing from the earlier cloud sessions (their scratch
files, sample downloads, analysis scripts) carried over: the samples come from the files listed
under [Environment](#environment), and the checks are described under
[Investigation toolbox](#investigation-toolbox).

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
  the sample based tests (layout below). Run them before every push; 70 as of this writing, all
  passing on Windows too.

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

The maintainer's usual conversion (placeholders for their paths):

```sh
python -m t4ff convert "<PC usermap folder>" -o "<output folder>" ^
    --xma-encoder "C:\Program Files (x86)\Microsoft Xbox 360 SDK\bin\win32\xma2encode.exe" ^
    --console-zone "<codxe-t4-fastfiles-v0.2.0>" ^
    --iwd "<PC World at War folder>" [--name "The Simpsons"] [--loading-image picture.png]
python -m t4ff menu "<game>\_codxe\t4"
```

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
  zone manager (see Open problems, 2).

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
(where a stuck thread is), the dynamic usermaps list, a mod menu entry for music boxes, VS2010
build script.

## Maps tested in game

| Map | State |
| --- | --- |
| Aztec (`nazi_zombie_aztec`) | converts and plays; the reference map |
| Zombie Woods (`nazi_zombie_wh`, 2008) | converts and plays; title card loading screen |
| The Simpsons (`simpsons`, 2010, mod heavy) | converts and plays; voices, music box, airstrike (tester), rounds and dog rounds (Xenia) work. The window crash (1) is fixed but awaits a console test; Moe's (2) not looked into |
| Dead Sand (`nazi_zombie_dead_sand`, 2009) | converts; loads and is playable for a few seconds, then crashes (3) |

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

### 3. Dead Sand: crash a few seconds into the game

Loads in Xenia and is playable for a few seconds, then Xenia stops with "Overflowed stackpoints!"
(the Server thread went 65536 calls deep). The same Xenia error came from The Simpsons' zipline
setup looping forever (fixed, see Scripts above), so a script loop is the prime suspect; on a real
console the game kills such a thread with "potential infinite loop in script". Its collision trees
and path node tree were checked against PC and are identical.

About the map: made with the first mod tools (April 2009); its scripts spawn campaign AI (marines,
commissars, SS with panzerschrecks and MG42s) and fire rocket barrages, so it also misses campaign
animations, effects and HUD materials the console's zones do not have (see 4). Its
`dead_sand_init()` calls `PrecacheItem` after a wait (a script error on PC too). Its sky
`flying_ft` is a stock cube map from the PC game's files, converted only with `--iwd`.

Next: run it on the console with xbWatson and `log_console` (and `thread_watch` in Xenia) to get
the looping script, then fix the pattern generally as with `modderHelp()`.

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

## Next steps

1. The Simpsons on a console: the window crash (1) after an airstrike and barrier repairs, rounds
   in the TV room and dog rounds (2); then Moe's (2).
2. Dead Sand in Xenia with `log_console` and `thread_watch` (the logging copies of the toolbox can
   find the looping script): find and fix the looping script (3). Its scripts may be shadowed by
   the game's zones too (the conversion now warns about the mod's).
3. Stock assets: named materials, then the PC game's fastfiles as a source (4).
4. A known complex map next, once these are done.

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
| `tools/t4ff/t4ff/zone.py` | zone reader and writer, `Node`, `Ptr` |
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
