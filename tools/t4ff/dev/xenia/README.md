# Headless Xenia tests (texture streaming, Kino Der Toten)

Scripts used to check t4ff's streaming and CoD Xe's `streaming.cpp` in Xenia without anyone at the
controller. Paths inside them are the maintainer's (adapt).

## Running a test

1. Copy one `t4ff_test_*.gsc` over `_codxe/t4/mods/t4ff_test/maps/t4ff_test.gsc` in the game folder
   (the test mod's `maps/_music.gsc` starts it; it answers UGX's vote menu itself).
2. Set `_codxe/t4/codxe.json` to `{"active_mod": "t4ff_test", "startup_command": "devmap kinodertoten",
   "dump_rawfile": false, "dump_map_ents": false, "log_console": true}`. **Put the user's own config
   back afterwards** (`codxe.json.user-backup` here: `mod_menu`).
3. Kill any Xenia, wait until it is gone, delete `xenia_canary_windows/xenia.log`, start
   `xenia_canary.exe "<game>/default.xex"`.
4. Watch `xenia.log`: every script step is a line `dvar set t4ff_test <step>`; CoD Xe's lines start
   `[codxe][T4 SP][Streaming]` (`read N bytes at ...`, `extra pool: ...`, `slots: ...`).
5. Screenshots: `powershell -File shot.ps1 -Out <png>` (the largest Xenia window that is not its
   console).

| Script | What it does |
| --- | --- |
| `t4ff_test_tour.gsc` | vote, then 10 teleports (4 views each) over Kino's streamed surfaces and props; ends in the wallpaper room (tour 9) |
| `t4ff_test_tour_clear.gsc` | the tour, then `r_stream 0` (every streamed level dropped, the revert path) and `r_stream 1` |
| `t4ff_test_tour_reload.gsc` | the tour, then `devmap kinodertoten` again (the extra pool given back and taken again); `done2` at the end |
| `t4ff_test_tour_settle.gsc` | the tour, then 15 seconds still (sharpness once everything is in) |
| `t4ff_test_zombie.gsc` | waits for a zombie, stands in front of it, streaming off and on |
| `t4ff_test_thundergun.gsc` | the Thundergun freeze test (fires both Thunderguns) |
| `t4ff_test_kr_intro.gsc` | Kino Rezurrection (map `d`): its helicopter intro, then the first round |
| `t4ff_test_kr_tour.gsc` | Kino Rezurrection: after the intro, 15 places (boxes, perks, theatre, Pack-a-Punch), four views each |
| `t4ff_test_lev_grass.gsc` | CoD Xenon's Leviathan by its grass (CoD Xenon's `mc_ambient_t0c0`): the map moves the player back, so it is no test yet |

Scripts cannot set internal dvars (`r_stream`, `r_streamClear`): they use CoD Xe's GSC builtin
`ExecuteCommand("r_stream 0")`.

## Tools

| Script | Use |
| --- | --- |
| `xenia_run.py` | runs one test: the test script in the test mod, `codxe.json` for it, screenshots at steps, the log; puts the user's `codxe.json` back. `--console-memory`: Xenia's patch enlarging the game's memory pool off for the run (a console's 414 MB), the patch file put back after |
| `mark_pak.py` | color-marks pack entries (deep: level 0 green, level 1 red, rest blue; top levels pink) to see which level shows; `--eighth`: every entry kept as an eighth; `--restore` |
| `headroom.py` | how many of a converted map's textures are below their PC resolution |
| `fidelity.py` | the same, split by identical data / normal maps repacked / re-encoded |
| `mip_tail.py` | what dropping the packed mip tails (16x16 and down) would save |
| `verify_bounds.py` | t4ff's streaming boxes against a disc zone's own |
| `why_not.py` | why a converted map's images do not stream (too small, used by effects...) |
