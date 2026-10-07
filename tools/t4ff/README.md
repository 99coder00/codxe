# t4ff: World at War PC to Xbox 360 fastfile converter

`t4ff` converts PC Call of Duty: World at War usermaps (custom zombie maps) into
fastfiles that CoD Xe loads on the Xbox 360 (`_codxe/t4/usermaps/<map>/`), and
keeps them small enough for the console's memory:

- **Every asset type of a map is converted**: models, animations, the world
  (GfxWorld, collision, paths), effects, weapons, materials, sounds, menus,
  scripts, string tables and localized strings.
- **Textures** are rebuilt as tiled Xenos textures with their mip chains.
  Uncompressed textures are DXT compressed. Normal maps become DXN, the format
  of all the console's (the PC keeps x in alpha and y in green, the console's
  shaders read x and y from DXN's two channels). Cube maps (the reflection
  probes of the map, skies) keep their six faces: the probes of PC Aztec
  convert to exactly the textures of CoD Xenon's Aztec, and like those stay
  uncompressed up to 64 texels. Each map keeps as much texture
  quality as fits in memory: the texture budget is what the game's main
  memory leaves (measured: about 220.7 MiB free on a console when a map loads,
  for every block of its zone: textures, models, animations, the world...; 286.7
  in Xenia, whose patch for the game enlarges its memory pool), and only when a
  map is over it do its largest textures lose top mip levels. Stock textures use the console's own versions.
- **Sounds** are encoded to XMA: loaded sounds into the fastfile (XMA1, as the
  console's in-memory sounds are), streamed sounds to `sounds/*.xma` files.
  Both can be downsampled or downmixed to save memory.
- **Technique sets (shaders)** cannot be converted from PC data. They are copied
  from Xbox 360 fastfiles you provide (`--console-zone`), together with any stock
  image, sound or other asset the PC map expects from the game. What the game's
  own zones (`common.ff`, ...) already load stays a name reference when those
  zones are among the fastfiles given.
- **Assets the game looks up by name** that a PC map lacks (player body
  animations, the animations of its anim trees such as the dogs', shellshock
  files) are added from the Xbox 360 fastfiles given.
- **One fastfile per map**, as in CoD Xenon's converted maps: `mod.ff` (the
  console has no mod zone) and `<map>_patch.ff` are merged into `<map>.ff`, and
  so is a mod's own language zone (`localized_*.ff`, which the PC loads with
  every map in place of the game's): UGX Mod ships its guns in
  `localized_common.ff` (Kino Der Toten: 69 weapons with their models,
  animations and sounds), and without it gungame gave the player nothing.
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
| `--console-zone PATH` | Xbox 360 fastfile (or folder of them) to copy console only assets from: technique sets, and stock images, sounds, models... the PC map expects from the game. Repeatable; the first one that has an asset wins. Maps t4ff converted (a `t4ff.txt` in their folder) are left out: an earlier conversion would hand its old copies back. See [Console fastfiles](#console-fastfiles). |
| `--iwd PATH` | The PC game's own files (e.g. its `main` folder): stock textures a map uses and no console fastfile has are converted from them. Stock textures the console fastfiles have keep the console's version (Treyarch sized them for the console), which leaves the memory to the map's own textures. The game's streamed sounds the map uses are encoded from them too, into the map's `sounds` folder: the console's disc has none of the downloadable maps' (Der Riese's voices, the easter egg songs of a music box). |
| `--texture-budget MIB` | Texture memory for the map and its mod together, `0` for no limit. Default `auto`: what `--memory-target` leaves. The texture whose next level saves the most memory loses its top level first: once a conversion has streamed, a streamed texture counts what it keeps in the fastfile (a sixteenth when deep), so the textures kept whole (effects, small ones) give way before the streamed ones, and a streamed one that would stop streaming keeps its level; 2D (menu, HUD) textures go last, the world's lightmaps never. |
| `--memory-target MIB` | Main memory the map's zone may use, for the automatic texture budget (default 212, for a console). The game allocates every block of a map's zone (its textures, and the virtual block of its models, animations, world and scripts too) from its main memory pool; the blocks are allocated in order and the first that does not fit stops the game: "Need <n> more bytes of 'main' physical ram", n counting the blocks up to that one. Xenia applies a patch to the game ("Memory allocator expansion") that makes the pool 480 MB instead of 414, so a map has about 286.7 MiB there (Kino Der Toten's 278.5 MiB loaded; Kino Rezurrection's 293.6 MiB was 5.2 MiB short at its large block) and about 220.7 on a console: `--memory-target 278` makes a map for Xenia only. The map is converted, measured and, when over, converted again: first without the PC versions of stock textures (`--upgrade-budget`), then without the textures' mip tails (`--keep-mip-tail`), then with less texture memory: deep streamed textures keep an eighth of their size in the fastfile first (`--keep-quarter`), then textures lose top levels. Over the console's figure the converter warns that the map loads in Xenia only (the GUI stops at 220). |
| `--keep-quarter` | Deep streamed textures keep a quarter of their size in the fastfile even when the map is over its memory target. By default the texture budget first has them keep an eighth (three levels streamed: the half size texture within 300 units of the texture's box, the whole texture inside it, the eighth farther away), before any texture loses its top level: that keeps every level up close, where a dropped level would blur. Needs a CoD Xe build that knows `images.pak` version 3 (older ones refuse such a pack, leaving the eighths). Kino Rezurrection: about 19 MiB. |
| `--keep-mip-tail` | Keep every texture's mip levels of 16 texels or less a side when the map is over its memory target. The GPU packs them into a tile of their own per texture (a 512x512 DXT5's 16 KiB, as much as its 128x128 level); by default they go before any texture loses its top level, so the smallest level left (32 texels a side) shows for what is farther: a little shimmer far away rather than a blur up close. Kino Rezurrection: 17.3 MiB. |
| `--stream-textures` | On by default since 2026-10-06 (`--no-stream-textures`: every texture whole in the fastfile, as before). Texture streaming, as the disc's own levels do it: textures only models and world surfaces use keep their top mip level in the map's `images.pak` (one file in its folder: entries 4 KiB aligned, an index at the end), and the fastfile a texture of half their size; the game loads the top level into its 64 MB streaming buffer (reserved at boot whether a map streams or not) when what uses it is close. t4ff writes the boxes that decide it as the disc's linker does (each triangle's box grown by 1931.2 / its texels per unit): each model surface's, each world surface's, and the world's tree of surfaces and static models, through which alone static models and the world stream. Stock textures the console has smaller (the campaign zones' copies, often 4 to 8 times smaller a side) are the PC game's (`--iwd`): streamed when they can (two levels with `--deep-stream`), else whole in the fastfile, within `--upgrade-budget`. Kino Der Toten: all 1066 textures at full quality, 364 streamed (127 of them PC stock textures), 156.4 MiB of textures, large and physical blocks. Needs t4ff's CoD Xe build (it serves the pack: without it the game finds no file, turns streaming off for the image and keeps its smaller copy). Textures saved without mip levels (the IWI's no-mipmaps flag) get a box filtered mip chain, their top level as it was, so they stream too (Kino Rezurrection: 41, 18.1 MiB whole in the fastfile, 2.7 MiB streamed). Console images that streamed on the disc keep doing it with their `.hi` copied from the console fastfiles' `highmip` folders, or stop. Images named like one of the game's own zones' do not stream (the game asks for files by name). See `t4ff/stream.py`. |
| `--stream-growth` | With `--stream-textures`: how far around a surface its streamed textures load their top level, as a share of the disc linker's distance (1931.2 / texels per unit). Default: 0.5 for a console's memory target, 1 above it. A console build has no extra stream pool (its main memory is full), so the game's 64 MB buffer alone holds the top levels the view is inside the boxes of: with the disc's boxes Kino Rezurrection's large textures wanted 53-61 MB of it at once and textures right in front of the view never loaded. The game renders 1024x600 (an 80 degree view): a top level shows alone up to about 600 / its texels per unit, so half the disc's distance still covers it. Needs the CoD Xe build that serves images showing their fastfile copy first. |
| `--upgrade-budget` | With `--stream-textures`: MiB the PC versions of stock textures may add to the fastfile (default 96); the converter lowers it first when the map is over its memory target. Kino: 463 PC originals for 9.4 MiB, every texture with a PC source at its PC size. |
| `--deep-stream` | With `--stream-textures`: images (comma separated names, or `all`) that stream two mip levels at once where they can: the fastfile keeps a texture of a quarter of their size and the pack the whole texture, which CoD Xe applies (t4ff's build, `streaming.cpp`). A texture over the streamer's 4 MB block streams one level. Kino with `all`: 356 of 364 streamed textures, 119.8 MiB of textures, large and physical blocks (36.5 MiB less), checked in Xenia with color-marked levels. |
| `--loaded-sound-memory MIB` | Memory of the loaded sounds (default 32; CoD Xenon's maps have up to 35). Beyond it the longest become streamed sounds, which keep their quality; looping ones stay loaded. 0: no limit. |
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
python -m t4ff streams <game>/_codxe/t4/usermaps   # streamed sounds of older conversions in the game's layout
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

The level script (`maps/<map>.gsc`) and client script (`clientscripts/<map>.csc`)
are loaded by name, so no script names them: maps made with PhilMod (Mini-Labor)
keep every script in an `.iwd` and none in the fastfiles, and the console
stopped with "Could not find script 'maps/nazi_zombie_002c'". They come from the
map's files when no zone has them, and so does everything they use. A script the
game's zones have too comes from the map's files when the map has its own (the
PC runs the mod's); whether the console can run it is decided below.

Zombie maps also get the two client scripts the game loads by name for them,
which no script names: `clientscripts/_zombie_mode.csc` and the zombie
`clientscripts/_callbacks.csc` (with `sound_notify`). The console's own zones
have neither (every map of CoD Xenon's carries both); maps made with the first
mod tools, such as Dead Sand, lack them, because the PC game's zones had them.
They come from the Xbox 360 fastfiles given, so give at least one map converted
by CoD Xenon with `--console-zone`.

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
  dvar 'bg_shock_viewKickPeriod'"; CoD Xenon's `nazi_zombie_derberg` has it),
  and the animations of the anim trees the scripts use (`animtrees/*.atr`):
  the game loads them by name, and the PC game's own zones have the dogs'
  (`german_shepherd_run`, window jumps, pain), so PC maps with dog rounds have
  none of them. Without them the dogs cannot run, jump through windows or feel
  pain (The Simpsons, "Could not load xanim german_shepherd_run"); CoD Xenon's
  `zm_tranzit` has them all.

With `patch_ui.ff` or `ui.ff` among them, the mod's own versions of the
console's menus are left out: PC mods ship restyled main menus and lobbies
(The Simpsons' `ui/main.menu` and `ui/xboxlive_lobby.menu`), which only take
memory in the map's zone and would replace the console's menus of the same
names. A menu list goes when most of its menus are the console's and the map's
scripts open or precache none of them; menus the scripts use (a music box menu
in `ui/scriptmenus`) stay, and so does a list another kept one points into. So
does a list of the name of one of the game's own (`ui/ingame.txt`, `ui/hud.txt`
of the console's `common.ff`), which takes the place of the game's: UGX Mod's
`ui/ingame.txt` is the PC's, and its pause menu's Options and Challenges open
menus the console has not in game ("Could not find menu 'options_new_pc'"), so
they only closed it; the console's own pause menu is used instead. When such a
list has to stay because something kept points into it (the PC linker stores
a string once, so UGX's vote menus and even weapons point into its
`ui/ingame.txt` and `ui/hud.txt`), it stays under a name of its own
(`ui/ingame_mod.txt`): the game loads its lists by name and gets the console's.
That alone is not enough: the console also opens menus by name, and a menu the
map's zone loads takes the place of the game's of the same name whatever list
holds it (UGX's PC `pausedmenu` still showed, with Options and Challenges that
only closed it). So every menu of the mod with the name of one of the
console's is renamed `<name>_mod` (Kino 102, Mini-Labor 76, most of them the PC
HUD and pause menus): the console's own pause menu, options and HUD are used.
Menus the scripts open by name keep theirs, as do menus whose name string
something else uses too (a warning says so).

That same override lets a map keep its own pause menu. The console's pause menu
can open only the game's in-game menus (`ui/ingame.txt`) and the script menus
the map precaches. When the mod's `pausedmenu` opens menus beyond those that
the zone has (UGX's Challenges: `menu_challenges`, then `popup_tier`), it stays
the map's own: its PC options menu becomes the console's `ingameoptions`, and
those menus join a menu list the scripts precache (UGX's vote menu's), so the
game loads them with its in-game menus. A menu context holds at most 120 menus,
so only the menus needed join, not the mod's whole list.

PC script menus are played with the keyboard: Tom_bmx's music box menu picks a
song with the keys 1 to 6 and closes with Escape, which a controller has not.
Their number keys also go to the controller's buttons (1 A, 2 X, 3 Y, 4 LB,
5 RB, 6 to 9 the D-pad up, down, left and right), Escape to B, and their labels
name the buttons ("A: Beauty Of Annihilation", "Press B to close menu"). Hints
the scripts set naming the PC's use key ("Press F To Play A Song") show the
console's use button.

Menus made for the mouse can also be pressed with a controller: the D-pad moves
the focus through the menu's items in their order (up and left to the previous
item, left only when it is on the same row, down and right likewise), and A acts
on the focused one. The console focuses any item that is not a decoration, so
backgrounds left without `decoration` stop the focus where A does nothing:
Mini-Labor's weapon choice puts one under each button, its ACCEPT's at the same
place and first, and ACCEPT could not be pressed (the game waited for it
forever). Items that do nothing (no action, handler or dvar) become decorations.
Moving through the list order suits a column of buttons, not a grid (down moved
only at the end of a row), and the focus shows nothing on buttons made of
pictures. So the buttons of the menus the map's scripts open get their own
D-pad and stick handlers, which the console runs before its own moves: each
gives the focus (`setfocus`) to the nearest button in that direction, and
unnamed buttons get a name for it (`t4ff_focus_<n>`). Buttons choosing a value
the menu shows (they set a local variable: the frame around a weapon) and
answer the scripts, with one other button confirming (ACCEPT), follow the focus:
their action runs when the focus arrives, and A on one confirms it. A sends one
answer only: a script waiting for them in a loop gets one per frame.

Menus also show one of several buttons in one place by a condition: UGX's vote
(Kino Der Toten) has a "Gamemode: ..." button per mode, the chosen one shown,
and the D-pad did nothing there, the nearest button that way being one not
shown ("setFocus: error focusing widget ... could not accept focus"). So a
direction tries every button that way, the nearest last, and the last one shown
keeps the focus; with none shown that way it wraps around (down from the bottom
button to the top one). Buttons off the screen are no targets: the console draws
menus within its safe area, and UGX's "Exit to Main Menu" (y 450 from its top)
is below the screen there, where the focus went from "Start Game". A button's
action, which may show another in its place, gives the focus to the one shown
there. A menu opens on its top-most button shown
rather than on its first (UGX's first is "Exit to Main Menu", off the screen: A
on the vote left the game), and the focused button's text takes the menu's
focus colour (`setitemcolor`; a gold when that is the button's own), back to its
own as the focus leaves: PC menus mark the button under the mouse, which a
controller has not.

Mods choose their options in their own front end menus (PhilMod's difficulty,
`philmod_gamemode`), which the console does not show. The multiple choice items
of the mod's menus bound to dvars the map's scripts read (not the game's own
settings: UGX's options menu sets the field of view, `cg_fov`) go to the map's
`options.txt` (the choices, the value the menus set as the default, the label of
the item's row): CoD Xe's Custom Maps menu shows them for the focused map
("Difficulty: Default (X)"), X and Y change them, and the map's row sets them
before loading it. The map also asks them in game, whatever menu started it: a
copy of the game's own difficulty list (`popmenu_difficulty` of `common.ff`,
always loaded; `t4ff_option0`, ... in a menu list of the map) opens as the player
connects, its rows the option's choices, the current one focused. A script
opening a menu replaces the one open, so the menus the level script opens
(PhilMod's weapon choice) wait for the answers (`t4ff_open_menu`); only the level
script's: a call from another script into the level script is one more of the
script compiler's references, whose number is limited ("MAX_PRECACHE_ENTRIES
exceeded", which PhilMod's scripts are close to). B keeps the current choice.
The level script reads the options once, as it starts, so a changed choice
restarts the level with it: a second menu runs `fast_restart` as it opens, as the
pause menu's Restart Level does (the game's scripts have no `map_restart`), and
the restarted level asks nothing. Until a choice, a dvar the map's scripts read
that the map's menus set to one value gets it at the start of the level script
when unset (Mini-Labor starts on "Default", not "Easy").

Maps made with Sparks' DLC2 / DLC3 modding kits check their entities with
`modderHelp()`, whose setups are meant to stop when one is missing, but it only
says so with `developer` on. In the game as played the setups go on without the
entities, and as the retail game keeps running a script past a runtime error,
a loop over a missing entity never ends: The Simpsons has no zipline, and its
zipline setup loops forever (the console kills the thread, "potential infinite
loop in script"; Xenia crashes with "Overflowed stackpoints!"). The converted
`modderHelp()` says a missing entity is missing also without `developer`.

The PC game spawns `script_struct` entities from scripts
(`spawn("script_struct", origin)`), the console's does not ("script_struct
cannot be spawned dynamically", then the script error "unable to spawn
"script_struct" entity" ends the thread): The Simpsons' rocket barrage links one
to each rocket for its explosion sounds, so on the console its rockets flew
nowhere and hurt no one. The converted scripts spawn a `script_origin`, the
entity made for that (`linkTo`, `playSound`).

The game runs the first script of a name it loads, and the console loads its
own zones before the map, so a script both have is the game's there; on PC, with
the map's mod active, the mod's scripts (its `mod.ff`, its own files) win
instead. The console's `patch.ff` has Der Riese's
`maps/_zombiemode_zone_manager.gsc`, and maps made with the DLC3 modding kit
ship a changed one in the mod: when no enabled zone has a player in it, the
kit's makes the map's first zone active, Der Riese's its `receiver_zone`, which
other maps have not. With the game's, a player of The Simpsons in the room with
the TV (its door enables no zone) left no zone active, so no spawner: the rounds
ended as they started, five at a time, and the dogs stayed where they spawned
until the failsafe killed them. A script of the mod that the game's zones have
too gets a name of its own (`maps/_zombiemode_zone_manager_mod.gsc`), and the
map's scripts call it by that name. Not when a script of the game's that the map
runs calls it too (the game's `maps/_load.gsc` calls `maps/_laststand.gsc`), nor
for scripts the engine runs by name (animscripts, client scripts): those go to
the map's `scripts` folder (`usermaps/<map>/scripts/maps/_load.gsc`, ...), which
CoD Xe loads in place of the game's copies while the map runs, as the PC loads a
mod's loose scripts (CoD Xe builds from before that feature run the game's).
PhilMod replaces much of the game's own: `_load`, `_gameskill` (zombie damage
kept at full), `_laststand` (its solo Quick Revive), `_loadout`,
`_callbackglobal`, `common_scripts/utility`, the death and melee animscripts and
two client scripts all run from there. Dead Sand's crash
("Overflowed stackpoints!") came from the same: the console ran `patch.ff`'s
`maps/_zombiemode_blockers.gsc` instead of the mod's.

Anim trees (`animtrees/*.atr`) are read by the same loader, and the same goes
for them: The Matrix's `animtrees/generic_human.atr` has some 250 animations
the game's has not (Black Ops zombies' board tears, a boss, the wave gun's
deaths), and the console compiled its scripts against the game's, stopping with
"Server script compile error: animation
'ai_zombie_crawl_microwave_death_walking_c' not defined in anim tree
'generic_human'". The mod's tree goes to the `scripts` folder with the scripts,
and the conversion warns that the map needs a CoD Xe build that loads it. Its
loose copy also replaces its fastfiles' as the scripts' do: The Matrix's tree in
its `.iwd` has the wave gun's animations, the one in its `_patch.ff` not.

More differences the converted scripts work around (Dead Sand's):

- The console's `SetCursorHint` crashes the game on a hint type it has not (it
  lists the valid ones past the end of their table): Dead Sand's Nebelwerfer
  sets `"HINT_NONE"`, which becomes `"HINT_NOICON"`.
- Client script effects name their local client first. UGX Mod's Thundergun
  (`clientscripts/ugx_thundergun.csc`) plays its steam with `playfx( i, ... )`
  in a loop over its three vents, for local clients 3, 2 and 1. The PC has one
  local client and ignores them; the console has four (split screen), and an
  effect for one that is not playing froze the game on the first shot (Kino Der
  Toten; CoD Xenon's conversion freezes too). In a function given its local
  client, such a call whose local client is a loop counter (a loop not over the
  players) uses the function's local client.
- The console refuses precaches once the level script has waited, and a model
  not precached cannot be set. Zombie maps call the zombie mode's `main()`,
  which waits for the players, then their own setup: its literal precaches are
  made at the start of the level script's `main()` instead (the setup's become
  comments), and so are those of the zone's models scripts set by name that
  nothing precaches (Dead Sand's rockets, `katyusha_rocket`).
- `speed_up_zombies()` of the first zombie scripts gives every axis AI the
  zombies' sprint; it hurries zombies only, not Dead Sand's SS.
- The zombie mode gives zombies their idles by replacing the stand and crouch
  idles of every AI (`init_animscripts()`). On maps with soldiers (actor
  spawners that are not zombies) the zombie idles get poses of their own, and
  zombies play them from the animscripts' per AI hook
  (`self.exception["stop_immediate"]`); the soldiers keep the game's.
- In splitscreen the game's `maps/_load.gsc` gives a map that set no
  splitscreen fog (`level.splitscreen_fog`) a placeholder meant to stand out:
  yellow fog 200 units away. The game's maps and CoD Xenon's set theirs in their
  art scripts (`maps/createart/<map>_art.gsc`, `if( IsSplitScreen() )`); PC maps
  never did, and The Simpsons played washed in yellow. The level script says the
  fog is set as it starts (a map without fog has none in splitscreen either), and
  the map's own `SetVolFog` calls (not a player's, not the game's scripts) set
  the game's splitscreen fog in splitscreen (`maps\_utility::set_splitscreen_fog`),
  which stops drawing the world where the fog is thick: at least 4000 units away,
  as Treyarch's, and two of the fog's halfway distances past its start for
  thinner fogs. Scripts that set a splitscreen fog of their own keep it.
- On the console a player on an MG42 turret (`mg42_bipod_stand`, `_crouch`,
  `_prone`) of a zombie map sees no world: the client draws the turret's view
  far from the gun (the console's `viewpos` gives coordinates of 1e24 on Dead
  Sand, the world's origin on CoD Xenon's Airport), while the server has the
  player's eye at the gun. Stock Nacht with one placed does the same; the
  campaign's MG42s and The Simpsons' .30 cals do not, and neither the gun's
  model, overheating, the player's weapons nor the AI change it. On zombie maps
  a player getting on one gets off it at once and holds the gun instead
  (`t4ff_mounted_guns` in the level script): in its place, the eye where the
  console puts it (`tag_player`), within its arcs and stance, with the portable
  MG42 the map has (`mg42`, else `mg42_bipod`) and endless ammo, until the use
  button again, going down or leaving; the turret hides meanwhile and AI still
  use it.
- Timed power-ups such as PhilMod's Unlimited Ammo turn `player_sustainammo`
  (endless ammo and grenades) on, and off when their time is up. A game that
  ends or restarts meanwhile never turns it off, and the dvar keeps its value
  into the next game; console testers of Mini-Labor reported endless ammo. The
  level script of a map whose scripts turn it on turns it off as it starts; a
  cheat that is on (`sf_use_ignoreammo`, which CoD Xe's mod menu sets with
  "Engine Infinite Ammo") still turns it on after that.

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
menu's map list: the stock maps stay, and CoD Xenon's own maps become one
"Custom Maps" entry, which opens a menu of its own (with the look of the Nazi
Zombies menu: its background, a "Custom Maps" title and Back). The game only
knows the menus of its own list, and a patch can replace them but not add new
ones, so this menu takes the place of one nothing opens, the developers' level
list `levels_dev`.
There 13 rows show the maps of the `usermaps` folder, sorted by name. The D-pad
scrolls past the first and last rows, LB / RB move a page, the line under the
rows tells where you are ("14-26 / 40"), and the right side shows the focused
map's name, description and picture; B goes back. New maps show up the next time
the menu opens, without running it again.

A map's name and description come from `description.txt` in its folder (first
line, next lines): `convert` writes the name (`--name`, the "Map name" field),
`menu` writes those of CoD Xenon's maps from their menu. The picture of CoD
Xenon's maps is theirs, in the menu zone (`preview.txt` names it); for the other
maps it is `preview.bin`, their loading screen at 512x288, which `convert`
writes next to `<map>_load.ff` and `menu` writes for the maps already installed
with one. CoD Xe copies it into a picture slot the menu zone has for it, so
maps converted before need `menu` run once more, for the slot.

CoD Xe has a custom maps list of its own since release r351 (the menu
`codxe_usermaps` of a menu zone, launching a map solo or in a co-op lobby),
which reads `map.json` (`{"version": 1, "name": ..., "description": ...}`) and
shows `preview.dds`, a 512x256 DXT1 picture it copies into the menu's image
`codxe_usermap_preview` (another size is refused). `convert` writes both, from
`description.txt` (where a map's name is still changed) and the loading screen,
and `menu` writes them for the maps of `usermaps` that have none. Maps made for
that list carry only `map.json`; the list above reads it when a map has no
`description.txt`.

The menu zone of that list is CoD Xenon's 0.3.0 `patch_ui.ff`: it replaces the
game's whole menu list (`ui/menus.txt`), which is how it adds a menu of its own.
`menu` uses it when the game's `zone\patch_ui.ff` is that one, or installs it
from a folder or file given with `--menu-zone` (the window gives its Xbox 360
fastfiles, so add CoD Xenon's 0.3.0 folder there), keeping the one before as
`patch_ui.ff.bak`; it then patches nothing and only gives the maps their
`map.json`, `preview.dds` and streams. Without such a menu zone it makes the list
above from CoD Xenon's 0.2.0 `patch_ui.ff`, as before. CoD Xe's own list has no
options: the maps that have some ask them in game (see Console fastfiles).

```sh
python -m t4ff menu "<game>/_codxe/t4" --menu-zone "<codxe-t4-fastfiles-v0.3.0>"
```

`menu` also rewrites the streamed sounds (`.xma`) of the maps in `usermaps`
that do not have the layout of the game's streams (CoD Xenon's maps, maps
converted before t4ff wrote it): theirs play a split second, then stop. The
first time takes seconds (about 6 for 1850 files, 230 MiB), later ones less
than a second; `--no-streams` leaves them. The `streams` command does only that,
for any folder.

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

Its picture is named `loadscreen_<map>_codxe`: the map's own zone often has a
`loadscreen_<map>` image too (The Simpsons does), which would replace the
picture once it loads. For the same reason the map's own zones never take assets
from other maps' load zones among the console fastfiles: a mod's menus name
`$levelbriefing`, and Mini-Labor carried Mario's, which showed once the map had
loaded.

In Xenia, a map loaded after another in the same session can show the previous
map's picture: every load zone made this way has its picture at the same place
and size, and Xenia keeps the texture it had there. A console reads it anew.

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
world's lightmaps keep theirs, and the images of 2D materials (menus, the HUD),
seen at their size on screen, lose levels only once nothing else can. Loaded
sounds beyond `--loaded-sound-memory` (32 MiB) are streamed, the longest first;
looping sounds stay loaded, since a
looping stream holds one of the console's few stream channels as long as it
plays and the music and voices, streamed too, are then cut off halfway. `--max-texture-size`,
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
  format), so they come from console fastfiles. A pass lists its shader
  arguments per primitive, per object, then stable (set once when the pass is
  set up); the game's own technique sets always put a code argument in the same
  section. CoD Xenon's have the dynamic shadow texture (code sampler 0x12, which
  the sun lit techniques sample) among the stable arguments, but the game only
  sets that texture before the per object arguments of what it draws lit: read
  earlier, it can still be unset, and the game stops with "Tried to use '(null)'
  when it isn't valid ... techType=10" (The Simpsons, at a window of the house).
  Copied technique sets get their code arguments moved to the game's sections.
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
  bounds; light grid row headers and vertex layer data (per layered vertex, the
  u, v floats of its other layers, some with a packed RGBA value: 8, 12 or 16
  bytes) are swapped as the console reads them. A light definition the game's
  own zones have (`light_point_linear`) becomes a reference to the console's, as
  in CoD Xenon's DerBerg: the PC map's copy has the PC linker's lookup index.
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
  chunk. Streamed sounds are XMA2 in an `SDNS` container laid out as the game's
  own (the `.xma` files of its disc, which the writer reproduces byte for byte):
  4 KiB XMA2 blocks (two packets), the header giving the sample count and the
  decoded sample count at the end of every block (as many as fit: 1016). The
  game reads a stream block after block along that table: xma2encode's 64 KiB
  blocks without it (as CoD Xenon's maps and earlier conversions had them) play
  their first block, a split second, then stop. Their names drop the extension
  and carry a hash (`h = h * 0x1003F + c` from 5381 over `dir\name` in lower
  case). Sounds of the map are served by CoD Xe from `sounds\`, with the hash of
  that path.
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
| image | converted from the zone or from `.iwi` files; stock images without pixel data are copied from console fastfiles or referenced. Cube maps are converted too (the probes in the zone, skies from `.iwi` files); volume maps become references. |
| sound (aliases) | converted, streamed names follow the console conventions |
| loaded sounds | encoded to XMA1 (needs `xma2encode`), otherwise copied from console fastfiles or referenced |
| streamed sounds | encoded to XMA2 `sounds/*.xma` (needs `xma2encode`); the game's own the map uses too, from `--iwd` |

Verify new console layouts with:

```sh
python dev/verify_samples.py path/to/console/*.ff   # updates t4ff/defs/x360_verified.txt
```

The game disc's own maps are samples too: Nacht (`nazi_zombie_prototype.ff`) and Makin (`mak.ff`)
proved the layouts of world portals (`GfxPortal`, which every map with portals has: Mini-Labor's
world was left out before), destructibles, water cells, LOD chains and physics constraints. Their
headers count the delayed image pixels in the zone's size and t4ff's do not (both load): only the
header's size, temp and runtime block fields may differ.

### Maps tested on the console

| Map | Result |
| --- | --- |
| Aztec | plays |
| Zombie Woods (2008) | plays, with a title card loading screen |
| The Simpsons (2010) | plays; voices, music box, rounds and dog rounds work. Known problems below |
| Dead Sand (2009) | plays; commissars, marines, SS and the Nebelwerfer work (Xenia). Known problems below |
| Mini-Labor (2014, PhilMod) | plays; weapon choice, doors, power, box, Pack-a-Punch, Perk-o-Matic work (Xenia). Known problems below |

### Known problems

- **The Simpsons' crash at the first window of the house** ("Tried to use '(null)' when it isn't
  valid. Material='mc/berlin_window_browirglas', tech='lp_sun_b0c0d0n0s0_dtex_sm3',
  techType=10") came from CoD Xenon's technique sets reading the dynamic shadow texture before the
  game sets it (fixed, see the technique sets above). Not seen again in Xenia, but not reproduced
  before the fix either: to confirm on a console.
- **The Simpsons' round skips** in the room with the TV, the zombies that did not find a way in and
  the dogs that died where they spawned came from Der Riese's zone manager of the console's
  `patch.ff` running instead of the mod's (fixed, see the scripts above; checked in Xenia). Moe's
  cannot be reached: not looked into yet. The mod's `_laststand.gsc`, `_loadout.gsc` and
  `_debug.gsc` stay the game's (the conversion warns).
- **Dead Sand's crash** ("Overflowed stackpoints!" a few seconds into a game), **its Nebelwerfer
  freeze**, its rockets that did not fire and its soldiers idling as zombies are fixed (see the
  scripts above; checked in Xenia). Its objective picture in the pause menu is a checkerboard. Its
  Nebelwerfer kills every zombie and SS of the map, not only those near its rockets: the map's
  script reads `target_pos.origin` of a position (undefined), the retail game skips the error and
  every AI passes the distance check. Seen on the console; PC runs the same script engine, so it
  is left as the map plays.
- **Mini-Labor** needs a CoD Xe build that loads the map's `scripts` folder for PhilMod's own
  core scripts; older builds run the game's (easier damage, the stock last stand). Its objective
  chain (wrench, uranium, C4, the Endgame-O-Matic) and traps are not checked yet.
- **Stock assets no console fastfile has** stay missing ("Could not load material/fx/xanim" in the
  console log): the PC game's own zones have them, and t4ff does not read those yet. Maps that use
  campaign AI or effects (Dead Sand) miss the most.
- Some streamed sounds of the game a map uses are in none of the PC files given and play silence.

[HANDOFF.md](HANDOFF.md) has the details of each (what was checked, what was ruled out, what to try
next), the rules the work follows and how to continue it.

## Tests

```sh
python -m unittest discover -s tests
T4FF_SAMPLES=/path/to/samples python -m unittest discover -s tests   # also sample based tests
```

## License

The tool reads OpenAssetTools' structure definitions and zone code commands at
run time. OpenAssetTools is GPL-3.0 licensed.
