"""Whole-terrain overrides (WP-17) from route-1 diffusion windows.

A route-1 run refines a context window (a16: the terrain's 128 x 128 1x layer plus a 16 px apron of its
map neighbours) at 12x. ``post()`` turns it into palette-0 indices at 6x (colour lock, label-safe cycling
pixels, local snap). The terrain's own 768 x 768 square, cut out of that plane, is what the engine paints
for every chunk of that terrain when ``x6/terrain/<t1>.png`` exists (DESIGN.md sections 3.4, 5.2): no
tile seams inside the chunk, which per-tile flats cannot avoid.

This module does the cut, the T1 key (from the terrain's own flats, as the engine computes it), the
engine's checks that can be made offline (size, P0, P4 against the 1x layer) and the PNG
(``Exult-Terrain-Key``, ``Exult-Origin``). ``tools/hires/mkterrain.py`` is the command line.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

import numpy as np

from . import S_ART, TILE
from .hashing import palette_crc32, t1_hex, t1_key
from .io import KIND_FLAT, KIND_FLAT_VOID, World
from .pngio import encode_indexed
from .rules import p4_violations
from .util import atomic_write_bytes

LAYER = 128                   # a terrain is 128 x 128 game px
SIDE = LAYER * S_ART          # 768


def terrain_t1(world: World, tnum: int) -> int:
    """T1 key of terrain ``tnum`` over its own flats (non-RLE tiles), as Chunk_terrain::get_t1_key."""
    kinds = world.terrain_kinds(tnum)
    is_flat = np.isin(kinds, (KIND_FLAT, KIND_FLAT_VOID))
    flats = [world.flat(int(s), int(f)) if is_flat[t] else None
             for t, (s, f) in enumerate(zip(world.chunk_shape[tnum], world.chunk_frame[tnum]))]
    return t1_key(is_flat, flats)


def cut_terrain(final6: np.ndarray, idx1: np.ndarray, oy: int, ox: int) -> tuple[np.ndarray, np.ndarray]:
    """The terrain's square out of a 6x window plane ``final6`` and its 1x indices ``idx1``, with the
    terrain's top-left at 1x pixel (oy, ox) of the window. Returns (768 x 768 indices, 128 x 128 layer)."""
    if final6.shape != (idx1.shape[0] * S_ART, idx1.shape[1] * S_ART):
        raise ValueError(f"plane {final6.shape} is not {S_ART}x the window {idx1.shape}")
    if oy < 0 or ox < 0 or oy + LAYER > idx1.shape[0] or ox + LAYER > idx1.shape[1]:
        raise ValueError(f"the window {idx1.shape} does not hold the terrain at ({oy}, {ox})")
    tile = final6[oy * S_ART:(oy + LAYER) * S_ART, ox * S_ART:(ox + LAYER) * S_ART]
    layer = idx1[oy:oy + LAYER, ox:ox + LAYER]
    return np.ascontiguousarray(tile, np.uint8), np.ascontiguousarray(layer, np.uint8)


def check_terrain(tile: np.ndarray, layer: np.ndarray) -> dict:
    """The engine's pixel rules for a terrain file (section 5.5): F3 size, P0 (no index 0xFF), P4 (cycling
    pixels only next to the same range in the 1x layer; the engine rejects above 0.5 %)."""
    out = {"size_ok": tile.shape == (SIDE, SIDE), "p0": int((tile == 0xFF).sum())}
    out["p4"] = p4_violations(tile, layer, S_ART) if out["size_ok"] else -1
    out["p4_fraction"] = out["p4"] / float(SIDE * SIDE) if out["size_ok"] else 1.0
    out["ok"] = out["size_ok"] and out["p0"] == 0 and out["p4_fraction"] <= 0.005
    return out


def terrain_png(tile: np.ndarray, pal8: np.ndarray, key: int, origin: str) -> bytes:
    return encode_indexed(tile, pal8, [("Exult-Terrain-Key", t1_hex(key)), ("Exult-Origin", origin)])


def write_pack_terrain(pack: str, tile: np.ndarray, world: World, key: int, origin: str) -> str:
    """Writes <pack>/x6/terrain/<t1>.png (and pack.txt when missing); returns the file's path."""
    tdir = os.path.join(pack, f"x{S_ART}", "terrain")
    os.makedirs(tdir, exist_ok=True)
    path = os.path.join(tdir, t1_hex(key) + ".png")
    atomic_write_bytes(path, terrain_png(tile, world.pal8, key, origin))
    meta = os.path.join(pack, "pack.txt")
    if not os.path.exists(meta):
        atomic_write_bytes(meta, (f"game=BG\nscale={S_ART}\npalette_crc32={palette_crc32(world.pal8):08x}\n"
                                  f"edge=none\nroute=route1-terrain\ntitle=whole-terrain overrides\n").encode())
    return path


def main(argv=None) -> int:
    from .route1 import PILOT_WINDOWS, POST_VARIANTS, load_ctx, post, window_data

    ap = argparse.ArgumentParser(description="Whole-terrain override (x6/terrain/<t1>.png) from a route-1 run")
    ap.add_argument("--static", required=True, help="BG static directory")
    ap.add_argument("--ctx", required=True, help="context set of the runs (work/ctx/a16)")
    ap.add_argument("--runs", required=True, help="route-1 runs directory (work/phase_b_pilot/runs)")
    ap.add_argument("--window", required=True, help="pilot window name whose crop holds a whole terrain "
                    "(t1826_full, grass_full, t1827_full)")
    ap.add_argument("--tag", default="soft_d0.50_c0.60_s1000", help="run tag (a subdirectory with out12.png)")
    ap.add_argument("--post", default="lp3", choices=sorted(POST_VARIANTS), help="post variant (default lp3)")
    ap.add_argument("--out", required=True, help="pack root to write x6/terrain/<t1>.png into")
    ap.add_argument("--force", action="store_true", help="write even if an offline rule fails")
    a = ap.parse_args(argv)

    pws = {p.name: p for p in PILOT_WINDOWS}
    if a.window not in pws:
        ap.error(f"unknown window {a.window!r} (one of {', '.join(sorted(pws))})")
    pw = pws[a.window]
    from PIL import Image
    c = load_ctx(a.static, a.ctx)
    wd = window_data(c, pw)
    idx = wd["idx"]
    out12 = np.asarray(Image.open(os.path.join(a.runs, pw.name, a.tag, "out12.png")).convert("RGB"))
    rx, gain, lock = POST_VARIANTS[a.post]
    final, _, _, _ = post(idx, out12.astype(np.float32), c.k, ramp_expand=rx, gain=gain, lock=lock,
                          ref6=c.world.pal8[wd["planes"]["mixed"]])
    w = c.windows[pw.terrain]
    apron = (w.idx.shape[0] - LAYER) // 2
    tile, layer = cut_terrain(final, idx, apron - pw.y0 * TILE, apron - pw.x0 * TILE)
    key = terrain_t1(c.world, pw.terrain)
    rep = check_terrain(tile, layer)
    rep.update({"terrain": pw.terrain, "t1": t1_hex(key), "window": pw.name, "tag": a.tag, "post": a.post})
    if not rep["ok"] and not a.force:
        print(json.dumps(rep), file=sys.stderr)
        print("mkterrain: an offline rule fails (use --force to write anyway)", file=sys.stderr)
        return 1
    rep["path"] = write_pack_terrain(a.out, tile, c.world, key, f"route1:{pw.name}:{a.tag}:{a.post}")
    print(json.dumps(rep))
    return 0
