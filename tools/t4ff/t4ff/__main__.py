"""Command line interface.

    python -m t4ff info <fastfile>
    python -m t4ff roundtrip <fastfile>...
    python -m t4ff convert <pc fastfile or usermap folder> -o <output folder> [options]
    python -m t4ff gui
    python -m t4ff setup [--xma2encode <exe, folder or zip>]
"""

from __future__ import annotations

import argparse
import collections
import os
import sys
import time
import zipfile

from . import progress
from .fastfile import read_fastfile, write_fastfile
from .memory import MEMORY_TARGET_MIB
from .soundbudget import DEFAULT_LOADED_SOUND_MIB, DEFAULT_MAX_LOADED_SOUNDS
from .platforms import for_endian, pc, x360
from .zone import BLOCK_NAMES, Reader, Writer


def cmd_info(args):
    for path in args.fastfiles:
        endian, _, data = read_fastfile(path)
        platform = for_endian(endian)
        zone = Reader(platform, data).load()
        print(f"{path}: {platform.name}, zone {len(data)} bytes, {len(zone.script_strings)} script strings, {len(zone.assets)} assets")
        for name, size in zip(BLOCK_NAMES, zone.block_sizes):
            print(f"  {name:30s} {size:>12,}")
        counts = collections.Counter(a.type for a in zone.assets)
        print("  " + ", ".join(f"{t}: {n}" for t, n in counts.most_common()))
        if args.list:
            for a in zone.assets:
                print(f"    {a.type:16s} {a.name}")


def cmd_roundtrip(args):
    ok = True
    for path in args.fastfiles:
        endian, _, data = read_fastfile(path)
        platform = for_endian(endian)
        zone = Reader(platform, data).load()
        same = Writer(platform).write(zone) == data
        ok &= same
        print(f"{path}: {'identical' if same else 'DIFFERENT'}")
    return 0 if ok else 1


def find_usermap(path: str):
    """Locate the fastfiles of a PC usermap from its folder or one of its fastfiles.

    Returns (map name, {role: path}, [iwd files]) where role is 'map', 'mod', 'patch', 'load' or
    'localized' (a list). ``mod.ff`` is taken from the same folder, or from ``mods/<map>`` when the map
    is in ``usermaps/<map>`` (where the game keeps them). A mod's ``localized_*.ff`` takes the place of
    the game's language zone of that name, which the PC loads with every map: UGX Mod ships its guns in
    ``localized_common.ff`` (Kino Der Toten: 69 weapons, their models, animations and sounds, none in
    the map's other fastfiles; gungame gave nothing on the console).
    """
    if not os.path.exists(path):
        raise SystemExit(f"{path}: not found")
    name = None
    folder = path
    if os.path.isfile(path):
        folder = os.path.dirname(os.path.abspath(path))
        stem = os.path.splitext(os.path.basename(path))[0]
        if stem.lower() != "mod":
            for suffix in ("_load", "_patch"):
                if stem.lower().endswith(suffix):
                    stem = stem[: -len(suffix)]
            name = stem

    ffs = {f.lower(): os.path.join(folder, f) for f in os.listdir(folder) if f.lower().endswith(".ff")}
    if name is None:
        base = [f for f in ffs if not f.endswith(("_load.ff", "_patch.ff")) and f != "mod.ff" and not f.startswith("localized_")]
        if len(base) != 1:
            raise SystemExit(f"{folder}: expected exactly one map fastfile, found {sorted(base)}")
        name = os.path.splitext(os.path.basename(ffs[base[0]]))[0]
    if f"{name.lower()}.ff" not in ffs:
        raise SystemExit(f"{folder}: {name}.ff not found")
    files = {"map": ffs[f"{name.lower()}.ff"]}
    for role, fname in (("mod", "mod.ff"), ("patch", f"{name.lower()}_patch.ff"), ("load", f"{name.lower()}_load.ff")):
        if fname in ffs:
            files[role] = ffs[fname]
    iwds = sorted(os.path.join(folder, f) for f in os.listdir(folder) if f.lower().endswith(".iwd"))
    localized = [ffs[f] for f in sorted(ffs) if f.startswith("localized_")]

    parent = os.path.dirname(os.path.abspath(folder))
    if "mod" not in files and os.path.basename(parent).lower() == "usermaps":
        mod_dir = os.path.join(os.path.dirname(parent), "mods", name)
        if os.path.isfile(os.path.join(mod_dir, "mod.ff")):
            files["mod"] = os.path.join(mod_dir, "mod.ff")
            iwds += sorted(os.path.join(mod_dir, f) for f in os.listdir(mod_dir) if f.lower().endswith(".iwd"))
            localized += sorted(os.path.join(mod_dir, f) for f in os.listdir(mod_dir) if f.lower().startswith("localized_") and f.lower().endswith(".ff"))
    if localized:
        files["localized"] = localized
    return name, files, iwds


def load_pc_zone(path: str):
    endian, _, data = read_fastfile(path)
    if endian != "<":
        raise SystemExit(f"{path}: not a PC fastfile")
    return Reader(pc(), data).load()


def converters(paths, options):
    """Zone converters for PC fastfiles that share one texture budget."""
    from .assets import plan_textures_shared
    from .convert import ZoneConverter

    convs = []
    for index, path in enumerate(paths):
        progress.step("Reading PC fastfiles", index, len(paths))
        convs.append(ZoneConverter(load_pc_zone(path), pc(), x360(), options))
    progress.step("Planning texture memory")
    plan_textures_shared(convs)
    for index, (path, conv) in enumerate(zip(paths, convs)):
        conv.progress_label = f"Converting {os.path.basename(path)}" + (f" (file {index + 1} of {len(paths)})" if len(paths) > 1 else "")
    return convs


def run_converter(path: str, converter):
    start = time.time()
    print(f"{os.path.basename(path)}: {len(converter.zone.assets)} assets")
    result = converter.convert()
    stats = converter.stats
    print(f"  converted:  {dict(sorted(stats.converted.items()))}")
    if stats.copied:
        print(f"  copied from console zones: {dict(sorted(stats.copied.items()))}")
    if stats.referenced:
        print(f"  referenced: {dict(sorted(stats.referenced.items()))}")
    print(f"  textures:   {stats.texture_bytes / 1048576:.1f} MiB, loaded sounds: {stats.sound_bytes / 1048576:.1f} MiB ({time.time() - start:.1f}s)")
    return result


def convert_fastfile(path: str, options):
    return run_converter(path, converters([path], options)[0])


def write_zone(zone, target: str, jobs: int = 0, out: bytes = None):
    """Write a console zone (``out``: the zone already serialised)."""
    progress.step(f"Writing {os.path.basename(target)}")
    if out is None:
        out = Writer(x360()).write(zone)
    write_fastfile(target, ">", out, jobs=jobs)
    # reading the zone back checks it with the console loading rules
    progress.step(f"Checking {os.path.basename(target)}")
    sizes = Reader(x360(), out).load().block_sizes
    blocks = ", ".join(f"{n.split('_BLOCK_')[1].lower()} {s / 1048576:.1f}" for n, s in zip(BLOCK_NAMES, sizes) if s)
    print(f"wrote {target} ({os.path.getsize(target) / 1048576:.1f} MiB compressed)")
    print(f"  memory: {sum(sizes) / 1048576:.1f} MiB ({blocks})")


def _texture_budget(text: str) -> str:
    """--texture-budget: "auto" or a number of MiB (0: no limit)."""
    if text.strip().lower() == "auto":
        return "auto"
    try:
        if float(text) >= 0:
            return text
    except ValueError:
        pass
    raise argparse.ArgumentTypeError("a number of MiB, 0 for no limit, or auto")


def cmd_convert(args):
    from .audio import XmaEncoder, convert_streamed_sounds
    from .convert import ConvertOptions
    from .images import IwdLibrary
    from .merge import prune_references

    name, files, iwds = find_usermap(args.input)
    print(f"usermap {name}:")
    for role in ("map", "patch", "mod", "load"):
        print(f"  {role + ':':6} {files.get(role, 'not found')}")
    for path in files.get("localized", []):
        print(f"  localized: {path} (the mod's language zone, merged too)")
    for iwd in iwds:
        print(f"  iwd:   {iwd}")
    # CoD Xe reads _codxe\t4 when it exists (its newer layout, CoD Xenon's 0.2.0 maps), else _codxe
    out_dir = os.path.join(args.output, "_codxe", *(["t4"] if args.t4_layout else []), "usermaps", name)
    os.makedirs(out_dir, exist_ok=True)

    encoder = XmaEncoder(args.xma_encoder, args.xma_quality)
    if not encoder.available and not args.no_sounds and not args.xma_encoder and not args.no_install:
        # e.g. a download of it (or a .zip with it) sitting in the Downloads folder
        from . import deps

        try:
            found = deps.ensure_xma2encode()
        except (OSError, zipfile.BadZipFile) as e:
            found = None
            print(f"warning: cannot use the xma2encode.exe found: {e}")
        if found:
            print(f"using {found}")
            encoder = XmaEncoder(found, args.xma_quality)
    if not encoder.available and not args.no_sounds:
        print("warning: xma2encode.exe not found: sounds are not converted, the map will reference console sounds (run python -m t4ff setup)")

    if not args.no_sounds and encoder.available:
        library = IwdLibrary(iwds)
        stats = convert_streamed_sounds(library, out_dir, encoder, args.stream_rate, args.mono_streams, jobs=args.jobs)
        if stats["sounds"]:
            print(
                f"streamed sounds: {stats['converted']}/{stats['sounds']} converted, "
                f"{stats['input_bytes'] / 1048576:.1f} MiB -> {stats['output_bytes'] / 1048576:.1f} MiB"
            )

    from .memory import (FREE_CONSOLE_MIB, FREE_MIB, MARGIN_MIB, MIB, MIN_TEXTURE_BUDGET_MIB, block_sizes, memory_bytes, next_texture_budget,
                         texture_bytes)

    auto_budget = args.texture_budget == "auto"
    args.upgrade_budget_bytes = int(args.upgrade_budget * MIB) if args.stream_textures else None
    target = int(args.memory_target * MIB)
    # a console's memory leaves no extra stream pool: boxes half the disc's (stream.CONSOLE_STREAM_GROWTH)
    from .stream import CONSOLE_STREAM_GROWTH

    args.stream_growth_scale = args.stream_growth if args.stream_growth else (CONSOLE_STREAM_GROWTH if args.memory_target <= FREE_CONSOLE_MIB else 1.0)
    if args.memory_target > FREE_CONSOLE_MIB:
        print(f"warning: the {args.memory_target:g} MiB memory target is over the {FREE_CONSOLE_MIB:.1f} MiB a console has free for a map: "
              f"the map will load in Xenia only (its patch enlarging the game's memory pool on)")
    options = ConvertOptions(
        allow_unverified=args.allow_unverified,
        max_texture_size=args.max_texture_size,
        # streamed textures leave most of their memory to the others: start from full quality
        texture_budget=(4096 * MIB if args.stream_textures else target) if auto_budget else int(float(args.texture_budget) * MIB),
        keep_mips=not args.no_mips,
        compress_textures=not args.no_compress,
        eighth_levels=not args.keep_quarter,
        iwd_paths=iwds,
        stock_paths=args.iwd,
        xma_encoder=None if args.no_sounds else encoder,
        sound_rate=args.sound_rate,
        mono_sounds=args.mono_sounds,
        sounds_dir=out_dir,
        console_zones=args.console_zone,
        map_name=name,
        jobs=args.jobs,
    )

    # One fastfile, as in CoD Xenon's converted maps: the console has no mod.ff, and <map>_patch.ff is
    # merged too. Scripts of later zones win (patch over map, mod over both). One texture budget.
    # The mod's language zones come first, as the PC loads them before the map: the map's and the
    # mod's scripts win over theirs, and their own assets (UGX's guns) are added.
    paths = list(files.get("localized", [])) if not args.no_mod else []
    paths.append(files["map"])
    if "patch" in files and not args.no_patch:
        paths.append(files["patch"])
    if "mod" in files and not args.no_mod:
        paths.append(files["mod"])
    map_files = IwdLibrary(list(dict.fromkeys([os.path.dirname(os.path.abspath(files["map"]))] + [os.path.dirname(os.path.abspath(p)) for p in iwds])))

    # The automatic texture budget: the textures get the main memory the target leaves,
    # measured on the converted map (converted again with less when it is over; sounds are encoded
    # once). See memory.py.
    import dataclasses

    measured = None  # (texture budget, memory) of the last conversion the budget changed
    attempts = 5
    for attempt in range(attempts):
        last = attempt == attempts - 1
        args.stream_report = {}
        main_zone, planned = _convert_map(args, paths, options, map_files, out_dir)
        progress.step("Measuring the memory")
        out = Writer(x360()).write(main_zone)
        used = memory_bytes(block_sizes(out))
        # over the target: what the PC versions of stock textures added gives way first, then the
        # textures' mip tails (a little shimmer far away, before any blur up close), in one go when
        # the upgrades are not enough
        upgrade = args.stream_report.get("upgrade_bytes", 0)
        tail = auto_budget and options.mip_tail and not args.keep_mip_tail and used - target > upgrade
        if used > target and (upgrade or tail) and not last:
            changes = []
            if upgrade:
                args.upgrade_budget_bytes = max(0, upgrade - (used - target))
                changes.append(f"{args.upgrade_budget_bytes / MIB:.1f} MiB for the PC versions of stock textures")
            if tail:
                options = dataclasses.replace(options, mip_tail=False)
                changes.append("textures without their mip levels of 16 texels or less (the packed mip tail)")
            print(f"memory: {used / MIB:.1f} MiB of main memory, over the {target / MIB:.0f} MiB target: converting again "
                  f"with {' and '.join(changes)}")
            continue
        if not auto_budget:
            break
        textures = texture_bytes(main_zone)
        steps = args.stream_report.get("steps")
        if used > target and steps and not options.stream_steps and not last:
            # the budget counts what the textures keep in the fastfile from now on, a streamed one
            # only its levels below the streamed ones: those that keep the most lose a level first
            cut = min(used - target + MARGIN_MIB * MIB, max(textures - MIN_TEXTURE_BUDGET_MIB * MIB, 0))
            print(f"memory: {used / MIB:.1f} MiB of main memory, over the {target / MIB:.0f} MiB target: converting again "
                  f"with {cut / MIB:.1f} MiB less of textures in the fastfile ({len(steps)} streamed)")
            options = dataclasses.replace(options, stream_steps=steps, texture_cut=cut)
            measured = None
            continue
        # what a byte less of planned textures saves: measured between two budgets, else estimated
        # from the share of the planned textures the fastfile keeps (streamed ones keep a part)
        effective = min(options.texture_budget, planned)
        if measured is not None and measured[0] > effective and measured[1] > used:
            efficiency = (measured[1] - used) / (measured[0] - effective)
        else:
            efficiency = min(1.0, textures / planned) if planned else 1.0
        measured = (effective, used)
        budget = next_texture_budget(used, target, planned, options.texture_budget, efficiency) if not last else None
        if budget is None:
            if used > target:
                print(
                    f"warning: the map needs {used / MIB:.1f} MiB of main memory, over the {target / MIB:.0f} MiB target "
                    f"({(used - textures) / MIB:.1f} MiB without its textures); the game has about {FREE_CONSOLE_MIB:.1f} MiB free for a map's zone on a console, "
                    f"{FREE_MIB} in Xenia"
                )
            else:
                print(f"memory: {used / MIB:.1f} MiB of main memory of the {target / MIB:.0f} MiB target, {textures / MIB:.1f} MiB of it textures")
            break
        print(f"memory: {used / MIB:.1f} MiB of main memory, over the {target / MIB:.0f} MiB target: converting again with {budget / MIB:.1f} MiB of textures")
        options = dataclasses.replace(options, texture_budget=budget, texture_cut=0)
    write_zone(main_zone, os.path.join(out_dir, f"{name}.ff"), args.jobs, out)
    from .library import T4FF_MARKER

    # streams kept from an earlier conversion, in the game's layout too (see audio.py)
    from .audio import upgrade_stream_files

    if os.path.isdir(os.path.join(out_dir, "sounds")):
        upgraded = upgrade_stream_files(os.path.join(out_dir, "sounds"))["upgraded"]
        if upgraded:
            print(f"streamed sounds: {upgraded} kept from an earlier conversion rewritten in the game's layout (4 KiB blocks)")
    # later conversions do not take this map's fastfiles for console data (--console-zone)
    with open(os.path.join(out_dir, T4FF_MARKER), "w", encoding="utf-8") as f:
        f.write("Converted from the PC by t4ff (tools/t4ff): not console data, t4ff leaves these fastfiles out of its console fastfiles.\n")

    # the map's name in the Nazi Zombies map list (CoD Xe reads the first line of description.txt)
    from .loadscreen import map_title
    from .menu import localized_strings

    title = args.name.strip() or map_title(map_files, name, localized_strings(x360(), main_zone))
    description = os.path.join(out_dir, "description.txt")
    if args.name.strip() or not os.path.exists(description) or _bare_description(description, name):
        with open(description, "w", encoding="latin-1", errors="replace", newline="\r\n") as f:
            f.write(title + "\n")
        print(f'map list name: "{title}" ({description}; change it there or with --name)')

    if args.load_zone:
        # CoD Xe serves <map>_load.ff as the loading screen zone (CoD Xenon's 0.2.0 maps have one):
        # made like theirs, with the map's picture
        from .library import library_files
        from .loadscreen import LoadScreenError, write_load_zone

        progress.step("Writing the loading screen")
        try:
            done = write_load_zone(name, out_dir, library_files(args.console_zone, name, (out_dir,)), map_files, files.get("load"), args.loading_image, args.jobs, title=title)
        except LoadScreenError as e:
            print(f"warning: {e}")
            done = False
        if not done and "load" in files:
            # no load zone of CoD Xenon's among the console fastfiles: the PC one converted
            zone = convert_fastfile(files["load"], dataclasses.replace(options, reference_techsets=True, texture_budget=0))
            prune_references(x360(), zone)
            write_zone(zone, os.path.join(out_dir, os.path.basename(files["load"])), args.jobs)
        elif not done:
            print("loading screen: none written (add CoD Xenon's _codxe\\t4 folder to the console fastfiles): the game shows a checkerboard while the map loads")
        # the same picture for the map list (CoD Xe shows it next to the map's name)
        from .loadscreen import write_preview

        if write_preview(out_dir, name):
            print("map list picture: preview.bin, from the loading screen")
    # and for CoD Xe's own custom maps list (release r351): map.json and preview.dds (see menu.py)
    from .menu import write_map_info

    info = write_map_info(out_dir, name)
    if info:
        print(f"CoD Xe's custom maps list: {' and '.join(info)} (from description.txt and the loading screen)")
    return 0


def _bare_description(path: str, name: str) -> bool:
    """Whether the description.txt at ``path`` only has the map's file name (as older conversions
    wrote it from an .arena name that repeats it): a better one replaces it, edited ones stay."""
    with open(path, "r", encoding="latin-1") as f:
        lines = [line.strip() for line in f if line.strip()]
    return lines == [name]


def _convert_map(args, paths, options, map_files, out_dir):
    """The map's fastfiles converted and merged into one console zone."""
    from .images import IwdLibrary
    from .merge import merge_zones, prune_references

    convs = converters(paths, options)
    # the map's own loose scripts win over those of its fastfiles, as on PC
    from .scripts import missing_scripts_zone, override_scripts

    override_scripts(pc(), [c.zone for c in convs], map_files)
    # scripts the PC game takes from the mod (its mod.ff, its own files) over the game's own
    from .scripts import _rawfiles, normalize

    mod_scripts = set()
    for path, conv in zip(paths, convs):
        for script, _ in _rawfiles(pc(), conv.zone):
            key = normalize(script)
            mod_zone = os.path.basename(path).lower() == "mod.ff" or os.path.basename(path).lower().startswith("localized_")
            if not script.startswith(",") and (mod_zone or map_files.read(key) is not None):
                mod_scripts.add(key)
    # streamed sounds of the game's own the console's disc does not have (Der Riese's...): from the
    # PC game's files, before the aliases are converted (they point to the files found next to the map)
    if options.xma_encoder is not None and getattr(options.xma_encoder, "available", False) and options.sounds_dir:
        from .assets import ship_stock_streams

        ship_stock_streams(pc(), [c.zone for c in convs], IwdLibrary(args.iwd) if args.iwd else None, options.sounds_dir,
                           options.xma_encoder, args.stream_rate, args.mono_streams, args.jobs)
    zones = [run_converter(path, conv) for path, conv in zip(paths, convs)]
    if len(zones) > 1:
        progress.step("Merging into one fastfile")
    main_zone = zones[0] if len(zones) == 1 else merge_zones(x360(), zones)
    progress.step("Checking the scripts")
    extra = missing_scripts_zone(x360(), main_zone, [map_files, IwdLibrary(args.iwd)], convs[0].console_library,
                                 roots=(f"maps/{options.map_name}.gsc", f"clientscripts/{options.map_name}.csc"), from_map=mod_scripts)
    if extra is not None:
        main_zone = merge_zones(x360(), [main_zone, extra], log=lambda msg: None)
    # assets the game looks up by name (player body animations, shellshock files) no zone has
    from .named import named_assets_zone

    extra = named_assets_zone(x360(), main_zone, [map_files, IwdLibrary(args.iwd)], convs[0].console_library)
    if extra is not None:
        main_zone = merge_zones(x360(), [main_zone, extra], log=lambda msg: None)
    # the mod's scripts that the game's own zones have too run under their own names (see scripts.py)
    from .scripts import keep_mod_scripts

    renamed = keep_mod_scripts(x360(), main_zone, mod_scripts, convs[0].console_library, f"maps/{options.map_name}.gsc")
    # the options the mod's own front end menus set, before those menus go
    from .scripts import menu_dvar_values

    menu_values = menu_dvar_values(main_zone)
    pause_menus = []  # menus the map's own pause menu opens
    if convs[0].console_library is not None:
        from .merge import bind_pause_menu, drop_frontend_menus, menu_names

        library = convs[0].console_library
        ingame = library.find_in_game_zones("MenuList", "ui/ingame.txt")
        kept, pause_menus = bind_pause_menu(x360(), main_zone, menu_names(ingame[1]) if ingame else [])
        drop_frontend_menus(x360(), main_zone, library.is_stock_menu, is_stock_list=lambda name: library.in_game_zones("MenuList", name), keep_menus=kept)
    from .merge import drop_unused_videos

    drop_unused_videos(x360(), main_zone)
    # PC script menus (a music box) and hints name keyboard keys: the controller's buttons instead;
    # their items that do nothing stay out of the controller's way, and the D-pad moves between
    # buttons as they are laid out
    from .menu import controller_navigation, decorate_inert_items, gamepad_script_menus
    from .scripts import (
        fix_modder_help, local_client_effects, menu_dvar_defaults, precache_before_waits, spawn_script_origins, speed_up_zombies_only,
        use_key_hints, valid_cursor_hints, zombie_idles_for_zombies,
    )  # fmt: skip

    gamepad_script_menus(x360(), main_zone)
    from .scripts import script_strings

    script_menus = script_strings(x360(), main_zone) | set(pause_menus)  # the menus the scripts open are named in them
    decorate_inert_items(x360(), main_zone, script_menus=script_menus)
    controller_navigation(x360(), main_zone, script_menus=script_menus)
    use_key_hints(x360(), main_zone)
    # the modding kits' setups stop at missing entities, as their authors meant (see scripts.py)
    fix_modder_help(x360(), main_zone)
    spawn_script_origins(x360(), main_zone)
    valid_cursor_hints(x360(), main_zone)
    local_client_effects(x360(), main_zone)
    precache_before_waits(x360(), main_zone, f"maps/{options.map_name}.gsc")
    menu_dvar_defaults(x360(), main_zone, menu_values, f"maps/{options.map_name}.gsc")
    speed_up_zombies_only(x360(), main_zone)
    zombie_idles_for_zombies(x360(), main_zone)
    # in splitscreen the map's own fog, not the game's yellow placeholder (see scripts.py)
    from .scripts import splitscreen_fog

    def is_game_script(name, library=convs[0].console_library):
        return name in renamed.values() or (library is not None and library.find_in_game_zones("RawFile", name) is not None)

    splitscreen_fog(x360(), main_zone, f"maps/{options.map_name}.gsc", is_game_script)
    # the console draws no world for a player on an MG42 turret of a zombie map: a held gun instead
    from .scripts import mounted_guns

    mounted_guns(x360(), main_zone, f"maps/{options.map_name}.gsc")
    # a timed power-up's endless ammo left on by a game that ended meanwhile (see scripts.py)
    from .scripts import reset_sustain_ammo

    reset_sustain_ammo(x360(), main_zone, f"maps/{options.map_name}.gsc")
    # technique sets copied from CoD Xenon's maps read the dynamic shadow texture before it is set
    from .techsets import fix_argument_sections

    fix_argument_sections(x360(), main_zone)
    prune_references(x360(), main_zone)
    if args.max_loaded_sounds or args.loaded_sound_memory:
        from .audio import LoadedXma
        from .soundbudget import limit_loaded_sounds

        progress.step("Checking the loaded sound limit")
        streams = {key[0].lower(): xma.stream for key, xma in options.sound_cache.items() if isinstance(xma, LoadedXma) and xma.stream is not None}
        limit_loaded_sounds(x360(), main_zone, args.max_loaded_sounds, streams, out_dir, max_bytes=int(args.loaded_sound_memory * 1048576))
    from .soundbudget import sync_alias_types

    fixed = sync_alias_types(x360(), main_zone)
    if fixed:
        print(f"sound aliases: the type in the flags of {fixed} aliases set to their sound file's")
    # the mod's versions of the game's scripts that keep no name of their own: the map's scripts folder,
    # which CoD Xe loads in place of the game's (after the fixes above, which they have too)
    from .scripts import usermap_scripts, write_usermap_scripts

    # options the mod's own front end menus choose (PhilMod's difficulty): the Custom Maps menu shows them
    from .menu import menu_options, write_options
    from .scripts import script_dvars

    pc_values = {}
    for conv in convs:
        for dvar, values in menu_dvar_values(conv.zone).items():
            pc_values.setdefault(dvar, set()).update(values)
    map_options = menu_options(pc(), [c.zone for c in convs], script_dvars(x360(), main_zone), pc_values)
    write_options(map_options, out_dir)
    # and asked in game as the level starts, for players without that menu (before the scripts
    # folder is written: the menus its scripts open wait for the options too)
    if map_options:
        from .menu import add_options_menus
        from .scripts import options_script

        menus = add_options_menus(x360(), main_zone, convs[0].console_library, map_options)
        if menus:
            options_script(x360(), main_zone, f"maps/{options.map_name}.gsc", map_options, menus)
    write_usermap_scripts(usermap_scripts(x360(), main_zone, mod_scripts, convs[0].console_library, renamed), out_dir)
    if args.stream_textures:
        from .stream import stream_textures

        # the console fastfiles' own highmip folders (the disc's, next to its zones)
        zone_dirs = {os.path.abspath(z if os.path.isdir(z) else os.path.dirname(z)) for z in args.console_zone or []}
        library = convs[0].console_library
        stream_textures(x360(), main_zone, out_dir, highmip_dirs=sorted(os.path.join(d, "highmip") for d in zone_dirs),
                        is_game_image=(lambda name: library.in_game_zones("GfxImage", name)) if library is not None else None,
                        stock_texture=_stock_texture(convs, options),
                        deep=True if args.deep_stream == "all" else {n.strip().lower() for n in args.deep_stream.split(",") if n.strip()} if args.deep_stream else None,
                        upgrade_budget=args.upgrade_budget_bytes, report=args.stream_report, mip_tail=options.mip_tail,
                        eighth=getattr(convs[0], "eighth_images", None) or None, growth=args.stream_growth_scale)
    return main_zone, getattr(convs[0], "planned_texture_bytes", 0)


def _stock_texture(convs, options):
    """The PC game's version of a stock texture (--iwd), tiled for the console, for the streaming
    pass to replace a smaller console copy with; None for the map's own textures."""
    from . import images as img
    from .assets import _pc_normal_map, stock_image_source

    own = {name for conv in convs for name, source in conv.__dict__.get("_image_sources", {}).items() if source is not None}

    def texture(name, semantic):
        if name in own or getattr(convs[0], "stock_library", None) is None:
            return None
        source = stock_image_source(convs[0], name)
        if source is None or source.faces != 1:
            return None
        try:
            return img.build_console_texture(source, 0, True, 0, options.compress_textures, _pc_normal_map(source, semantic), mip_tail=options.mip_tail)
        except img.ImageError:
            return None

    return texture


def cmd_menu(args):
    """Get the game's menu zone ready for the maps of its usermaps folder: CoD Xe's own custom maps
    list when its patch_ui.ff has it, or one given with --menu-zone has it (CoD Xenon's 0.3.0), else
    t4ff's list made from CoD Xenon's 0.2.0 patch_ui.ff (see menu.py)."""
    import shutil

    from .menu import MAP_JSON, PREVIEW_DDS, has_own_usermaps_list, menu_zone_candidates, write_map_info

    root = args.folder
    zone_dir = os.path.join(root, "zone") if os.path.isdir(os.path.join(root, "zone")) else root
    target = os.path.join(zone_dir, "patch_ui.ff")
    usermaps = os.path.join(root, "usermaps")

    def own_list(path):
        try:
            return has_own_usermaps_list(x360(), Reader(x360(), read_fastfile(path)[2]).load())
        except Exception:
            return False

    own = os.path.exists(target) and own_list(target)
    if not own:
        # a menu zone with CoD Xe's own list takes the place of the game's (the one before is kept)
        for candidate in menu_zone_candidates(args.menu_zone or []):
            if os.path.abspath(candidate) == os.path.abspath(target) or not own_list(candidate):
                continue
            backup = target + ".bak"
            if os.path.exists(target) and not os.path.exists(backup):
                shutil.copyfile(target, backup)
            shutil.copyfile(candidate, target)
            print(f"{target}: the menu zone of {candidate}" + (f" (the one before is {backup})" if os.path.exists(backup) else ""))
            own = True
            break
    if own:
        print(f"{target}: \"Custom Maps\" is CoD Xe's own list (the menu codxe_usermaps), which shows the maps of the usermaps folder "
              "from their map.json and preview.dds (CoD Xe r351 or later)")
    else:
        status = _t4ff_menu(args, zone_dir, target, usermaps)
        if status:
            return status

    # map.json and preview.dds, which CoD Xe's own list reads, where a map has none (see menu.py)
    infos = []
    for name in sorted(os.listdir(usermaps)) if os.path.isdir(usermaps) else []:
        folder = os.path.join(usermaps, name)
        if not os.path.isdir(folder):
            continue
        written = write_map_info(folder, name, metadata=not os.path.exists(os.path.join(folder, MAP_JSON)),
                                 picture=not os.path.exists(os.path.join(folder, PREVIEW_DDS)))
        if written:
            infos.append(f"{name} ({', '.join(written)})")
    if infos:
        print(f"CoD Xe's custom maps list: {', '.join(infos)}")
    # the streamed sounds of the maps (CoD Xenon's, older conversions) in the game's layout
    if os.path.isdir(usermaps) and not args.no_streams:
        upgrade_map_streams(usermaps)
    return 0


def _t4ff_menu(args, zone_dir: str, target: str, usermaps: str) -> int:
    """t4ff's own custom maps list, made from CoD Xenon's 0.2.0 patch_ui.ff (see menu.py), with the
    names, descriptions and pictures of the maps. Returns 0, or 1 when it cannot be made."""
    import shutil

    from .menu import MenuError, description_text, localized_strings, make_dynamic, not_cod_xenon_menu

    original = target + ".orig"
    # CoD Xenon's menu is kept as patch_ui.ff.orig and every run starts again from it
    source = original if os.path.exists(original) else target
    if not os.path.exists(source):
        print(f"error: {target} not found (give CoD Xenon's _codxe\\t4 folder, or a menu zone with CoD Xe's own list with --menu-zone)")
        return 1
    endian, _, data = read_fastfile(source)
    zone = Reader(x360(), data).load()
    problem = not_cod_xenon_menu(x360(), zone)
    if problem:
        print(f"error: {source} is not CoD Xenon's menu: {problem}.")
        print(
            f"Put CoD Xenon's patch_ui.ff (_codxe\\t4\\zone\\patch_ui.ff of their 0.2.0 zip) in {zone_dir}"
            + (f" and delete {original}" if source == original else "")
            + ", or give their 0.3.0 one with --menu-zone, then run this again."
        )
        return 1
    if source == target:
        shutil.copyfile(target, original)
        print(f"kept CoD Xenon's menu as {original}")
    try:
        rows = make_dynamic(x360(), zone, args.rows)
    except MenuError as e:
        print(f"error: {e}")
        return 1
    write_zone(zone, target)
    print(f'{target}: "Custom Maps" in the Nazi Zombies menu lists the maps of the usermaps folder, {args.rows} at a time (LB / RB: a page)')

    # names, descriptions and pictures of CoD Xenon's maps, whose rows the list replaces
    patch = os.path.join(zone_dir, "patch.ff")
    strings = localized_strings(x360(), Reader(x360(), read_fastfile(patch)[2]).load()) if os.path.exists(patch) else {}
    written = 0
    for row in rows:
        folder = os.path.join(usermaps, row["map"])
        if not os.path.isdir(folder):
            continue
        title = strings.get((row["title"] or "").lstrip("@"), row["map"])
        description = strings.get((row["description"] or "").lstrip("@"), "")
        path = os.path.join(folder, "description.txt")
        if not os.path.exists(path):
            with open(path, "w", encoding="latin-1", newline="\r\n") as f:
                f.write(description_text(title, description))
            written += 1
        if row["image"]:
            with open(os.path.join(folder, "preview.txt"), "w", encoding="latin-1") as f:
                f.write(row["image"] + "\n")
    if written:
        print(f"wrote the names and descriptions of {written} of CoD Xenon's maps (description.txt in their folders)")

    # pictures of the other maps, from their loading screens
    from .loadscreen import write_preview
    from .menu import PREVIEW_FILE

    pictures = []
    for name in sorted(os.listdir(usermaps)) if os.path.isdir(usermaps) else []:
        folder = os.path.join(usermaps, name)
        if not os.path.isdir(folder) or any(os.path.exists(os.path.join(folder, f)) for f in ("preview.txt", PREVIEW_FILE)):
            continue
        if write_preview(folder, name):
            pictures.append(name)
    if pictures:
        print(f"map list pictures (preview.bin) from the loading screens of {', '.join(pictures)}")
    return 0

def upgrade_map_streams(folder: str) -> int:
    """Rewrite the streamed sounds (.xma) under ``folder`` in the game's layout, reporting it."""
    from .audio import upgrade_stream_files

    print(f"streamed sounds: checking the .xma files in {folder}")
    stats = upgrade_stream_files(folder)
    if stats["upgraded"]:
        print(
            f"streamed sounds: {stats['upgraded']} of {stats['files']} in {folder} rewritten in the game's layout "
            "(4 KiB blocks and their table; before, they stopped after a split second)"
        )
    elif stats["files"]:
        print(f"streamed sounds: the {stats['files']} in {folder} have the game's layout already")
    return 1 if stats["failed"] else 0


def cmd_streams(args):
    return max(upgrade_map_streams(folder) for folder in args.folders)


def cmd_setup(args):
    from . import deps

    state = deps.setup(args.xma2encode, test=not args.no_test)
    print()
    ready = state.get("python") and state.get("openassettools")
    if ready and state.get("xma2encode"):
        print("Ready: everything t4ff needs is installed.")
    elif ready:
        print("Ready to convert, without sounds (xma2encode.exe is missing, see above).")
    else:
        print("Not ready, see above.")
    return 0 if ready else 1


def cmd_gui(args):
    from .gui import main as gui_main

    gui_main()
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(prog="t4ff", description="World at War fastfile tools (PC -> Xbox 360 conversion for CoD Xe)")
    parser.add_argument("--no-install", action="store_true", help="do not install missing Python packages or xma2encode.exe automatically")
    parser.add_argument("--progress-lines", action="store_true", help="report progress as '@progress <done> <total> <step>' lines (for the window)")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("info", help="list the content of a fastfile")
    p.add_argument("fastfiles", nargs="+")
    p.add_argument("--list", action="store_true", help="list every asset")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("roundtrip", help="read and rewrite fastfiles, checking the result is identical")
    p.add_argument("fastfiles", nargs="+")
    p.set_defaults(func=cmd_roundtrip)

    p = sub.add_parser("convert", help="convert a PC usermap to the Xbox 360")
    p.add_argument("input", help="PC usermap folder (containing <map>.ff and .iwd files) or a PC .ff")
    p.add_argument("-o", "--output", required=True, help="output folder (a _codxe folder is created inside)")
    p.add_argument("--iwd", action="append", default=[], help="extra .iwd files or folders to take images/sounds from (e.g. the PC game's main folder)")
    p.add_argument("--max-texture-size", type=int, default=0, help="largest texture dimension, bigger textures are downscaled (default: no limit)")
    p.add_argument("--texture-budget", type=_texture_budget, default="auto", help="texture memory budget in MiB, 0 for no limit, or auto (default): what the memory target leaves")
    p.add_argument("--stream-textures", action=argparse.BooleanOptionalAction, default=True, help="the textures of models and world surfaces keep their top mip level in the map's images.pak, loaded when what uses them is close (default; needs a CoD Xe build serving the map's images.pak, with another the game keeps the fastfile's smaller copies). --no-stream-textures: every texture whole in the fastfile")
    p.add_argument("--upgrade-budget", type=float, default=96.0, help="with --stream-textures: MiB the PC versions of stock textures the console has smaller may add to the fastfile (streamed); lowered when the map is over its memory target (default 96)")
    p.add_argument("--deep-stream", default="", help="with --stream-textures: images (comma separated names, or all) that stream two mip levels at once where they can, the fastfile keeping a quarter of their size (needs a CoD Xe build applying them)")
    p.add_argument("--memory-target", type=float, default=MEMORY_TARGET_MIB, help=f"main memory the map's zone may use in MiB (all its blocks: textures, models, animations, the world...), for the automatic texture budget (default {MEMORY_TARGET_MIB}: a console has about 220.7 MiB free for it when a map loads; Xenia, whose patch for the game enlarges its memory pool, 286.7)")
    p.add_argument("--stream-growth", type=float, default=0, help="with --stream-textures: how far around a surface its streamed textures load their top level, of the disc linker's distance (1931.2 / texels per unit); default: 0.5 for a console's memory target (the 64 MB buffer alone), 1 above it")
    p.add_argument("--keep-quarter", action="store_true", help="deep streamed textures keep a quarter of their size in the fastfile when the map is over its memory target (by default those the budget needs keep an eighth, three levels streamed, before any texture loses its top level)")
    p.add_argument("--keep-mip-tail", action="store_true", help="keep every texture's mip levels of 16 texels or less when the map is over its memory target (by default they go before any texture loses its top level)")
    p.add_argument("--xma-encoder", help="path to xma2encode.exe (Xbox 360 XDK); also read from XMA2ENCODE or XEDK")
    p.add_argument("--xma-quality", type=int, default=60, help="xma2encode quality 1-100 (default 60)")
    p.add_argument("--stream-rate", type=int, default=0, help="resample streamed sounds above this rate (e.g. 32000)")
    p.add_argument("--mono-streams", action="store_true", help="downmix streamed sounds to mono")
    p.add_argument("--sound-rate", type=int, default=0, help="highest sample rate of loaded (in memory) sounds: 24000, 32000, 44100 or 48000 (default: keep)")
    p.add_argument("--mono-sounds", action="store_true", help="downmix loaded (in memory) sounds to mono")
    p.add_argument("--console-zone", action="append", default=[], help="Xbox 360 fastfile (stock or already converted) to copy console only assets from, e.g. technique sets; repeatable")
    p.add_argument("--no-sounds", action="store_true", help="do not convert streamed sounds")
    p.add_argument("--no-mod", action="store_true", help="do not merge the usermap's mod.ff into the map fastfile")
    p.add_argument("--no-patch", action="store_true", help="do not merge the usermap's <map>_patch.ff into the map fastfile")
    p.add_argument("--t4-layout", action=argparse.BooleanOptionalAction, default=True, help="write _codxe/t4/usermaps/<map>, CoD Xe's newer layout (default; CoD Xe reads _codxe/t4 when it exists, e.g. with CoD Xenon's 0.2.0 maps, and then ignores _codxe/usermaps). --no-t4-layout: _codxe/usermaps/<map>")
    p.add_argument("--load-zone", action=argparse.BooleanOptionalAction, default=True, help="write <map>_load.ff, the loading screen (default; made like CoD Xenon's, whose 0.2.0 maps all have one)")
    p.add_argument("--name", default="", help="the map's name in the map list and on its title card (default: the longname of its .arena file, else from the map's file name)")
    p.add_argument("--loading-image", default="", help="picture for the loading screen (.png, .jpg, .bmp, .tga, .dds or .iwi; default: CoD Xenon's for the map, the map's own, else a title card)")
    p.add_argument("--no-load", action="store_true", help=argparse.SUPPRESS)  # the default now
    p.add_argument("--no-compress", action="store_true", help="keep uncompressed textures uncompressed (they are DXT compressed by default)")
    p.add_argument("--no-mips", action="store_true", help="drop all mip levels (saves ~25%% memory, textures shimmer at distance)")
    p.add_argument("--allow-unverified", action="store_true", help="also convert assets whose console layout is not verified (may crash the game)")
    p.add_argument("--max-loaded-sounds", type=int, default=DEFAULT_MAX_LOADED_SOUNDS, help=f"loaded sounds the map may have: identical ones are shared, then the longest are streamed (default {DEFAULT_MAX_LOADED_SOUNDS}; the console holds 1600 with the game's own; 0: no limit)")
    p.add_argument("--loaded-sound-memory", type=float, default=DEFAULT_LOADED_SOUND_MIB, help=f"memory of the loaded sounds in MiB: beyond it the longest are streamed (default {DEFAULT_LOADED_SOUND_MIB}; CoD Xenon's maps have up to 35; 0: no limit)")
    p.add_argument("--jobs", type=int, default=0, help="sounds encoded / compression threads at a time (default: one per processor)")
    p.set_defaults(func=cmd_convert)

    p = sub.add_parser("menu", help="make the Nazi Zombies map list of CoD Xenon's patch_ui.ff show every map of the usermaps folder (needs the CoD Xe build with the usermaps list)")
    p.add_argument("folder", help="the _codxe\\t4 folder the game reads (with zone\\patch_ui.ff and usermaps)")
    p.add_argument("--rows", type=int, default=13, help="rows the list shows at a time (default 13, as CoD Xenon's)")
    p.add_argument("--no-streams", action="store_true", help="do not check the streamed sounds (.xma) of the maps in usermaps (see the streams command)")
    p.add_argument("--menu-zone", action="append", default=[], metavar="PATH",
                   help="a patch_ui.ff with CoD Xe's own custom maps list (CoD Xenon's 0.3.0, or its folder) to use when the game's has none")
    p.set_defaults(func=cmd_menu)

    p = sub.add_parser("streams", help="rewrite the streamed sounds (.xma) of converted maps in the game's layout: those of older conversions and of CoD Xenon's maps stop after a split second")
    p.add_argument("folders", nargs="+", help="folders searched for .xma files (e.g. the game's _codxe\\t4\\usermaps)")
    p.set_defaults(func=cmd_streams)

    p = sub.add_parser("gui", help="open the converter window")
    p.set_defaults(func=cmd_gui)

    p = sub.add_parser("setup", help="install missing dependencies (Python packages, xma2encode.exe) and check them")
    p.add_argument("--xma2encode", help="xma2encode.exe, a folder or a .zip containing it (default: search this computer)")
    p.add_argument("--no-test", action="store_true", help="do not test the encoder")
    p.set_defaults(func=cmd_setup)

    args = parser.parse_args(argv)
    progress.use_lines(args.progress_lines)
    if args.command in ("convert", "info", "roundtrip") and not args.no_install:
        from . import deps

        if not deps.ensure_python_packages():
            return 1
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
