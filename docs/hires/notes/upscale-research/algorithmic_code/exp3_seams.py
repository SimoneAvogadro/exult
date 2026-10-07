import numpy as np, time, collections, sys
from PIL import Image
from u7algo import *
w = World(); pal = w.pal
f = 6
STATIC_IDX = np.arange(1, 224)

def lab(img_rgb): return srgb_to_oklab(img_rgb)

def seam_excess(rgb, tile=48):
    """mean OKLab dE across tile boundaries divided by mean dE across non-boundary neighbour pairs."""
    L = lab(rgb)
    dx = np.sqrt(((L[:, 1:] - L[:, :-1])**2).sum(-1))
    dy = np.sqrt(((L[1:, :] - L[:-1, :])**2).sum(-1))
    bx = np.zeros(dx.shape[1], bool); bx[tile-1::tile] = True
    by = np.zeros(dy.shape[0], bool); by[tile-1::tile] = True
    b = np.concatenate([dx[:, bx].ravel(), dy[by, :].ravel()])
    nb = np.concatenate([dx[:, ~bx].ravel(), dy[~by, :].ravel()])
    return b.mean() / nb.mean()

def per_tile(S, F, fn):
    H, W = S.shape
    out = None
    cache = {}
    for y in range(H):
        for x in range(W):
            k = (S[y, x], F[y, x])
            if k not in cache:
                cache[k] = fn(w.frame(*k))
            t = cache[k]
            if out is None:
                out = np.zeros((H*t.shape[0], W*t.shape[1]) + t.shape[2:], t.dtype)
            out[y*t.shape[0]:(y+1)*t.shape[0], x*t.shape[1]:(x+1)*t.shape[1]] = t
    return out

def xb_clamp(t8): return xbrz_rgb(pal[t8], f)
def xb_wrap(t8):
    p = np.pad(t8, 2, mode="wrap"); return xbrz_rgb(pal[p], f)[2*f:-2*f, 2*f:-2*f]

def run(cx, cy, n=4, tag=""):
    S, F = w.tile_ids(cx, cy, n, n)
    # 1-tile margin of real context around the window so region result is "true context"
    Sm, Fm = w.tile_ids(cx-1, cy-1, n+2, n+2)
    idx_m = w.render(Sm, Fm)
    idx = idx_m[128:-128, 128:-128]
    t0 = time.time(); region = xbrz_rgb(pal[idx_m], f)[768:-768, 768:-768]; t_region = time.time()-t0
    t0 = time.time(); clamp = per_tile(S, F, xb_clamp); t_clamp = time.time()-t0
    wrap = per_tile(S, F, xb_wrap)
    nn = pal[nearest(idx, f)]
    # consensus: per (shape,frame) per-pixel mode over its instances in region result
    H, W = S.shape
    inst = collections.defaultdict(list)
    for y in range(H):
        for x in range(W):
            inst[(S[y,x],F[y,x])].append(region[y*48:(y+1)*48, x*48:(x+1)*48])
    cons = {}
    var = []
    for k, L in inst.items():
        A = np.stack(L).reshape(len(L), -1, 3)
        packed = (A[...,0].astype(np.uint32)<<16)|(A[...,1].astype(np.uint32)<<8)|A[...,2]
        # per-pixel mode
        m = np.empty(packed.shape[1], np.uint32)
        for p in range(packed.shape[1]):
            v, c = np.unique(packed[:, p], return_counts=True); m[p] = v[c.argmax()]
        cons[k] = np.stack([(m>>16)&255,(m>>8)&255,m&255],-1).astype(np.uint8).reshape(48,48,3)
        if len(L) > 1:
            var.append(float((packed != m[None]).mean()))
    consensus = per_tile(S, F, lambda t: None) if False else None
    out = np.zeros_like(region)
    for y in range(H):
        for x in range(W):
            out[y*48:(y+1)*48, x*48:(x+1)*48] = cons[(S[y,x],F[y,x])]
    consensus = out
    res = {}
    for name, img in [("nearest", nn), ("xbrz_region(context)", region), ("xbrz_tile_clamp", clamp),
                      ("xbrz_tile_wrap", wrap), ("xbrz_consensus_mode", consensus)]:
        diff_vs_region = float(np.sqrt(((lab(img)-lab(region))**2).sum(-1)).mean())
        res[name] = (round(seam_excess(img), 3), round(diff_vs_region, 4))
    print(f"[{tag}] window cx={cx} cy={cy} {n}x{n} chunks; distinct flats={len(inst)}; "
          f"xBRZ6x region {idx_m.shape}->{t_region:.2f}s; per-tile cached {t_clamp:.2f}s")
    print("   instance disagreement with per-frame mode (mean frac of 48x48 px differing, multi-instance frames):",
          round(float(np.mean(var)),3), "median", round(float(np.median(var)),3))
    for k,(se,dv) in res.items(): print(f"   {k:24s} seam_excess={se:6.3f}  meanDE_vs_context={dv:.4f}")
    # Scale2x->Scale3x on index plane
    s6_tile = per_tile(S, F, lambda t: scale3x(scale2x(t, "edge"), "edge"))
    s6_wrap = per_tile(S, F, lambda t: scale3x(scale2x(np.pad(t,1,mode="wrap"))[2:-2,2:-2] if False else t, "edge"))
    s6_reg = scale3x(scale2x(idx_m))[768:-768, 768:-768]
    print(f"   scale2x*3x region: seam_excess={seam_excess(pal[s6_reg]):.3f}  per-tile(edge): {seam_excess(pal[s6_tile]):.3f}  "
          f"uses only source indices: {set(np.unique(s6_reg)) <= set(np.unique(idx_m))}")
    # palette mapping of xBRZ region result
    q_rgb, err, nuniq = nearest_index(region, pal, STATIC_IDX, "oklab")
    q_rgbE, errE, _ = nearest_index(region, pal, STATIC_IDX, "rgb")
    snap = local_snap(region, idx, pal, f, 1)
    cyc_src = int((nearest(idx, f) >= 0xE0).sum())
    print(f"   xBRZ region unique RGB colours: {nuniq}; px exactly in palette: "
          f"{float(np.isin(((region[...,0].astype(np.uint32)<<16)|(region[...,1].astype(np.uint32)<<8)|region[...,2]), ((pal[:,0].astype(np.uint32)<<16)|(pal[:,1].astype(np.uint32)<<8)|pal[:,2])).mean()):.3f}")
    print(f"   remap OKLab-nearest(1..223): mean dE={err.mean():.4f} p99={np.percentile(err,99):.4f}; "
          f"RGB-euclid pick differs from OKLab pick on {float((q_rgb!=q_rgbE).mean()):.3f} of px")
    snap_err = np.sqrt(((lab(pal[snap])-lab(region))**2).sum(-1))
    print(f"   local-snap(3x3 source idx): mean dE={snap_err.mean():.4f}; cycling px NN={cyc_src} snap={int((snap>=0xE0).sum())} "
          f"global-remap={int((q_rgb>=0xE0).sum())}; seam_excess(region snap)={seam_excess(pal[snap]):.3f}")
    Image.fromarray(pal[idx]).save(f"out/{tag}_1x.png")
    for name, img in [("nn", nn), ("xbrz_region", region), ("xbrz_tile_clamp", clamp), ("xbrz_consensus", consensus),
                      ("scale2x3x_region", pal[s6_reg]), ("xbrz_region_oklabq", pal[q_rgb]), ("xbrz_region_localsnap", pal[snap])]:
        Image.fromarray(img[:768, :768]).save(f"out/{tag}_{name}_crop.png")

run(84, 74, 4, "A")
run(146, 44, 4, "B")
