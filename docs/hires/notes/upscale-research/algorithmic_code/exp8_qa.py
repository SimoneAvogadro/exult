import numpy as np
from u7algo import *
w = World(); pal = w.pal
Sm, Fm = w.tile_ids(83, 73, 6, 6); idx_m = w.render(Sm, Fm)
src = idx_m[128:-128,128:-128]
x6 = xbrz_rgb(pal[idx_m], 6)
meth = {
 "scale2x>scale3x": scale3x(scale2x(idx_m)),
 "mmpx2x>scale3x": scale3x(mmpx2x_idx(idx_m, pal)),
 "xbrz6>localsnap": local_snap(x6, idx_m, pal, 6, 1),
 "xbrz6>oklab(1..223)": nearest_index(x6, pal, np.arange(1,224), "oklab")[0],
}
Ls = srgb_to_oklab(pal)
for nm, hr in meth.items():
    hr = hr[768:-768,768:-768]
    H,W = src.shape
    blocks = hr.reshape(H,6,W,6).transpose(0,2,1,3).reshape(H,W,36)
    # block majority
    maj = np.apply_along_axis(lambda v: np.bincount(v, minlength=256).argmax(), 2, blocks[::4, ::4])
    agree = float((maj == src[::4, ::4]).mean())
    # block mean colour (OKLab) vs source colour
    mean_lab = Ls[blocks].mean(2)
    dE = np.sqrt(((mean_lab - Ls[src])**2).sum(-1))
    print(f"{nm:22s} block-majority==source: {agree:.3f}   block-mean dE: mean {dE.mean():.4f} p99 {np.percentile(dE,99):.4f}")
