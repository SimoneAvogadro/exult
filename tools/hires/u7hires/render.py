"""World renders at 6x from final tiles (QA C1, previews), plus an approximate RLE-terrain overlay.

The flat layer follows the engine (per-tile override of the *effective source* flat, NN fallback).
The overlay paints the RLE tiles of the chunk terrains (shore pieces, ...) as lift-0 objects anchored
at their tile's bottom-right pixel, in (y, x) order, NN-scaled. It ignores fixed/ifix objects and the
engine's real dependency sort, so it is a visual aid only.
"""

from __future__ import annotations

import numpy as np

from . import TILE, WORLD_TILES
from .ctx import Region
from .io import KIND_RLE, World


def final_render(region: Region, tiles: dict, world: World, scale: int = 6) -> tuple[np.ndarray, np.ndarray]:
    """(6x index image, (th, tw) bool 'override used') for a region from per-(shape, frame) tiles."""
    T = TILE * scale
    out = np.zeros((region.th * T, region.tw * T), np.uint8)
    used = np.zeros((region.th, region.tw), bool)
    for y in range(region.th):
        for x in range(region.tw):
            s, f = int(region.src_shape[y, x]), int(region.src_frame[y, x])
            if s < 0:
                continue
            t = tiles.get((s, f))
            if t is None:
                t = np.repeat(np.repeat(world.flat(s, f), scale, 0), scale, 1)
            else:
                used[y, x] = True
            out[y * T:(y + 1) * T, x * T:(x + 1) * T] = t
    return out, used


def nn_render(region: Region, scale: int = 6) -> np.ndarray:
    return np.repeat(np.repeat(region.idx, scale, 0), scale, 1)


def rle_overlay(world: World, region: Region, margin: int = 8) -> tuple[np.ndarray, np.ndarray]:
    """1x (indices, mask) of the RLE terrain tiles drawn over the region (approximate)."""
    H, W = region.th * TILE, region.tw * TILE
    ov = np.zeros((H, W), np.uint8)
    mask = np.zeros((H, W), bool)
    lut = world.kind_lut
    anchors = []
    for y in range(region.th + margin):
        wy = (region.ty0 + y) % WORLD_TILES
        cy, ly = divmod(wy, 16)
        for x in range(region.tw + margin):
            wx = (region.tx0 + x) % WORLD_TILES
            cx, lx = divmod(wx, 16)
            t = int(world.tmap[cy, cx])
            s, f = int(world.chunk_shape[t, ly * 16 + lx]), int(world.chunk_frame[t, ly * 16 + lx])
            if s < lut.shape[0] and lut[s, f & 0xFF] == KIND_RLE:
                anchors.append((y, x, s, f))
    for y, x, s, f in sorted(anchors):
        fr = world.shapes.rle_frame(s, f)
        if fr is None:
            continue
        ax, ay = (x + 1) * TILE - 1, (y + 1) * TILE - 1
        x0, y0 = ax - fr.xleft, ay - fr.yabove
        h, w = fr.pixels.shape
        sy0, sx0 = max(0, -y0), max(0, -x0)
        dy0, dx0 = max(0, y0), max(0, x0)
        dy1, dx1 = min(H, y0 + h), min(W, x0 + w)
        if dy1 <= dy0 or dx1 <= dx0:
            continue
        m = fr.mask[sy0:sy0 + dy1 - dy0, sx0:sx0 + dx1 - dx0]
        p = fr.pixels[sy0:sy0 + dy1 - dy0, sx0:sx0 + dx1 - dx0]
        sub = ov[dy0:dy1, dx0:dx1]
        sub[m] = p[m]
        mask[dy0:dy1, dx0:dx1] |= m
    return ov, mask


def composite(img: np.ndarray, ov: np.ndarray, mask: np.ndarray, scale: int) -> np.ndarray:
    ov6 = np.repeat(np.repeat(ov, scale, 0), scale, 1)
    m6 = np.repeat(np.repeat(mask, scale, 0), scale, 1)
    return np.where(m6, ov6, img)
