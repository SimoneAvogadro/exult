"""Route 1: structure-locked diffusion refine (recommendation §1.1 route 1, §2.4; diffusion.md §5, §9).

Per context window (or a crop of one):
1. base ("mould"): the route-3 6x index plane of the window (``mixed``: material hybrid on the
   grass/dirt/sand families, xBRZ elsewhere, i.e. the ``packs/bg`` look; or ``xbrz``), in palette-0
   RGB, Lanczos 2x to **12x**;
2. refine with **SDXL base 1.0 + xinsir controlnet-tile-sdxl-1.0** (diffusers img2img + ControlNet,
   fp16, fp16-fix VAE, DPM++ 2M Karras, fixed seed): denoise 0.30-0.50, CN strength 0.6-0.9, the base
   image is both the init image and the control image;
3. box 2:1 to 6x, then the route-2 post (``route2.post_window``): back-projection colour lock against
   the 1x source in linear RGB, cycling pixels from route 3's label-safe plane, local snap of static
   pixels to the parent's 3x3 neighbourhood (optionally ramp-expanded);
4. cut the ROLE_OWN cells and take a per-(shape, frame) consensus: ``mode`` (per-pixel weighted mode,
   as routes 2 and 3) or ``pick`` (the whole-instance OKLab medoid, which keeps one coherent texture
   instead of a per-pixel patchwork), then ``Quantizer.enforce``.

``pilot`` (Phase B pilot, ``docs-hires/art/phase_b_pilot.md``) runs this on 12x12-tile crops of a16
terrain windows and evaluates the 8x8-tile centre against ``packs/bg`` (route-3 hybrid) and
``packs/bg-r2`` (4x-NXbrz): QA B1-B4 / A2 / A3 per tile, C1 seam ratio, C2 edge excess, D1, detail
measures, and 1:1 comparison sheets. Needs torch + diffusers (``venv-gpu``); models from ``HF_HOME``.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from dataclasses import asdict, dataclass, field

import numpy as np

from . import S_ART, TILE
from .consensus import ModeConsensus
from .ctx import ROLE_OWN, load_context, world_region
from .fill import FlatLayerCache
from .io import World
from .quant import srgb_to_oklab
from .route3 import R3Kernel, R3Params, hybrid_key_set
from .rules import IS_CYCLING, engine_ramps, ramp_lut
from .util import atomic_write_bytes, atomic_write_json, get_logger, key_name

HF_HOME = "/home/simonea/ultima7_exult/models/hf"
BASE_MODEL = "stabilityai/stable-diffusion-xl-base-1.0"
CN_MODEL = "xinsir/controlnet-tile-sdxl-1.0"
VAE_MODEL = "madebyollin/sdxl-vae-fp16-fix"
WORK = 12                       # working scale of the diffusion canvas (1 source px = 1.5 latent cells)

NEGATIVE = ("blurry, soft focus, text, letters, watermark, signature, objects, buildings, people, animals, "
            "perspective, horizon, sky, frame, border, photo, photorealistic, 3d render, jpeg artifacts, noise")
PROMPTS = {
    "grass": "top-down view of a grass meadow ground texture, 16-bit RPG game terrain, hand-painted, "
             "detailed grass blades, crisp, even lighting",
    "dirt": "top-down view of bare brown dirt ground texture with small pebbles and clods, 16-bit RPG game "
            "terrain, hand-painted, detailed, crisp, even lighting",
    "sand": "top-down view of sand ground texture with small stones, 16-bit RPG game terrain, hand-painted, "
            "detailed grains, crisp, even lighting",
    "grass_dirt": "top-down view of grass ground with patches of bare brown dirt, 16-bit RPG game terrain, "
                  "hand-painted, detailed grass blades, crisp, even lighting",
    "grass_sand": "top-down view of grass ground with a patch of sand, 16-bit RPG game terrain, hand-painted, "
                  "detailed, crisp, even lighting",
    "marsh": "top-down view of marsh ground, swamp grass with small shallow water pools, 16-bit RPG game "
             "terrain, hand-painted, detailed, crisp, even lighting",
    "road": "top-down view of a grey cobblestone road with stone curbs, 16-bit RPG game terrain, hand-painted, "
            "detailed, crisp, even lighting",
}


# "painted" style: no "16-bit"/"RPG" wording (SDXL reads it as pixel art) and pixel art in the negative
PAINTED = {k: v.replace("16-bit RPG game terrain, hand-painted", "hand-painted fantasy game terrain, fine detail")
           for k, v in PROMPTS.items()}
NEGATIVE_PAINTED = NEGATIVE + ", pixel art, pixelated, mosaic, blocky, dithering, low resolution"


@dataclass
class R1Params:
    base: str = "mixed"            # mixed | xbrz | hybrid (route-3 plane) | soft (1x Lanczos 12x) | r2 | r3h (pack)
    denoise: float = 0.4
    cn: float = 0.8
    cn_end: float = 0.9            # ControlNet active over the first 90 % of the steps
    seed: int = 1000
    steps: int = 28
    cfg: float = 5.0
    material: str = "grass"
    prompt: str = ""
    negative: str = ""
    style: str = "rpg"             # rpg (PROMPTS, NEGATIVE) | painted (PAINTED, NEGATIVE_PAINTED)

    @property
    def tag(self) -> str:
        t = f"{self.base}_d{self.denoise:.2f}_c{self.cn:.2f}_s{self.seed}"
        return t if self.style == "rpg" else f"{t}_{self.style}"

    def prompts(self) -> tuple[str, str]:
        if self.style == "painted":
            return self.prompt or PAINTED[self.material], self.negative or NEGATIVE_PAINTED
        return self.prompt or PROMPTS[self.material], self.negative or NEGATIVE


# ---------------------------------------------------------------------------- GPU side
def snapshot(repo: str) -> str:
    """Local snapshot dir of a repo in ``$HF_HOME/hub`` (loaded by path: the SDXL snapshot holds only the
    fp16 variant, so the hub's completeness check would refuse it offline)."""
    import glob
    d = os.path.join(os.environ.get("HF_HOME", HF_HOME), "hub", "models--" + repo.replace("/", "--"), "snapshots")
    snaps = sorted(glob.glob(os.path.join(d, "*")))
    if not snaps:
        raise FileNotFoundError(f"{repo}: no snapshot under {d}")
    return snaps[-1]


class Refiner:
    """SDXL + xinsir tile ControlNet img2img (diffusers), fp16 on CUDA."""

    def __init__(self, device: str = "cuda", vae_tiling: bool = False):
        os.environ.setdefault("HF_HOME", HF_HOME)
        os.environ.setdefault("HF_HUB_OFFLINE", "1")
        import torch
        from diffusers import (AutoencoderKL, ControlNetModel, DPMSolverMultistepScheduler,
                               StableDiffusionXLControlNetImg2ImgPipeline)
        self.torch = torch
        t0 = time.time()
        cn = ControlNetModel.from_pretrained(snapshot(CN_MODEL), torch_dtype=torch.float16)
        vae = AutoencoderKL.from_pretrained(snapshot(VAE_MODEL), torch_dtype=torch.float16)
        pipe = StableDiffusionXLControlNetImg2ImgPipeline.from_pretrained(
            snapshot(BASE_MODEL), controlnet=cn, vae=vae, torch_dtype=torch.float16, variant="fp16", use_safetensors=True)
        pipe.scheduler = DPMSolverMultistepScheduler.from_config(
            pipe.scheduler.config, use_karras_sigmas=True, algorithm_type="dpmsolver++")
        self.pipe = pipe.to(device)
        self.pipe.set_progress_bar_config(disable=True)
        if vae_tiling:
            self.pipe.vae.enable_tiling()
        self.device = device
        self.load_seconds = time.time() - t0
        torch.cuda.reset_peak_memory_stats()

    def __call__(self, rgb12: np.ndarray, p: R1Params) -> tuple[np.ndarray, float]:
        """(H, W, 3) uint8 base at 12x -> ((H, W, 3) float32 0..255, seconds)."""
        from PIL import Image
        torch = self.torch
        img = Image.fromarray(np.ascontiguousarray(rgb12))
        pos, neg = p.prompts()
        g = torch.Generator(device=self.device).manual_seed(p.seed)
        torch.cuda.synchronize()
        t0 = time.time()
        out = self.pipe(prompt=pos, negative_prompt=neg, image=img, control_image=img, strength=p.denoise,
                        controlnet_conditioning_scale=p.cn, control_guidance_start=0.0,
                        control_guidance_end=p.cn_end, num_inference_steps=p.steps, guidance_scale=p.cfg,
                        height=img.height, width=img.width, generator=g, output_type="np").images[0]
        torch.cuda.synchronize()
        return np.clip(out * 255.0, 0, 255).astype(np.float32), time.time() - t0

    def vram(self) -> dict:
        c = self.torch.cuda
        mb = lambda v: round(v / 2 ** 20, 1)  # noqa: E731
        return {"device_name": c.get_device_name(0), "max_allocated_mb": mb(c.max_memory_allocated()),
                "max_reserved_mb": mb(c.max_memory_reserved()), "total_mb": mb(c.get_device_properties(0).total_memory)}


# ---------------------------------------------------------------------------- CPU side
def route3_planes(k: R3Kernel, idx: np.ndarray) -> dict[str, np.ndarray]:
    return {"xbrz": k.window(idx, False), "hybrid": k.window(idx, True)}


def mixed_plane(planes: dict, tile_shape: np.ndarray, tile_frame: np.ndarray, hyb: set, scale: int = S_ART):
    """Route-3 ``mixed`` plane: hybrid cells for keys of the hybrid families, xBRZ elsewhere."""
    out = planes["xbrz"].copy()
    T = TILE * scale
    for ty in range(tile_shape.shape[0]):
        for tx in range(tile_shape.shape[1]):
            if (int(tile_shape[ty, tx]), int(tile_frame[ty, tx])) in hyb:
                sl = (slice(ty * T, (ty + 1) * T), slice(tx * T, (tx + 1) * T))
                out[sl] = planes["hybrid"][sl]
    return out


def to_work(rgb6: np.ndarray, factor: int = WORK // S_ART) -> np.ndarray:
    """6x RGB uint8 -> Lanczos ``factor``x (12x) RGB uint8."""
    from PIL import Image
    im = Image.fromarray(np.ascontiguousarray(rgb6, np.uint8))
    return np.asarray(im.resize((im.width * factor, im.height * factor), Image.LANCZOS))


def detail_gain(rgb6: np.ndarray, gain: float, scale: int = S_ART) -> np.ndarray:
    """Scale every 6x6 block's deviation from its own mean by ``gain`` (linear RGB): fine texture that
    is weaker than half a palette step would otherwise snap back to the parent index."""
    from .quant import linear_to_srgb, srgb_to_linear
    if gain == 1.0:
        return rgb6
    x = srgb_to_linear(rgb6)
    H, W, _ = x.shape
    xb = x.reshape(H // scale, scale, W // scale, scale, 3)
    m = xb.mean((1, 3), keepdims=True)
    return linear_to_srgb(np.clip(m + gain * (xb - m), 0, 1).reshape(H, W, 3))


def backproject_smooth(rgb6: np.ndarray, src: np.ndarray, pal8: np.ndarray, cyc6: np.ndarray, scale: int = S_ART,
                       iters: int = 4, factor: int = 1) -> np.ndarray:
    """Iterative back-projection with a **bicubic** U (recommendation §2.6 "iterate with bicubic U if
    blocks show"), in linear RGB: the correction field ``y - D(x)`` is computed per ``factor``x``factor``
    group of source pixels and upsampled smoothly, so it does not stamp the 6x6 block grid into the
    texture. Cycling pixels and blocks with a cycling parent are left alone."""
    from PIL import Image
    from .quant import linear_to_srgb, srgb_to_linear
    h, w = src.shape
    x = srgb_to_linear(rgb6)
    y = srgb_to_linear(pal8[src])
    stat = (~cyc6).astype(np.float64)
    par_static = ~IS_CYCLING[src]
    B = scale * factor
    hb, wb = h // factor, w // factor
    yb = y.reshape(hb, factor, wb, factor, 3).mean((1, 3))
    ok = par_static.reshape(hb, factor, wb, factor).all((1, 3))
    sb = stat.reshape(hb, B, wb, B)
    nst = sb.sum((1, 3))
    for _ in range(iters):
        mean = (x.reshape(hb, B, wb, B, 3) * sb[..., None]).sum((1, 3)) / np.maximum(nst, 1)[..., None]
        corr = np.where((ok & (nst > 0))[..., None], yb - mean, 0.0)
        up = np.empty((h * scale, w * scale, 3))
        for ch in range(3):
            up[..., ch] = np.asarray(Image.fromarray(np.ascontiguousarray(corr[..., ch], np.float32), "F")
                                     .resize((w * scale, h * scale), Image.BICUBIC))
        x = np.clip(x + up * stat[..., None], 0.0, 1.0)
    return linear_to_srgb(x)


def gaussian_blur(x: np.ndarray, sigma: float) -> np.ndarray:
    """Separable Gaussian blur of an (H, W, C) float array, edge-replicated."""
    r = max(1, int(round(3 * sigma)))
    t = np.arange(-r, r + 1)
    kern = np.exp(-0.5 * (t / sigma) ** 2)
    kern /= kern.sum()
    xp = np.pad(x, ((r, r), (0, 0), (0, 0)), mode="edge")
    y = sum(kern[i] * xp[i:i + x.shape[0]] for i in range(2 * r + 1))
    yp = np.pad(y, ((0, 0), (r, r), (0, 0)), mode="edge")
    return sum(kern[i] * yp[:, i:i + x.shape[1]] for i in range(2 * r + 1))


def lowpass_lock(rgb6: np.ndarray, ref6: np.ndarray, cyc6: np.ndarray, sigma: float = 3.0) -> np.ndarray:
    """Frequency split in linear RGB: low frequencies from a reference 6x image (the route-3 plane, whose
    material boundaries are smooth), high frequencies (texture) from the diffusion output."""
    from .quant import linear_to_srgb, srgb_to_linear
    x, y = srgb_to_linear(rgb6), srgb_to_linear(ref6)
    out = x + gaussian_blur(y - x, sigma)
    out = np.where(cyc6[..., None], x, out)
    return linear_to_srgb(np.clip(out, 0, 1))


LOCKS = ("nn", "bicubic", "bicubic2", "lp3", "lp4")


def post(idx: np.ndarray, out12: np.ndarray, k: R3Kernel, ramp_expand: bool = False, iters: int = 2,
         gain: float = 1.0, lock: str = "nn", ref6: np.ndarray | None = None):
    """12x model output -> (final 6x indices, colour-locked 6x RGB, route-3 xBRZ plane, box-only 6x RGB).
    Box 2:1, optional detail gain, colour lock (``nn``: route 2's back-projection, exact block means;
    ``bicubic``/``bicubic2``: smooth correction per source pixel / per 2x2 source pixels), cycling
    pixels from route 3's label-safe plane, local snap of static pixels (3x3 parent neighbourhood)."""
    from .route2 import backproject, to_6x
    h, w = idx.shape
    box6 = to_6x(out12, WORK, h, w, S_ART)
    x = detail_gain(box6, gain)
    r3 = k.window_xbrz(idx)
    cyc6 = IS_CYCLING[r3]
    if lock == "nn":
        rgb6 = backproject(x, idx, k.qz.pal8, cyc6, S_ART, iters)
    elif lock in ("bicubic", "bicubic2"):
        rgb6 = backproject_smooth(x, idx, k.qz.pal8, cyc6, S_ART, 4, 1 if lock == "bicubic" else 2)
    elif lock in ("lp3", "lp4"):
        if ref6 is None:
            raise ValueError("lowpass lock needs ref6 (route-3 6x RGB of the window)")
        rgb6 = lowpass_lock(x, ref6, cyc6, 3.0 if lock == "lp3" else 4.5)
    else:
        raise ValueError(f"lock {lock!r} not in {LOCKS}")
    stat = k.qz.local_snap(rgb6, idx, S_ART, 1, forbid=IS_CYCLING, ramp_expand=ramp_expand)
    return np.where(cyc6, r3, stat).astype(np.uint8), rgb6, r3, box6


def pick_consensus(instances: list[tuple[np.ndarray, np.ndarray]]) -> tuple[np.ndarray, dict]:
    """Whole-instance OKLab medoid over (index tile, colour-locked RGB tile) instances."""
    if len(instances) == 1:
        return instances[0][0].copy(), {"instances": 1, "picked": 0, "disagreement": 0.0}
    lab = np.stack([srgb_to_oklab(r).reshape(-1, 3) for _, r in instances])
    n = len(lab)
    cost = np.zeros(n)
    for i in range(n):
        cost += np.sqrt(((lab - lab[i][None]) ** 2).sum(-1)).mean(1)
    j = int(np.argmin(cost))
    idx = np.stack([t for t, _ in instances]).reshape(n, -1)
    diff = (idx != idx[j][None]).mean(1)
    return instances[j][0].copy(), {"instances": n, "picked": j, "disagreement": float(diff.mean())}


# ---------------------------------------------------------------------------- pilot
@dataclass
class PilotWindow:
    name: str
    terrain: int
    y0: int                         # crop origin in the a16 window, tiles
    x0: int
    material: str
    crop: int = 12                  # crop size, tiles (12 -> 1152 px at 12x)
    margin: int = 2                 # tiles of context on every side; the centre is evaluated


PILOT_WINDOWS = [
    PilotWindow("grass", 1825, 0, 0, "grass"),
    PilotWindow("dirt", 2699, 4, 0, "dirt"),
    PilotWindow("sand", 25, 0, 4, "sand"),
    PilotWindow("dirt_grass", 163, 8, 4, "grass_dirt"),
    PilotWindow("grass_sand", 1941, 2, 6, "grass_sand"),
    PilotWindow("marsh", 1121, 8, 2, "marsh"),
    PilotWindow("road", 239, 0, 4, "road"),
    PilotWindow("grass_full", 1825, 0, 0, "grass", crop=20),   # whole a16 window: 1920x1920 at 12x (timing)
    PilotWindow("t1826_full", 1826, 0, 0, "grass", crop=20),   # right neighbour of 1825 (chunk-border seam)
    PilotWindow("t1827_full", 1827, 0, 0, "grass", crop=20),   # lower neighbour of 1825
]
DEFAULT_OUT = "/home/simonea/ultima7_exult/art_work/phase_b_pilot"
PACKS = {"r3h": "/home/simonea/ultima7_exult/packs/bg", "r2": "/home/simonea/ultima7_exult/packs/bg-r2"}


@dataclass
class Ctx:
    world: World
    k: R3Kernel
    hyb: set
    windows: dict = field(default_factory=dict)


def load_ctx(static: str | None, ctx_dir: str) -> Ctx:
    world = World.load(static)
    k = R3Kernel(world.pal8, world.pal6, R3Params())
    hyb = set(hybrid_key_set(world, R3Params(variant="mixed")))
    cs = load_context(ctx_dir)
    return Ctx(world, k, hyb, {w.ident: w for w in cs.terrain})


def window_data(c: Ctx, pw: PilotWindow) -> dict:
    """1x crop, route-3 planes of the crop (computed on the whole a16 window), eval region."""
    w = c.windows[pw.terrain]
    planes = route3_planes(c.k, w.idx)
    planes["mixed"] = mixed_plane(planes, w.tile_shape, w.tile_frame, c.hyb)
    T = TILE * S_ART
    ys, xs = slice(pw.y0 * T, (pw.y0 + pw.crop) * T), slice(pw.x0 * T, (pw.x0 + pw.crop) * T)
    crop_planes = {kk: v[ys, xs] for kk, v in planes.items()}
    idx = w.idx[pw.y0 * TILE:(pw.y0 + pw.crop) * TILE, pw.x0 * TILE:(pw.x0 + pw.crop) * TILE]
    oy, ox = w.origin[0] + pw.y0, w.origin[1] + pw.x0
    reg = world_region(c.world, FlatLayerCache(c.world), oy, ox, pw.crop, pw.crop)
    if not np.array_equal(reg.idx, idx):
        raise RuntimeError(f"window {pw.name}: world region differs from the a16 window crop")
    return {"idx": idx, "planes": crop_planes, "region": reg, "origin": (oy, ox)}


def base_image(c: Ctx, wd: dict, base: str) -> np.ndarray:
    """12x RGB mould: a route-3 plane (6x indices) Lanczos 2x, or ``soft`` = the 1x crop Lanczos 12x."""
    if base == "soft":
        return to_work(c.world.pal8[wd["idx"]], WORK)
    if base in PACKS:                    # a finished pack's tiles assembled over the crop (NN fallback)
        from .render import final_render
        reg = wd["region"]
        keys = {(int(s), int(f)) for s, f in zip(reg.src_shape.ravel(), reg.src_frame.ravel()) if s >= 0}
        img, _ = final_render(reg, load_pack_tiles(PACKS[base], keys), c.world, S_ART)
        return to_work(c.world.pal8[img])
    return to_work(c.world.pal8[wd["planes"][base]])


def eval_cells(pw: PilotWindow, reg) -> list[tuple[int, int, int, int]]:
    """(ty, tx, shape, frame) of the own-flat cells in the evaluated centre of the crop."""
    m = pw.margin
    out = []
    for ty in range(m, pw.crop - m):
        for tx in range(m, pw.crop - m):
            if reg.own[ty, tx] and reg.src_shape[ty, tx] >= 0:
                out.append((ty, tx, int(reg.src_shape[ty, tx]), int(reg.src_frame[ty, tx])))
    return out


def run_gen(c: Ctx, refiner: Refiner, pw: PilotWindow, settings: list[R1Params], out: str, log) -> list[dict]:
    """Generate (or reuse) every setting for one window; writes raw 12x PNG + post npz + timing json."""
    from PIL import Image
    wd = window_data(c, pw)
    res = []
    for p in settings:
        d = os.path.join(out, "runs", pw.name, p.tag)
        meta_p = os.path.join(d, "run.json")
        if os.path.exists(meta_p):
            with open(meta_p) as f:
                res.append(json.load(f))
            continue
        os.makedirs(d, exist_ok=True)
        base12 = base_image(c, wd, p.base)
        out12, secs = refiner(base12, p)
        o8 = np.clip(np.rint(out12), 0, 255).astype(np.uint8)
        Image.fromarray(o8).save(os.path.join(d, "out12.png"))
        if not os.path.exists(os.path.join(out, "runs", pw.name, f"base12_{p.base}.png")):
            Image.fromarray(base12).save(os.path.join(out, "runs", pw.name, f"base12_{p.base}.png"))
        r = {"window": pw.name, "params": asdict(p), "tag": p.tag, "seconds": round(secs, 2),
             "canvas": list(base12.shape[:2]), "vram": refiner.vram()}
        atomic_write_json(meta_p, r)
        log.info("%s %s: %.1fs, peak alloc %s MB", pw.name, p.tag, secs, r["vram"]["max_allocated_mb"])
        res.append(r)
    return res


def load_pack_tiles(root: str, keys) -> dict:
    from .pngio import read_indexed
    out = {}
    for s, f in keys:
        p = os.path.join(root, "x6", "flats", f"{s:04d}", f"{key_name(s, f)}.png")
        if os.path.exists(p):
            out[(s, f)] = read_indexed(p)[0]
    return out


def consensus_tiles(cells, final: np.ndarray, rgb6: np.ndarray, src_of, k: R3Kernel, how: str):
    """Per-key consensus over the cut instances of the eval cells -> (tiles, stats)."""
    T = TILE * S_ART
    inst: dict = {}
    for ty, tx, s, f in cells:
        sl = (slice(ty * T, (ty + 1) * T), slice(tx * T, (tx + 1) * T))
        inst.setdefault((s, f), []).append((final[sl].copy(), np.clip(np.rint(rgb6[sl]), 0, 255)))
    tiles, stats = {}, {}
    for key, lst in inst.items():
        src = src_of(*key)
        if how == "mode":
            mc = ModeConsensus()
            for t, _ in lst:
                mc.add(key, t)
            tile, st = mc.result(key, parent=src)
        else:
            tile, st = pick_consensus(lst)
        tile, est = k.qz.enforce(tile, src, S_ART)
        tiles[key], stats[key] = tile, {**st, "enforce": est}
    return tiles, stats


def region_tiles_render(reg, tiles: dict, world: World, m: int) -> np.ndarray:
    """6x index render of the eval centre (tiles, NN fallback for keys without a tile)."""
    from .render import final_render
    img, _ = final_render(reg, tiles, world, S_ART)
    T = TILE * S_ART
    return img[m * T:(reg.th - m) * T, m * T:(reg.tw - m) * T]


def c2_window(cells, tiles: dict, world: World, lab: np.ndarray) -> dict:
    """Edge-strip dE excess (6x minus 1x) over the adjacent own pairs inside the eval centre."""
    pos = {(ty, tx): (s, f) for ty, tx, s, f in cells}
    exc = []
    for (ty, tx), a in pos.items():
        for (dy, dx, sa, sb) in ((0, 1, "r", "l"), (1, 0, "b", "t")):
            b = pos.get((ty + dy, tx + dx))
            if b is None or a not in tiles or b not in tiles:
                continue
            ta, tb, la, lb = tiles[a], tiles[b], world.flat(*a), world.flat(*b)
            if dx:
                d6 = np.sqrt(((lab[ta[:, -1]] - lab[tb[:, 0]]) ** 2).sum(-1)).mean()
                d1 = np.sqrt(((lab[la[:, -1]] - lab[lb[:, 0]]) ** 2).sum(-1)).mean()
            else:
                d6 = np.sqrt(((lab[ta[-1]] - lab[tb[0]]) ** 2).sum(-1)).mean()
                d1 = np.sqrt(((lab[la[-1]] - lab[lb[0]]) ** 2).sum(-1)).mean()
            exc.append(d6 - d1)
    e = np.array(exc) if exc else np.zeros(1)
    return {"pairs": len(exc), "excess_mean": float(e.mean()), "excess_p99": float(np.percentile(e, 99)),
            "excess_max": float(e.max())}


def detail_stats(img: np.ndarray, nn: np.ndarray, pal8: np.ndarray) -> dict:
    """Share of 6x pixels != NN, and mean OKLab L std inside the 6x6 blocks (sub-pixel texture)."""
    L = srgb_to_oklab(pal8[img])[..., 0]
    H, W = L.shape
    b = L.reshape(H // S_ART, S_ART, W // S_ART, S_ART).std(axis=(1, 3))
    return {"px_ne_nn": float((img != nn).mean()), "block_L_std": float(b.mean())}


# name -> (ramp_expand, detail gain, colour lock)
POST_VARIANTS = {"snap": (False, 1.0, "nn"), "rx": (True, 1.0, "nn"), "g2rx": (True, 2.0, "nn"),
                 "bc": (False, 1.0, "bicubic"), "bcrx": (True, 1.0, "bicubic"), "bc2": (False, 1.0, "bicubic2"),
                 "bc2rx": (True, 1.0, "bicubic2"), "bc2g2rx": (True, 2.0, "bicubic2"),
                 "lp3": (False, 1.0, "lp3"), "lp3rx": (True, 1.0, "lp3"), "lp4rx": (True, 1.0, "lp4")}


def evaluate(c: Ctx, pw: PilotWindow, tags: list[str], out: str, how_list=("pick", "mode"),
             posts=("snap",)) -> dict:
    """Metrics for r3h / r2 and every diffusion run of a window; returns {label: metrics} and renders.
    Labels: ``<tag>|<post>|<consensus>``; renders also hold ``<tag>|<post>|window`` (the window-level
    quantized result), ``<tag>|raw`` (box 2:1 only) and ``<tag>|<post>|locked`` (colour-locked RGB)."""
    from .qa import seam_excess, tile_metrics
    world, k = c.world, c.k
    wd = window_data(c, pw)
    reg, idx, m = wd["region"], wd["idx"], pw.margin
    cells = eval_cells(pw, reg)
    keys = sorted({(s, f) for _, _, s, f in cells})
    lut = ramp_lut(engine_ramps(world.pal6))
    lab = k.qz.lab
    T = TILE * S_ART
    centre = (slice(m * T, (pw.crop - m) * T), slice(m * T, (pw.crop - m) * T))
    nn = np.repeat(np.repeat(idx, S_ART, 0), S_ART, 1)[centre]

    def metrics(tiles: dict, region_level: np.ndarray | None, extra: dict | None = None) -> dict:
        kk = [x for x in keys if x in tiles]
        tm = tile_metrics(kk, [tiles[x] for x in kk], [world.flat(*x) for x in kk], k.qz, lut, S_ART)
        img = region_tiles_render(reg, tiles, world, m)
        r_t = seam_excess(world.pal8[img].astype(np.float64), T)[0]
        r = {"tiles": len(kk), "b1": float(tm["b1"].mean()), "b1_min": float(tm["b1"].min()),
             "b2": float(tm["b2"].mean()), "b3_mean": float(tm["block_de"].mean()),
             "b3_p99": float(np.percentile(tm["block_de"], 99)), "b4": float(tm["b4"].mean()),
             "a2_bad": int(tm["a2_bad"].sum()), "a3_p4": int(tm["a3_p4"].sum()),
             "a3_dropped": int(tm["a3_dropped"].sum()), "seam_ratio_tiles": r_t,
             **detail_stats(img, nn, world.pal8), "c2": c2_window(cells, tiles, world, lab)}
        if region_level is not None:
            r_r = seam_excess(world.pal8[region_level].astype(np.float64), T)[0]
            r["seam_ratio_region"] = r_r
            r["c1"] = r_t / max(r_r, 1e-12)
        if extra:
            r.update(extra)
        return r, img

    results, renders = {}, {}
    nn_tiles = {x: np.repeat(np.repeat(world.flat(*x), S_ART, 0), S_ART, 1) for x in keys}
    results["nn"], renders["nn"] = metrics(nn_tiles, None)
    r3h = load_pack_tiles(PACKS["r3h"], keys)
    results["r3h"], renders["r3h"] = metrics(r3h, wd["planes"]["mixed"][centre])
    r2 = load_pack_tiles(PACKS["r2"], keys)
    results["r2"], renders["r2"] = metrics(r2, None)
    for tag in tags:
        d = os.path.join(out, "runs", pw.name, tag)
        from PIL import Image
        out12 = np.asarray(Image.open(os.path.join(d, "out12.png")).convert("RGB")).astype(np.float32)
        with open(os.path.join(d, "run.json")) as f:
            run = json.load(f)
        renders[f"{tag}|raw"] = None
        for pv in posts:
            rx, gain, lock = POST_VARIANTS[pv]
            final, rgb6, r3, box6 = post(idx, out12, k, ramp_expand=rx, gain=gain, lock=lock,
                                         ref6=world.pal8[wd["planes"]["mixed"]])
            renders[f"{tag}|{pv}|window"] = final[centre]
            # the window plane itself, as a per-terrain (chunk) override would ship it: per-cell metrics
            ct = [final[ty * T:(ty + 1) * T, tx * T:(tx + 1) * T] for ty, tx, _, _ in cells]
            cs_ = [world.flat(s_, f_) for _, _, s_, f_ in cells]
            tmw = tile_metrics(list(range(len(ct))), ct, cs_, k.qz, lut, S_ART)
            results[f"{tag}|{pv}|window"] = {
                "tiles": len(ct), "b1": float(tmw["b1"].mean()), "b1_min": float(tmw["b1"].min()),
                "b2": float(tmw["b2"].mean()), "b3_mean": float(tmw["block_de"].mean()),
                "b3_p99": float(np.percentile(tmw["block_de"], 99)), "b4": float(tmw["b4"].mean()),
                "a2_bad": int(tmw["a2_bad"].sum()), "a3_p4": int(tmw["a3_p4"].sum()),
                "a3_dropped": int(tmw["a3_dropped"].sum()),
                "seam_ratio_tiles": seam_excess(world.pal8[final[centre]].astype(np.float64), T)[0], "c1": 1.0,
                **detail_stats(final[centre], nn, world.pal8), "c2": {"excess_mean": float("nan")},
                "seconds": run["seconds"]}
            renders[f"{tag}|raw"] = np.clip(np.rint(box6[centre]), 0, 255).astype(np.uint8)
            renders[f"{tag}|{pv}|locked"] = np.clip(np.rint(rgb6[centre]), 0, 255).astype(np.uint8)
            for how in how_list:
                tiles, st = consensus_tiles(cells, final, rgb6, world.flat, k, how)
                lbl = f"{tag}|{pv}|{how}"
                dis = [v["disagreement"] for v in st.values()]
                results[lbl], renders[lbl] = metrics(tiles, final[centre], {
                    "seconds": run["seconds"], "d1_mean": float(np.mean(dis)),
                    "instances_mean": float(np.mean([v["instances"] for v in st.values()]))})
                renders[lbl + "#tiles"] = tiles
    return {"keys": [key_name(*x) for x in keys], "cells": len(cells), "results": results}, renders


# ---------------------------------------------------------------------------- sheets
def _font(size: int):
    from PIL import ImageFont
    try:
        return ImageFont.load_default(size=size)
    except Exception:  # noqa: BLE001
        return ImageFont.load_default()


def sheet(path: str, title: str, panels: list[tuple[str, np.ndarray]], cols: int, pad: int = 8, head: int = 36):
    """Grid of equally sized RGB panels at 1:1 with a two-line caption above each."""
    from PIL import Image, ImageDraw
    h, w = panels[0][1].shape[:2]
    rows = (len(panels) + cols - 1) // cols
    W = cols * w + (cols + 1) * pad
    H = 34 + rows * (h + head + pad) + pad
    im = Image.new("RGB", (W, H), (24, 24, 24))
    dr = ImageDraw.Draw(im)
    dr.text((pad, 8), title, fill=(240, 240, 240), font=_font(18))
    f = _font(13)
    for i, (cap, rgb) in enumerate(panels):
        r, cc = divmod(i, cols)
        x, y = pad + cc * (w + pad), 34 + r * (h + head + pad)
        for j, line in enumerate(cap.split("\n")[:2]):
            dr.text((x, y + 2 + j * 16), line, fill=(230, 230, 230), font=f)
        im.paste(Image.fromarray(np.ascontiguousarray(rgb, np.uint8)), (x, y + head))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp.png"
    im.save(tmp, optimize=True)
    os.replace(tmp, path)


def cap(lbl: str, r: dict) -> str:
    s = f"B1 {100 * r['b1']:.1f}% min {100 * r['b1_min']:.0f}  B3p99 {r['b3_p99']:.3f}"
    if "c1" in r:
        s += f"  C1 {r['c1']:.2f}"
    s += f"  C2x {r['c2']['excess_mean']:+.3f}  std {r['block_L_std']:.3f}"
    return f"{lbl}\n{s}"


# ---------------------------------------------------------------------------- macro sheets (Tier 1)
def macro_data(c: Ctx, shape: int, apron: int = 16) -> dict:
    """Toroidally padded 8x4 macro unit of ``shape`` (ctx.macro_window) with its route-3 planes."""
    from .ctx import macro_window
    w = macro_window(c.world, shape, apron)
    planes = route3_planes(c.k, w.idx)
    planes["mixed"] = mixed_plane(planes, w.tile_shape, w.tile_frame, c.hyb)
    return {"win": w, "idx": w.idx, "planes": planes}


def run_macro_gen(c: Ctx, refiner: "Refiner", shapes: list[int], settings: list[R1Params], out: str, log) -> list:
    from dataclasses import replace
    from PIL import Image
    from .families import shape_families
    fam_mat = {"grass": "grass", "dirt": "dirt", "sand": "sand", "dirt+grass": "grass_dirt",
               "grass+sand": "grass_sand", "dirt+sand": "sand", "marsh": "marsh"}
    sf = shape_families(c.world)
    res = []
    for shp in shapes:
        md = macro_data(c, shp)
        mat = fam_mat.get(sf.get(shp, "grass"), "grass")
        for p in (replace(q, material=mat) for q in settings):
            d = os.path.join(out, "runs", "macro", f"{shp:04d}", p.tag)
            if os.path.exists(os.path.join(d, "run.json")):
                continue
            os.makedirs(d, exist_ok=True)
            base12 = base_image(c, md, p.base)
            o12, secs = refiner(base12, p)
            Image.fromarray(np.clip(np.rint(o12), 0, 255).astype(np.uint8)).save(os.path.join(d, "out12.png"))
            r = {"shape": shp, "params": asdict(p), "tag": p.tag, "seconds": round(secs, 2),
                 "canvas": list(base12.shape[:2]), "vram": refiner.vram()}
            atomic_write_json(os.path.join(d, "run.json"), r)
            log.info("macro %d %s: %.1fs", shp, p.tag, secs)
            res.append(r)
    return res


def macro_tiles(c: Ctx, shape: int, tag: str, out: str, post_v: str) -> dict:
    """Per-frame tiles cut from the centre period of a macro canvas (one instance per frame)."""
    from PIL import Image
    md = macro_data(c, shape)
    o12 = np.asarray(Image.open(os.path.join(out, "runs", "macro", f"{shape:04d}", tag, "out12.png")).convert("RGB"))
    rx, gain, lock = POST_VARIANTS[post_v]
    final, rgb6, _, _ = post(md["idx"], o12.astype(np.float32), c.k, rx, 2, gain, lock,
                             c.world.pal8[md["planes"]["mixed"]])
    T = TILE * S_ART
    tiles = {}
    for ty, tx, s_, f_ in md["win"].own_cells():
        t, _ = c.k.qz.enforce(final[ty * T:(ty + 1) * T, tx * T:(tx + 1) * T], c.world.flat(s_, f_), S_ART)
        tiles[(s_, f_)] = t
    return tiles


# ---------------------------------------------------------------------------- pilot report
def _rgb(world: World, x: np.ndarray) -> np.ndarray:
    return x if x.ndim == 3 else world.pal8[x]


def alt_render(cells, finals: list[np.ndarray], m: int, crop: int) -> np.ndarray:
    """Seam stress test: eval-centre cells taken alternately (checkerboard) from two runs' window planes,
    as a per-tile pack does when a key's tile comes from another context."""
    T = TILE * S_ART
    out = finals[0].copy()
    for ty, tx, _, _ in cells:
        if (ty + tx) % 2:
            sl = (slice(ty * T, (ty + 1) * T), slice(tx * T, (tx + 1) * T))
            out[sl] = finals[1][sl]
    return out[m * T:(crop - m) * T, m * T:(crop - m) * T]


def report(c: Ctx, out: str, settings: dict[str, str], seeds=(1000, 1001), post_main: str = "bc",
           windows=None, log=None) -> dict:
    """Phase B pilot report: per window metrics + 1:1 sheets for each setting (``name -> tag prefix``,
    e.g. ``S1 -> soft_d0.50_c0.60``), alternating-seed seam test, and a cross-window pick consensus per
    setting written as a mini pack and validated with the engine-equivalent checker."""
    from .check import Provider, check_root
    from .pack import PackWriter
    from .qa import seam_excess, tile_metrics
    log = log or get_logger()
    world, k = c.world, c.k
    lut = ramp_lut(engine_ramps(world.pal6))
    T = TILE * S_ART
    wins = windows or PILOT_WINDOWS
    rep = {"settings": settings, "seeds": list(seeds), "post_main": post_main, "windows": {}, "packs": {}}
    inst: dict[str, dict] = {n: {} for n in settings}         # setting -> key -> [(idx tile, rgb tile)]
    sheets = []
    for pw in wins:
        tags = [f"{settings[n]}_s{sd}" for n in settings for sd in seeds]
        res, rd = evaluate(c, pw, tags, out, how_list=("pick", "mode"), posts=(post_main, "snap"))
        wd = window_data(c, pw)
        cells = eval_cells(pw, wd["region"])
        m = pw.margin
        extra = {}
        for n in settings:
            finals = []
            for sd in seeds:
                tag = f"{settings[n]}_s{sd}"
                from PIL import Image
                o12 = np.asarray(Image.open(os.path.join(out, "runs", pw.name, tag, "out12.png")).convert("RGB"))
                rx, gain, lock = POST_VARIANTS[post_main]
                final, rgb6, _, _ = post(wd["idx"], o12.astype(np.float32), k, rx, 2, gain, lock,
                                         world.pal8[wd["planes"]["mixed"]])
                finals.append(final)
                if sd == seeds[0]:
                    for ty, tx, s_, f_ in cells:
                        sl = (slice(ty * T, (ty + 1) * T), slice(tx * T, (tx + 1) * T))
                        inst[n].setdefault((s_, f_), []).append((final[sl].copy(), np.clip(np.rint(rgb6[sl]), 0, 255)))
            alt = alt_render(cells, finals, m, pw.crop)
            centre = (slice(m * T, (pw.crop - m) * T),) * 2
            r_alt = seam_excess(world.pal8[alt].astype(np.float64), T)[0]
            r_win = seam_excess(world.pal8[finals[0][centre]].astype(np.float64), T)[0]
            extra[n] = {"c1_altseed": r_alt / r_win, "seam_ratio_altseed": r_alt, "seam_ratio_window": r_win}
            rd[f"{n}|alt"] = alt
        R = res["results"]
        panels = [("1x NN (shown at 6x)", _rgb(world, rd["nn"])), (cap("r3h = packs/bg (route-3 hybrid)", R["r3h"]), _rgb(world, rd["r3h"])),
                  (cap("r2 = packs/bg-r2 (4x-NXbrz)", R["r2"]), _rgb(world, rd["r2"]))]
        for n in settings:
            t0, t1 = (f"{settings[n]}_s{sd}" for sd in seeds[:2])
            panels.append((f"{n} {t0} raw (box 2:1, no lock)", rd[f"{t0}|raw"]))
            panels.append((cap(f"{n} s{seeds[0]} {post_main}-lock, per-key pick", R[f"{t0}|{post_main}|pick"]),
                           _rgb(world, rd[f"{t0}|{post_main}|pick"])))
            panels.append((cap(f"{n} s{seeds[1]} {post_main}-lock, per-key pick", R[f"{t1}|{post_main}|pick"]),
                           _rgb(world, rd[f"{t1}|{post_main}|pick"])))
            rw = R[f"{t0}|{post_main}|window"]
            panels.append((f"{n} s{seeds[0]} {post_main}-lock, WINDOW plane (per-terrain override)\n"
                           f"B1 {100 * rw['b1']:.1f}% min {100 * rw['b1_min']:.0f}  B3p99 {rw['b3_p99']:.3f}  "
                           f"std {rw['block_L_std']:.3f}", _rgb(world, rd[f"{t0}|{post_main}|window"])))
            panels.append((f"{n} seam test: seeds alternate per tile\nC1 vs window {extra[n]['c1_altseed']:.2f}",
                           _rgb(world, rd[f"{n}|alt"])))
        path = os.path.join(out, "sheets", post_main, f"{pw.name}.png")
        sheet(path, f"Phase B pilot - {pw.name} (terrain {pw.terrain}, 8x8-tile centre at 1:1 6x; lock {post_main})",
              panels, 4)
        sheets.append(path)
        rep["windows"][pw.name] = {"terrain": pw.terrain, "crop": [pw.y0, pw.x0, pw.crop], "material": pw.material,
                                   "keys": res["keys"], "cells": res["cells"], "results": R, "altseed": extra}
        log.info("%s: sheet %s", pw.name, path)
    # cross-window consensus -> mini pack per setting, engine checks, metrics, re-render windows
    prov = Provider.from_world(world)
    for n in settings:
        tiles, dis = {}, []
        for key, lst in sorted(inst[n].items()):
            t, st = pick_consensus(lst)
            tiles[key], _ = k.qz.enforce(t, world.flat(*key), S_ART)
            if st["instances"] > 1:
                dis.append(st["disagreement"])
        root = os.path.join(out, "packs", f"pilot-{n.lower()}-{post_main}")
        pwr = PackWriter(root, prov, S_ART, route=f"r1-{settings[n]}-{post_main}", title=f"phase B pilot {n}")
        for key in sorted(tiles):
            pwr.add(key[0], key[1], tiles[key], {"setting": settings[n], "seed": seeds[0], "post": post_main})
        summ = pwr.finish()
        chk = check_root(root, prov, S_ART, offline=True).summary()
        keys = sorted(tiles)
        tm = tile_metrics(keys, [tiles[x] for x in keys], [world.flat(*x) for x in keys], k.qz, lut, S_ART)
        per_win = {}
        for pw in wins:
            wd = window_data(c, pw)
            img = region_tiles_render(wd["region"], tiles, world, pw.margin)
            per_win[pw.name] = seam_excess(world.pal8[img].astype(np.float64), T)[0]
        rep["packs"][n] = {"root": root, "written": summ["written"], "refused": summ["refused"], "check": chk,
                           "keys": len(keys), "multi_instance_keys": len(dis),
                           "d1_mean_multi": float(np.mean(dis)) if dis else 0.0,
                           "b1": float(tm["b1"].mean()), "b1_min": float(tm["b1"].min()),
                           "b1_below_97": int((tm["b1"] < 0.97).sum()), "b1_below_85": int((tm["b1"] < 0.85).sum()),
                           "b2": float(tm["b2"].mean()), "b3_mean": float(tm["block_de"].mean()),
                           "b3_p99": float(np.percentile(tm["block_de"], 99)), "b4": float(tm["b4"].mean()),
                           "a2_bad": int(tm["a2_bad"].sum()), "a3_p4": int(tm["a3_p4"].sum()),
                           "a3_dropped": int(tm["a3_dropped"].sum()), "seam_ratio_by_window": per_win}
        log.info("pack %s: %d tiles, check %s", root, summ["written"], chk)
    rep["sheets"] = sheets
    atomic_write_json(os.path.join(out, f"report_{post_main}.json"), rep)
    return rep


# ---------------------------------------------------------------------------- CLI
def parse_settings(spec: str, material: str) -> list[R1Params]:
    """'base:d:c:seed[:style],...' -> R1Params list."""
    out = []
    for it in (x for x in spec.split(",") if x):
        b, d, cc, s, *st = it.split(":")
        out.append(R1Params(base=b, denoise=float(d), cn=float(cc), seed=int(s), material=material,
                            style=st[0] if st else "rpg"))
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="route1.py", description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen", help="generate diffusion runs for pilot windows")
    g.add_argument("--windows", default="grass")
    g.add_argument("--settings", required=True, help="base:denoise:cn:seed,... (e.g. mixed:0.4:0.8:1000)")
    g.add_argument("--repeat-check", action="store_true", help="re-run the first setting and compare")
    g.add_argument("--vae-tiling", action="store_true", help="tiled VAE encode/decode (large canvases)")
    g.add_argument("--macro", default="", help="comma-separated macro shapes: generate macro sheets instead")
    r = sub.add_parser("report", help="metrics, sheets and mini packs for the pilot")
    r.add_argument("--setting", action="append", default=[], help="NAME=TAGPREFIX, e.g. S1=soft_d0.50_c0.60")
    r.add_argument("--seeds", default="1000,1001")
    r.add_argument("--post", default="bc", choices=sorted(POST_VARIANTS))
    r.add_argument("--windows", default="")
    for p in (g, r):
        p.add_argument("--static", default=None)
        p.add_argument("--ctx", default="/home/simonea/ultima7_exult/art_work/ctx/a16")
        p.add_argument("--out", default=DEFAULT_OUT)
    a = ap.parse_args(argv)
    log = get_logger()
    c = load_ctx(a.static, a.ctx)
    byname = {w.name: w for w in PILOT_WINDOWS}
    if a.cmd == "gen":
        import torch
        torch.set_num_threads(4)
        ref = Refiner(vae_tiling=a.vae_tiling)
        log.info("pipeline loaded in %.1fs", ref.load_seconds)
        if a.macro:
            run_macro_gen(c, ref, [int(x) for x in a.macro.split(",")], parse_settings(a.settings, "grass"), a.out, log)
            atomic_write_json(os.path.join(a.out, "vram_last_gen.json"), ref.vram())
            return 0
        for wn in a.windows.split(","):
            pw = byname[wn]
            res = run_gen(c, ref, pw, parse_settings(a.settings, pw.material), a.out, log)
            log.info("%s: %d runs, mean %.1fs", wn, len(res), np.mean([r["seconds"] for r in res]))
            if a.repeat_check:
                p = parse_settings(a.settings, pw.material)[0]
                wd = window_data(c, pw)
                o2, secs = ref(base_image(c, wd, p.base), p)
                from PIL import Image
                o1 = np.asarray(Image.open(os.path.join(a.out, "runs", pw.name, p.tag, "out12.png")).convert("RGB"))
                o2 = np.clip(np.rint(o2), 0, 255).astype(np.uint8)
                diff = np.abs(o1.astype(int) - o2.astype(int))
                rep = {"tag": p.tag, "window": wn, "max_abs": int(diff.max()), "mean_abs": float(diff.mean()),
                       "identical": bool((diff == 0).all()), "seconds": round(secs, 2)}
                atomic_write_json(os.path.join(a.out, "runs", pw.name, f"repeat_{p.tag}.json"), rep)
                log.info("repeat check %s", rep)
        atomic_write_json(os.path.join(a.out, "vram_last_gen.json"), ref.vram())
    elif a.cmd == "report":
        settings = dict(x.split("=", 1) for x in a.setting)
        wins = [byname[w] for w in a.windows.split(",") if w] or None
        report(c, a.out, settings, tuple(int(x) for x in a.seeds.split(",")), a.post, wins, log)
    return 0


if __name__ == "__main__":
    sys.exit(main())
