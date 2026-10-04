"""Route 2: SR model (spandrel, 4x-NXbrz) on GPU -> Lanczos 1.5x -> back-projection colour lock ->
local snap -> per-frame consensus (DESIGN §8.4 B1; recommendation §2.4-§2.6).

Per context window (same windows as route 3):
1. the 1x window in true palette-0 RGB goes through the model (CUDA, fp16 by default, fixed batches;
   cuDNN deterministic), giving 4x; Lanczos 1.5x brings it to exactly 6x (8x models such as
   8x-Arzenal-v1-1: box 8->6);
2. cycling pixels come from route 3's label-safe plane of the same window (xBRZ + snap on the
   uniquified palette); they are excluded from the colour lock and from the static quantizer;
3. back-projection in linear RGB: ``x' = x + U(y - D(x))`` with D = 6x6 mean over the block's static
   pixels and U = NN, iterated (blocks whose 1x parent is cycling are left alone);
4. local snap of static pixels to the 3x3 parent neighbourhood (cycling indices are never targets);
5. consensus: ``mode`` on the per-instance index tiles (default) or ``medoid`` (weighted OKLab medoid of
   the per-instance RGB tiles, then an in-tile snap); then the in-tile index rules are enforced.
The model file's SHA-256 is verified and recorded in every sidecar.

``--redundancy N`` repeats the CPU stage of every window until N results agree (exceptions are
retried; ``util.redundant_call``), because the production machine shows transient CPU/RAM faults
under all-core load. Fault counts and the GPU time / peak VRAM go into ``tiles.json``. Independent
whole runs are compared with ``vote.py``. ``--subset SPEC`` restricts a run to the windows holding the
keys picked by ``families.select_keys`` (look comparisons on a representative frame set); every
picked key's consensus sees the same windows as in a full run (only the GPU batching differs).
GPU inference of the next chunk of windows overlaps the CPU stage of the current one; results are
consumed in window order, so the output does not depend on timing.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
import time
from dataclasses import asdict, dataclass
from multiprocessing import get_context

import numpy as np

from . import S_ART
from .consensus import MedoidConsensus, ModeConsensus
from .ctx import ContextSet, build_context, load_context
from .families import frame_families, select_keys
from .io import World, sha256_file
from .pack import save_candidates
from .quant import dropped_sparkles, linear_to_srgb, srgb_to_linear
from .route3 import R3Kernel, R3Params
from .rules import IS_CYCLING
from .util import Progress, clamp_workers, get_logger, key_name, redundant_call, single_thread_env

DEFAULT_MODEL_DIR = "/home/simonea/ultima7_exult/tmp/sr_bakeoff/models"
DEFAULT_CUDA_PY = "/home/simonea/ultima7_exult/tmp/factcheck/venv_cu130/bin/python"
KNOWN_MODELS = {
    "4x-NXbrz": {"file": "4x-NXbrz.pth", "sha256": "a822ae2dcf86934e53ad3201d5a39c86d3da1f75ff186e5b7be340b3a605dccd",
                 "scale": 4, "license": "CC-BY-NC-SA-4.0"},
    "8x-Arzenal-v1-1": {"file": "8x-Arzenal-v1-1.pth",
                        "sha256": "2166b4c42fb9cea14fc61f55711c3f4d2cfe9c922d902a1a0f0b416dfc2cab35",
                        "scale": 8, "license": "CC-BY-NC-SA-4.0"},
    "8x-MS-Unpainter": {"file": "8x-MS-Unpainter.pth",
                        "sha256": "f5780ebec9335dbebe71c328da319d699a413d7a913d704b4b73c91ccb028503",
                        "scale": 8, "license": "CC-BY-NC-SA-4.0"},
}
DEFAULT_OUT = "/home/simonea/ultima7_exult/art_work/raw"
MAX_FAULT_EVENTS = 200


@dataclass
class R2Params:
    model: str = "4x-NXbrz"
    model_path: str = ""
    scale: int = S_ART
    fp16: bool = True
    batch: int = 8
    backproject_iters: int = 2
    ramp_expand: bool = False
    consensus: str = "mode"          # mode | medoid
    medoid_cap: int = 48
    apron: int = 16
    weighted: bool = True
    chunk: int = 64                  # windows per GPU->CPU hand-off
    subset: str = ""                 # families.select_keys spec; "" = every flat
    redundancy: int = 1              # CPU stage repeated until this many results agree


def model_path_for(params: R2Params) -> tuple[str, dict]:
    info = KNOWN_MODELS.get(params.model, {})
    return params.model_path or os.path.join(DEFAULT_MODEL_DIR, info.get("file", params.model + ".pth")), info


# ---------------------------------------------------------------------------- GPU side
class SRModel:
    """spandrel model on CUDA (or CPU if no GPU)."""

    def __init__(self, path: str, fp16: bool = True, device: str | None = None):
        os.environ.setdefault("CUBLAS_WORKSPACE_CONFIG", ":4096:8")
        import torch
        from spandrel import ModelLoader
        torch.backends.cudnn.deterministic = True
        torch.backends.cudnn.benchmark = False
        torch.use_deterministic_algorithms(True, warn_only=True)
        self.torch = torch
        self.device = device or ("cuda" if torch.cuda.is_available() else "cpu")
        d = ModelLoader().load_from_file(path)
        self.scale = int(d.scale)
        self.model = d.model.eval().to(self.device)
        self.fp16 = bool(fp16 and self.device == "cuda")
        if self.fp16:
            self.model = self.model.half()
        if self.device == "cuda":
            torch.cuda.reset_peak_memory_stats()

    def __call__(self, batch_rgb: np.ndarray) -> np.ndarray:
        """(B, H, W, 3) uint8 -> (B, sH, sW, 3) float32 in 0..255."""
        torch = self.torch
        t = torch.from_numpy(np.ascontiguousarray(batch_rgb)).to(self.device).permute(0, 3, 1, 2)
        t = t.half() if self.fp16 else t.float()
        with torch.inference_mode():
            o = self.model(t / 255.0)
        return (o.float().clamp(0, 1) * 255.0).permute(0, 2, 3, 1).cpu().numpy()

    def vram(self) -> dict:
        """Peak CUDA memory of this process since the model was loaded (torch allocator)."""
        if self.device != "cuda":
            return {"device": self.device}
        c = self.torch.cuda
        mb = lambda v: round(v / 2 ** 20, 1)  # noqa: E731
        return {"device": self.device, "device_name": c.get_device_name(0),
                "max_allocated_mb": mb(c.max_memory_allocated()), "max_reserved_mb": mb(c.max_memory_reserved()),
                "total_mb": mb(c.get_device_properties(0).total_memory)}


def to_6x(sr: np.ndarray, model_scale: int, h: int, w: int, scale: int = S_ART) -> np.ndarray:
    """Bring a model output (sh, sw, 3) to exactly (h*scale, w*scale, 3): Lanczos up, box down."""
    from PIL import Image
    tw, th = w * scale, h * scale
    if sr.shape[:2] == (th, tw):
        return sr.astype(np.float32)
    filt = Image.LANCZOS if model_scale < scale else Image.BOX
    out = np.empty((th, tw, 3), np.float32)
    for c in range(3):
        out[..., c] = np.asarray(Image.fromarray(np.ascontiguousarray(sr[..., c], np.float32), "F").resize((tw, th), filt))
    return np.clip(out, 0, 255)


def backproject(rgb6: np.ndarray, src: np.ndarray, pal8: np.ndarray, cyc6: np.ndarray, scale: int = 6,
                iters: int = 2) -> np.ndarray:
    """Colour lock in linear RGB: every static 6x6 block's mean over its non-cycling pixels moves to the
    1x source colour (``x' = x + U(y - D(x))``, iterated because of clipping)."""
    h, w = src.shape
    x = srgb_to_linear(rgb6)
    y = srgb_to_linear(pal8[src])
    stat = (~cyc6).astype(np.float64)
    sb = stat.reshape(h, scale, w, scale)
    nstat = sb.sum((1, 3))
    par_static = ~IS_CYCLING[src]
    for _ in range(iters):
        xb = x.reshape(h, scale, w, scale, 3)
        mean = (xb * sb[..., None]).sum((1, 3)) / np.maximum(nstat, 1)[..., None]
        corr = np.where(((nstat > 0) & par_static)[..., None], y - mean, 0.0)
        x = np.clip(x + np.repeat(np.repeat(corr, scale, 0), scale, 1) * stat[..., None], 0.0, 1.0)
    return linear_to_srgb(x)


# ---------------------------------------------------------------------------- CPU side
_K: R3Kernel | None = None
_P: R2Params | None = None
_INIT_ARGS: tuple | None = None
_K_DIGEST: bytes | None = None


def _kernel_digest(k: R3Kernel) -> bytes:
    """Digest of the lookup tables a worker keeps for its whole life (fault detection)."""
    from . import quant, rules
    h = hashlib.blake2b(digest_size=16)
    for a in (k.qz.pal8, k.qz.lab, k.qz.pal_u, k.qz.lab_u, k.qz.ramp_lut, k.qz.ramp_members, k.lut,
              rules.IS_CYCLING, rules.CYCLE_ID, quant.STATIC_MASK):
        h.update(np.ascontiguousarray(a).tobytes())
    return h.digest()


def _make_kernel(pal8, pal6, p: R2Params) -> R3Kernel:
    return R3Kernel(pal8, pal6, R3Params(scale=p.scale, ramp_expand=p.ramp_expand))


def _init(pal8, pal6, pdict):
    global _K, _P, _INIT_ARGS, _K_DIGEST
    single_thread_env()
    _P = R2Params(**pdict)
    _INIT_ARGS = (np.array(pal8, copy=True), np.array(pal6, copy=True))
    _K = _make_kernel(pal8, pal6, _P)
    _K_DIGEST = _kernel_digest(_K)


def _check_kernel() -> int:
    """Rebuild the worker's kernel when its tables changed since start-up; 1 if it was rebuilt."""
    global _K
    if _kernel_digest(_K) == _K_DIGEST:
        return 0
    _K = _make_kernel(*_INIT_ARGS, _P)
    if _kernel_digest(_K) != _K_DIGEST:
        raise RuntimeError("worker tables differ from start-up even after a rebuild")
    return 1


def post_window(idx: np.ndarray, sr: np.ndarray, model_scale: int, k: R3Kernel, p: R2Params):
    """Model output of one window -> (final 6x indices, colour-locked 6x RGB, route-3 plane)."""
    h, w = idx.shape
    rgb6 = to_6x(sr, model_scale, h, w, p.scale)
    r3 = k.window_xbrz(idx)
    cyc6 = IS_CYCLING[r3]
    rgb6 = backproject(rgb6, idx, k.qz.pal8, cyc6, p.scale, p.backproject_iters)
    stat = k.qz.local_snap(rgb6, idx, p.scale, 1, forbid=IS_CYCLING, ramp_expand=p.ramp_expand)
    return np.where(cyc6, r3, stat).astype(np.uint8), rgb6, r3


def cut_window(win, final: np.ndarray, rgb6: np.ndarray, r3: np.ndarray, p: R2Params) -> list:
    """(shape, frame, index tile | None, RGB tile | None, route-3 tile | None) per ROLE_OWN cell."""
    T = 8 * p.scale
    out = []
    for ty, tx, s, f in win.own_cells():
        sl = (slice(ty * T, (ty + 1) * T), slice(tx * T, (tx + 1) * T))
        if p.consensus == "mode":
            out.append((s, f, final[sl].copy(), None, None))
        else:
            out.append((s, f, None, np.clip(np.rint(rgb6[sl]), 0, 255).astype(np.uint8), r3[sl].copy()))
    return out


def instances_digest(inst: list) -> bytes:
    h = hashlib.blake2b(digest_size=16)
    for s, f, ti, trgb, tr3 in inst:
        h.update(f"{s},{f};".encode())
        for a in (ti, trgb, tr3):
            if a is not None:
                h.update(np.ascontiguousarray(a).tobytes())
    return h.digest()


def _post_job(job):
    win, sr, model_scale = job

    def once():
        final, rgb6, r3 = post_window(win.idx, sr, model_scale, _K, _P)
        return cut_window(win, final, rgb6, r3, _P)

    return redundant_call(once, _P.redundancy, digest=instances_digest, before=_check_kernel)


def run(world: World, out_dir: str, params: R2Params, workers: int = 8, cs: ContextSet | None = None,
        limit: int | None = None, log=None) -> dict:
    log = log or get_logger()
    t0 = time.time()
    path, info = model_path_for(params)
    sha = sha256_file(path)
    if info and sha != info["sha256"]:
        raise RuntimeError(f"model {path} sha256 {sha} != expected {info['sha256']}")
    model = SRModel(path, params.fp16)
    log.info("model %s (x%d) on %s fp16=%s sha256 %s", params.model, model.scale, model.device, model.fp16, sha[:16])
    if cs is None:
        cs = build_context(world, params.apron)
    windows = (cs.terrain[:limit] if limit else cs.terrain) + cs.macro + cs.selfwrap
    keyset, subset_info = None, None
    if params.subset:
        keys, groups = select_keys(world, params.subset)
        keyset = set(keys)
        windows = [w for w in windows if any((s, f) in keyset for _, _, s, f in w.own_cells())]
        subset_info = {"spec": params.subset, "keys": len(keys), "groups": groups, "windows": len(windows)}
        log.info("subset %r: %d keys in %d windows", params.subset, len(keys), len(windows))
    workers = clamp_workers(workers)
    pdict = asdict(params)
    pool = get_context("spawn").Pool(workers, _init, (world.pal8, world.pal6, pdict)) if workers > 1 else None
    if pool is None:
        _init(world.pal8, world.pal6, pdict)
    mode = ModeConsensus()
    med = MedoidConsensus(params.medoid_cap) if params.consensus == "medoid" else None
    r3c = ModeConsensus() if params.consensus == "medoid" else None
    prog = Progress(log, len(windows), "windows")
    faults = {"windows": 0, "attempts": 0, "errors": 0, "mismatches": 0, "rebuilds": 0, "events": []}
    gpu_s = 0.0

    def infer(chunk):
        nonlocal gpu_s
        outs = [None] * len(chunk)
        groups: dict[tuple, list[int]] = {}
        for i, w in enumerate(chunk):
            groups.setdefault(w.idx.shape, []).append(i)
        tg = time.time()
        for shp, ids in groups.items():
            for b0 in range(0, len(ids), params.batch):
                bi = ids[b0:b0 + params.batch]
                res = model(np.stack([world.pal8[chunk[i].idx] for i in bi]))
                for i, r in zip(bi, res):
                    outs[i] = r.astype(np.float16)
        gpu_s += time.time() - tg
        return outs

    def consume(chunk, results):
        for w, (inst, st) in zip(chunk, results):
            faults["attempts"] += st["attempts"]
            for k in ("errors", "mismatches", "rebuilds"):
                faults[k] += st[k]
            if st["errors"] or st["mismatches"] or st["rebuilds"]:
                faults["windows"] += 1
                if len(faults["events"]) < MAX_FAULT_EVENTS:
                    faults["events"].append({"window": w.name, **st})
                log.warning("window %s: %d attempts, %d errors, %d mismatches, %d rebuilds %s", w.name,
                            st["attempts"], st["errors"], st["mismatches"], st["rebuilds"], st["error_text"][:1])
            wt = w.weight if params.weighted else 1.0
            for s, f, ti, trgb, tr3 in inst:
                if keyset is not None and (s, f) not in keyset:
                    continue
                if ti is not None:
                    mode.add((s, f), ti, wt, w.tier)
                else:
                    med.add((s, f), trgb, wt, w.tier)
                    r3c.add((s, f), tr3, wt, w.tier)
            prog.step()

    pending = None
    try:
        for c0 in range(0, len(windows), params.chunk):
            chunk = windows[c0:c0 + params.chunk]
            outs = infer(chunk)                        # overlaps the workers' CPU stage of the previous chunk
            jobs = [(w, o, model.scale) for w, o in zip(chunk, outs)]
            results = pool.imap(_post_job, jobs, chunksize=2) if pool else map(_post_job, jobs)
            if pending is not None:
                consume(*pending)
            pending = (chunk, results)
        if pending is not None:
            consume(*pending)
    except BaseException:
        if pool is not None:
            pool.terminate()
            pool = None
        raise
    finally:
        if pool is not None:
            pool.close()
            pool.join()
    vram = model.vram()
    fams = frame_families(world)
    k = R3Kernel(world.pal8, world.pal6, R3Params(scale=params.scale))
    tiles, per_key = {}, {}
    keys = mode.keys() if params.consensus == "mode" else med.keys()
    for key in keys:
        src = world.flat(*key)
        if params.consensus == "mode":
            tile, st = mode.result(key, parent=src)
        else:
            rgb, st = med.result(key)
            cy, _ = r3c.result(key, parent=src)
            stat = k.qz.local_snap(rgb, src, params.scale, 1, forbid=IS_CYCLING, ramp_expand=params.ramp_expand)
            tile = np.where(IS_CYCLING[cy], cy, stat).astype(np.uint8)
        tile, est = k.qz.enforce(tile, src, params.scale)
        tiles[key] = tile
        per_key[key_name(*key)] = {**st, "enforce": est, "dropped_sparkles": dropped_sparkles(tile, src, params.scale),
                                   "family": fams.get(key, "other")}
    import torch
    meta = {"route": f"r2-{params.model.lower()}", "origin": f"route2 {params.model} spandrel {params.consensus}",
            "params": pdict, "model_sha256": sha, "model_file": path, "model_license": info.get("license"),
            "model_scale": model.scale, "torch": torch.__version__, "device": model.device,
            "inputs": world.input_hashes(), "keys": per_key,
            "windows": {"terrain": len(cs.terrain), "macro": len(cs.macro), "selfwrap": len(cs.selfwrap),
                        "processed": len(windows)},
            "subset": subset_info, "faults": faults, "gpu": {"seconds": round(gpu_s, 1), **vram},
            "gpu_seconds": round(gpu_s, 1), "seconds": round(time.time() - t0, 1)}
    save_candidates(out_dir, tiles, meta)
    log.info("route2 done: %d tiles in %.1fs (GPU %.1fs, peak alloc %s MB); faults: %d windows, %d errors, "
             "%d mismatches -> %s", len(tiles), time.time() - t0, gpu_s, vram.get("max_allocated_mb"),
             faults["windows"], faults["errors"], faults["mismatches"], out_dir)
    return meta


_REGION_MODEL = {}


def region_render(world: World, idx: np.ndarray, params: dict | None = None) -> np.ndarray:
    """Region-level route-2 render of a 1x index region (QA C1 baseline). Needs torch + spandrel."""
    fields = {k: v for k, v in (params or {}).items() if k in R2Params.__dataclass_fields__}
    p = R2Params(**fields)
    path, _ = model_path_for(p)
    if path not in _REGION_MODEL:
        _REGION_MODEL[path] = SRModel(path, p.fp16)
    m = _REGION_MODEL[path]
    k = R3Kernel(world.pal8, world.pal6, R3Params(scale=p.scale, ramp_expand=p.ramp_expand))
    sr = m(world.pal8[idx][None])[0]
    return post_window(idx, sr, m.scale, k, p)[0]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="route2.py", description=__doc__.split("\n")[0])
    ap.add_argument("--static", default=None)
    ap.add_argument("--out", default=None, help="candidate dir (default art_work/raw/r2-<model>-<consensus>)")
    ap.add_argument("--ctx", default=None, help="load prebuilt context from this dir (mkctx.py)")
    ap.add_argument("--model", default="4x-NXbrz", help=f"known: {', '.join(KNOWN_MODELS)} (others need "
                    "--model-path and are not SHA-checked)")
    ap.add_argument("--model-path", default="")
    ap.add_argument("--fp32", action="store_true")
    ap.add_argument("--batch", type=int, default=8)
    ap.add_argument("--chunk", type=int, default=64, help="windows per GPU->CPU hand-off")
    ap.add_argument("--iters", type=int, default=2, help="back-projection iterations")
    ap.add_argument("--consensus", choices=("mode", "medoid"), default="mode")
    ap.add_argument("--ramp-expand", action="store_true")
    ap.add_argument("--apron", type=int, default=16)
    ap.add_argument("--unweighted", action="store_true")
    ap.add_argument("--subset", default="", help="only these keys, e.g. 'water:50,shore:50,roads=21+24:50' "
                    "(families.select_keys)")
    ap.add_argument("--redundancy", type=int, default=1, help="repeat each window's CPU stage until N results "
                    "agree (2 on machines with transient faults)")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--limit", type=int, default=None)
    a = ap.parse_args(argv)
    p = R2Params(model=a.model, model_path=a.model_path, fp16=not a.fp32, batch=a.batch, backproject_iters=a.iters,
                 consensus=a.consensus, ramp_expand=a.ramp_expand, apron=a.apron, weighted=not a.unweighted,
                 chunk=a.chunk, subset=a.subset, redundancy=a.redundancy)
    world = World.load(a.static)
    cs = load_context(a.ctx) if a.ctx else None
    out = a.out or os.path.join(DEFAULT_OUT, f"r2-{a.model.lower()}-{a.consensus}" + ("-subset" if a.subset else ""))
    run(world, out, p, a.workers, cs, a.limit)
    return 0


if __name__ == "__main__":
    sys.exit(main())
