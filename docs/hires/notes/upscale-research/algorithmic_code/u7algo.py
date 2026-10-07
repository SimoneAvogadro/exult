"""Small, self-contained helpers for the algorithmic-upscaler experiments.

Reads Ultima VII (BG) static data read-only, builds 1x index images of map regions,
and implements: xBRZ (via ctypes to xBRZ 1.9), Scale2x/Scale3x on index planes,
OKLab nearest-palette mapping, local-palette snapping.
"""
import ctypes, os, struct
import numpy as np

STATIC = "/mnt/e/Games/RolePlayingGames/ultima7/static"
HERE = os.path.dirname(os.path.abspath(__file__))


# ----------------------------------------------------------------------------- data
def flex_entries(path):
    with open(path, "rb") as f:
        data = f.read()
    count = struct.unpack_from("<I", data, 0x54)[0]
    out = []
    for i in range(count):
        off, ln = struct.unpack_from("<II", data, 0x80 + 8 * i)
        out.append(data[off:off + ln] if ln else b"")
    return out


def load_palette(idx=0):
    e = flex_entries(os.path.join(STATIC, "palettes.flx"))[idx]
    p = np.frombuffer(e[:768], dtype=np.uint8).reshape(256, 3).astype(np.int32)
    return (p * 255 // 63).clip(0, 255).astype(np.uint8)  # engine conversion, idx 255 garbage


def load_flats():
    """dict (shape, frame) -> 8x8 uint8 index array, for shapes 0..149."""
    ents = flex_entries(os.path.join(STATIC, "shapes.vga"))
    flats = {}
    for sh in range(150):
        e = ents[sh]
        if not e:
            continue
        n = len(e) // 64
        for fr in range(n):
            flats[(sh, fr)] = np.frombuffer(e[fr * 64:(fr + 1) * 64], dtype=np.uint8).reshape(8, 8)
    return flats


def load_chunks():
    d = open(os.path.join(STATIC, "u7chunks"), "rb").read()
    n = len(d) // 512
    a = np.frombuffer(d, dtype=np.uint8).reshape(n, 256, 2).astype(np.int32)
    shp = a[:, :, 0] + 256 * (a[:, :, 1] & 3)
    frm = (a[:, :, 1] >> 2) & 0x1F
    return shp.reshape(n, 16, 16), frm.reshape(n, 16, 16)


def load_map():
    d = open(os.path.join(STATIC, "u7map"), "rb").read()
    tm = np.zeros((192, 192), dtype=np.int32)  # [cy, cx]
    for sc in range(144):
        buf = np.frombuffer(d[sc * 512:(sc + 1) * 512], dtype="<u2").reshape(16, 16)
        scy, scx = 16 * (sc // 12), 16 * (sc % 12)
        tm[scy:scy + 16, scx:scx + 16] = buf
    return tm


class World:
    def __init__(self):
        self.pal = load_palette(0)
        self.flats = load_flats()
        self.cshp, self.cfrm = load_chunks()
        self.tmap = load_map()

    def tile_ids(self, cx0, cy0, ncx, ncy):
        """(H,W) arrays of shape and frame for a chunk window, in tiles."""
        S = np.zeros((ncy * 16, ncx * 16), np.int32)
        F = np.zeros_like(S)
        for j in range(ncy):
            for i in range(ncx):
                t = self.tmap[(cy0 + j) % 192, (cx0 + i) % 192]
                S[j * 16:(j + 1) * 16, i * 16:(i + 1) * 16] = self.cshp[t]
                F[j * 16:(j + 1) * 16, i * 16:(i + 1) * 16] = self.cfrm[t]
        return S, F

    def frame(self, sh, fr):
        f = self.flats.get((sh, fr & 31))
        if f is None:  # RLE flat object or missing: show as index 0
            return np.zeros((8, 8), np.uint8)
        return f

    def render(self, S, F):
        H, W = S.shape
        img = np.zeros((H * 8, W * 8), np.uint8)
        for y in range(H):
            for x in range(W):
                img[y * 8:(y + 1) * 8, x * 8:(x + 1) * 8] = self.frame(S[y, x], F[y, x])
        return img


# ----------------------------------------------------------------------------- colour
def srgb_to_oklab(rgb):
    """rgb uint8 (...,3) -> float32 OKLab (...,3). Björn Ottosson 2020 matrices."""
    c = rgb.astype(np.float32) / 255.0
    lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
                   [0.2119034982, 0.6806995451, 0.1073969566],
                   [0.0883024619, 0.2817188376, 0.6299787005]], np.float32)
    M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
                   [1.9779984951, -2.4285922050, 0.4505937099],
                   [0.0259040371, 0.7827717662, -0.8086757660]], np.float32)
    lms = lin @ M1.T
    lms = np.cbrt(lms)
    return lms @ M2.T


def nearest_index(rgb_img, pal, allowed, space="oklab"):
    """Map an (H,W,3) uint8 RGB image to palette indices restricted to `allowed`."""
    flat = rgb_img.reshape(-1, 3)
    packed = (flat[:, 0].astype(np.uint32) << 16) | (flat[:, 1].astype(np.uint32) << 8) | flat[:, 2]
    uniq, inv = np.unique(packed, return_inverse=True)
    u = np.stack([(uniq >> 16) & 255, (uniq >> 8) & 255, uniq & 255], 1).astype(np.uint8)
    cand = np.asarray(allowed)
    if space == "oklab":
        a, b = srgb_to_oklab(u), srgb_to_oklab(pal[cand])
    else:
        a, b = u.astype(np.float32), pal[cand].astype(np.float32)
    d = ((a[:, None, :] - b[None, :, :]) ** 2).sum(-1)
    best = cand[d.argmin(1)]
    err = np.sqrt(d.min(1))
    return best[inv].reshape(rgb_img.shape[:2]).astype(np.uint8), err[inv].reshape(rgb_img.shape[:2]), len(uniq)


# ----------------------------------------------------------------------------- xBRZ
_lib = None


def _xbrz():
    global _lib
    if _lib is None:
        _lib = ctypes.CDLL(os.path.join(HERE, "xbrz19", "libxbrz19.so"))
        _lib.xbrz_scale.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                    ctypes.c_int, ctypes.c_double, ctypes.c_double, ctypes.c_double, ctypes.c_double]
    return _lib


def xbrz_rgb(rgb, factor=6, eq_tol=30.0, center_bias=4.0, dom=3.6, steep=2.4):
    """rgb (H,W,3) uint8 -> (H*f, W*f, 3) uint8 using xBRZ 1.9 in RGB mode (no alpha)."""
    H, W, _ = rgb.shape
    src = (rgb[..., 0].astype(np.uint32) << 16) | (rgb[..., 1].astype(np.uint32) << 8) | rgb[..., 2]
    src = np.ascontiguousarray(src)
    dst = np.zeros((H * factor, W * factor), np.uint32)
    _xbrz().xbrz_scale(factor, src.ctypes.data, dst.ctypes.data, W, H, 0, eq_tol, center_bias, dom, steep)
    return np.stack([(dst >> 16) & 255, (dst >> 8) & 255, dst & 255], -1).astype(np.uint8)


# ----------------------------------------------------------------------------- Scale2x / Scale3x on index planes
def _pad(a, mode):
    return np.pad(a, 1, mode=mode)


def scale2x(idx, pad="edge"):
    """AdvMAME2x/EPX on any equality-comparable plane (palette indices work directly)."""
    p = _pad(idx, pad)
    A, B, C = p[:-2, :-2], p[:-2, 1:-1], p[:-2, 2:]
    D, E, Fv = p[1:-1, :-2], p[1:-1, 1:-1], p[1:-1, 2:]
    G, Hh, I = p[2:, :-2], p[2:, 1:-1], p[2:, 2:]
    cond = (B != Hh) & (D != Fv)
    e0 = np.where(cond & (D == B), D, E)
    e1 = np.where(cond & (B == Fv), Fv, E)
    e2 = np.where(cond & (D == Hh), D, E)
    e3 = np.where(cond & (Hh == Fv), Fv, E)
    h, w = idx.shape
    out = np.empty((h * 2, w * 2), idx.dtype)
    out[0::2, 0::2], out[0::2, 1::2], out[1::2, 0::2], out[1::2, 1::2] = e0, e1, e2, e3
    return out


def scale3x(idx, pad="edge"):
    p = _pad(idx, pad)
    A, B, C = p[:-2, :-2], p[:-2, 1:-1], p[:-2, 2:]
    D, E, Fv = p[1:-1, :-2], p[1:-1, 1:-1], p[1:-1, 2:]
    G, Hh, I = p[2:, :-2], p[2:, 1:-1], p[2:, 2:]
    cond = (B != Hh) & (D != Fv)
    o = [None] * 9
    o[0] = np.where(cond & (D == B), D, E)
    o[1] = np.where(cond & (((D == B) & (E != C)) | ((B == Fv) & (E != A))), B, E)
    o[2] = np.where(cond & (B == Fv), Fv, E)
    o[3] = np.where(cond & (((D == B) & (E != G)) | ((D == Hh) & (E != A))), D, E)
    o[4] = E
    o[5] = np.where(cond & (((B == Fv) & (E != I)) | ((Hh == Fv) & (E != C))), Fv, E)
    o[6] = np.where(cond & (D == Hh), D, E)
    o[7] = np.where(cond & (((D == Hh) & (E != I)) | ((Hh == Fv) & (E != G))), Hh, E)
    o[8] = np.where(cond & (Hh == Fv), Fv, E)
    h, w = idx.shape
    out = np.empty((h * 3, w * 3), idx.dtype)
    for k in range(9):
        out[k // 3::3, k % 3::3] = o[k]
    return out


def nearest(idx, f):
    return np.repeat(np.repeat(idx, f, 0), f, 1)


# ----------------------------------------------------------------------------- hybrid: local-palette snap
def local_snap(hr_rgb, src_idx, pal, f=6, radius=1):
    """For every hi-res pixel choose, among the source indices in the (2r+1)^2 neighbourhood of
    its source pixel, the one nearest in OKLab to the hi-res RGB value. Output is an index image
    that only uses indices present locally in the source (palette- and cycling-index-preserving)."""
    H, W = src_idx.shape
    p = np.pad(src_idx, radius, mode="edge")
    cands = []
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            cands.append(p[radius + dy:radius + dy + H, radius + dx:radius + dx + W])
    cands = np.stack(cands, -1)                               # (H,W,K)
    cands_hr = np.repeat(np.repeat(cands, f, 0), f, 1)        # (H*f,W*f,K)
    lab_pal = srgb_to_oklab(pal)                              # (256,3)
    lab_hr = srgb_to_oklab(hr_rgb)                            # (H*f,W*f,3)
    d = ((lab_pal[cands_hr] - lab_hr[..., None, :]) ** 2).sum(-1)
    k = d.argmin(-1)
    return np.take_along_axis(cands_hr, k[..., None], -1)[..., 0]


# ----------------------------------------------------------------------------- MMPX (C99 port, MIT) on index planes
_mm = None


def unique_rgb_palette(pal):
    """Nudge duplicate palette RGB values by 1 LSB so RGB<->index is a bijection (for scalers that
    only copy source colours). Returns (pal_u, packed_u32)."""
    pu = pal.astype(np.int32).copy()
    seen = {}
    deltas = [(0, 0, d) for d in (1, -1, 2, -2, 3, -3)] + [(0, d, 0) for d in (1, -1, 2, -2)] + [(d, 0, 0) for d in (1, -1)]
    for i in range(256):
        k = tuple(int(v) for v in pu[i])
        if k in seen:
            for dl in deltas:
                c = tuple(min(255, max(0, k[j] + dl[j])) for j in range(3))
                if c not in seen:
                    pu[i] = c; k = c; break
            else:
                raise RuntimeError("could not uniquify palette")
        seen[k] = i
    pu = pu.astype(np.uint8)
    packed = (pu[:, 0].astype(np.uint32) << 16) | (pu[:, 1].astype(np.uint32) << 8) | pu[:, 2]
    return pu, packed


def mmpx2x_idx(idx, pal):
    global _mm
    if _mm is None:
        _mm = ctypes.CDLL(os.path.join(HERE, "mmpx", "libmmpx.so"))
        _mm.mmpx_scale2x.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
    _, packed = unique_rgb_palette(pal)
    H, W = idx.shape
    src = np.ascontiguousarray(packed[idx])            # alpha byte 0 = opaque in MMPX luma
    dst = np.zeros((H * 2, W * 2), np.uint32)
    _mm.mmpx_scale2x(src.ctypes.data, dst.ctypes.data, W, H)
    lut = {int(v): i for i, v in enumerate(packed)}
    u, inv = np.unique(dst, return_inverse=True)
    return np.array([lut[int(v)] for v in u], np.uint8)[inv].reshape(dst.shape)
