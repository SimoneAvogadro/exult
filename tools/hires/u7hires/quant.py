"""Palette tools: OKLab, uniquified palette, local snap, global nearest and index-rule enforcement.

Index rules (DESIGN §5.5, §8.2, recommendation §2.6):
* static output indices are 0x01-0xDF; 0x00 only in tiles whose 1x source uses 0x00;
* cycling indices (0xE0-0xFE) only where P4 allows them (in-tile, clamped 3x3 parent neighbourhood
  in the same cycle range); every other cycling pixel is snapped to the nearest static index of its
  parent's ramp;
* 0xFF never.
The quantizer is our own exact NumPy mapper (no Pillow quantize, no libimagequant, no -remap).
"""

from __future__ import annotations

import numpy as np

from .rules import (BORDER_INDEX, CYCLE_ID, IS_CYCLING, STATIC_MAX, STATIC_MIN, p4_noncompliant)

_M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
                [0.2119034982, 0.6806995451, 0.1073969566],
                [0.0883024619, 0.2817188376, 0.6299787005]])
_M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
                [1.9779984951, -2.4285922050, 0.4505937099],
                [0.0259040371, 0.7827717662, -0.8086757660]])

STATIC_MASK = np.zeros(256, bool)
STATIC_MASK[STATIC_MIN:STATIC_MAX + 1] = True


def srgb_to_linear(rgb) -> np.ndarray:
    c = np.asarray(rgb, np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(lin) -> np.ndarray:
    c = np.clip(np.asarray(lin, np.float64), 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055) * 255.0


def srgb_to_oklab(rgb) -> np.ndarray:
    """sRGB 0..255 (any shape (...,3)) -> OKLab float64 (Ottosson 2020)."""
    lms = srgb_to_linear(rgb) @ _M1.T
    return np.cbrt(lms) @ _M2.T


def oklab_de(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    return np.sqrt(((np.asarray(a) - np.asarray(b)) ** 2).sum(-1))


def unique_rgb_palette(pal8: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Nudge duplicate palette colours by 1-3 LSB so RGB <-> index is a bijection.

    Colour-copying scalers (xBRZ before the snap, MMPX) run on this palette; the result is inverted
    by lookup. Earlier indices keep their colour; later duplicates move. Returns (pal_u, packed).
    """
    pu = np.asarray(pal8, np.int32).reshape(256, 3).copy()
    seen: dict[tuple[int, int, int], int] = {}
    deltas = ([(0, 0, d) for d in (1, -1, 2, -2, 3, -3)] + [(0, d, 0) for d in (1, -1, 2, -2, 3, -3)]
              + [(d, 0, 0) for d in (1, -1, 2, -2, 3, -3)])
    for i in range(256):
        k = tuple(int(v) for v in pu[i])
        if k in seen:
            for dl in deltas:
                c = tuple(min(255, max(0, k[j] + dl[j])) for j in range(3))
                if c not in seen and c not in {tuple(int(v) for v in pu[j]) for j in range(i + 1, 256)}:
                    pu[i] = c
                    k = c
                    break
            else:
                raise RuntimeError("could not uniquify palette")
        seen[k] = i
    pu = pu.astype(np.uint8)
    packed = (pu[:, 0].astype(np.uint32) << 16) | (pu[:, 1].astype(np.uint32) << 8) | pu[:, 2]
    return pu, packed


def neighbourhood_stack(src: np.ndarray, radius: int = 1) -> np.ndarray:
    """(h, w, (2r+1)^2) indices of the edge-clamped neighbourhood, centre first."""
    h, w = src.shape
    p = np.pad(src, radius, mode="edge")
    offs = [(0, 0)] + [(dy, dx) for dy in range(-radius, radius + 1) for dx in range(-radius, radius + 1)
                       if (dy, dx) != (0, 0)]
    return np.stack([p[radius + dy:radius + dy + h, radius + dx:radius + dx + w] for dy, dx in offs], -1)


class Quantizer:
    """Exact OKLab mapper bound to one palette (palette 0, 8-bit) and its engine ramps."""

    def __init__(self, pal8: np.ndarray, ramp_lut: np.ndarray | None = None):
        self.pal8 = np.asarray(pal8, np.uint8).reshape(256, 3)
        self.lab = srgb_to_oklab(self.pal8)
        self.pal_u, self.packed_u = unique_rgb_palette(self.pal8)
        self.lab_u = srgb_to_oklab(self.pal_u)
        self.ramp_lut = None if ramp_lut is None else np.asarray(ramp_lut, np.int16)
        if self.ramp_lut is not None:
            nr = int(self.ramp_lut.max()) + 1
            self.ramp_members = np.zeros((nr, 256), bool)       # static members of each ramp
            for i in range(256):
                r = self.ramp_lut[i]
                if r >= 0 and STATIC_MASK[i]:
                    self.ramp_members[r, i] = True

    # ------------------------------------------------------------------ nearest
    def nearest(self, rgb: np.ndarray, allowed=None, lab_pal: np.ndarray | None = None) -> np.ndarray:
        """Global OKLab-nearest index for each RGB pixel among ``allowed`` (default 0x01-0xDF)."""
        rgb = np.asarray(rgb)
        shp = rgb.shape[:-1]
        cand = np.arange(STATIC_MIN, STATIC_MAX + 1) if allowed is None else np.asarray(allowed)
        lab_pal = self.lab if lab_pal is None else lab_pal
        flat = np.clip(np.rint(rgb.reshape(-1, 3)), 0, 255).astype(np.uint32)
        packed = (flat[:, 0] << 16) | (flat[:, 1] << 8) | flat[:, 2]
        uniq, inv = np.unique(packed, return_inverse=True)
        u = np.stack([(uniq >> 16) & 255, (uniq >> 8) & 255, uniq & 255], 1)
        lu = srgb_to_oklab(u)
        lc = lab_pal[cand]
        best = np.empty(len(uniq), np.int64)
        for i in range(0, len(uniq), 8192):
            d = ((lu[i:i + 8192, None, :] - lc[None]) ** 2).sum(-1)
            best[i:i + 8192] = d.argmin(1)
        return cand[best][inv.reshape(-1)].reshape(shp).astype(np.uint8)

    # ------------------------------------------------------------------ local snap
    def candidate_masks(self, src: np.ndarray, radius: int = 1, forbid=None, ramp_expand: bool = False) -> np.ndarray:
        """(h, w, 256) bool: allowed indices per 1x parent pixel."""
        h, w = src.shape
        nb = neighbourhood_stack(src, radius)
        mask = np.zeros((h, w, 256), bool)
        yy, xx = np.meshgrid(np.arange(h), np.arange(w), indexing="ij")
        for k in range(nb.shape[-1]):
            mask[yy, xx, nb[..., k]] = True
        if ramp_expand:
            if self.ramp_lut is None:
                raise ValueError("ramp_expand needs ramp_lut")
            nr = self.ramp_members.shape[0]
            stat = mask & STATIC_MASK[None, None, :]
            rid = np.where(stat, self.ramp_lut[None, None, :], -1)
            has_ramp = np.zeros((h, w, nr), bool)
            for r in range(nr):
                has_ramp[..., r] = (rid == r).any(-1)
            mask |= (has_ramp.reshape(-1, nr).astype(np.float32) @ self.ramp_members.astype(np.float32)
                     ).reshape(h, w, 256) > 0
        mask[..., BORDER_INDEX] = False
        if forbid is not None:
            mask &= ~np.asarray(forbid, bool)[None, None, :]
        return mask

    def local_snap(self, rgb_hi: np.ndarray, src: np.ndarray, scale: int, radius: int = 1, forbid=None,
                   ramp_expand: bool = False, lab_pal: np.ndarray | None = None,
                   fallback=None, strip: int = 4) -> np.ndarray:
        """Re-index every hi-res pixel to the OKLab-nearest index among its 1x parent's (2r+1)^2
        neighbourhood (edge-clamped), optionally widened to the static members of those indices'
        ramps. Ties prefer the parent's own index, then the smaller index. Parents left with no
        candidate fall back to the global nearest over ``fallback`` (default 0x01-0xDF).
        ``lab_pal``: OKLab of the palette the scaler worked in (the uniquified one for xBRZ)."""
        src = np.asarray(src, np.uint8)
        h, w = src.shape
        H, W = h * scale, w * scale
        rgb_hi = np.asarray(rgb_hi)
        if rgb_hi.shape[:2] != (H, W):
            raise ValueError(f"rgb_hi {rgb_hi.shape} does not match src {src.shape} x{scale}")
        lab_pal = self.lab if lab_pal is None else lab_pal
        mask = self.candidate_masks(src, radius, forbid, ramp_expand)
        counts = mask.sum(-1)
        K = max(1, int(counts.max()))
        order = np.argsort(~mask, axis=-1, kind="stable")[..., :K]          # allowed indices first, ascending
        valid = np.arange(K)[None, None, :] < counts[..., None]
        cand = np.where(valid, order, -1).astype(np.int16)                    # (h, w, K)
        out = np.empty((H, W), np.uint8)
        for y0 in range(0, h, strip):
            y1 = min(h, y0 + strip)
            c = np.repeat(np.repeat(cand[y0:y1], scale, 0), scale, 1)          # (rows, W, K)
            par = np.repeat(np.repeat(src[y0:y1], scale, 0), scale, 1)
            lab_hi = srgb_to_oklab(rgb_hi[y0 * scale:y1 * scale])
            lc = lab_pal[np.maximum(c, 0)]
            d = ((lc - lab_hi[..., None, :]) ** 2).sum(-1)
            d = np.where(c < 0, np.inf, d + 1e-12 * (c != par[..., None]))
            k = d.argmin(-1)
            out[y0 * scale:y1 * scale] = np.take_along_axis(c, k[..., None], -1)[..., 0].astype(np.uint8)
        empty = counts == 0
        if empty.any():
            fb = np.arange(STATIC_MIN, STATIC_MAX + 1) if fallback is None else fallback
            emp_hi = np.repeat(np.repeat(empty, scale, 0), scale, 1)
            out[emp_hi] = self.nearest(rgb_hi[emp_hi], fb, lab_pal)
        return out

    # ------------------------------------------------------------------ enforcement
    def static_candidates(self, parent: int, neighbours: np.ndarray) -> np.ndarray:
        """Static (0x01-0xDF) indices a non-compliant pixel may become."""
        if STATIC_MASK[parent] and self.ramp_lut is not None and self.ramp_lut[parent] >= 0:
            c = np.nonzero(self.ramp_members[self.ramp_lut[parent]])[0]
            if len(c):
                return c
        c = np.unique(neighbours[STATIC_MASK[neighbours]])
        if len(c):
            return c
        return np.arange(STATIC_MIN, STATIC_MAX + 1)

    def enforce(self, tile: np.ndarray, src: np.ndarray, scale: int = 6, ref_rgb: np.ndarray | None = None,
                zero_ok: bool | None = None, restore_sparkles: bool = False) -> tuple[np.ndarray, dict]:
        """Make a hi-res index tile satisfy A2/P0/P4 against its own 1x source (in-tile rules).

        Non-compliant cycling pixels (P4), 0xFF, and 0x00 in tiles whose source has no 0x00 are
        replaced by the OKLab-nearest static index of the parent's ramp (parent static), else of the
        static indices in the parent's in-tile clamped 3x3 neighbourhood, else of 0x01-0xDF. The
        reference colour is ``ref_rgb`` (pre-quantization colour) when given, else the palette colour.
        ``restore_sparkles`` puts the parent index back into the centre of blocks whose source
        cycling pixel lost every cycling pixel of its range (A3 'dropped sparkle').
        """
        tile = np.array(tile, np.uint8, copy=True)
        src = np.asarray(src, np.uint8)
        if zero_ok is None:
            zero_ok = bool((src == 0).any())
        bad = p4_noncompliant(tile, src, scale) | (tile == BORDER_INDEX)
        if not zero_ok:
            bad |= tile == 0
        stats = {"p4_fixed": 0, "ff_fixed": int((tile == BORDER_INDEX).sum()),
                 "zero_fixed": 0 if zero_ok else int((tile == 0).sum()), "sparkles_restored": 0}
        stats["p4_fixed"] = int(bad.sum()) - stats["ff_fixed"] - stats["zero_fixed"]
        if bad.any():
            nb = neighbourhood_stack(src, 1)
            ys, xs = np.nonzero(bad)
            for y, x in zip(ys.tolist(), xs.tolist()):
                py, px = y // scale, x // scale
                cands = self.static_candidates(int(src[py, px]), nb[py, px])
                ref = self.lab[tile[y, x]] if ref_rgb is None else srgb_to_oklab(ref_rgb[y, x])
                d = ((self.lab[cands] - ref) ** 2).sum(-1)
                tile[y, x] = cands[int(np.argmin(d))]
        if restore_sparkles:
            cid = CYCLE_ID[src]
            tid = CYCLE_ID[tile]
            c0, c1 = scale // 2 - (scale > 1), scale // 2 + 1
            for py, px in zip(*np.nonzero(cid >= 0)):
                blk = tid[py * scale:(py + 1) * scale, px * scale:(px + 1) * scale]
                if not (blk == cid[py, px]).any():
                    tile[py * scale + c0:py * scale + c1, px * scale + c0:px * scale + c1] = src[py, px]
                    stats["sparkles_restored"] += 1
        return tile, stats


def dropped_sparkles(tile: np.ndarray, src: np.ndarray, scale: int = 6) -> int:
    """Number of source cycling pixels whose 6x6 block holds no pixel of the same cycle range."""
    cid = CYCLE_ID[np.asarray(src)]
    tid = CYCLE_ID[np.asarray(tile)]
    n = 0
    for py, px in zip(*np.nonzero(cid >= 0)):
        blk = tid[py * scale:(py + 1) * scale, px * scale:(px + 1) * scale]
        n += int(not (blk == cid[py, px]).any())
    return n


def is_cycling(idx) -> np.ndarray:
    return IS_CYCLING[np.asarray(idx)]
