"""The Python t4ff's side of the native port's checks.

    python native/tools/py_reference.py dump <fastfile> <out.txt>      the node tree, as t4ff-cli dump
    python native/tools/py_reference.py rewrite <fastfile> <out.zone>  the zone written back, as t4ff-cli rewrite

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


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] not in ("dump", "rewrite"):
        sys.exit(__doc__)
    {"dump": dump, "rewrite": rewrite}[sys.argv[1]](sys.argv[2], sys.argv[3])
