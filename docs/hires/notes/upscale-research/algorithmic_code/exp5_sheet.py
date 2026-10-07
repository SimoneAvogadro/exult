import numpy as np
from PIL import Image, ImageDraw
from u7algo import *
w = World(); pal = w.pal
cx, cy, n = 84, 74, 4
Sm, Fm = w.tile_ids(cx-1, cy-1, n+2, n+2); idx_m = w.render(Sm, Fm); idx = idx_m[128:-128,128:-128]
# find 3x3 tile window with most distinct indices incl. cycling + mixed
best=None
for ty in range(0, 62):
    for tx in range(0, 62):
        b = idx[ty*8:ty*8+24, tx*8:tx*8+24]
        sc = len(np.unique(b)) + 40*((b>=0xE0).any()) + 0
        nonzero = (b==0).mean() < 0.05
        if nonzero and (best is None or sc > best[0]): best=(sc,ty,tx)
_,ty,tx = best; print("crop tiles", ty, tx)
def crop(img, f=6): return img[ty*8*f:(ty*8+24)*f, tx*8*f:(tx*8+24)*f]
x6 = xbrz_rgb(pal[idx_m], 6)[768:-768,768:-768]
q,_,_ = nearest_index(x6, pal, np.arange(1,224), "oklab")
ims = {
 "nearest": pal[crop(nearest(idx,6))],
 "xBRZ6 raw RGB": crop(x6),
 "xBRZ6>OKLab(1..223)": pal[crop(q)],
 "xBRZ6>localsnap": pal[crop(local_snap(x6, idx, pal, 6, 1))],
 "scale2x>scale3x": pal[crop(scale3x(scale2x(idx_m))[768:-768,768:-768])],
 "mmpx2x>scale3x": pal[crop(scale3x(mmpx2x_idx(idx_m, pal))[768:-768,768:-768])],
}
W = 144*1+4
sheet = Image.new("RGB", (len(ims)*(W), 144+20), (40,40,40)); d = ImageDraw.Draw(sheet)
for i,(nm,im) in enumerate(ims.items()):
    sheet.paste(Image.fromarray(im), (i*W+2, 18)); d.text((i*W+3, 3), nm, fill=(255,255,255))
sheet = sheet.resize((sheet.width*2, sheet.height*2), Image.NEAREST)
sheet.save("out/A5_sheet.png"); print(sheet.size)
