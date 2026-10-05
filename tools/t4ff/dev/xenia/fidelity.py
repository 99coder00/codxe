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
SEM = find_field(rec, "semantic").offset
cat = collections.Counter(); area = collections.Counter()
for name, nodes in S._images_by_name(p, zone).items():
    n = nodes[0]
    w, h = struct.unpack_from(">HH", n.data, find_field(rec, "width").offset)
    entry = pak.get(name)
    scale = 1 if entry is None else (4 if entry[1] & 1 else 2)
    fw, fh = w * scale, h * scale
    try:
        src = own.image(name) or stock.image(name)
    except Exception:
        src = None
    if src is None:
        cat["no PC source (console-only)"] += 1
        continue
    texels = src.width * src.height
    if texels > fw * fh:
        c = "below PC resolution"
    elif n.data[SEM] == 5 and src.format in ("DXT5", "A8R8G8B8"):
        c = "PC resolution, normal map repacked to DXN (near-lossless)"
    elif src.format in ("A8R8G8B8", "X8R8G8B8", "R8G8B8") and (src.faces == 1 or max(src.width, src.height) > 64):
        c = "PC resolution, uncompressed source compressed to DXT (lossy)"
    else:
        c = "PC resolution, same pixel data as PC"
    cat[c] += 1; area[c] += texels
total = sum(v for k, v in cat.items() if not k.startswith("no PC"))
tarea = sum(area.values())
for k in sorted(cat, key=lambda k: -cat[k]):
    extra = "" if k.startswith("no PC") else f"  ({100*cat[k]/total:.1f}% of textures, {100*area[k]/tarea:.1f}% of texels)"
    print(f"{cat[k]:5d}  {k}{extra}")
