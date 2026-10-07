import numpy as np, time
from PIL import Image
import PIL
from u7algo import *
w = World(); pal = w.pal
Sm, Fm = w.tile_ids(83, 73, 6, 6); idx_m = w.render(Sm, Fm)
x6 = xbrz_rgb(pal[idx_m], 6)
img = Image.fromarray(x6)
print("Pillow", PIL.__version__)
# A: full 256 palette
pimg = Image.new("P", (1,1)); pimg.putpalette(pal.ravel().tolist())
t=time.time(); qa = np.array(img.quantize(palette=pimg, dither=Image.Dither.NONE)); print("A full pal: idx>=224 share", float((qa>=224).mean()), "idx0 share", float((qa==0).mean()), f"{time.time()-t:.2f}s")
# B: putpalette with only entries 0..223
pimg2 = Image.new("P", (1,1)); pimg2.putpalette(pal[:224].ravel().tolist())
t=time.time(); qb = np.array(img.quantize(palette=pimg2, dither=Image.Dither.NONE)); print("B 224-entry pal: max idx", int(qb.max()), f"{time.time()-t:.2f}s")
# compare B with exact RGB-euclid nearest among 0..223
qe,_,_ = nearest_index(x6, pal, np.arange(0,224), "rgb")
print("B agrees with exact RGB-nearest(0..223) on", float((qb==qe).mean()))
qo,_,_ = nearest_index(x6, pal, np.arange(0,224), "oklab")
print("B agrees with OKLab-nearest(0..223) on", float((qb==qo).mean()))
# C: Floyd-Steinberg
qc = np.array(img.quantize(palette=pimg2, dither=Image.Dither.FLOYDSTEINBERG)); print("C FS dither changes", float((qc!=qb).mean()), "of px")
