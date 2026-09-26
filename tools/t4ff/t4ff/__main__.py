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

    Returns (map name, {role: path}, [iwd files]) where role is 'map', 'mod', 'patch' or 'load'.
    ``mod.ff`` is taken from the same folder, or from ``mods/<map>`` when the map is in
    ``usermaps/<map>`` (where the game keeps them).
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
        base = [f for f in ffs if not f.endswith(("_load.ff", "_patch.ff")) and f != "mod.ff"]
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

    parent = os.path.dirname(os.path.abspath(folder))
    if "mod" not in files and os.path.basename(parent).lower() == "usermaps":
        mod_dir = os.path.join(os.path.dirname(parent), "mods", name)
        if os.path.isfile(os.path.join(mod_dir, "mod.ff")):
            files["mod"] = os.path.join(mod_dir, "mod.ff")
            iwds += sorted(os.path.join(mod_dir, f) for f in os.listdir(mod_dir) if f.lower().endswith(".iwd"))
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

    from .memory import MIB, TEXTURE_CAP_MIB, block_sizes, next_texture_budget, texture_bytes

    auto_budget = args.texture_budget == "auto"
    target = int(args.memory_target * MIB)
    options = ConvertOptions(
        allow_unverified=args.allow_unverified,
        max_texture_size=args.max_texture_size,
        texture_budget=TEXTURE_CAP_MIB * MIB if auto_budget else int(float(args.texture_budget) * MIB),
        keep_mips=not args.no_mips,
        compress_textures=not args.no_compress,
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
    paths = [files["map"]]
    if "patch" in files and not args.no_patch:
        paths.append(files["patch"])
    if "mod" in files and not args.no_mod:
        paths.append(files["mod"])
    map_files = IwdLibrary(list(dict.fromkeys([os.path.dirname(os.path.abspath(files["map"]))] + [os.path.dirname(os.path.abspath(p)) for p in iwds])))

    # The automatic texture budget: the textures get what the memory target leaves, measured on the
    # converted map (converted again with less when it is over; sounds are encoded once).
    import dataclasses

    for attempt in range(4):
        main_zone, planned = _convert_map(args, paths, options, map_files, out_dir)
        progress.step("Measuring the memory")
        out = Writer(x360()).write(main_zone)
        total = sum(block_sizes(out))
        if not auto_budget:
            break
        textures = texture_bytes(main_zone)
        budget = next_texture_budget(total, target, planned, options.texture_budget) if attempt < 3 else None
        if budget is None:
            if total > target:
                print(
                    f"warning: the map needs {total / MIB:.1f} MiB, over the {target / MIB:.0f} MiB target "
                    f"({(total - textures) / MIB:.1f} MiB without its textures); CoD Xenon's largest map needs 219.5 MiB"
                )
            else:
                print(f"memory: {total / MIB:.1f} MiB of the {target / MIB:.0f} MiB target, {textures / MIB:.1f} MiB of it textures")
            break
        print(f"memory: {total / MIB:.1f} MiB, over the {target / MIB:.0f} MiB target: converting again with {budget / MIB:.1f} MiB of textures")
        options = dataclasses.replace(options, texture_budget=budget)
    write_zone(main_zone, os.path.join(out_dir, f"{name}.ff"), args.jobs, out)

    # the map's name in the Nazi Zombies map list (CoD Xe reads the first line of description.txt)
    from .loadscreen import map_title

    title = args.name.strip() or map_title(map_files, name)
    description = os.path.join(out_dir, "description.txt")
    if args.name.strip() or not os.path.exists(description):
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
            done = write_load_zone(name, out_dir, library_files(args.console_zone, name), map_files, files.get("load"), args.loading_image, args.jobs, title=title)
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
    return 0


def _convert_map(args, paths, options, map_files, out_dir):
    """The map's fastfiles converted and merged into one console zone."""
    from .images import IwdLibrary
    from .merge import merge_zones, prune_references

    convs = converters(paths, options)
    # the map's own loose scripts win over those of its fastfiles, as on PC
    from .scripts import missing_scripts_zone, override_scripts

    override_scripts(pc(), [c.zone for c in convs], map_files)
    zones = [run_converter(path, conv) for path, conv in zip(paths, convs)]
    if len(zones) > 1:
        progress.step("Merging into one fastfile")
    main_zone = zones[0] if len(zones) == 1 else merge_zones(x360(), zones)
    progress.step("Checking the scripts")
    extra = missing_scripts_zone(x360(), main_zone, [map_files, IwdLibrary(args.iwd)], convs[0].console_library)
    if extra is not None:
        main_zone = merge_zones(x360(), [main_zone, extra], log=lambda msg: None)
    # assets the game looks up by name (player body animations, shellshock files) no zone has
    from .named import named_assets_zone

    extra = named_assets_zone(x360(), main_zone, [map_files, IwdLibrary(args.iwd)], convs[0].console_library)
    if extra is not None:
        main_zone = merge_zones(x360(), [main_zone, extra], log=lambda msg: None)
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
    return main_zone, getattr(convs[0], "planned_texture_bytes", 0)


def cmd_menu(args):
    """Make the Nazi Zombies map list of CoD Xenon's patch_ui.ff dynamic (see menu.py)."""
    import shutil

    from .menu import MenuError, description_text, localized_strings, make_dynamic

    root = args.folder
    zone_dir = os.path.join(root, "zone") if os.path.isdir(os.path.join(root, "zone")) else root
    target = os.path.join(zone_dir, "patch_ui.ff")
    original = target + ".orig"
    if not os.path.exists(original):
        if not os.path.exists(target):
            print(f"error: {target} not found (give CoD Xenon's _codxe\\t4 folder)")
            return 1
        shutil.copyfile(target, original)
        print(f"kept CoD Xenon's menu as {original}")
    endian, _, data = read_fastfile(original)
    zone = Reader(x360(), data).load()
    try:
        rows = make_dynamic(x360(), zone, args.rows)
    except MenuError as e:
        print(f"error: {e}")
        return 1
    write_zone(zone, target)
    print(f"{target}: the map list shows the maps of the usermaps folder, {args.rows} at a time (LB / RB: a page)")

    # names, descriptions and pictures of CoD Xenon's maps, whose rows the list replaces
    usermaps = os.path.join(root, "usermaps")
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
    p.add_argument("--texture-budget", type=_texture_budget, default="auto", help="texture memory budget in MiB, 0 for no limit, or auto (default): what the memory target leaves, at most 96 MiB")
    p.add_argument("--memory-target", type=float, default=MEMORY_TARGET_MIB, help=f"memory the map may use in MiB, for the automatic texture budget (default {MEMORY_TARGET_MIB}; CoD Xenon's maps use 148 to 220)")
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
    p.set_defaults(func=cmd_menu)

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
