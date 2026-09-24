"""Command line interface.

    python -m t4ff info <fastfile>
    python -m t4ff roundtrip <fastfile>...
    python -m t4ff convert <pc fastfile or usermap folder> -o <output folder> [options]
"""

from __future__ import annotations

import argparse
import collections
import os
import sys
import time

from .fastfile import read_fastfile, write_fastfile
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
    """Locate the fastfiles of a PC usermap.

    Returns (map name, {role: path}, [iwd files]) where role is 'map', 'mod', 'patch' or 'load'.
    """
    if os.path.isfile(path):
        folder = os.path.dirname(os.path.abspath(path))
        name = os.path.splitext(os.path.basename(path))[0]
        return name, {"map": path}, sorted(os.path.join(folder, f) for f in os.listdir(folder) if f.lower().endswith(".iwd"))

    folder = path
    ffs = {f.lower(): os.path.join(folder, f) for f in os.listdir(folder) if f.lower().endswith(".ff")}
    base = [f for f in ffs if not f.endswith(("_load.ff", "_patch.ff")) and f != "mod.ff"]
    if len(base) != 1:
        raise SystemExit(f"{folder}: expected exactly one map fastfile, found {sorted(base)}")
    name = os.path.splitext(os.path.basename(ffs[base[0]]))[0]
    files = {"map": ffs[base[0]]}
    for role, fname in (("mod", "mod.ff"), ("patch", f"{name.lower()}_patch.ff"), ("load", f"{name.lower()}_load.ff")):
        if fname in ffs:
            files[role] = ffs[fname]
    iwds = sorted(os.path.join(folder, f) for f in os.listdir(folder) if f.lower().endswith(".iwd"))
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

    convs = [ZoneConverter(load_pc_zone(path), pc(), x360(), options) for path in paths]
    plan_textures_shared(convs)
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


def write_zone(zone, target: str):
    out = Writer(x360()).write(zone)
    write_fastfile(target, ">", out)
    # reading the zone back checks it with the console loading rules
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
    out_dir = os.path.join(args.output, "_codxe", "usermaps", name)
    os.makedirs(out_dir, exist_ok=True)

    encoder = XmaEncoder(args.xma_encoder, args.xma_quality)
    if not encoder.available and not args.no_sounds:
        print("warning: xma2encode.exe not found (--xma-encoder): sounds are not converted, the map will reference console sounds")

    if not args.no_sounds and encoder.available:
        library = IwdLibrary(iwds)
        stats = convert_streamed_sounds(library, out_dir, encoder, args.stream_rate, args.mono_streams)
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
    )

    # The console has no mod.ff: the mod's assets are merged into the map zone (one texture budget).
    paths = [files["map"]] + ([files["mod"]] if "mod" in files and not args.no_mod else [])
    zones = [run_converter(path, conv) for path, conv in zip(paths, converters(paths, options))]
    main_zone = zones[0] if len(zones) == 1 else merge_zones(x360(), zones)
    prune_references(x360(), main_zone)
    write_zone(main_zone, os.path.join(out_dir, f"{name}.ff"))

    for role in ("patch", "load"):
        if role in files and not (role == "load" and args.no_load):
            zone = convert_fastfile(files[role], options)
            prune_references(x360(), zone)
            write_zone(zone, os.path.join(out_dir, os.path.basename(files[role])))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(prog="t4ff", description="World at War fastfile tools (PC -> Xbox 360 conversion for CoD Xe)")
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
    p.add_argument("--no-load", action="store_true", help="do not convert <map>_load.ff")
    p.add_argument("--no-compress", action="store_true", help="keep uncompressed textures uncompressed (they are DXT compressed by default)")
    p.add_argument("--no-mips", action="store_true", help="drop all mip levels (saves ~25%% memory, textures shimmer at distance)")
    p.add_argument("--allow-unverified", action="store_true", help="also convert assets whose console layout is not verified (may crash the game)")
    p.set_defaults(func=cmd_convert)

    args = parser.parse_args(argv)
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
