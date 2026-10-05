import sys
sys.path.insert(0, r"C:\Users\Hunter\Downloads\codxe\tools\t4ff")
from t4ff.platforms import x360
from t4ff.fastfile import read_fastfile
from t4ff.zone import Reader
from t4ff import stream as S, xenos
p = x360()
_, _, data = read_fastfile(sys.argv[1])
zone = Reader(p, data).load()
al = lambda n: (n + 4095) & ~4095
n = saved = saved32 = total = 0
for name, nodes in S._images_by_name(p, zone).items():
    for img in nodes:
        parts = S._image_parts(p, img)
        if parts is None: continue
        _, _, pixels, fmt, w, h, lv = parts
        full = xenos.mip_chain_layout(w, h, fmt, lv)[2]
        packed = xenos.packed_mip_level(w, h)
        total += al(len(pixels.data))
        if packed and lv > packed:
            n += 1
            saved += al(full) - al(xenos.mip_chain_layout(w, h, fmt, packed)[2])
        # also dropping levels below 64 (32x32 and the tail)
        cut = max(1, (min(w, h).bit_length() - 1) - 5)  # keep levels down to 64
        if lv > cut + 1:
            saved32 += al(full) - al(xenos.mip_chain_layout(w, h, fmt, cut + 1)[2])
print("images with a packed tail: %d; dropping it saves %.2f MiB of %.1f MiB of pixels; down to 64x64: %.2f MiB" % (n, saved / 1048576, total / 1048576, saved32 / 1048576))
