import sys, struct, numpy as np
sys.path.insert(0, r"C:\Users\Hunter\Downloads\codxe\tools\t4ff")
from t4ff.platforms import x360
from t4ff.fastfile import read_fastfile
from t4ff.zone import Reader, asset_name
from t4ff import stream as S
p = x360(); E = ">"
_, _, data = read_fastfile(sys.argv[1])
zone = Reader(p, data).load()
texels = S._MaterialTexels(p)
# models
diffs = []
bo = S._offset(p, "XModel", "streamInfo.highMipBounds")
for m in zone.extra_root.walk():
    if m.type.name != "XModel" or (m.extra.get("origin") or ("",))[0] != "asset": continue
    node = S._target(m, bo)
    if node is None: continue
    mine = S.model_stream_bounds(p, m, texels)
    for k, box in enumerate(mine or []):
        disc = struct.unpack_from(">6f", node.data, k * 24)
        if not S._box_streams(disc) or not S._box_streams(box): 
            diffs.append(("empty-mismatch" if S._box_streams(disc) != S._box_streams(box) else "both-empty", asset_name(p, m), k)); continue
        d = max(abs(a - b) for a, b in zip(disc, box))
        g = sum(abs(a - b) for a, b in zip(disc, box)) / 6
        diffs.append((d, asset_name(p, m), k))
num = [d for d in diffs if not isinstance(d[0], str)]
print("models: surfaces compared", len(num), "empty mismatch", sum(1 for d in diffs if d[0] == "empty-mismatch"), "both empty", sum(1 for d in diffs if d[0] == "both-empty"))
arr = np.array([d[0] for d in num])
print("  max abs diff percentiles 50/75/90/99:", np.percentile(arr, [50, 75, 90, 99]).round(1))
for d in [d for d in diffs if d[0] == "empty-mismatch"][:8]: print("   ", d)
# world
world = next(n for n in zone.extra_root.walk() if n.type.name == "GfxWorld" and (n.extra.get("origin") or ("",))[0] == "asset")
before = {}
surfs = S._target(world, S._offset(p, "GfxWorld", "dpvs.surfaces"))
nsurf = struct.unpack_from(">i", world.data, 56)[0]
disc = [struct.unpack_from(">6f", surfs.data, i * 76 + 16) for i in range(nsurf)]
S.write_stream_bounds(p, zone)
mine = [struct.unpack_from(">6f", surfs.data, i * 76 + 16) for i in range(nsurf)]
wd = []
for i in range(nsurf):
    if S._box_streams(mine[i]) and S._box_streams(disc[i]):
        wd.append(max(abs(a - b) for a, b in zip(disc[i], mine[i])))
print("world: surfaces compared", len(wd), "percentiles 50/75/90/99/max:", np.percentile(wd, [50, 75, 90, 99, 100]).round(2))
