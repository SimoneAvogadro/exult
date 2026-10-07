import numpy as np, collections
from u7algo import *
w = World()
pal = w.pal
# palette duplicates
cols = collections.defaultdict(list)
for i in range(256): cols[tuple(pal[i])].append(i)
dups = {k:v for k,v in cols.items() if len(v)>1}
print("palette0 entries with duplicate RGB:", len(dups), "groups; examples:", list(dups.values())[:12])
print("dups involving static(1..223) only:", [v for v in dups.values() if all(1<=i<=223 for i in v)][:20])
print("dups static vs cycling:", [v for v in dups.values() if any(i>=224 for i in v) and any(i<224 for i in v)][:20])
print("n flats frames:", len(w.flats), "shapes:", len({s for s,_ in w.flats}))
# map usage
used = collections.Counter()
used_chunks = np.unique(w.tmap)
print("chunk terrains used by map:", len(used_chunks), "of", w.cshp.shape[0])
for t in used_chunks:
    for s,f in zip(w.cshp[t].ravel(), w.cfrm[t].ravel()):
        used[(int(s),int(f))]+=1
flat_used = {k:v for k,v in used.items() if k[0]<150}
print("distinct flat (shape,frame) used in chunk terrains:", len(flat_used), " RLE/other ids:", len(used)-len(flat_used))
# contexts: whole world tile grid
S = np.zeros((192*16,192*16),np.int32); F=np.zeros_like(S)
for cy in range(192):
    for cx in range(192):
        t=w.tmap[cy,cx]; S[cy*16:cy*16+16,cx*16:cx*16+16]=w.cshp[t]; F[cy*16:cy*16+16,cx*16:cx*16+16]=w.cfrm[t]
ID = S*32+F
np.save("world_ids.npy", ID)
# 4-neighbour context per tile
p = np.pad(ID,1,mode='wrap')
ctx = collections.defaultdict(set)
N,Sx,E,Wd = p[:-2,1:-1],p[2:,1:-1],p[1:-1,2:],p[1:-1,:-2]
idf = ID.ravel(); keys = np.stack([N.ravel(),Sx.ravel(),E.ravel(),Wd.ravel()],1)
# count distinct 4-neighbour contexts per id via unique rows
comb = np.concatenate([idf[:,None], keys],1)
u = np.unique(comb, axis=0)
cnt = collections.Counter(u[:,0])
flat_ids = [k for k in cnt if (k//32)<150]
vals = np.array([cnt[k] for k in flat_ids])
print("flat ids in world:", len(flat_ids), "4-neighbour contexts per flat: median", np.median(vals), "p90", np.percentile(vals,90), "max", vals.max())
# how many flat ids always have the same neighbour type as itself on all 4 sides (self-tiling)
inst = collections.Counter(idf)
print("total tile instances:", len(idf), "flat instances:", sum(v for k,v in inst.items() if k//32<150))
# fraction of flats that contain cycling indices
cyc = [k for k,v in w.flats.items() if (v>=0xE0).any()]
print("flat frames containing E0..FE:", len(cyc), "e.g.", cyc[:10])
print("cycling pixel share among water-ish frames:", {k: round(float((w.flats[k]>=0xE0).mean()),3) for k in cyc[:8]})
