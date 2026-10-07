import numpy as np
from PIL import Image, ImageDraw
from u7algo import *
w = World(); pal = w.pal
Sm, Fm = w.tile_ids(83, 73, 6, 6); idx_m = w.render(Sm, Fm)
ty, tx = 16+3, 16+5
def crop(img, f=6): return img[ty*8*f:(ty*8+16)*f, tx*8*f:(tx*8+16)*f]
ims = {"nearest": pal[crop(nearest(idx_m, 6))]}
for tol in (10, 30, 60, 120):
    ims[f"xBRZ6 eqTol={tol}"] = crop(xbrz_rgb(pal[idx_m], 6, eq_tol=tol))
ims["xBRZ6 dom=6 steep=4"] = crop(xbrz_rgb(pal[idx_m], 6, dom=6.0, steep=4.0))
W = 96+4
sheet = Image.new("RGB", (len(ims)*W, 96+18), (40,40,40)); d = ImageDraw.Draw(sheet)
for i,(nm,im) in enumerate(ims.items()):
    sheet.paste(Image.fromarray(im), (i*W+2, 16)); d.text((i*W+2, 2), nm, fill=(255,255,255))
sheet = sheet.resize((sheet.width*3, sheet.height*3), Image.NEAREST); sheet.save("out/A6_tol.png"); print(sheet.size)
# unique colours of xBRZ result vs tolerance (whole window)
for tol in (10,30,60,120):
    x = xbrz_rgb(pal[idx_m], 6, eq_tol=tol); pk=(x[...,0].astype(np.uint32)<<16)|(x[...,1].astype(np.uint32)<<8)|x[...,2]
    print(tol, "unique", len(np.unique(pk)))
