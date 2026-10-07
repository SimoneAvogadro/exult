import numpy as np
from PIL import Image, ImageDraw
from u7algo import *
w = World(); pal = w.pal
ramps = [(0x01,0x0E),(0x0F,0x1E),(0x1F,0x2E),(0x2F,0x3A),(0x3B,0x48),(0x49,0x57),(0x58,0x65),(0x66,0x75),(0x76,0x85),
         (0x86,0x93),(0x94,0xA2),(0xA3,0xB1),(0xB2,0xBF),(0xC0,0xC7),(0xC8,0xD0),(0xD1,0xDF),
         (0xE0,0xE7),(0xE8,0xEF),(0xF0,0xF3),(0xF4,0xF7),(0xF8,0xFB),(0xFC,0xFE),(0xFF,0xFF)]
rid = np.zeros(256, np.uint8)
for k,(a,b) in enumerate(ramps): rid[a:b+1] = k+1
# water: treat cycling sparkle ramp E0-E7 as same material as the blues it sits in? keep separate for now
Sm, Fm = w.tile_ids(83, 73, 6, 6); idx_m = w.render(Sm, Fm)
R = rid[idx_m]
# share of 1x pixels whose 4-neighbours are all the same ramp (pure texture interior)
p = np.pad(R,1,mode='edge'); same = (p[:-2,1:-1]==R)&(p[2:,1:-1]==R)&(p[1:-1,:-2]==R)&(p[1:-1,2:]==R)
print("1x px with all 4-nbrs in same ramp (texture interior):", round(float(same.mean()),3))
# material boundary upscaled with xBRZ on ramp pseudo-colours (mean colour of ramp), local-snapped to ramp ids
ramp_col = np.zeros((256,3),np.uint8)
for k in range(1,len(ramps)+1):
    m = rid==k
    if m.any(): ramp_col[k] = pal[m].mean(0).astype(np.uint8)
ramp_col[0] = pal[0]
xr = xbrz_rgb(ramp_col[R], 6)
R6 = local_snap(xr, R, ramp_col, 6, 1)
# texture: nearest-neighbour indices; fix pixels whose ramp disagrees with R6 using nearest source pixel of that ramp in 3x3
T6 = nearest(idx_m, 6)
H, W = idx_m.shape
pi = np.pad(idx_m, 1, mode='edge')
cand = np.stack([pi[1+dy:1+dy+H, 1+dx:1+dx+W] for dy in (0,-1,1) for dx in (0,-1,1)], -1)  # centre first
cand6 = np.repeat(np.repeat(cand,6,0),6,1)
ok = rid[cand6] == R6[...,None]
first = np.where(ok.any(-1), ok.argmax(-1), 0)
M6 = np.take_along_axis(cand6, first[...,None], -1)[...,0]
x6 = xbrz_rgb(pal[idx_m], 6); snap = local_snap(x6, idx_m, pal, 6, 1)
ty, tx = 16+1, 16+21
def crop(a): return a[ty*48:(ty+3)*48, tx*48:(tx+3)*48]
ty2, tx2 = 16+3, 16+5
def crop2(a): return a[ty2*48:(ty2+3)*48, tx2*48:(tx2+3)*48]
ims = [("nearest", pal[crop(T6)]), ("material-xBRZ + NN texture", pal[crop(M6)]), ("xBRZ6>localsnap", pal[crop(snap)]),
       ("nearest", pal[crop2(T6)]), ("material-xBRZ + NN texture", pal[crop2(M6)]), ("xBRZ6>localsnap", pal[crop2(snap)])]
Wd = 144+4
sheet = Image.new("RGB", (len(ims)*Wd, 144+18), (40,40,40)); d = ImageDraw.Draw(sheet)
for i,(nm,im) in enumerate(ims):
    sheet.paste(Image.fromarray(im), (i*Wd+2, 16)); d.text((i*Wd+2, 2), nm, fill=(255,255,255))
sheet = sheet.resize((sheet.width*2, sheet.height*2), Image.NEAREST); sheet.save("out/A9_material.png"); print(sheet.size)
src = idx_m
blocks = M6.reshape(H,6,W,6).transpose(0,2,1,3).reshape(H,W,36)
print("material method: share of hi-res px equal to NN:", round(float((M6==T6).mean()),3))
