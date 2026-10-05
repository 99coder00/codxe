import sys, struct, collections
sys.path.insert(0, r"C:\Users\Hunter\Downloads\codxe\tools\t4ff")
from t4ff.platforms import x360
from t4ff.fastfile import read_fastfile
from t4ff.zone import Reader
from t4ff import stream as S
from t4ff.images import IwdLibrary
from t4ff.commands import find_field
p = x360()
K = r"C:\Users\Hunter\Downloads\Compressed\Call of Duty - World at War (axekin.com).iso\WaW\_codxe\t4\usermaps\kinodertoten"
_, _, data = read_fastfile(K + r"\kinodertoten.ff")
zone = Reader(p, data).load()
pak = S.read_pak(K + r"\images.pak")
own = IwdLibrary([r"C:\Users\Hunter\Downloads\kinodertoten_updated"])
stock = IwdLibrary([r"C:\Users\Hunter\Downloads\Compressed\Call of Duty World at War B252004~AG\Call of Duty World at War"])
rec = p.record("GfxImage")
streamable = {id(n) for n in S.streamable_images(p, zone)}
rows = collections.Counter(); mib = collections.Counter(); ex = collections.defaultdict(list)
for name, nodes in S._images_by_name(p, zone).items():
    n = nodes[0]
    w, h = struct.unpack_from(">HH", n.data, find_field(rec, "width").offset)
    entry = pak.get(name)
    scale = 1 if entry is None else (4 if entry[1] & 1 else 2)
    fw, fh = w * scale, h * scale  # what the game shows close up
    try:
        src = own.image(name)
        kind = "own"
        if src is None:
            src = stock.image(name)
            kind = "stock"
    except Exception:
        src, kind = None, "?"
    if src is None:
        rows["no PC source (console only)"] += 1
        continue
    if src.width * src.height > fw * fh:
        key = f"{kind}: below PC size ({'streamable' if id(n) in streamable else 'not streamable'})"
        rows[key] += 1
        mib[key] += src.width * src.height
        ex[key].append(f"{name} {fw}x{fh}<{src.width}x{src.height}")
    else:
        rows[f"{kind}: at PC size"] += 1
for k in sorted(rows):
    print(f"{rows[k]:5d}  {k}" + (f"  e.g. {', '.join(ex[k][:4])}" if ex[k] else ""))
