import sys, struct, collections
sys.path.insert(0, r"C:\Users\Hunter\Downloads\codxe\tools\t4ff")
from t4ff.platforms import x360
from t4ff.fastfile import read_fastfile
from t4ff.zone import Reader, asset_name
from t4ff.commands import find_field
from t4ff import stream as S, xenos
p = x360()
_, _, data = read_fastfile(sys.argv[1])
zone = Reader(p, data).load()
rec = p.record("GfxImage")
flag = find_field(rec, "streaming").offset
streamable = {id(n) for n in S.streamable_images(p, zone)}
owner = S._innermost_assets(zone)
# users of images by kind
users = collections.defaultdict(set)
for node in zone.extra_root.walk():
    for ptr in node.relocs.values():
        t = ptr.target()
        if t is None or t.type.name not in ("Material", "GfxImage"): continue
        h = owner.get(id(node))
        if h is None or h is t: continue
        k = h.type.name + ("." + node.type.name if h.type.name == "GfxWorld" else "")
        users[id(t)].add(k)
cat = collections.Counter(); bytes_ = collections.Counter(); ex = collections.defaultdict(list)
for name, nodes in S._images_by_name(p, zone).items():
    for img in nodes:
        parts = S._image_parts(p, img)
        size = len(parts[2].data) if parts else 0
        if img.data[flag]:
            c = "streams"
        elif id(img) not in streamable:
            mats = [ ]
            kinds = set()
            # material users' kinds
            for node in zone.extra_root.walk():
                pass
            c = "not streamable (users)"
        elif parts is None:
            c = "not 2D / no data"
        else:
            _, _, _, fmt, w, h, lv = parts
            base = xenos.mip_chain_layout(w, h, fmt, lv)[0] if lv else 0
            if lv < 2: c = "single level"
            elif w & (w - 1) or h & (h - 1): c = "not power of two"
            elif base < S.MIN_HIGHMIP_BYTES: c = "top level < 128 KiB"
            else: c = "split refused (layout)"
        cat[c] += 1; bytes_[c] += size; ex[c].append(name)
for c in cat:
    print("%-28s %4d  %7.1f MiB  e.g. %s" % (c, cat[c], bytes_[c] / 1048576, ", ".join(ex[c][:5])))
