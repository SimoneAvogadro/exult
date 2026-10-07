"""Route 3: xBRZ 6x + local snap + per-frame mode consensus (DESIGN §8.2 A2; CPU, deterministic).

Per context window:
1. xBRZ 1.9 at native 6x on the **uniquified** palette (duplicates nudged 1-3 LSB);
2. **local snap**: every 6x pixel becomes the OKLab-nearest index among its 1x parent's 3x3
   (edge-clamped) neighbourhood, distances in the uniquified palette so exact colours invert exactly
   (this also upscales the cycling mask label-safely: E0-E7/FE glints survive);
   ``--ramp-expand`` widens the candidates to the static members of their engine ramps;
3. variant ``hybrid`` (material-boundary hybrid, against xBRZ "worms" on noisy grass/dirt/sand):
   xBRZ+snap runs on a ramp-id plane only; texture interiors stay NN, and pixels whose ramp differs
   from the upscaled ramp map take the first 3x3 parent-neighbour (centre first) of that ramp;
   ``mixed`` = hybrid for the shapes of ``--hybrid-families``, xbrz elsewhere.
Then the ROLE_OWN cells are cut out, a weighted **mode** consensus per (shape, frame) is taken
(real map context first, then macro sheets, then self-wrap), and the in-tile index rules are
enforced (P4 cycling restriction, no 0xFF, 0x00 only where the source has it).
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from dataclasses import asdict, dataclass
from multiprocessing import get_context

import numpy as np

from . import S_ART
from .consensus import ModeConsensus
from .ctx import ContextSet, build_context, load_context
from .families import frame_families, is_hybrid_family
from .io import World, sha256_file
from .pngio import encode_indexed
from .quant import Quantizer, dropped_sparkles, srgb_to_oklab
from .rules import engine_ramps, ramp_lut
from .scalers import XBRZ_PATCHED_SOURCE_SHA256, xbrz_lib_path, xbrz_rgb
from .util import (Progress, atomic_write_bytes, clamp_workers, get_logger, key_name, single_thread_env)
from .pack import save_candidates

DEFAULT_OUT = "/home/simonea/ultima7_exult/art_work/raw"


@dataclass
class R3Params:
    scale: int = S_ART
    variant: str = "xbrz"                       # xbrz | hybrid | mixed
    hybrid_families: tuple = ("grass", "dirt", "sand")
    radius: int = 1
    ramp_expand: bool = False
    eq_tol: float = 30.0
    center_bias: float = 4.0
    dominant: float = 3.6
    steep: float = 2.4
    apron: int = 16
    weighted: bool = True
    restore_sparkles: bool = False


# ---------------------------------------------------------------------------- per-window kernels
class R3Kernel:
    """Everything a worker needs; constructed once per process."""

    def __init__(self, pal8: np.ndarray, pal6: np.ndarray, p: R3Params):
        self.p = p
        self.lut = ramp_lut(engine_ramps(pal6))
        self.qz = Quantizer(pal8, self.lut)
        # ramp pseudo-palette for the hybrid: label k = ramp id + 1 (0 = index 0), mean colour of the ramp
        nr = int(self.lut.max()) + 1
        rc = np.zeros((256, 3), np.float64)
        rc[0] = pal8[0]
        for r in range(nr):
            m = self.lut == r
            rc[r + 1] = pal8[m].mean(0)
        for i in range(nr + 1, 256):                 # unused labels: distinct dummy colours
            rc[i] = (i, 255 - i, (i * 7) % 256)
        rc8 = np.clip(np.rint(rc), 0, 255).astype(np.uint8)
        from .quant import unique_rgb_palette
        self.ramp_rgb, _ = unique_rgb_palette(rc8)
        self.ramp_lab = srgb_to_oklab(self.ramp_rgb)
        self.label_of = (self.lut.astype(np.int16) + 1).astype(np.uint8)       # index -> ramp label

    def xbrz(self, rgb: np.ndarray) -> np.ndarray:
        p = self.p
        return xbrz_rgb(rgb, p.scale, p.eq_tol, p.center_bias, p.dominant, p.steep)

    def window_xbrz(self, idx: np.ndarray) -> np.ndarray:
        """xBRZ 6x on the uniquified palette + local snap -> 6x index plane."""
        x6 = self.xbrz(self.qz.pal_u[idx])
        return self.qz.local_snap(x6, idx, self.p.scale, self.p.radius, ramp_expand=self.p.ramp_expand,
                                  lab_pal=self.qz.lab_u)

    def window_hybrid(self, idx: np.ndarray) -> np.ndarray:
        """Material-boundary hybrid: xBRZ on ramp labels, NN texture, ramp-consistent repair."""
        S = self.p.scale
        R = self.label_of[idx]
        xr = self.xbrz(self.ramp_rgb[R])
        R6 = self.qz.local_snap(xr, R, S, 1, lab_pal=self.ramp_lab)
        h, w = idx.shape
        pi = np.pad(idx, 1, mode="edge")
        order = [(0, 0), (-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)]
        cand = np.stack([pi[1 + dy:1 + dy + h, 1 + dx:1 + dx + w] for dy, dx in order], -1)
        cand6 = np.repeat(np.repeat(cand, S, 0), S, 1)
        ok = self.label_of[cand6] == R6[..., None]
        first = np.where(ok.any(-1), ok.argmax(-1), 0)
        return np.take_along_axis(cand6, first[..., None], -1)[..., 0]

    def window(self, idx: np.ndarray, hybrid: bool) -> np.ndarray:
        return self.window_hybrid(idx) if hybrid else self.window_xbrz(idx)


_K: R3Kernel | None = None
_HYB: set = set()
_RETRIES = 0
# Exceptions a transient hardware error (a flipped bit in an index array) produces in the kernels;
# a deterministic bug raises again on every attempt and still ends the run (see vote.py).
TRANSIENT_ERRORS = (IndexError, ValueError, FloatingPointError, OverflowError, MemoryError)
DEFAULT_RETRIES = 2


def _init(pal8, pal6, params_dict, hybrid_keys, retries=0):
    global _K, _HYB, _RETRIES
    single_thread_env()
    _K = R3Kernel(pal8, pal6, R3Params(**params_dict))
    _HYB = set(tuple(k) for k in hybrid_keys)
    _RETRIES = int(retries)


def _run_window(job):
    """Worker: (window, variant) -> (instances, raw plane or None, retries used). A window whose
    computation raises one of TRANSIENT_ERRORS is recomputed up to ``_RETRIES`` times."""
    for attempt in range(_RETRIES + 1):
        try:
            return (*_window_instances(job), attempt)
        except TRANSIENT_ERRORS as e:
            if attempt >= _RETRIES:
                raise
            get_logger().warning("window %s: %r; recomputing (transient error?)", job[0].name, e)


def _window_instances(job):
    """(window, variant) -> (list of (shape, frame, tile48) instances, raw plane or None)."""
    win, variant = job
    S = _K.p.scale
    planes = {}
    if variant in ("xbrz", "mixed"):
        planes["xbrz"] = _K.window(win.idx, False)
    if variant in ("hybrid", "mixed"):
        planes["hybrid"] = _K.window(win.idx, True)
    out = []
    for ty, tx, s, f in win.own_cells():
        if variant == "mixed":
            plane = planes["hybrid"] if (s, f) in _HYB else planes["xbrz"]
        else:
            plane = planes[variant]
        T = 8 * S
        out.append((s, f, plane[ty * T:(ty + 1) * T, tx * T:(tx + 1) * T].copy()))
    keep = None
    if win.meta.get("_keep_raw"):
        keep = planes.get("xbrz", planes.get("hybrid"))
    return out, keep


# ---------------------------------------------------------------------------- driver
def hybrid_key_set(world: World, params: R3Params) -> list[tuple[int, int]]:
    """Keys whose frame family (families.frame_families) lies within ``params.hybrid_families``."""
    ff = frame_families(world)
    return sorted(k for k, fam in ff.items() if is_hybrid_family(fam, params.hybrid_families))


def run(world: World, out_dir: str, params: R3Params, workers: int = 8, cs: ContextSet | None = None,
        keep_raw: bool = False, limit: int | None = None, log=None, retries: int = DEFAULT_RETRIES) -> dict:
    """``retries``: recompute a window or a key whose computation raised one of TRANSIENT_ERRORS up
    to that many times (counted in meta['retries']; the output does not depend on it)."""
    log = log or get_logger()
    t0 = time.time()
    if cs is None:
        log.info("building context windows (apron %d)", params.apron)
        cs = build_context(world, params.apron)
    windows = cs.all()
    if limit:
        windows = cs.terrain[:limit] + cs.macro + cs.selfwrap
    fams = frame_families(world)
    hybrid_keys = hybrid_key_set(world, params) if params.variant == "mixed" else []
    if keep_raw:
        for w in windows:
            w.meta["_keep_raw"] = True
    cons = ModeConsensus()
    workers = clamp_workers(workers)
    log.info("route3 %s: %d windows (%d terrain, %d macro, %d selfwrap) on %d workers", params.variant,
             len(windows), len(cs.terrain) if not limit else limit, len(cs.macro), len(cs.selfwrap), workers)
    prog = Progress(log, len(windows), "windows")
    jobs = ((w, params.variant) for w in windows)
    pdict = asdict(params)
    retried = {"windows": 0, "keys": 0}
    if workers > 1:
        ctx = get_context("fork")
        pool = ctx.Pool(workers, _init, (world.pal8, world.pal6, pdict, hybrid_keys, retries))
        it = pool.imap(_run_window, jobs, chunksize=4)
    else:
        _init(world.pal8, world.pal6, pdict, hybrid_keys, retries)
        pool = None
        it = map(_run_window, jobs)
    try:
        for w, (inst, raw, rt) in zip(windows, it):
            retried["windows"] += rt
            wt = w.weight if params.weighted else 1.0
            for s, f, tile in inst:
                cons.add((s, f), tile, wt, w.tier)
            if raw is not None:
                atomic_write_bytes(os.path.join(out_dir, "windows", w.name + ".png"), encode_indexed(raw, world.pal8))
            prog.step()
    finally:
        if pool is not None:
            pool.close()
            pool.join()
    log.info("consensus over %d keys", len(cons.keys()))
    k = R3Kernel(world.pal8, world.pal6, params)
    tiles, per_key = {}, {}
    hset = set(hybrid_keys)
    for key in cons.keys():
        src = world.flat(*key)
        for attempt in range(retries + 1):
            try:
                tile, st = cons.result(key, parent=src)           # pure: safe to recompute
                tile, est = k.qz.enforce(tile, src, params.scale, restore_sparkles=params.restore_sparkles)
                dropped = dropped_sparkles(tile, src, params.scale)
                break
            except TRANSIENT_ERRORS as e:
                if attempt >= retries:
                    raise
                retried["keys"] += 1
                log.warning("consensus %s: %r; recomputing (transient error?)", key_name(*key), e)
        tiles[key] = tile
        per_key[key_name(*key)] = {**st, "enforce": est, "dropped_sparkles": dropped,
                                   "family": fams.get(key, "other"),
                                   "variant": ("hybrid" if key in hset else "xbrz")
                                   if params.variant == "mixed" else params.variant}
    lib = xbrz_lib_path()
    meta = {"route": f"r3-{params.variant}", "origin": f"route3 xbrz19 {params.variant}",
            "params": pdict, "hybrid_keys": [key_name(*k) for k in hybrid_keys],
            "xbrz": {"lib": lib, "lib_sha256": sha256_file(lib) if os.path.exists(lib) else None,
                     "patched_source_sha256": XBRZ_PATCHED_SOURCE_SHA256},
            "model_sha256": None, "inputs": world.input_hashes(), "keys": per_key,
            "windows": {"terrain": len(cs.terrain), "macro": len(cs.macro), "selfwrap": len(cs.selfwrap)},
            "seconds": round(time.time() - t0, 1), "retries": retried}
    save_candidates(out_dir, tiles, meta)
    log.info("route3 done: %d tiles in %.1fs (retried: %s) -> %s", len(tiles), time.time() - t0, retried, out_dir)
    return meta


def region_render(world: World, idx: np.ndarray, params: R3Params, kernel: R3Kernel | None = None) -> np.ndarray:
    """Region-level route-3 render of an arbitrary 1x index region (QA C1 baseline)."""
    k = kernel or R3Kernel(world.pal8, world.pal6, params)
    return k.window(idx, params.variant == "hybrid")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="route3.py", description=__doc__.split("\n")[0])
    ap.add_argument("--static", default=None)
    ap.add_argument("--out", default=None, help="candidate dir (default art_work/raw/r3-<variant>)")
    ap.add_argument("--ctx", default=None, help="load prebuilt context from this dir (mkctx.py)")
    ap.add_argument("--variant", choices=("xbrz", "hybrid", "mixed"), default="xbrz")
    ap.add_argument("--hybrid-families", default="grass,dirt,sand")
    ap.add_argument("--apron", type=int, default=16)
    ap.add_argument("--ramp-expand", action="store_true")
    ap.add_argument("--unweighted", action="store_true", help="every instance weighs 1 (default: map usage)")
    ap.add_argument("--restore-sparkles", action="store_true")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--limit", type=int, default=None, help="only the first N terrain windows (smoke runs)")
    ap.add_argument("--keep-raw", action="store_true", help="save every 6x window plane as PNG")
    ap.add_argument("--retries", type=int, default=DEFAULT_RETRIES,
                    help="recompute a window/key after a transient IndexError/ValueError (default %(default)s)")
    a = ap.parse_args(argv)
    p = R3Params(variant=a.variant, hybrid_families=tuple(x for x in a.hybrid_families.split(",") if x),
                 apron=a.apron, ramp_expand=a.ramp_expand, weighted=not a.unweighted,
                 restore_sparkles=a.restore_sparkles)
    world = World.load(a.static)
    cs = load_context(a.ctx) if a.ctx else None
    out = a.out or os.path.join(DEFAULT_OUT, f"r3-{a.variant}")
    run(world, out, p, a.workers, cs, a.keep_raw, a.limit, retries=a.retries)
    return 0


if __name__ == "__main__":
    sys.exit(main())
