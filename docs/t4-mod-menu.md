# CoD Xe Mod Menu: World at War Singleplayer

A GSC mod menu for **Call of Duty: World at War (T4) singleplayer** on Xbox 360: the campaign and
Nazi Zombies, including split-screen and online co-op. It lives in
[`resources/t4/_codxe/mods/mod_menu`](/resources/t4/_codxe/mods/mod_menu). The whole menu is plain
GSC, loaded at runtime by CoD Xe's GSC loader.

## Setup

### What you need

- Call of Duty: World at War with **Title Update 7** installed. See
  [Installing title updates](title-updates.md).
- CoD Xe **r347 or newer** running on either:
  - an Xbox 360 that can run unsigned code, or
  - [Xenia Canary](https://github.com/xenia-canary/xenia-canary) with plugins set up as described
    in the [README](../README.md#xenia-canary-setup).
- The game as an **extracted folder**, meaning the folder that contains `default.xex`. CoD Xe reads
  its files from next to the running `default.xex` (`game:` in Xenia), so you can't add them to a
  disc image.

To check that CoD Xe is running, look for the version text (for example `CoD Xe r348`) in the
top-left corner of the game's menus. Builds older than r347 can't load the menu.

### Install the menu

1. Copy the `resources/t4/_codxe` folder from this repo (or a release zip) into the game folder, so
   it sits next to `default.xex`:

   ```text
   <game folder>
   |-- default.xex
   `-- _codxe
       |-- codxe.json
       `-- mods
           `-- mod_menu
               `-- maps
                   |-- _music.gsc
                   `-- mod_menu
                       |-- core.gsc
                       |-- menus.gsc
                       `-- ... (the other .gsc files)
   ```

2. Open `_codxe/codxe.json` in a text editor and check that the active mod is `mod_menu`. This
   repo's copy already says so. The value must match the folder name under `_codxe/mods`
   exactly; a name with no folder loads nothing, and the level plays stock with no error.

   ```json
   {
     "active_mod": "mod_menu",
     "dump_rawfile": false,
     "dump_map_ents": false
   }
   ```

   Leave both dump options `false` for normal play: with `dump_rawfile` on, mods aren't loaded.
   The options are described in [Feature overview](features.md#setup).

3. Start the game and load any campaign mission or Nazi Zombies map. Once you can move, wait a
   few seconds. Player 1 sees these messages on screen:

   ```text
   CoD Xe Menu loaded
   Hold LT (aim) and press RS (melee) to open the menu
   ```

4. Hold **LT** and click **RS** (press the right stick in) to open the menu.

`codxe.json` picks one mod for both singleplayer and multiplayer. The mod menu only contains
singleplayer scripts, so multiplayer runs unmodded while it is active. To play the `codjumper`
multiplayer mod, set `active_mod` to `codjumper`.

### Troubleshooting

| Problem | What to check |
| --- | --- |
| No "CoD Xe Menu loaded" message | CoD Xe isn't running (no version text on the menus), `active_mod` isn't exactly `mod_menu` (a mistyped name loads nothing, with no error), or `dump_rawfile` is `true`. Also check that `_codxe/mods/mod_menu/maps/_music.gsc` exists next to `default.xex` and that TU7 is installed. |
| Message shows but the menu won't open | Only player 1 (the host) has the menu by default. Wait until any mission intro has finished. On a different button layout, use your **Aim** and **Melee** buttons (see [Button layouts](#button-layouts)). |
| `Server script compile error` / `unknown function` | The scripts call something your CoD Xe build doesn't register. This usually means the mod files are newer than the CoD Xe build. Use matching files from the same release, or check them with `--codxe-ref` (see [Validating GSC changes](#validating-gsc-changes)). |
| The level won't load after editing a script | A GSC compile error stops the level from loading. Run the [checker](#validating-gsc-changes) on your changes. |
| Text starts with `UNLOCALIZED:` | The menu turns `loc_warnings` off by itself. If you still see it, your mod files are older than the fix; copy the `mod_menu` folder again. |
| Menu rows run together on one line, separated by dots | Your mod files are from before rows became separate elements (T4 singleplayer HUD text can't show line breaks). Copy the `mod_menu` folder again. |
| Values (`ON`/`OFF`, slider numbers, `>`) missing from the lower rows, no footer | Your mod files are from before the menu split its HUD elements across both element pools. Copy the `mod_menu` folder again. |
| Some rows show `...` | The menu has used its budget of distinct on-screen strings for this level (see [How it works](#how-it-works)). Highlight the row: its name is printed in the message feed. The budget resets on the next level. |
| Menu feels sluggish | Scripts run on game time, so the menu slows down with **World & Physics → Timescale**. Set it back to 1. |

## Controls

Everything is done with a standard Xbox 360 controller. The table shows the **Default** button
layout.

| Button | Menu closed | Menu open |
| --- | --- | --- |
| **LT** | Hold, then press **RS** to open | Scroll up (hold to repeat) |
| **RT** | | Scroll down (hold to repeat) |
| **A** | | Select, toggle, or apply |
| **X** | | Select (same as A) |
| **RS** (click) | With LT held: open | Back one page. **Hold** to close from anywhere |
| **LB** / **RB** | | Decrease / increase a value, or cycle a choice |
| Left stick | | Up/down scrolls, left/right changes a value |
| D-pad | | Up/down scrolls, left/right changes a value (player 1 only) |
| **B** | Crouch, for the "Crouch + RS" open option | |

While the menu is open you can't move and your gun is lowered, so RT can't fire and the bumpers
can't throw grenades. Closing the menu gives everything back. Opening with LT + RS still plays a
knife swing; that's normal.

The page footer repeats the essentials: `LT/RT Move  A Select  RS Back`.
**Menu Settings → Controls Help** prints the full list in-game.

### Opening options

**Menu Settings → Open With** switches between:

- **LT + RS** (default): hold aim and click melee.
- **Crouch + RS**: press B to crouch, then click melee. Pick this if you often knife while aiming
  and the menu keeps opening in fights.

The choice, theme and menu side are saved per player until you quit the game, so they carry over
between missions.

### Button layouts

The menu reads game actions (aim, fire, jump, use, melee, grenades, movement), not physical
buttons. On a different **Button Layout** or **Stick Layout** in the game's controller options,
use the button that does that action. For example:

- **Tactical** moves melee to **B**, so the menu opens with LT + B, and B is back.
- **Southpaw** swaps the sticks, so scroll with whichever stick moves you.

### D-pad

The D-pad uses the stock `buttonPressed()` script function. It only reads player 1's controller,
and some builds may only allow it in developer mode. If it doesn't respond, everything else
still works.

### Co-op

Each split-screen or online player uses their own controller and has their own menu, cursor and
theme. Only player 1 gets the menu by default. Share it with
**Players → (player) → Toggle Menu Access**, or give everyone access with
**Menu Settings → All Players Get Menu**.

## Features

### Player

God Mode, Demigod (you still feel hits but can't die), Noclip, UFO, Invisible (AI ignore you and
you're hidden), Infinite Ammo (no reloads), Move Speed (0.5x–4x), Super Jump, No Fall Damage,
Unlimited Sprint, Third Person, Field of View (65–120) and Max Health.

Teleport: to your crosshair, save/load position, behind a random enemy, or **Skydive** to the top
of the sky with fall damage off until you land.

### Weapons

- **Give Weapon** lists every weapon the level has actually loaded: your loadout, the level
  loadout, zombie box weapons and weapons the AI are carrying. It never offers one that would fail.
- Random weapon, refill ammo, take current weapon.
- Rapid Fire, Fast Reload, Cluster Grenades, engine-level infinite ammo.
- **Pack-a-Punch** (zombies): upgrades the current weapon when the map has an `_upgraded` version.

### Bullets & Aim

Bullet Mode applies to every shot:

| Mode           | What happens                                                        |
| -------------- | ------------------------------------------------------------------- |
| Explosive      | Explosion with physics push. It never hurts players                 |
| Teleport       | You teleport to the impact point                                    |
| Gib Blaster    | Limbs fly off the target                                            |
| Ragdoll Cannon | The target is killed and launched as a ragdoll                      |
| Tesla Chain    | Electrocutes the target and arcs through up to 6 nearby enemies     |
| Magic Missile  | Fires a real rocket from any rocket weapon the map has loaded       |
| Airstrike      | Six explosions scattered around the impact                          |
| Cluster Bomb   | One blast, then six more in a ring around it                        |
| Time Bomb      | The enemy you hit sparks for two seconds, then explodes             |
| Black Hole     | 6-second vortex that pulls enemies and physics objects in, then detonates |
| Portal Gun     | Shots alternate orange/blue portals; players and AI walk through    |
| Prop Cannon    | Fires random props from the map. They knock over whatever they hit  |
| Zombie Cannon  | Fires a zombie (a soldier in the campaign, where it's called Soldier Cannon) out of your gun. It lands alive |
| Zombie Rocket  | The same, but it explodes on impact (Soldier Rocket in the campaign) |
| AI Summoner    | Spawns a soldier (or zombie) where you shoot                        |
| FX Gun         | Plays the effect last picked in the FX Browser                      |

Also on this page:

- **Model Gun**: every shot also fires a model (the same toggle as in [Forge](#forge)).
- **Projectile Speed** (600–3000) for the Model Gun, Prop Cannon and Zombie Cannon/Rocket.
- **Aimbot**: hold LT to snap to the nearest visible enemy.
- **Death Stare**: enemies you look at die.

Projectiles fly on a real arc and stop at the first wall or character they hit. At most 24 props
and 6 fired enemies exist at once; firing more removes the oldest. On zombies maps a fired zombie
that is still alive after 45 seconds is removed, so the round can end.

### Fun & Chaos

- **Chaos Mode**: a random event every 5–60 seconds, from 22 events. Timed events undo
  themselves.
  - World: moon gravity, bullet time, fast forward, weird vision, earthquake, disco, silent
    film, hurricane, upside-down physics.
  - Carnage: meteor shower, rapture, mass gibbing, blood rain, enemies launched.
  - Players: weapon roulette, sonic speed.
  - Spawns: props raining from the sky, random effects from the level's FX list, an ambush of
    zombies/soldiers around a player, zombies/paratroopers dropped from the sky, a prop tornado
    around a player, and zombie artillery (zombies fired at the players that explode on impact).

  Chaos spawns are capped: 40 props and effects and 6 AI. Each one is removed on its own after
  8–30 seconds, and turning Chaos Mode off wipes them all at once.
- **Jetpack**: hold A in the air to fly where you look.
- **Rocket Ride**: ride a missile, steer it with your view, and explode on impact.
- **Human Cannonball**, **Ground Pound** (RS in the air slams down with a shockwave) and
  **Force Field**.
- **Matrix Mode** slows time while you aim. Also **Drunk Mode** and **Disco Mode**.
- Tactical Nuke, Meteor Shower, The Rapture (every enemy floats up and explodes), Blood Rain and
  Earthquake.
- **FX Browser**: lists every effect the current level loaded. Selecting one plays it at your
  crosshair for 10 seconds and loads it into the FX Gun. Looping effects stop too.
- **Clean Up Spawns** removes every temporary prop, effect and AI the menu has spawned right away.
  Forge props stay.

### Enemies

Kill / gib / launch all enemies, bring every enemy to your crosshair, Freeze, One Hit Kills,
Stormtrooper Aim and Enemy Speed. Campaign only: Switch Sides, Civil War (half the enemies turn on
the other half), Pacifist, Invincible Squad and Spawn Bodyguard.

**Spawn Enemy** lists one entry per actor type the level's spawners can create and spawns it at
your crosshair.

### Nazi Zombies

These items only appear on zombies maps.

- Points (+1k / +10k / +100k, reset, infinite).
- All perks or individual perks. They are lost when you go down, like bought perks.
- **Open All Doors**: hold X. Every door and debris pile opens for free.
- **Turn On Power** on maps with a power switch.
- **Drop Power-Up Now** kills the nearest zombie at your feet so the drop lands there. **Rig Next
  Drop** makes the next kill drop your choice.
- Skip Round, Jump To Round (1–100; press A to apply).
- Zombie Speed (walkers / runners / sprinters), **Crawler Army** (every zombie loses its legs),
  **Headless Horde**, Revive Everyone and an on-screen Zombie Counter.

### World & Physics

Timescale (0.2x–3x), Gravity, Ragdoll Gravity (Moon / Zero-G / Upside Down) and Hurricane Winds.
Also detonate every destructible, destroy all vehicles, and **Drivable Vehicles** (walk up to a
tank or truck and press X).

### Visuals

- Vision sets and fog presets (Blood Moon, Toxic, Silent Hill, Midnight, Bubblegum, Crystal Clear).
- Sun color.
- WaW's built-in special features: Black & White, Photo Negative, Super Contrast, **Silent Film**
  (the hidden Chaplin mode), Slow-mo Ability.
- Fullbright, Motion Blur, Double Vision, Film Grain, Hide HUD, Hide Gun.

### Forge

- **Spawn Model** lists props the level placed plus weapon world models, so every entry is loaded.
  **Spawn Random Model** picks one for you.
- **Solid Spawns** uses CoD Xe's `SpawnCollision()`, so props you spawn have real collision if the
  model has collision data.
- **Physics Spawns** uses real physics where the model supports it.
- **Model Gun**: every shot also fires a model.
  - **Gun Model**: pick the model, or Random Models.
  - **Gun Style**:
    - **Bounce**: the model tumbles on physics if it has any and disappears after 10 seconds.
    - **Build**: the model becomes a Forge prop where it lands, following Solid/Physics Spawns.
    - **Explode**: the model explodes on impact.
  - **Models Per Shot** (1–5) turns it into a shotgun.
- **Model Rain** drops 14 random props around your crosshair. They're temporary.
- **Ring Of Props** places eight copies of your last model in a circle around you.
- **Grab Mode**: hold LB to pick up the prop you're aiming at, RB spins it, and letting go throws
  it.
- Rotate (45°), clone, launch and delete what you're aiming at (delete also works on doors and
  AI). Also undo and clear.

All players share a limit of 64 Forge props, because each prop is a game entity and running out
of entities is a fatal error.

### Death Cards (campaign)

Toggle the campaign's collectible cheats live, mid-mission: Thunder (explosive headshots that
launch ragdolls), Paintball, Cold Dead Hands, Undead, Hard Headed, Berserker, Vampire, Sticks and
Stones, Flak Jacket, Body Armor, Morphine Shot, Dirty Harry and Hardcore.

### Players & Settings

For each co-op player: bring to you, go to them, god mode, menu access, launch them, revive, and
give points (zombies).

Menu Settings: 8 color themes, left/right placement, the open button combo, Controls Help,
**Reset All Mods** and About.

## How it works

- **Entry point.** `maps/_music.gsc` is the stock file plus one call.
  `_load.gsc` calls `music_init()` on every singleplayer level before the level script's first
  `wait`, so precaching is safe there and the same hook covers campaign and zombies.
- **Works on any map.** Referencing a script that isn't in the level's fastfile is a fatal compile
  error. Campaign levels don't contain the `_zombiemode*` scripts, so the menu never calls them.
  Zombies features drive the same level data and entity triggers the zombie scripts listen to:
  - door triggers, the power switch, `level.zombie_powerup_index`;
  - `level.zombie_total`, `level.scr_anim["zombie"]`.

  Weapon, effect, model and spawner lists are read from the running level.
- **Only guaranteed assets.** Explosions use `level._effect["thunder"]` and gore uses
  `anim._effect["animscript_gib_fx"]`. The stock scripts load both on every singleplayer level.
- **One text element per row.** T4 singleplayer HUD text doesn't render line breaks (they show
  up as dots), so each row and each value is its own element, at fixed positions.
- **Two HUD element pools.** Each player's snapshot has two fixed-size HUD element arrays, and an
  element's `archived` flag picks one (IW3 has `current[31]` and `archival[31]`). With all 27
  menu elements in one array, only about 21 were drawn. Now the background, highlight, header,
  title and labels (16) use one array, and the value column and footer (11) use the other, along
  with the menu's zombie counter and perk icons.
- **Temporary entity pools.** Projectiles, chaos props, effect anchors and extra AI go into named
  pools, each with a size cap and a lifetime (`mm_pool_add` in `util.gsc`).
  - A full pool removes its oldest entry.
  - One level thread removes expired entries.
  - Spam recycles old spawns instead of exhausting the level's entities or AI slots.
- **Effects that can be wiped.** A looping effect started with `playFX()` never stops. Temporary
  effects are played with `playFXOnTag()` on a pooled `tag_origin` model, and deleting the model
  ends the effect.
- **HUD string budget.** Every distinct string passed to `setText()` takes a localized-string
  config slot until the level ends. WaW has roughly 1,070 of them, shared with
  `PrecacheString()`, and running out ends the game with `G_FindConfigstringIndex: overflow`.
  - Repeated strings reuse their slot, so opening the same pages again costs nothing.
  - Values use `setValue()` (no slot) or a small fixed set of strings such as `ON`/`OFF`.
  - The menu counts its own distinct strings and stops at 400. After that, new labels show
    `...` and the highlighted item's name is printed in the message feed instead.
  - Browsing every page and every dynamic list comes to about 450 strings, so most sessions never
    reach the cap.
- **Script errors are contained.** Each menu action runs in its own thread, so a runtime error only
  ends that action. The input loop has a watchdog that restarts it if it ever stops.
- **CoD Xe builtins used:**
  - the `god`, `noclip` and `ufo` client fields;
  - the `JumpButtonPressed`, `SecondaryOffhandButtonPressed`, `SprintButtonPressed` and
    `Move*ButtonPressed` methods;
  - `SpawnCollision()`, added in r347. On an older build the level fails to load with
    `unknown function`.

## Known limitations

- Fog presets can't be undone. Levels set fog once at load and don't store the values, so the
  **Unchanged** option only stops further changes.
- D-pad navigation uses the stock `buttonPressed()`, which may be developer-only on retail builds.
  If it is, the menu quietly falls back to the other buttons.
- Solid Spawns only gives collision to models that have collision data.
- Fired and chaos enemies come from the level's own spawners, so after landing they carry on
  with whatever the level's AI scripts give that spawner to do. On campaign maps the cannon fires
  enemy soldiers, while AI Summoner can also spawn friendlies.
- Death Cards appear only in the campaign menu.

## Adding features

Menus are defined in `maps/mod_menu/menus.gsc` with the helpers from `maps/mod_menu/core.gsc`:

```gsc
mm_add_toggle("player", "Moon Boots", "moon_boots", maps\mod_menu\player::moon_boots_set);
mm_add_slider("world", "Gravity", "gravity", maps\mod_menu\world::gravity_set, 50, 1600, 50, 800, "level");
mm_add_action("fun", "Earthquake", maps\mod_menu\fun::earthquake_now);
```

- Toggle callbacks receive `on`.
- Slider callbacks receive the value.
- Choice callbacks receive the index.
- `self` is always the player who used the menu.

## Validating GSC changes

A compile error stops the level from loading, so check scripts offline before copying them to
the console:

```sh
python tools/gsc_check/gsc_check.py resources/t4/_codxe/mods/mod_menu
```

[`tools/gsc_check`](/tools/gsc_check) parses T4 GSC and reports the errors that would stop a level
from loading:
- bad syntax;
- unknown functions or methods;
- builtins called the wrong way;
- scripts that don't exist on every singleplayer map;
- duplicate or colliding functions;
- extra call arguments;
- locals read before they're assigned on every path. This is the compiler's
  `uninitialised variable` error, for example a variable set only inside a loop and read after it;
- `break`/`continue` outside a loop, duplicate `case` values, and assignments to `self`.

The flow rules were checked against about 750 stock scripts. None of the scripts that compile on a
retail build are flagged.

CoD Xe builtins are read from `src/game/t4/sp/components/gsc.cpp`. To check against the build a
player actually runs, pass its release number or commit:

```sh
python tools/gsc_check/gsc_check.py resources/t4/_codxe/mods/mod_menu --codxe-ref r347
```

### Building the builtin index from your own console

Stock builtin names come from `tools/gsc_check/t4_sp_index.json`, which was built from PC scripts.
To build an index from the exact Xbox 360 scripts:

1. Set `"dump_rawfile": true` in `_codxe/codxe.json` (the option is already in the file). Mod
   scripts aren't loaded while dumping, so the levels load normally.
2. Load a campaign mission and each zombies map you care about. CoD Xe writes every script the
   game compiles to `_codxe/dump`.
3. Copy `_codxe/dump` to your PC and run:

   ```sh
   python tools/gsc_check/build_index.py path/to/dump
   ```

4. Set `"dump_rawfile": false` again.
