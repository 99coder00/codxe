"""The Python t4ff's side of the native port's checks.

    python native/tools/py_reference.py dump <fastfile> <out.txt>      the node tree, as t4ff-cli dump
    python native/tools/py_reference.py rewrite <fastfile> <out.zone>  the zone written back, as t4ff-cli rewrite
    python native/tools/py_reference.py textures <out.txt> <iwd or folder>...  texture battery, as t4ff-cli textures

Run from tools/t4ff. The outputs of both programs must be byte identical.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

from t4ff.fastfile import read_fastfile  # noqa: E402
from t4ff.platforms import for_endian  # noqa: E402
from t4ff.zone import Reader, Writer  # noqa: E402


def load(path):
    endian, _, data = read_fastfile(path)
    platform = for_endian(endian)
    return platform, data, Reader(platform, data).load()


def opt(v):
    return "-" if v is None else str(v)


def origin_text(node):
    o = node.extra.get("origin")
    if o is None:
        return "-"
    if o[0] == "asset":
        return f"asset:{o[1]}"
    if o[0] in ("member", "ptrarray"):
        return f"{o[0]}:{o[1]}.{o[2]}"
    return f"ptrelem:{o[1]}"


def dump(path, out_path):
    platform, data, zone = load(path)
    nodes = list(zone.extra_root.walk())
    ids = {id(n): i for i, n in enumerate(nodes)}

    def nid(n):
        return "None" if n is None else f"N{ids[id(n)]}"

    out = [
        f"zone {zone.platform} size={zone.size} external={zone.external_size} blocks={','.join(str(b) for b in zone.block_sizes)}\n"
    ]
    for i, s in enumerate(zone.script_strings):
        out.append(f"string {i} {'(null)' if s is None else s}\n")
    for i, a in enumerate(zone.assets):
        out.append(f"asset {i} {a.type} {a.name}\n")
    for n in nodes:
        flags = ("s" if n.string else "") + ("i" if n.insert else "") + ("d" if n.extra.get("delayed") else "")
        what = "string" if n.string else repr(n.type)
        out.append(
            f"{nid(n)} {what} x{n.count} blk={n.block} off={n.offset} size={len(n.data)} rt={n.runtime_size} "
            f"align={opt(n.extra.get('align'))} pb={opt(n.push_before)} pa={opt(n.push_after)} flags={flags or '-'} "
            f"asset={n.asset or '-'} origin={origin_text(n)} children={len(n.children)}\n"
        )
        for t, count, size, partial in n.segments:
            out.append(f" seg {t!r} {count} {size} {1 if partial else 0}\n")
        for off, ptr in n.relocs.items():
            line = f" ptr {off} addr={opt(ptr.addr)} "
            if ptr.kind == "null":
                line += "null"
            elif ptr.kind == "follow":
                line += f"follow {nid(ptr.node)}"
            elif ptr.kind == "insert":
                line += f"insert {nid(ptr.node)} ia={opt(ptr.insert_addr)}"
            elif ptr.kind == "ref":
                line += f"ref {nid(ptr.node)} {ptr.index} {ptr.inner}"
            else:
                line += f"alias {nid(ptr.slot.owner)}@{ptr.slot.offset} {ptr.index}"
            out.append(line + "\n")
    with open(out_path, "wb") as f:
        f.write("".join(out).encode("latin-1"))
    print(f"{out_path}: {len(nodes)} nodes")


def rewrite(path, out_path):
    platform, data, zone = load(path)
    with open(out_path, "wb") as f:
        f.write(Writer(platform).write(zone))


def crc_text(parts):
    import zlib

    crc, size = 0, 0
    for p in parts:
        crc = zlib.crc32(p, crc)
        size += len(p)
    return f"{crc:08x}/{size}"


def texture_report(image, label, options, with_mips_check):
    from t4ff import images as img
    from t4ff import xenos
    from t4ff.stream import with_mips

    line = f"{image.name} {label} "
    try:
        tex = img.build_console_texture(image, **options)
        line += (f"{tex.format.name} {tex.width}x{tex.height} levels={tex.levels} drop={tex.dropped_levels} faces={tex.faces} "
                 f"base={tex.base_size} header={crc_text([tex.header])} pixels={crc_text([tex.pixels])}")
        if tex.levels > 1 or tex.faces > 1:
            linear = xenos.untile_mip_chain(tex.pixels, tex.width, tex.height, tex.format, tex.levels, tex.faces)
        else:
            linear = [xenos.untile_level(tex.pixels, tex.width, tex.height, 0, tex.format)]
        line += f" untile={crc_text(linear)}"
        if with_mips_check:
            line += " withmips="
            if tex.levels == 1 and tex.faces == 1:
                m = with_mips(tex.pixels, tex.width, tex.height, tex.format, options.get("mip_tail", True))
                line += f"{crc_text([m[0]])}:{m[1]}" if m else "none"
            else:
                line += "-"
    except Exception:
        line += "error"
    try:
        line += f" size={img.console_texture_size(image, **options)}/{img.console_texture_size(image, make_mips=True, **options)}"
    except Exception:
        line += " size=error"
    return line


def textures(out_path, *paths):
    from t4ff.images import IwdLibrary

    library = IwdLibrary(list(paths))
    names = sorted(n[7:-4] for n in library.names("images/") if n.endswith(".iwi") and len(n) > 11)
    print(f"{len(names)} images")
    out = []
    battery = [
        ("A", {}, False),
        ("B", {"drop_levels": 2, "normal_map": True, "mip_tail": False}, False),
        ("C", {"max_size": 64, "keep_mips": False, "compress": False}, True),
        ("D", {"drop_levels": 20}, False),
    ]
    for i, name in enumerate(names):
        try:
            image = library.image(name)
        except Exception:
            image = None
        if image is None:
            out.append(f"{name} parse error\n")
            continue
        out.append(f"{name} parse {image.format} {image.width}x{image.height} faces={image.faces} flags={image.flags} "
                   f"levels={len(image.levels)} crc={crc_text(image.levels)}\n")
        for label, options, check in battery:
            out.append(texture_report(image, label, options, check) + "\n")
        if (i + 1) % 200 == 0:
            print(f"  {i + 1}/{len(names)}", flush=True)
    with open(out_path, "wb") as f:
        f.write("".join(out).encode("latin-1"))


if __name__ == "__main__":
    commands = {"dump": dump, "rewrite": rewrite, "textures": textures}
    if len(sys.argv) < 4 or sys.argv[1] not in commands or (sys.argv[1] != "textures" and len(sys.argv) != 4):
        sys.exit(__doc__)
    commands[sys.argv[1]](*sys.argv[2:])
