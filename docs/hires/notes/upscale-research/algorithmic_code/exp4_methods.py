import numpy as np, time
from PIL import Image, ImageDraw
from u7algo import *
w = World(); pal = w.pal; f = 6
def lab(x): return srgb_to_oklab(x)
def bgr_ratio(rgb, tile=48):
    L = lab(rgb)
    dx = np.sqrt(((L[:, 1:] - L[:, :-1])**2).sum(-1)); dy = np.sqrt(((L[1:] - L[:-1])**2).sum(-1))
    bx = np.zeros(dx.shape[1], bool); bx[tile-1::tile] = True
    by = np.zeros(dy.shape[0], bool); by[tile-1::tile] = True
    return np.concatenate([dx[:, bx].ravel(), dy[by].ravel()]).mean() / np.concatenate([dx[:, ~bx].ravel(), dy[~by].ravel()]).mean()

cx, cy, n = 84, 74, 4
Sm, Fm = w.tile_ids(cx-1, cy-1, n+2, n+2)
idx_m = w.render(Sm, Fm); idx = idx_m[128:-128, 128:-128]
S, F = w.tile_ids(cx, cy, n, n)
def per_tile(fn):
    out = np.zeros((idx.shape[0]*f, idx.shape[1]*f), np.uint8); c = {}
    for y in range(S.shape[0]):
        for x in range(S.shape[1]):
            k = (S[y,x], F[y,x])
            if k not in c: c[k] = fn(np.pad(w.frame(*k), 8, mode="wrap"))[8*f:-8*f, 8*f:-8*f]
            out[y*48:(y+1)*48, x*48:(x+1)*48] = c[k]
    return out
methods = {
  "nearest":          lambda a: nearest(a, 6),
  "scale2x>scale3x":  lambda a: scale3x(scale2x(a)),
  "scale3x>scale2x":  lambda a: scale2x(scale3x(a)),
  "mmpx2x>scale3x":   lambda a: scale3x(mmpx2x_idx(a, pal)),
  "scale3x>mmpx2x":   lambda a: mmpx2x_idx(scale3x(a), pal),
  "xbrz6>localsnap":  lambda a: local_snap(xbrz_rgb(pal[a], 6), a, pal, 6, 1),
}
src_set = set(np.unique(idx_m).tolist())
rows = []
for name, fn in methods.items():
    t0 = time.time(); R = fn(idx_m)[768:-768, 768:-768]; dt = time.time() - t0
    T = per_tile(fn)
    band = np.zeros(R.shape, bool)
    for k in range(0, R.shape[0], 48):
        band[max(k-6,0):k+6, :] = True; band[:, max(k-6,0):k+6] = True
    dE = np.sqrt(((lab(pal[T]) - lab(pal[R]))**2).sum(-1))
    cyc = int((R >= 0xE0).sum()); cyc_nn = int((nearest(idx, 6) >= 0xE0).sum())
    rows.append((name, dt, bgr_ratio(pal[R]), bgr_ratio(pal[T]), float(dE[band].mean()), float(dE[~band].mean()),
                 set(np.unique(R).tolist()) <= src_set, cyc, cyc_nn))
    Image.fromarray(pal[R][:576, :576]).save(f"out/A4_{name.replace('>','_')}.png")
print(f"{'method':18s} {'t(768^2 src)':>12s} {'gradratio region':>16s} {'per-tile(wrap)':>14s} {'dE band':>8s} {'dE inner':>8s} {'idx-preserving':>14s} {'cyc px':>8s} {'cyc NN':>7s}")
for r in rows: print(f"{r[0]:18s} {r[1]:12.2f} {r[2]:16.3f} {r[3]:14.3f} {r[4]:8.4f} {r[5]:8.4f} {str(r[6]):>14s} {r[7]:8d} {r[8]:7d}")
# contact sheet: 12x12 source px crop -> 72x72, magnified x3 for viewing
names = ["source(NN)"] + list(methods)
crop = (3*8, 5*8)  # tile row/col offset
tiles = []
for name in names:
    if name == "source(NN)": im = pal[nearest(idx, 6)]
    else: im = np.array(Image.open(f"out/A4_{name.replace('>','_')}.png"))
    y0, x0 = crop[0]*6, crop[1]*6
    tiles.append(np.repeat(np.repeat(im[y0:y0+144, x0:x0+144], 2, 0), 2, 1))
sheet = Image.new("RGB", (len(tiles)*292, 310), (40,40,40)); d = ImageDraw.Draw(sheet)
for i,(t,nm) in enumerate(zip(tiles, names)):
    sheet.paste(Image.fromarray(t), (i*292+2, 20)); d.text((i*292+4, 4), nm, fill=(255,255,255))
sheet.save("out/A4_sheet.png"); print("sheet", sheet.size)
