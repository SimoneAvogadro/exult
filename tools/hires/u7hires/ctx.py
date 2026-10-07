"""Context windows for upscaling (DESIGN §8.2 A1, recommendation §2.3).

Never upscale a lone 8x8 tile. Three kinds of windows, in order of priority (``tier``):

0. ``terrain``: per used terrain, the engine's 1x flat layer (with the under-RLE fill) plus an apron
   of ``apron`` px (16-32, a multiple of 8) taken from its most frequent real neighbours: the anchor is
   the map occurrence whose 8 neighbour terrains are, summed over directions, the most frequent ones
   for that terrain (ties: smallest (cy, cx)), so the apron is real, consistent map context.
1. ``macro``: per 8x4 macro-texture shape (frame f's right neighbour is f+1, lower f+8; detected from
   map edges with the >60 % rule), the 64x32 periodic unit padded toroidally by ``apron``.
2. ``selfwrap``: a lone frame padded toroidally by ``apron``; only for frames that have neither a
   real-map instance nor a macro sheet.

Every window carries a per-tile instance map (shape, frame&31, role, world tile origin); the
per-pixel map is ``Window.instance_map()``. Only ``ROLE_OWN`` cells are consensus instances.
"""

from __future__ import annotations

import json
import os
from collections import Counter, defaultdict
from dataclasses import dataclass, field

import numpy as np

from . import CHUNK_PX, NUM_CHUNKS, TILE, WORLD_TILES
from .fill import FlatLayerCache, terrain_t1
from .hashing import t1_hex
from .io import KIND_FLAT, KIND_FLAT_VOID, World

ROLE_NONE, ROLE_OWN, ROLE_FILL, ROLE_APRON, ROLE_PAD = 0, 1, 2, 3, 4
ROLE_NAMES = {ROLE_NONE: "none", ROLE_OWN: "own", ROLE_FILL: "fill", ROLE_APRON: "apron", ROLE_PAD: "pad"}
TIER_TERRAIN, TIER_MACRO, TIER_SELFWRAP = 0, 1, 2
TIER_NAMES = {TIER_TERRAIN: "terrain", TIER_MACRO: "macro", TIER_SELFWRAP: "selfwrap"}
DIRS = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]
MACRO_THRESHOLD = 0.6


@dataclass
class Window:
    kind: str
    ident: int                      # terrain number | macro shape | shape*32+frame
    idx: np.ndarray                 # (H, W) uint8 1x indices
    tile_shape: np.ndarray          # (H/8, W/8) int16, -1 = nothing painted
    tile_frame: np.ndarray          # (H/8, W/8) int16 (frame & 31)
    tile_role: np.ndarray           # (H/8, W/8) uint8 ROLE_*
    origin: tuple[int, int] = (-1, -1)   # world tile (ty, tx) of the window's top-left; -1 if synthetic
    weight: float = 1.0             # instance weight (map occurrences of the terrain)
    tier: int = TIER_TERRAIN
    meta: dict = field(default_factory=dict)

    @property
    def name(self) -> str:
        if self.kind == "terrain":
            return f"t{self.ident:04d}"
        if self.kind == "macro":
            return f"m{self.ident:04d}"
        return f"s{self.ident >> 5:04d}_{self.ident & 31:02d}"

    def own_cells(self) -> list[tuple[int, int, int, int]]:
        """(ty, tx, shape, frame) of every consensus instance in the window."""
        ys, xs = np.nonzero(self.tile_role == ROLE_OWN)
        return [(int(y), int(x), int(self.tile_shape[y, x]), int(self.tile_frame[y, x])) for y, x in zip(ys, xs)]

    def instance_map(self) -> dict[str, np.ndarray]:
        """Per-pixel instance map: shape, frame, role, and world pixel coordinates (or -1)."""
        rep = lambda a: np.repeat(np.repeat(a, TILE, 0), TILE, 1)  # noqa: E731
        H, W = self.idx.shape
        out = {"shape": rep(self.tile_shape), "frame": rep(self.tile_frame), "role": rep(self.tile_role)}
        if self.origin[0] >= 0:
            wy = (self.origin[0] * TILE + np.arange(H)) % (WORLD_TILES * TILE)
            wx = (self.origin[1] * TILE + np.arange(W)) % (WORLD_TILES * TILE)
            out["world_y"] = np.repeat(wy[:, None], W, 1).astype(np.int32)
            out["world_x"] = np.repeat(wx[None, :], H, 0).astype(np.int32)
        else:
            out["world_y"] = np.full((H, W), -1, np.int32)
            out["world_x"] = np.full((H, W), -1, np.int32)
        return out


# ---------------------------------------------------------------------------- macro shapes
def macro_shapes(world: World, threshold: float = MACRO_THRESHOLD) -> list[int]:
    """Flat shapes whose same-shape map edges follow the 8x4 layout more than ``threshold``."""
    S, F = world.tile_grid
    Wid = S.astype(np.int64) * 32 + F
    flat_shape = np.array([world.shapes.is_flat_shape(s) for s in range(max(world.shapes.num_shapes, 1))])
    nsh = len(flat_shape)
    same_n = np.zeros(nsh)
    macro_n = np.zeros(nsh)
    for horiz, (a, b) in ((True, (Wid[:, :-1].ravel(), Wid[:, 1:].ravel())),
                          (False, (Wid[:-1].ravel(), Wid[1:].ravel()))):
        sa, sb = a // 32, b // 32
        ok = (sa < nsh) & (sb < nsh)
        ok[ok] = flat_shape[sa[ok]] & flat_shape[sb[ok]]
        a, b = a[ok], b[ok]
        same = a // 32 == b // 32
        fa, df = a % 32, b % 32 - a % 32
        if horiz:
            macro = same & (((df == 1) & (fa % 8 != 7)) | ((df == -7) & (fa % 8 == 7)))
        else:
            macro = same & (((df == 8) & (fa < 24)) | ((df == -24) & (fa >= 24)))
        same_n += np.bincount(a[same] // 32, minlength=nsh)[:nsh]
        macro_n += np.bincount(a[macro] // 32, minlength=nsh)[:nsh]
    r = np.where(same_n > 0, macro_n / np.maximum(same_n, 1), 0.0)
    return [int(s) for s in np.nonzero(r > threshold)[0]]


# ---------------------------------------------------------------------------- terrain windows
def choose_anchors(world: World) -> dict[int, tuple[int, int]]:
    """Most typical real occurrence (cy, cx) of every used terrain (see module docstring)."""
    tm = world.tmap
    occ: dict[int, list[tuple[int, int]]] = defaultdict(list)
    for cy in range(NUM_CHUNKS):
        row = tm[cy]
        for cx in range(NUM_CHUNKS):
            occ[int(row[cx])].append((cy, cx))
    anchors = {}
    for t, lst in occ.items():
        freq = [Counter() for _ in DIRS]
        nbs = []
        for cy, cx in lst:
            nb = [int(tm[(cy + dy) % NUM_CHUNKS, (cx + dx) % NUM_CHUNKS]) for dy, dx in DIRS]
            nbs.append(nb)
            for d, n in enumerate(nb):
                freq[d][n] += 1
        best, best_score = None, -1
        for (cy, cx), nb in zip(lst, nbs):       # lst is in row-major order: first max wins ties
            score = sum(freq[d][n] for d, n in enumerate(nb))
            if score > best_score:
                best, best_score = (cy, cx), score
        anchors[t] = best
    return anchors


def _terrain_tiles(world: World, cache: FlatLayerCache, tnum: int):
    """Per-cell (shape, frame, role-if-centre) of terrain tnum as painted (own or fill source)."""
    _, src = cache.get(tnum)
    kinds = world.terrain_kinds(tnum)
    shp = np.full(256, -1, np.int16)
    frm = np.full(256, -1, np.int16)
    role = np.zeros(256, np.uint8)
    for t in range(256):
        s = int(src[t])
        if s < 0:
            continue
        shp[t] = world.chunk_shape[tnum, s]
        frm[t] = world.chunk_frame[tnum, s] & 31
        role[t] = ROLE_OWN if kinds[t] in (KIND_FLAT, KIND_FLAT_VOID) else ROLE_FILL
    return shp.reshape(16, 16), frm.reshape(16, 16), role.reshape(16, 16)


def terrain_window(world: World, cache: FlatLayerCache, tnum: int, anchor: tuple[int, int], apron: int) -> Window:
    if apron % TILE or not (0 <= apron <= CHUNK_PX):
        raise ValueError("apron must be a multiple of 8 in [0, 128]")
    cy, cx = anchor
    a = apron // TILE
    big = np.zeros((3 * CHUNK_PX, 3 * CHUNK_PX), np.uint8)
    bs = np.full((48, 48), -1, np.int16)
    bf = np.full((48, 48), -1, np.int16)
    br = np.zeros((48, 48), np.uint8)
    nbr = {}
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            t = tnum if (dy, dx) == (0, 0) else int(world.tmap[(cy + dy) % NUM_CHUNKS, (cx + dx) % NUM_CHUNKS])
            nbr[f"{dy},{dx}"] = t
            px, _ = cache.get(t)
            oy, ox = (dy + 1) * CHUNK_PX, (dx + 1) * CHUNK_PX
            big[oy:oy + CHUNK_PX, ox:ox + CHUNK_PX] = px
            s, f, r = _terrain_tiles(world, cache, t)
            ty0, tx0 = (dy + 1) * 16, (dx + 1) * 16
            bs[ty0:ty0 + 16, tx0:tx0 + 16] = s
            bf[ty0:ty0 + 16, tx0:tx0 + 16] = f
            br[ty0:ty0 + 16, tx0:tx0 + 16] = r if (dy, dx) == (0, 0) else np.where(s >= 0, ROLE_APRON, ROLE_NONE)
    p0, p1 = CHUNK_PX - apron, 2 * CHUNK_PX + apron
    t0, t1 = 16 - a, 32 + a
    origin = ((cy * 16 - a) % WORLD_TILES, (cx * 16 - a) % WORLD_TILES)
    return Window("terrain", int(tnum), big[p0:p1, p0:p1].copy(), bs[t0:t1, t0:t1].copy(), bf[t0:t1, t0:t1].copy(),
                  br[t0:t1, t0:t1].copy(), origin, float(world.terrain_usage[tnum]), TIER_TERRAIN,
                  {"anchor": [cy, cx], "neighbours": nbr, "apron": apron, "t1": t1_hex(terrain_t1(world, tnum))})


def macro_window(world: World, shape: int, apron: int) -> Window:
    """8x4 periodic unit of ``shape`` (64x32 px) padded toroidally by ``apron`` px."""
    n = min(world.shapes.nframes(shape), 32)
    unit = np.zeros((32, 64), np.uint8)
    us = np.full((4, 8), shape, np.int16)
    uf = np.zeros((4, 8), np.int16)
    ur = np.zeros((4, 8), np.uint8)
    for f in range(32):
        r, c = divmod(f, 8)
        src_f = f if f < n else f % max(n, 1)
        unit[r * 8:(r + 1) * 8, c * 8:(c + 1) * 8] = world.flat(shape, src_f)
        uf[r, c] = src_f
        ur[r, c] = ROLE_OWN if f < n else ROLE_PAD
    a = apron // TILE
    idx = np.pad(unit, apron, mode="wrap")
    ts = np.pad(us, a, mode="wrap")
    tf = np.pad(uf, a, mode="wrap")
    tr = np.pad(ur, a, mode="wrap")
    centre = np.zeros_like(tr, bool)
    centre[a:a + 4, a:a + 8] = True
    tr = np.where(centre, tr, np.where(tr == ROLE_OWN, ROLE_APRON, tr)).astype(np.uint8)
    return Window("macro", int(shape), idx, ts, tf, tr, (-1, -1), 1.0, TIER_MACRO, {"apron": apron, "frames": n})


def selfwrap_window(world: World, shape: int, frame: int, apron: int) -> Window:
    t = world.flat(shape, frame)
    a = apron // TILE
    idx = np.pad(t, apron, mode="wrap")
    n = 1 + 2 * a
    ts = np.full((n, n), shape, np.int16)
    tf = np.full((n, n), frame & 31, np.int16)
    tr = np.full((n, n), ROLE_APRON, np.uint8)
    tr[a, a] = ROLE_OWN
    return Window("selfwrap", int(shape * 32 + (frame & 31)), idx, ts, tf, tr, (-1, -1), 1.0, TIER_SELFWRAP,
                  {"apron": apron})


# ---------------------------------------------------------------------------- the full set
@dataclass
class ContextSet:
    apron: int
    terrain: list[Window]
    macro: list[Window]
    selfwrap: list[Window]
    macro_shape_list: list[int]
    coverage: dict          # (shape, frame) -> best tier available

    def all(self) -> list[Window]:
        return self.terrain + self.macro + self.selfwrap


def build_context(world: World, apron: int = 16, terrains=None, include_macro: bool = True,
                  include_selfwrap: bool = True, all_macro: bool = False) -> ContextSet:
    """Build every window. ``terrains`` restricts the terrain windows (default: all used terrains);
    macro sheets are built for macro shapes with frames lacking a real instance (or all with
    ``all_macro``); self-wrap windows for flats covered by neither."""
    cache = FlatLayerCache(world)
    anchors = choose_anchors(world)
    tlist = [int(t) for t in (world.used_terrains if terrains is None else terrains)]
    tw = [terrain_window(world, cache, t, anchors[t], apron) for t in tlist]
    covered: dict[tuple[int, int], int] = {}
    for w in tw:
        for _, _, s, f in w.own_cells():
            covered[(s, f)] = TIER_TERRAIN
    mshapes = macro_shapes(world)
    mw = []
    if include_macro:
        for s in mshapes:
            n = min(world.shapes.nframes(s), 32)
            if all_macro or any((s, f) not in covered for f in range(n)):
                mw.append(macro_window(world, s, apron))
                for f in range(n):
                    covered.setdefault((s, f), TIER_MACRO)
    sw = []
    if include_selfwrap:
        for key in world.flat_keys:
            if key not in covered:
                sw.append(selfwrap_window(world, key[0], key[1], apron))
                covered[key] = TIER_SELFWRAP
    return ContextSet(apron, tw, mw, sw, mshapes, covered)


@dataclass
class Region:
    """A world rectangle in tiles, rendered at 1x with the engine fill."""
    ty0: int
    tx0: int
    th: int
    tw: int
    idx: np.ndarray            # (th*8, tw*8) uint8 flat layer (engine fill)
    src_shape: np.ndarray      # (th, tw) int16 effective source flat shape (-1 unpainted)
    src_frame: np.ndarray      # (th, tw) int16 effective source frame & 31
    own: np.ndarray            # (th, tw) bool, the cell's own tile is a flat
    tile_shape: np.ndarray     # (th, tw) int16 the cell's own tile (flat or RLE) as stored in the terrain
    tile_frame: np.ndarray     # (th, tw) int16


def world_region(world: World, cache: FlatLayerCache, ty0: int, tx0: int, th: int, tw: int) -> Region:
    """Render world tiles [ty0, ty0+th) x [tx0, tx0+tw) (wrapping) from the per-terrain flat layers."""
    idx = np.zeros((th * TILE, tw * TILE), np.uint8)
    ss = np.full((th, tw), -1, np.int16)
    sf = np.full((th, tw), -1, np.int16)
    own = np.zeros((th, tw), bool)
    ts = np.zeros((th, tw), np.int16)
    tf = np.zeros((th, tw), np.int16)
    infos = {}
    for y in range(th):
        wy = (ty0 + y) % WORLD_TILES
        cy, ly = divmod(wy, 16)
        for x in range(tw):
            wx = (tx0 + x) % WORLD_TILES
            cx, lx = divmod(wx, 16)
            t = int(world.tmap[cy, cx])
            if t not in infos:
                px, _ = cache.get(t)
                infos[t] = (px, *_terrain_tiles(world, cache, t))
            px, s, f, r = infos[t]
            idx[y * TILE:(y + 1) * TILE, x * TILE:(x + 1) * TILE] = px[ly * TILE:(ly + 1) * TILE, lx * TILE:(lx + 1) * TILE]
            ss[y, x], sf[y, x], own[y, x] = s[ly, lx], f[ly, lx], r[ly, lx] == ROLE_OWN
            ts[y, x] = world.chunk_shape[t, ly * 16 + lx]
            tf[y, x] = world.chunk_frame[t, ly * 16 + lx]
    return Region(ty0, tx0, th, tw, idx, ss, sf, own, ts, tf)


def cut_instances(win: Window, out_hi: np.ndarray, scale: int):
    """Yield (shape, frame, ty, tx, tile_hi) for every ROLE_OWN cell of a hi-res window output."""
    T = TILE * scale
    for ty, tx, s, f in win.own_cells():
        yield s, f, ty, tx, out_hi[ty * T:(ty + 1) * T, tx * T:(tx + 1) * T]


# ---------------------------------------------------------------------------- persistence / CLI
def save_context(cs: ContextSet, out_dir: str, world: World, png: bool = False) -> dict:
    """Write ``windows_<kind>.npz`` + ``meta.json`` (and window PNGs with ``png``) under out_dir."""
    from .pngio import encode_indexed
    from .util import atomic_write_bytes, atomic_write_json
    os.makedirs(out_dir, exist_ok=True)
    meta = {"apron": cs.apron, "macro_shapes": cs.macro_shape_list, "inputs": world.input_hashes(),
            "counts": {"terrain": len(cs.terrain), "macro": len(cs.macro), "selfwrap": len(cs.selfwrap)},
            "coverage": {"terrain": sum(1 for v in cs.coverage.values() if v == TIER_TERRAIN),
                         "macro": sum(1 for v in cs.coverage.values() if v == TIER_MACRO),
                         "selfwrap": sum(1 for v in cs.coverage.values() if v == TIER_SELFWRAP)},
            "windows": []}
    for kind, lst in (("terrain", cs.terrain), ("macro", cs.macro), ("selfwrap", cs.selfwrap)):
        if not lst:
            continue
        tmp = os.path.join(out_dir, f"windows_{kind}.tmp.npz")
        np.savez_compressed(tmp, idx=np.stack([w.idx for w in lst]),
                            tile_shape=np.stack([w.tile_shape for w in lst]),
                            tile_frame=np.stack([w.tile_frame for w in lst]),
                            tile_role=np.stack([w.tile_role for w in lst]),
                            ident=np.array([w.ident for w in lst]), weight=np.array([w.weight for w in lst]),
                            origin=np.array([w.origin for w in lst]))
        os.replace(tmp, os.path.join(out_dir, f"windows_{kind}.npz"))
        for w in lst:
            meta["windows"].append({"name": w.name, "kind": w.kind, "ident": w.ident, "weight": w.weight,
                                    "origin": list(w.origin), "size": list(w.idx.shape), **w.meta})
            if png:
                atomic_write_bytes(os.path.join(out_dir, "png", kind, w.name + ".png"),
                                   encode_indexed(w.idx, world.pal8))
    atomic_write_json(os.path.join(out_dir, "meta.json"), meta)
    return meta


def load_context(out_dir: str) -> ContextSet:
    """Inverse of save_context (window meta beyond the arrays is read from meta.json)."""
    with open(os.path.join(out_dir, "meta.json")) as f:
        meta = json.load(f)
    per_name = {m["name"]: m for m in meta["windows"]}
    lists = {}
    for kind, tier in (("terrain", TIER_TERRAIN), ("macro", TIER_MACRO), ("selfwrap", TIER_SELFWRAP)):
        p = os.path.join(out_dir, f"windows_{kind}.npz")
        lst = []
        if os.path.exists(p):
            with np.load(p) as zf:
                z = {k: zf[k] for k in zf.files}      # read each array once
            for i in range(len(z["ident"])):
                w = Window(kind, int(z["ident"][i]), z["idx"][i], z["tile_shape"][i], z["tile_frame"][i],
                           z["tile_role"][i], tuple(int(v) for v in z["origin"][i]), float(z["weight"][i]), tier)
                w.meta = {k: v for k, v in per_name.get(w.name, {}).items()
                          if k not in ("name", "kind", "ident", "weight", "origin", "size")}
                lst.append(w)
        lists[kind] = lst
    cov = {}
    for kind, tier in (("terrain", TIER_TERRAIN), ("macro", TIER_MACRO), ("selfwrap", TIER_SELFWRAP)):
        for w in lists[kind]:
            for _, _, s, f in w.own_cells():
                cov.setdefault((s, f), tier)
                cov[(s, f)] = min(cov[(s, f)], tier)
    return ContextSet(meta["apron"], lists["terrain"], lists["macro"], lists["selfwrap"], meta["macro_shapes"], cov)


def main(argv=None) -> int:
    import argparse
    from .util import get_logger, timed
    ap = argparse.ArgumentParser(prog="mkctx.py", description="Build context windows (DESIGN §8.2 A1).")
    ap.add_argument("--static", default=None, help="BG STATIC dir (default: ext4 cache or $U7_BG_STATIC)")
    ap.add_argument("--out", default="/home/simonea/ultima7_exult/art_work/ctx/a16")
    ap.add_argument("--apron", type=int, default=16, help="apron in px, multiple of 8 (16-32)")
    ap.add_argument("--png", action="store_true", help="also write every window as an indexed PNG")
    ap.add_argument("--all-macro", action="store_true", help="build macro sheets for every macro shape")
    ap.add_argument("--parity-dump", default=None, help="engine --dump-art dir: compare its terrain/<t1>.png "
                    "with the Python fill port and exit")
    a = ap.parse_args(argv)
    log = get_logger()
    world = World.load(a.static)
    if a.parity_dump:
        from .fill import compare_with_dump
        r = compare_with_dump(world, a.parity_dump)
        log.info("fill parity: %d compared, %d equal, %d missing, %d mismatching", r["compared"], r["equal"],
                 r["missing"], len(r["mismatch"]))
        for m in r["mismatch"][:20]:
            log.info("  mismatch terrain %d (%s): %d px", m["terrain"], m["file"], m["pixels"])
        return 0 if not r["mismatch"] else 1
    with timed(log, "build context"):
        cs = build_context(world, a.apron, all_macro=a.all_macro)
    with timed(log, "save context"):
        meta = save_context(cs, a.out, world, a.png)
    log.info("windows: %s; frame coverage by tier: %s", meta["counts"], meta["coverage"])
    return 0
