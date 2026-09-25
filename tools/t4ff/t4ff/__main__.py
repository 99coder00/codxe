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
from .soundbudget import DEFAULT_MAX_LOADED_SOUNDS
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


def write_zone(zone, target: str, jobs: int = 0):
    progress.step(f"Writing {os.path.basename(target)}")
    out = Writer(x360()).write(zone)
    write_fastfile(target, ">", out, jobs=jobs)
    # reading the zone back checks it with the console loading rules
    progress.step(f"Checking {os.path.basename(target)}")
    sizes = Reader(x360(), out).load().block_sizes
    blocks = ", ".join(f"{n.split('_BLOCK_')[1].lower()} {s / 1048576:.1f}" for n, s in zip(BLOCK_NAMES, sizes) if s)
    print(f"wrote {target} ({os.path.getsize(target) / 1048576:.1f} MiB compressed)")
    print(f"  memory: {sum(sizes) / 1048576:.1f} MiB ({blocks})")


def cmd_convert(args):
    from .audio import XmaEncoder, convert_streamed_sounds
    from .convert import ConvertOptions
    from .images import IwdLibrary
    from .merge import merge_zones, prune_references

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

    options = ConvertOptions(
        allow_unverified=args.allow_unverified,
        max_texture_size=args.max_texture_size,
        texture_budget=int(args.texture_budget * 1024 * 1024),
        keep_mips=not args.no_mips,
        compress_textures=not args.no_compress,
        iwd_paths=iwds + args.iwd,
        xma_encoder=None if args.no_sounds else encoder,
        sound_rate=args.sound_rate,
        mono_sounds=args.mono_sounds,
        sounds_dir=out_dir,
        console_zones=args.console_zone,
        jobs=args.jobs,
    )

    # One fastfile, as in CoD Xenon's converted maps: the console has no mod.ff, and <map>_patch.ff is
    # merged too. Scripts of later zones win (patch over map, mod over both). One texture budget.
    paths = [files["map"]]
    if "patch" in files and not args.no_patch:
        paths.append(files["patch"])
    if "mod" in files and not args.no_mod:
        paths.append(files["mod"])
    convs = converters(paths, options)
    # the map's own loose scripts win over those of its fastfiles, as on PC
    from .scripts import missing_scripts_zone, override_scripts

    map_files = IwdLibrary(list(dict.fromkeys([os.path.dirname(os.path.abspath(files["map"]))] + [os.path.dirname(os.path.abspath(p)) for p in iwds])))
    override_scripts(pc(), [c.zone for c in convs], map_files)
    zones = [run_converter(path, conv) for path, conv in zip(paths, convs)]
    if len(zones) > 1:
        progress.step("Merging into one fastfile")
    main_zone = zones[0] if len(zones) == 1 else merge_zones(x360(), zones)
    progress.step("Checking the scripts")
    extra = missing_scripts_zone(x360(), main_zone, [map_files, IwdLibrary(args.iwd)], convs[0].console_library)
    if extra is not None:
        main_zone = merge_zones(x360(), [main_zone, extra], log=lambda msg: None)
    prune_references(x360(), main_zone)
    if args.max_loaded_sounds:
        from .audio import LoadedXma
        from .soundbudget import limit_loaded_sounds

        progress.step("Checking the loaded sound limit")
        streams = {key[0].lower(): xma.stream for key, xma in options.sound_cache.items() if isinstance(xma, LoadedXma) and xma.stream is not None}
        limit_loaded_sounds(x360(), main_zone, args.max_loaded_sounds, streams, out_dir)
    from .soundbudget import sync_alias_types

    fixed = sync_alias_types(x360(), main_zone)
    if fixed:
        print(f"sound aliases: the type in the flags of {fixed} aliases set to their sound file's")
    write_zone(main_zone, os.path.join(out_dir, f"{name}.ff"), args.jobs)

    if "load" in files and args.load_zone:
        # CoD Xe serves <map>_load.ff as the loading screen zone (CoD Xenon's 0.2.0 maps have one)
        import dataclasses

        zone = convert_fastfile(files["load"], dataclasses.replace(options, reference_techsets=True))
        prune_references(x360(), zone)
        write_zone(zone, os.path.join(out_dir, os.path.basename(files["load"])), args.jobs)
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
    p.add_argument("--texture-budget", type=float, default=0, help="texture memory budget in MiB (default: no limit)")
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
    p.add_argument("--t4-layout", action="store_true", help="write _codxe/t4/usermaps/<map> (CoD Xe's newer layout: use it when the console has a _codxe/t4 folder, e.g. from CoD Xenon's 0.2.0 maps; CoD Xe then ignores _codxe/usermaps)")
    p.add_argument("--load-zone", action="store_true", help="also convert <map>_load.ff (loading screen; experimental, CoD Xenon's maps have none)")
    p.add_argument("--no-load", action="store_true", help=argparse.SUPPRESS)  # the default now
    p.add_argument("--no-compress", action="store_true", help="keep uncompressed textures uncompressed (they are DXT compressed by default)")
    p.add_argument("--no-mips", action="store_true", help="drop all mip levels (saves ~25%% memory, textures shimmer at distance)")
    p.add_argument("--allow-unverified", action="store_true", help="also convert assets whose console layout is not verified (may crash the game)")
    p.add_argument("--max-loaded-sounds", type=int, default=DEFAULT_MAX_LOADED_SOUNDS, help=f"loaded sounds the map may have: identical ones are shared, then the longest are streamed (default {DEFAULT_MAX_LOADED_SOUNDS}; the console holds 1600 with the game's own; 0: no limit)")
    p.add_argument("--jobs", type=int, default=0, help="sounds encoded / compression threads at a time (default: one per processor)")
    p.set_defaults(func=cmd_convert)

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
