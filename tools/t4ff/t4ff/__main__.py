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
    """Return (map name, [fastfiles in merge priority order], [iwd files]) for a PC usermap folder or .ff."""
    if os.path.isfile(path):
        folder = os.path.dirname(os.path.abspath(path))
        name = os.path.splitext(os.path.basename(path))[0]
        files = [path]
    else:
        folder = path
        ffs = [f for f in os.listdir(folder) if f.lower().endswith(".ff")]
        base = [f for f in ffs if not f.lower().endswith(("_load.ff", "_patch.ff")) and f.lower() != "mod.ff"]
        if len(base) != 1:
            raise SystemExit(f"{folder}: expected exactly one map fastfile, found {base}")
        name = os.path.splitext(base[0])[0]
        files = [os.path.join(folder, base[0])]
    iwds = sorted(os.path.join(folder, f) for f in os.listdir(folder) if f.lower().endswith(".iwd"))
    return name, files, iwds


def cmd_convert(args):
    from .convert import ConvertOptions, ZoneConverter

    name, files, iwds = find_usermap(args.input)
    out_dir = os.path.join(args.output, "_codxe", "usermaps", name)
    os.makedirs(out_dir, exist_ok=True)

    options = ConvertOptions(
        allow_unverified=args.allow_unverified,
        max_texture_size=args.max_texture_size,
        texture_budget=int(args.texture_budget * 1024 * 1024),
        keep_mips=not args.no_mips,
        iwd_paths=iwds + args.iwd,
    )

    if not args.no_sounds:
        from .audio import XmaEncoder, convert_streamed_sounds
        from .images import IwdLibrary

        library = IwdLibrary(iwds)
        encoder = XmaEncoder(args.xma_encoder, args.xma_quality)
        stats = convert_streamed_sounds(library, out_dir, encoder, args.stream_rate, args.mono_streams)
        if stats["sounds"]:
            print(
                f"streamed sounds: {stats['converted']}/{stats['sounds']} converted, "
                f"{stats['input_bytes'] / 1048576:.1f} MiB -> {stats['output_bytes'] / 1048576:.1f} MiB"
            )

    for path in files:
        start = time.time()
        endian, _, data = read_fastfile(path)
        if endian != "<":
            raise SystemExit(f"{path}: not a PC fastfile")
        zone = Reader(pc(), data).load()
        print(f"{path}: {len(zone.assets)} assets")
        converter = ZoneConverter(zone, pc(), x360(), options)
        result = converter.convert()
        out = Writer(x360()).write(result)
        target = os.path.join(out_dir, os.path.basename(path))
        write_fastfile(target, ">", out)
        stats = converter.stats
        print(f"  converted:  {dict(stats.converted)}")
        print(f"  referenced: {dict(stats.referenced)}")
        print(f"  textures:   {stats.texture_bytes / 1048576:.1f} MiB")
        print(f"  wrote {target} ({os.path.getsize(target):,} bytes) in {time.time() - start:.1f}s")
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
    p.add_argument("--no-sounds", action="store_true", help="do not convert streamed sounds")
    p.add_argument("--no-mips", action="store_true", help="drop all mip levels (saves ~25%% memory, textures shimmer at distance)")
    p.add_argument("--allow-unverified", action="store_true", help="also convert assets whose console layout is not verified (may crash the game)")
    p.set_defaults(func=cmd_convert)

    args = parser.parse_args(argv)
    return args.func(args) or 0


if __name__ == "__main__":
    sys.exit(main())
