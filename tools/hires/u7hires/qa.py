"""QA for 6x flat packs (DESIGN §8.3; recommendation §2.8).

Metrics (per tile and aggregated):

* A1 format: 48x48, colour type 3, PLTE == palette 0, no tRNS (from the engine-equivalent checker).
* A2 static compliance: no 0xFF; non-cycling pixels in 0x01-0xDF; 0x00 only if the source uses it.
* A3 cycling compliance = engine P4 (0 non-compliant pixels); dropped sparkles are a warning.
* B1 6x6 block majority (ties: smallest index) == source index. Gate: aggregate >= 97 % and per-tile
  floor >= 85 %; tiles below 97 % are flagged (Q8).
* B2 box-mean round trip: mean sRGB of each block -> OKLab-nearest over 0x00-0xDF == source index,
  on non-cycling source pixels. Per route; the bottom 5 % of tiles are flagged.
* B3 block-mean OKLab dE (mean of the block's OKLab values vs the source colour): mean and p99;
  gate p99 <= 0.10.
* B4 dominant engine ramp of the block == ramp of the source pixel: aggregate >= 95 %; tiles below
  95 % are flagged.
* C1 seam gradient ratio on world renders assembled from the final tiles (mean OKLab dE across tile
  boundaries / across other neighbour pairs), divided by the same route's region-level render of the
  same views. Gate <= 1.2.
* C2 edge-strip dE for every adjacent own-flat pair that occurs on the map, as the excess over the 1x
  seam; the worst 1 % (by excess) are listed for repair or regeneration.
* D1 instance disagreement (from the sidecars): flagged above 2x the route median.

Outputs: ``report.json``, ``report.md``, contact sheets per material family (1x NN | candidate
[| baseline]) and 6x world previews (plus 1280x800-equivalent downscales) of representative chunks.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys
import time
from collections import Counter, defaultdict

import numpy as np

from .check import Provider, check_root, read_pack_txt
from .ctx import choose_anchors, world_region
from .families import family_order, frame_families
from .fill import FlatLayerCache
from .io import KIND_FLAT, KIND_FLAT_VOID, KIND_RLE, World
from .pngio import decode_indexed, parse_png
from .quant import Quantizer, srgb_to_oklab
from .render import composite, final_render, nn_render, rle_overlay
from .rules import BORDER_INDEX, CYCLE_ID, IS_CYCLING, engine_ramps, p4_violations, ramp_lut
from .util import atomic_write_bytes, atomic_write_json, get_logger, key_name

GATES = {"b1_aggregate": 0.97, "b1_floor": 0.85, "b1_flag": 0.97, "b2_bottom_frac": 0.05, "b3_p99": 0.10,
         "b4_aggregate": 0.95, "b4_flag": 0.95, "c1_ratio": 1.2, "c2_worst_frac": 0.01, "d1_factor": 2.0}

PREVIEW_CATEGORIES = ("coast", "town", "roads", "forest", "swamp", "dungeon")


# ---------------------------------------------------------------------------- loading
def load_tiles(path: str, provider: Provider, scale: int = 6, log=None):
    """Tiles + per-key sidecar stats + check report from a pack root or a candidate dir."""
    log = log or get_logger()
    if os.path.exists(os.path.join(path, "tiles.npz")):
        from .pack import load_candidates
        tiles, meta = load_candidates(path)
        side = {tuple(int(v) for v in k.split("_")): v for k, v in meta.get("keys", {}).items()}
        return tiles, side, None, {"route": meta.get("route", "?"), "kind": "candidates", "params": meta.get("params", {})}
    rep = check_root(path, provider, scale, offline=False)
    tiles, side = {}, {}
    xdir = os.path.join(path, f"x{scale}")
    files = sorted(glob.glob(os.path.join(xdir, "flats", "*.png")) + glob.glob(os.path.join(xdir, "flats", "*", "*.png")))
    for fp in files:
        n = os.path.basename(fp)[:-4]
        try:
            k = tuple(int(v) for v in n.split("_"))
            with open(fp, "rb") as f:
                data = f.read()
            info = parse_png(data)
            if info.color_type != 3 or (info.width, info.height) != (8 * scale, 8 * scale):
                continue
            tiles[k] = decode_indexed(data, info)
        except (ValueError, OSError):
            continue
        jp = fp[:-4] + ".json"
        if os.path.exists(jp):
            with open(jp) as f:
                side[k] = json.load(f)
    if os.path.exists(os.path.join(xdir, "flats.bundle")):
        from .pack import read_bundle
        try:
            for e in read_bundle(os.path.join(xdir, "flats.bundle"))[0]:
                tiles.setdefault((e["shape"], e["frame"]), np.array(e["tile"]))
        except ValueError as ex:
            log.warning("bundle unreadable: %s", ex)
    meta = read_pack_txt(os.path.join(path, "pack.txt")) if os.path.exists(os.path.join(path, "pack.txt")) else {}
    params = next(iter(side.values()), {}).get("params", {}) if side else {}
    return tiles, side, rep, {"route": meta.get("route", "?"), "kind": "pack", "edge": meta.get("edge", "none"),
                              "params": params}


# ---------------------------------------------------------------------------- per-tile metrics
def tile_metrics(keys, tiles, srcs, qz: Quantizer, lut: np.ndarray, scale: int = 6, chunk: int = 256) -> dict:
    """Vectorised A2/A3/B1-B4 per tile. Returns arrays aligned with ``keys`` plus all block dE values."""
    N = len(keys)
    out = {k: np.zeros(N) for k in ("b1", "b2", "b3_mean", "b4", "a2_bad", "a3_p4", "a3_dropped")}
    all_de = []
    lab = qz.lab
    nr = int(lut.max()) + 2
    static_lab = lab[: 0xE0]
    for i0 in range(0, N, chunk):
        T = np.stack(tiles[i0:i0 + chunk]).astype(np.int64)        # (n, 48, 48)
        Sx = np.stack(srcs[i0:i0 + chunk]).astype(np.int64)         # (n, 8, 8)
        n = len(T)
        blk = T.reshape(n, 8, scale, 8, scale).transpose(0, 1, 3, 2, 4).reshape(n * 64, scale * scale)
        src = Sx.reshape(n * 64)
        rows = np.arange(n * 64)[:, None]
        cnt = np.bincount((rows * 256 + blk).ravel(), minlength=n * 64 * 256).reshape(n * 64, 256)
        maj = cnt.argmax(1)
        out["b1"][i0:i0 + n] = (maj == src).reshape(n, 64).mean(1)
        mrgb = qz.pal8[blk].astype(np.float64).mean(1)               # (n*64, 3) box mean in sRGB
        ml = srgb_to_oklab(mrgb)
        rt = ((ml[:, None, :] - static_lab[None]) ** 2).sum(-1).argmin(1)
        stat = ~IS_CYCLING[src]
        ok = (rt == src) & stat
        out["b2"][i0:i0 + n] = ok.reshape(n, 64).sum(1) / np.maximum(stat.reshape(n, 64).sum(1), 1)
        bl = lab[blk].mean(1)
        de = np.sqrt(((bl - lab[src]) ** 2).sum(-1))
        all_de.append(de)
        out["b3_mean"][i0:i0 + n] = de.reshape(n, 64).mean(1)
        rb = lut[blk] + 1
        rc = np.bincount((rows * nr + rb).ravel(), minlength=n * 64 * nr).reshape(n * 64, nr)
        out["b4"][i0:i0 + n] = ((rc.argmax(1) - 1) == lut[src]).reshape(n, 64).mean(1)
        for j in range(n):
            t, s = T[j], Sx[j]
            bad = int((t == BORDER_INDEX).sum())
            if not (s == 0).any():
                bad += int((t == 0).sum())
            out["a2_bad"][i0 + j] = bad
            out["a3_p4"][i0 + j] = p4_violations(t.astype(np.uint8), s.astype(np.uint8), scale)
            cid, tid = CYCLE_ID[s], CYCLE_ID[t]
            dropped = 0
            for py, px in zip(*np.nonzero(cid >= 0)):
                if not (tid[py * scale:(py + 1) * scale, px * scale:(px + 1) * scale] == cid[py, px]).any():
                    dropped += 1
            out["a3_dropped"][i0 + j] = dropped
    out["block_de"] = np.concatenate(all_de) if all_de else np.zeros(0)
    return out


def seam_excess(rgb: np.ndarray, tile_px: int) -> tuple[float, float, float]:
    """(ratio, boundary mean dE, interior mean dE) of an RGB image cut in tile_px tiles."""
    L = srgb_to_oklab(rgb)
    dx = np.sqrt(((L[:, 1:] - L[:, :-1]) ** 2).sum(-1))
    dy = np.sqrt(((L[1:, :] - L[:-1, :]) ** 2).sum(-1))
    bx = np.zeros(dx.shape[1], bool)
    bx[tile_px - 1::tile_px] = True
    by = np.zeros(dy.shape[0], bool)
    by[tile_px - 1::tile_px] = True
    b = np.concatenate([dx[:, bx].ravel(), dy[by, :].ravel()])
    nb = np.concatenate([dx[:, ~bx].ravel(), dy[~by, :].ravel()])
    bm, nm = float(b.mean()), float(nb.mean())
    return bm / max(nm, 1e-12), bm, nm


# ---------------------------------------------------------------------------- C2
def c2_pairs(world: World, tiles: dict, qz: Quantizer, scale: int = 6) -> dict:
    """Edge-strip dE excess for every adjacent own-flat pair on the map (both orientations)."""
    S, F = world.tile_grid
    lut = world.kind_lut
    K = lut[S.astype(np.int64), F.astype(np.int64)]
    own = (K == KIND_FLAT) | (K == KIND_FLAT_VOID)
    kid = S.astype(np.int64) * 32 + F
    res = {}
    keys = sorted(k for k in tiles)
    kindex = {k[0] * 32 + k[1]: i for i, k in enumerate(keys)}
    lab = qz.lab
    # strips (n, 4, 48, 3) at 6x and (n, 4, 8, 3) at 1x: right, left, bottom, top
    hi = np.stack([tiles[k] for k in keys])
    lo = np.stack([world.flat(*k) for k in keys])
    hs = np.stack([hi[:, :, -1], hi[:, :, 0], hi[:, -1, :], hi[:, 0, :]], 1)
    ls = np.stack([lo[:, :, -1], lo[:, :, 0], lo[:, -1, :], lo[:, 0, :]], 1)
    hl, ll = lab[hs], lab[ls]
    rows = []
    for name, a, b, side_a, side_b in (("h", (slice(None), slice(None, -1)), (slice(None), slice(1, None)), 0, 1),
                                       ("v", (slice(None, -1), slice(None)), (slice(1, None), slice(None)), 2, 3)):
        m = own[a] & own[b]
        pa, pb = kid[a][m], kid[b][m]
        pair = pa * 65536 + pb
        u, cnt = np.unique(pair, return_counts=True)
        A, B = u // 65536, u % 65536
        okk = np.array([(x in kindex) and (y in kindex) for x, y in zip(A.tolist(), B.tolist())], bool)
        A, B, cnt = A[okk], B[okk], cnt[okk]
        ia = np.array([kindex[x] for x in A.tolist()], np.int64)
        ib = np.array([kindex[x] for x in B.tolist()], np.int64)
        de6 = np.sqrt(((hl[ia, side_a] - hl[ib, side_b]) ** 2).sum(-1)).mean(1)
        de1 = np.sqrt(((ll[ia, side_a] - ll[ib, side_b]) ** 2).sum(-1)).mean(1)
        for x, y, c, d6, d1 in zip(A.tolist(), B.tolist(), cnt.tolist(), de6.tolist(), de1.tolist()):
            rows.append((name, x >> 5, x & 31, y >> 5, y & 31, c, d6, d1, d6 - d1))
    if not rows:
        return {"pairs": 0}
    exc = np.array([r[8] for r in rows])
    w = np.array([r[5] for r in rows], np.float64)
    order = np.argsort(-exc, kind="stable")
    nworst = max(1, int(np.ceil(len(rows) * GATES["c2_worst_frac"])))
    worst = [rows[i] for i in order[:nworst]]
    involved = Counter()
    for r in worst:
        involved[key_name(r[1], r[2])] += 1
        involved[key_name(r[3], r[4])] += 1
    res = {"pairs": len(rows), "occurrences": int(w.sum()),
           "excess_mean": float(exc.mean()), "excess_weighted_mean": float((exc * w).sum() / w.sum()),
           "excess_p99": float(np.percentile(exc, 99)), "de6_mean": float(np.mean([r[6] for r in rows])),
           "de1_mean": float(np.mean([r[7] for r in rows])),
           "worst": [{"dir": r[0], "a": key_name(r[1], r[2]), "b": key_name(r[3], r[4]), "count": r[5],
                      "de6": round(r[6], 4), "de1": round(r[7], 4), "excess": round(r[8], 4)} for r in worst],
           "worst_tiles": dict(involved.most_common(50))}
    return res


# ---------------------------------------------------------------------------- previews / C1
def terrain_family_shares(world: World, fams: dict) -> dict[int, dict]:
    """Per used terrain: share of own cells per frame family, plus 'rle' and 'rle_tile' (8x8 RLE
    ground frames, e.g. animated swamp) shares."""
    out = {}
    for t in world.used_terrains:
        t = int(t)
        kinds = world.terrain_kinds(t)
        c = Counter()
        for i in range(256):
            s, f = int(world.chunk_shape[t, i]), int(world.chunk_frame[t, i])
            if kinds[i] in (KIND_FLAT, KIND_FLAT_VOID):
                c[fams.get((s, f & 31), "other")] += 1
            elif kinds[i] == KIND_RLE:
                c["rle"] += 1
                fr = world.shapes.rle_frame(s, f)
                if fr is not None and fr.pixels.shape == (8, 8) and fr.mask.all():
                    c["rle_tile"] += 1
        out[t] = {k: v / 256 for k, v in c.items()}
    return out


def pick_preview_chunks(world: World, fams: dict, per_category: int = 2, min_dist: int = 24) -> list[dict]:
    """Deterministic pick of representative chunks: 2 per category in PREVIEW_CATEGORIES."""
    shares = terrain_family_shares(world, fams)
    anchors = choose_anchors(world)
    g = lambda d, *ks: sum(d.get(k, 0.0) for k in ks)  # noqa: E731
    land = lambda d: sum(v for k, v in d.items() if k not in ("water", "shore", "void", "other", "rle", "rle_tile",  # noqa: E731
                                                              "marsh", "floor"))
    preds = {
        "coast": lambda d: (g(d, "shore") >= 0.08 and g(d, "water", "shore") >= 0.25 and land(d) >= 0.2
                            and g(d, "stone", "floor") < 0.05, min(g(d, "water", "shore"), land(d))),
        "town": lambda d: (g(d, "stone", "sand+stone", "dirt+stone", "grass+stone") >= 0.4 and g(d, "rle") >= 0.03,
                           g(d, "stone") + g(d, "rle")),
        "roads": lambda d: (0.1 <= g(d, "stone", "dirt+stone", "grass+stone", "sand+stone") <= 0.4
                            and g(d, "grass", "dirt", "sand", "dirt+grass", "grass+sand", "dirt+sand") >= 0.5,
                            min(g(d, "stone", "dirt+stone", "grass+stone", "sand+stone"), 0.3)),
        "forest": lambda d: (g(d, "dirt+grass") >= 0.15 and g(d, "rle") >= 0.05 and g(d, "water", "shore") < 0.05,
                             g(d, "dirt+grass") + g(d, "rle")),
        "swamp": lambda d: (g(d, "marsh") >= 0.5 or g(d, "rle_tile") >= 0.3, g(d, "marsh") + g(d, "rle_tile")),
        "dungeon": lambda d: (g(d, "void") >= 0.15 and g(d, "stone", "dirt", "dirt+stone", "other") >= 0.35,
                              g(d, "stone", "dirt+stone") + g(d, "void")),
    }
    usage = world.terrain_usage
    picks = []
    for cat in PREVIEW_CATEGORIES:
        cands = []
        for t, d in shares.items():
            ok, score = preds[cat](d)
            if ok:
                cands.append((-score, -int(usage[t]), t))
        cands.sort()
        chosen = []
        for _, _, t in cands:
            cy, cx = anchors[t]
            far = all(min(abs(cy - c[0]), 192 - abs(cy - c[0])) + min(abs(cx - c[1]), 192 - abs(cx - c[1])) >= min_dist
                      for c in [(p["cy"], p["cx"]) for p in picks + chosen])
            if far:
                chosen.append({"category": cat, "terrain": t, "cy": cy, "cx": cx,
                               "shares": {k: round(v, 3) for k, v in sorted(shares[t].items())}})
            if len(chosen) >= per_category:
                break
        picks.extend(chosen)
    return picks


def view_region(world: World, cache: FlatLayerCache, cy: int, cx: int, tw: int = 40, th: int = 25, pad: int = 0):
    ty0 = cy * 16 + 8 - th // 2 - pad
    tx0 = cx * 16 + 8 - tw // 2 - pad
    return world_region(world, cache, ty0, tx0, th + 2 * pad, tw + 2 * pad)


def region_baseline(world: World, route: str, params: dict, idx_padded: np.ndarray, region_padded, fams):
    """Region-level render of the same route (QA C1 baseline), or None if the route has no CPU kernel."""
    if route.startswith("r3"):
        from .route3 import R3Kernel, R3Params
        fields = {k: v for k, v in params.items() if k in R3Params.__dataclass_fields__}
        if "hybrid_families" in fields:
            fields["hybrid_families"] = tuple(fields["hybrid_families"])
        p = R3Params(**fields)
        k = R3Kernel(world.pal8, world.pal6, p)
        if p.variant == "mixed":
            a, b = k.window(idx_padded, False), k.window(idx_padded, True)
            from .route3 import hybrid_key_set
            hyb = set(hybrid_key_set(world, p))
            m = np.array([[(int(s), int(f)) in hyb for s, f in zip(rs, rf)]
                          for rs, rf in zip(region_padded.src_shape, region_padded.src_frame)], bool)
            m6 = np.repeat(np.repeat(m, 8 * p.scale, 0), 8 * p.scale, 1)
            return np.where(m6, b, a)
        return k.window(idx_padded, p.variant == "hybrid")
    if route.startswith("r2"):
        try:
            from .route2 import region_render
            return region_render(world, idx_padded, params)
        except Exception as e:  # noqa: BLE001 - GPU/torch missing: no baseline
            get_logger().warning("no route-2 region baseline: %s", e)
            return None
    return None


def save_rgb(path: str, rgb: np.ndarray) -> None:
    from PIL import Image
    import io as _io
    b = _io.BytesIO()
    Image.fromarray(np.asarray(rgb, np.uint8)).save(b, format="PNG", optimize=False, compress_level=6)
    atomic_write_bytes(path, b.getvalue())


def downscale(rgb: np.ndarray, factor: float) -> np.ndarray:
    from PIL import Image
    im = Image.fromarray(np.asarray(rgb, np.uint8))
    w, h = im.size
    return np.asarray(im.resize((round(w * factor), round(h * factor)), Image.LANCZOS))


def contact_sheets(out_dir: str, keys, tiles: dict, world: World, fams: dict, metrics_by_key: dict,
                   baseline: dict | None = None, per_page: int = 96, cols: int = 8, zoom: int = 2) -> list[str]:
    from PIL import Image, ImageDraw
    use = world.flat_use
    by_fam = defaultdict(list)
    for k in keys:
        by_fam[fams.get(k, "other")].append(k)
    paths = []
    T = 48 * zoom
    panels = 3 if baseline else 2
    cw, chh = panels * T + (panels - 1) * 2 + 8, T + 16
    for fam in family_order(by_fam):
        ks = sorted(by_fam.get(fam, []), key=lambda k: (-use.get(k, 0), k))
        for p0 in range(0, len(ks), per_page):
            page = ks[p0:p0 + per_page]
            rows = (len(page) + cols - 1) // cols
            img = Image.new("RGB", (cols * cw, rows * chh + 18), (32, 32, 32))
            d = ImageDraw.Draw(img)
            d.text((4, 3), f"{fam} {p0 // per_page + 1}: 1x NN | candidate" + (" | baseline" if baseline else "")
                   + "  (orange: B1<97%, red: B1<85% or A2/A3 error)", fill=(230, 230, 230))
            for i, k in enumerate(page):
                r, c = divmod(i, cols)
                x0, y0 = c * cw + 4, r * chh + 18
                m = metrics_by_key.get(k, {})
                b1 = m.get("b1", 1.0)
                col = (230, 230, 230)
                if b1 < GATES["b1_flag"]:
                    col = (255, 160, 40)
                if b1 < GATES["b1_floor"] or m.get("a2_bad", 0) or m.get("a3_p4", 0):
                    col = (255, 60, 60)
                d.text((x0, y0), f"{key_name(*k)} B1 {b1 * 100:.0f}% u{use.get(k, 0)}", fill=col)
                ims = [np.repeat(np.repeat(world.flat(*k), 6, 0), 6, 1), tiles[k]]
                if baseline:
                    ims.append(baseline.get(k, ims[0]))
                for j, t in enumerate(ims):
                    rgb = np.repeat(np.repeat(world.pal8[t], zoom, 0), zoom, 1)
                    img.paste(Image.fromarray(rgb), (x0 + j * (T + 2), y0 + 12))
                if col != (230, 230, 230):
                    d.rectangle([x0 - 2, y0 + 10, x0 + panels * (T + 2), y0 + 13 + T], outline=col)
            p = os.path.join(out_dir, "sheets", f"{fam.replace('+', '-')}_{p0 // per_page + 1:02d}.png")
            save_rgb(p, np.asarray(img))
            paths.append(p)
    return paths


# ---------------------------------------------------------------------------- driver
def run_qa(path: str, world: World, out_dir: str, baseline_path: str | None = None, previews: bool = True,
           sheets: bool = True, c1: bool = True, c2: bool = True, overlay: bool = True, scale: int = 6,
           chunks: list[dict] | None = None, log=None) -> dict:
    log = log or get_logger()
    t0 = time.time()
    prov = Provider.from_world(world)
    lut = ramp_lut(engine_ramps(world.pal6))
    qz = Quantizer(world.pal8, lut)
    fams = frame_families(world)
    tiles, side, rep, info = load_tiles(path, prov, scale, log)
    keys = sorted(k for k in tiles if world.flat(*k) is not None)
    log.info("QA %s: %d tiles (%s, route %s)", path, len(keys), info["kind"], info["route"])
    m = tile_metrics(keys, [tiles[k] for k in keys], [world.flat(*k) for k in keys], qz, lut, scale)
    use = world.flat_use
    report = {"path": path, "route": info["route"], "kind": info["kind"], "tiles": len(keys),
              "missing_flats": len([k for k in world.flat_keys if k not in tiles]), "gates": GATES}
    # A1
    if rep is not None:
        s = rep.summary()
        a1_bad = sum(v for k, v in s["by_rule"].items() if k.split(":")[0] in ("F1", "F2", "F3", "F4") and "info" not in k)
        report["A1"] = {"format_findings": a1_bad, "check": s, "pass": a1_bad == 0 and not s["root_disabled"]
                        and s["rejected"] == 0}
    else:
        report["A1"] = {"pass": None, "note": "candidate set: no files to check"}
    report["A2"] = {"tiles_with_errors": int((m["a2_bad"] > 0).sum()), "pixels": int(m["a2_bad"].sum()),
                    "pass": bool((m["a2_bad"] == 0).all())}
    report["A3"] = {"tiles_p4": int((m["a3_p4"] > 0).sum()), "p4_pixels": int(m["a3_p4"].sum()),
                    "dropped_sparkles": int(m["a3_dropped"].sum()), "tiles_dropped": int((m["a3_dropped"] > 0).sum()),
                    "pass": bool((m["a3_p4"] == 0).all())}
    w = np.array([max(use.get(k, 0), 0) for k in keys], np.float64)
    b1 = m["b1"]
    hist_edges = [0, 0.85, 0.90, 0.95, 0.97, 0.99, 1.0000001]
    hist = np.histogram(b1, bins=hist_edges)[0].tolist()
    flagged = [key_name(*keys[i]) for i in np.nonzero(b1 < GATES["b1_flag"])[0]]
    failed = [key_name(*keys[i]) for i in np.nonzero(b1 < GATES["b1_floor"])[0]]
    report["B1"] = {"aggregate": float(b1.mean()), "usage_weighted": float((b1 * w).sum() / max(w.sum(), 1)),
                    "min": float(b1.min()), "below_97": len(flagged), "below_85": len(failed),
                    "histogram": {"bins": ["<85", "85-90", "90-95", "95-97", "97-99", ">=99"], "counts": hist},
                    "flagged": flagged, "failed": failed,
                    "pass": bool(b1.mean() >= GATES["b1_aggregate"] and not failed)}
    b2 = m["b2"]
    thr = float(np.quantile(b2, GATES["b2_bottom_frac"]))
    report["B2"] = {"aggregate": float(b2.mean()), "bottom5_threshold": thr,
                    "bottom5": [key_name(*keys[i]) for i in np.nonzero(b2 <= thr)[0]][:400]}
    de = m["block_de"]
    report["B3"] = {"mean": float(de.mean()), "p99": float(np.percentile(de, 99)),
                    "pass": bool(np.percentile(de, 99) <= GATES["b3_p99"])}
    b4 = m["b4"]
    report["B4"] = {"aggregate": float(b4.mean()), "below_95": int((b4 < GATES["b4_flag"]).sum()),
                    "flagged": [key_name(*keys[i]) for i in np.nonzero(b4 < GATES["b4_flag"])[0]][:400],
                    "pass": bool(b4.mean() >= GATES["b4_aggregate"])}
    # D1
    dis = {k: side[k].get("disagreement") for k in keys if k in side and side[k].get("disagreement") is not None
           and side[k].get("unique_instances", 1) > 1}
    if dis:
        med = float(np.median(list(dis.values())))
        fl = sorted((key_name(*k) for k, v in dis.items() if v > GATES["d1_factor"] * med))
        report["D1"] = {"median": med, "mean": float(np.mean(list(dis.values()))), "flagged": fl,
                        "n_flagged": len(fl), "multi_instance_keys": len(dis)}
    else:
        report["D1"] = {"note": "no sidecar disagreement data"}
    if c2:
        log.info("C2 edge pairs")
        report["C2"] = c2_pairs(world, tiles, qz, scale)
    metrics_by_key = {k: {n: float(m[n][i]) for n in ("b1", "b2", "b3_mean", "b4", "a2_bad", "a3_p4", "a3_dropped")}
                      for i, k in enumerate(keys)}
    os.makedirs(out_dir, exist_ok=True)
    cache = FlatLayerCache(world)
    if chunks is None:
        chunks = pick_preview_chunks(world, fams)
    report["preview_chunks"] = chunks
    if c1 or previews:
        log.info("C1/previews on %d views", len(chunks))
        c1_rows = []
        pad = 2
        for ch in chunks:
            reg = view_region(world, cache, ch["cy"], ch["cx"], pad=0)
            fin, used = final_render(reg, tiles, world, scale)
            nn = nn_render(reg, scale)
            tag = f"{ch['category']}_{ch['cy']:03d}_{ch['cx']:03d}"
            row = {"view": tag, "override_cells": float(used.mean())}
            fr, _, _ = seam_excess(world.pal8[fin], 8 * scale)
            row["final"] = fr
            row["nn"] = seam_excess(world.pal8[nn], 8 * scale)[0]
            if c1:
                regp = view_region(world, cache, ch["cy"], ch["cx"], pad=pad)
                base = region_baseline(world, info["route"], info.get("params", {}), regp.idx, regp, fams)
                if base is not None:
                    P = pad * 8 * scale
                    base = base[P:-P, P:-P]
                    row["region"] = seam_excess(world.pal8[base], 8 * scale)[0]
                    row["ratio"] = row["final"] / max(row["region"], 1e-12)
            c1_rows.append(row)
            if previews:
                img_f, img_n = world.pal8[fin], world.pal8[nn]
                if overlay:
                    ov, mk = rle_overlay(world, reg)
                    img_f = world.pal8[composite(fin, ov, mk, scale)]
                    img_n = world.pal8[composite(nn, ov, mk, scale)]
                pd = os.path.join(out_dir, "previews")
                save_rgb(os.path.join(pd, f"{tag}_6x.png"), img_f)
                save_rgb(os.path.join(pd, f"{tag}_6x_1280x800.png"), downscale(img_f, 2 / 3))
                save_rgb(os.path.join(pd, f"{tag}_nn6x_1280x800.png"), downscale(img_n, 2 / 3))
                save_rgb(os.path.join(pd, f"{tag}_nn6x.png"), img_n)
        ratios = [r["ratio"] for r in c1_rows if "ratio" in r]
        report["C1"] = {"views": c1_rows}
        if ratios:
            fin_m = float(np.mean([r["final"] for r in c1_rows if "ratio" in r]))
            reg_m = float(np.mean([r["region"] for r in c1_rows if "ratio" in r]))
            report["C1"].update({"final_mean": fin_m, "region_mean": reg_m, "ratio": fin_m / reg_m,
                                 "ratio_max": float(max(ratios)), "pass": bool(fin_m / reg_m <= GATES["c1_ratio"])})
        else:
            report["C1"]["pass"] = None
    if sheets:
        base_tiles = None
        if baseline_path:
            base_tiles = load_tiles(baseline_path, prov, scale, log)[0]
        report["sheets"] = contact_sheets(out_dir, keys, tiles, world, fams, metrics_by_key, base_tiles)
    fam_rows = defaultdict(list)
    for i, k in enumerate(keys):
        fam_rows[fams.get(k, "other")].append(i)
    report["families"] = {f: {"tiles": len(ix), "b1": float(b1[ix].mean()), "b4": float(b4[ix].mean()),
                              "b3_mean": float(m["b3_mean"][ix].mean())} for f, ix in sorted(fam_rows.items())}
    gates = {g: report[g].get("pass") for g in ("A1", "A2", "A3", "B1", "B3", "B4", "C1") if g in report}
    report["gates_pass"] = gates
    report["pass"] = all(v is not False for v in gates.values())
    report["seconds"] = round(time.time() - t0, 1)
    atomic_write_json(os.path.join(out_dir, "report.json"), report)
    atomic_write_json(os.path.join(out_dir, "per_tile.json"), {key_name(*k): v for k, v in metrics_by_key.items()})
    write_markdown(report, os.path.join(out_dir, "report.md"))
    log.info("QA done in %.1fs: pass=%s %s", report["seconds"], report["pass"], gates)
    return report


def write_markdown(r: dict, path: str) -> None:
    pct = lambda v: f"{100 * v:.2f} %"  # noqa: E731
    L = [f"# QA report: `{r['path']}`", "", f"Route `{r['route']}` ({r['kind']}), {r['tiles']} tiles, "
         f"{r['missing_flats']} flats without a tile (NN). Overall: **{'PASS' if r['pass'] else 'FAIL'}**.", "",
         "| Gate | Result | Pass |", "|---|---|---|"]
    yn = lambda v: "n/a" if v is None else ("yes" if v else "**no**")  # noqa: E731
    if "A1" in r:
        L.append(f"| A1 format | {r['A1'].get('format_findings', 'n/a')} format findings | {yn(r['A1'].get('pass'))} |")
    L.append(f"| A2 static palette | {r['A2']['pixels']} bad px in {r['A2']['tiles_with_errors']} tiles | {yn(r['A2']['pass'])} |")
    L.append(f"| A3 cycling (P4) | {r['A3']['p4_pixels']} non-compliant px; {r['A3']['dropped_sparkles']} dropped "
             f"sparkles (warning) | {yn(r['A3']['pass'])} |")
    b1 = r["B1"]
    L.append(f"| B1 block majority | aggregate {pct(b1['aggregate'])} (usage-weighted {pct(b1['usage_weighted'])}), "
             f"min {pct(b1['min'])}; {b1['below_97']} flagged < 97 %, {b1['below_85']} < 85 % | {yn(b1['pass'])} |")
    L.append(f"| B2 box-mean round trip | aggregate {pct(r['B2']['aggregate'])}; bottom 5 % <= "
             f"{pct(r['B2']['bottom5_threshold'])} | info |")
    L.append(f"| B3 block-mean dE | mean {r['B3']['mean']:.4f}, p99 {r['B3']['p99']:.4f} | {yn(r['B3']['pass'])} |")
    L.append(f"| B4 ramp identity | aggregate {pct(r['B4']['aggregate'])}; {r['B4']['below_95']} tiles < 95 % | "
             f"{yn(r['B4']['pass'])} |")
    if "C1" in r:
        c = r["C1"]
        if c.get("ratio") is not None:
            L.append(f"| C1 seam ratio | final {c['final_mean']:.3f} / region {c['region_mean']:.3f} = "
                     f"{c['ratio']:.3f} (max view {c['ratio_max']:.3f}) | {yn(c['pass'])} |")
        else:
            L.append("| C1 seam ratio | no region baseline | n/a |")
    if "C2" in r and r["C2"].get("pairs"):
        c = r["C2"]
        L.append(f"| C2 edge pairs | {c['pairs']} pairs; excess mean {c['excess_mean']:.4f}, p99 "
                 f"{c['excess_p99']:.4f}; worst 1 % listed | info |")
    if "D1" in r and "median" in r["D1"]:
        L.append(f"| D1 disagreement | median {r['D1']['median']:.4f}; {r['D1']['n_flagged']} flagged > 2x | info |")
    L += ["", "## B1 histogram", "", "| bin | tiles |", "|---|---|"]
    L += [f"| {b} | {c} |" for b, c in zip(b1["histogram"]["bins"], b1["histogram"]["counts"])]
    L += ["", "## Families", "", "| family | tiles | B1 | B4 | B3 mean |", "|---|---|---|---|---|"]
    L += [f"| {f} | {v['tiles']} | {pct(v['b1'])} | {pct(v['b4'])} | {v['b3_mean']:.4f} |" for f, v in r["families"].items()]
    if "C1" in r:
        L += ["", "## C1 views", "", "| view | final | region | NN (1x art) | ratio |", "|---|---|---|---|---|"]
        for v in r["C1"]["views"]:
            L.append(f"| {v['view']} | {v['final']:.3f} | {v.get('region', float('nan')):.3f} | {v['nn']:.3f} | "
                     f"{v.get('ratio', float('nan')):.3f} |")
    if "C2" in r and r["C2"].get("worst"):
        L += ["", "## C2 worst pairs (top 25)", "", "| dir | a | b | count | dE6 | dE1 | excess |", "|---|---|---|---|---|---|---|"]
        L += [f"| {w['dir']} | {w['a']} | {w['b']} | {w['count']} | {w['de6']} | {w['de1']} | {w['excess']} |"
              for w in r["C2"]["worst"][:25]]
    L += ["", f"Flagged for review (B1 < 97 %): {len(b1['flagged'])} tiles; see report.json.", ""]
    atomic_write_bytes(path, "\n".join(L).encode())


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="hiresqa.py", description="QA metrics, report, contact sheets and previews.")
    ap.add_argument("path", help="pack root (with pack.txt and x6/) or candidate dir (tiles.npz)")
    ap.add_argument("--out", default=None, help="default: art_work/qa/<basename>")
    ap.add_argument("--static", default=None)
    ap.add_argument("--baseline", default=None, help="second pack/candidates shown as a third panel")
    ap.add_argument("--no-previews", action="store_true")
    ap.add_argument("--no-sheets", action="store_true")
    ap.add_argument("--no-c1", action="store_true")
    ap.add_argument("--no-c2", action="store_true")
    ap.add_argument("--no-overlay", action="store_true", help="previews without the RLE terrain overlay")
    ap.add_argument("--chunks", default=None, help="'cat:cy:cx,...' instead of the automatic pick")
    a = ap.parse_args(argv)
    world = World.load(a.static)
    out = a.out or os.path.join("/home/simonea/ultima7_exult/art_work/qa", os.path.basename(os.path.normpath(a.path)))
    chunks = None
    if a.chunks:
        chunks = [{"category": c.split(":")[0], "cy": int(c.split(":")[1]), "cx": int(c.split(":")[2])}
                  for c in a.chunks.split(",")]
    r = run_qa(a.path, world, out, a.baseline, not a.no_previews, not a.no_sheets, not a.no_c1, not a.no_c2,
               not a.no_overlay, chunks=chunks)
    print(json.dumps(r["gates_pass"]), "PASS" if r["pass"] else "FAIL", "->", out)
    return 0 if r["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
