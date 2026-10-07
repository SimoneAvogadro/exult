"""Side-by-side comparison of flat packs (Phase A report; DESIGN §8.3 F1 review aid).

``hirescompare.py --pack LABEL=PATH[@BASE] ... [--out DIR]`` compares packs given as pack roots or
candidate dirs. ``@BASE`` names the label of a full pack that a subset pack is drawn over in the world
views (the way a per-family promotion would look in the engine); its tile sheets show only its own
tiles.

Outputs under ``--out`` (default ``art_work/qa/compare``):

* ``sheets/<group>.png``: the ``--per-group`` most used keys of every material family (``families.py``)
  and of each ``--groups`` item (BG: ``roads=21+24``). One cell per key: 1x NN (at 6x) | pack 1 | ... |
  pack N at zoom 2, each panel with its B1 (orange < 97 %, red < 85 %); an empty frame where a pack has
  no tile for the key.
* ``sheets/sparkles_<family>.png``: the same cells for the families whose shown keys use cycling
  indices, static pixels dimmed and each cycle range in a flat signal colour, so the shape and the
  placement of the water glints can be compared.
* ``sheets/flagged_low_b1.png``: the most used keys that some full pack has below ``--flag-b1``.
* ``views/<view>.png``: 1:1 crops (``--crop`` tiles square) of the 6x render of every representative
  view (``qa.pick_preview_chunks``, i.e. the QA preview views), approximate RLE overlay included:
  NN first, one panel per pack, and a legend with the view's family shares and each pack's QA C1.
* ``previews/<view>/<label>_6x.png``: the whole view (40x25 tiles, 1920x1200) per pack, and ``nn``.
* ``metrics.json`` / ``metrics.md``: per pack and family (tiles, B1 mean and usage-weighted, tiles
  below 97 % / 85 %, B2, B3 mean, B4, and the share of 6x pixels that differ from the NN upscale of the
  1x art); the same on the keys of each subset pack, per subset group; the
  gate summary of ``<qa-root>/<pack basename>/report.json`` when it exists. Per-key metrics are
  recomputed with ``qa.tile_metrics`` and cross-checked against that QA run's ``per_tile.json`` (a guard
  against silent corruption on the production host).

The output holds no timestamps, so two runs can be compared file by file with ``vote.py compare``.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections import defaultdict
from dataclasses import dataclass, field

import numpy as np

from .check import Provider
from .families import family_order, frame_families, select_keys
from .fill import FlatLayerCache
from .io import World
from .qa import GATES, load_tiles, pick_preview_chunks, save_rgb, tile_metrics, view_region
from .quant import Quantizer
from .render import composite, final_render, nn_render, rle_overlay
from .rules import CYCLE_ID, engine_ramps, ramp_lut
from .util import atomic_write_bytes, atomic_write_json, get_logger, key_name

DEFAULT_OUT = "/home/simonea/ultima7_exult/art_work/qa/compare"
DEFAULT_QA_ROOT = "/home/simonea/ultima7_exult/art_work/qa"
METRIC_NAMES = ("b1", "b2", "b3_mean", "b4", "a2_bad", "a3_p4", "a3_dropped")

BG_COL = (24, 24, 24)
FG = (232, 232, 232)
FG_DIM = (150, 150, 150)
ORANGE = (255, 160, 40)
RED = (255, 70, 70)
# signal colours per cycle range: E0-E7 white, E8-EF cyan, F0-F3 yellow, F4-F7 orange, F8-FB magenta, FC-FE green
CYCLE_COLOURS = np.array([(255, 255, 255), (0, 255, 255), (255, 255, 0), (255, 128, 0), (255, 0, 255),
                          (0, 255, 0)], np.uint8)


@dataclass
class PackData:
    label: str
    path: str
    base: str | None
    tiles: dict
    route: str
    subset_spec: str | None = None
    metrics: dict = field(default_factory=dict)
    qa_dir: str | None = None
    qa_report: dict | None = None

    @property
    def title(self) -> str:
        t = f"{self.label}: {os.path.basename(os.path.normpath(self.path))} ({self.route})"
        return t + (f" over {self.base}" if self.base else "")


def parse_pack_spec(spec: str) -> tuple[str, str, str | None]:
    """``LABEL=PATH[@BASE]`` -> (label, path, base label or None)."""
    label, sep, rest = spec.partition("=")
    path, _, base = rest.partition("@")
    if not sep or not label or not path:
        raise ValueError(f"bad --pack {spec!r} (want LABEL=PATH[@BASE])")
    return label, path, (base or None)


def load_pack(world: World, prov: Provider, label: str, path: str, base: str | None, qa_root: str | None,
              scale: int = 6, log=None) -> PackData:
    tiles, side, _rep, info = load_tiles(path, prov, scale, log)
    tiles = {k: np.asarray(t) for k, t in tiles.items() if world.flat(*k) is not None}
    spec = (info.get("params") or {}).get("subset")
    if spec is None:
        for v in side.values():
            spec = (v.get("params") or {}).get("subset")
            break
    p = PackData(label, path, base, tiles, info.get("route", "?"), spec)
    if qa_root:
        qd = os.path.join(qa_root, os.path.basename(os.path.normpath(path)))
        if os.path.exists(os.path.join(qd, "report.json")):
            p.qa_dir = qd
            with open(os.path.join(qd, "report.json")) as f:
                p.qa_report = json.load(f)
    return p


# ---------------------------------------------------------------------------- metrics
def compute_metrics(world: World, pack: PackData, qz: Quantizer, lut: np.ndarray, scale: int = 6) -> None:
    keys = sorted(pack.tiles)
    if not keys:
        pack.metrics = {}
        return
    m = tile_metrics(keys, [pack.tiles[k] for k in keys], [world.flat(*k) for k in keys], qz, lut, scale)
    pack.metrics = {k: {n: float(m[n][i]) for n in METRIC_NAMES} for i, k in enumerate(keys)}
    for k in keys:                      # how much the route changes the art at all (0 = NN upscale)
        nn = np.repeat(np.repeat(world.flat(*k), scale, 0), scale, 1)
        pack.metrics[k]["nn_diff"] = float((np.asarray(pack.tiles[k]) != nn).mean())


def crosscheck_qa(pack: PackData) -> dict:
    """Recomputed per-key metrics vs the pack's QA ``per_tile.json`` (exact up to 1e-9)."""
    if not pack.qa_dir or not os.path.exists(os.path.join(pack.qa_dir, "per_tile.json")):
        return {"checked": 0, "ok": None, "note": "no QA per_tile.json"}
    with open(os.path.join(pack.qa_dir, "per_tile.json")) as f:
        q = json.load(f)
    bad = []
    for k, m in sorted(pack.metrics.items()):
        r = q.get(key_name(*k))
        if r is None or any(abs(float(r[n]) - m[n]) > 1e-9 for n in METRIC_NAMES if n in r):
            bad.append(key_name(*k))
    extra = sorted(set(q) - {key_name(*k) for k in pack.metrics})
    return {"checked": len(pack.metrics), "mismatched": bad[:50], "n_mismatched": len(bad), "only_in_qa": extra[:50],
            "ok": not bad and not extra}


def aggregate(keys, metrics: dict, use: dict) -> dict:
    ks = [k for k in keys if k in metrics]
    if not ks:
        return {"tiles": 0}
    a = {n: np.array([metrics[k].get(n, np.nan) for k in ks]) for n in ("b1", "b2", "b3_mean", "b4", "nn_diff")}
    w = np.array([max(use.get(k, 0), 0) for k in ks], np.float64)
    used = lambda v: float((v * w).sum() / w.sum()) if w.sum() > 0 else float(v.mean())  # noqa: E731
    b1 = a["b1"]
    return {"tiles": len(ks), "use": int(w.sum()), "b1": float(b1.mean()), "b1_used": used(b1),
            "below_97": int((b1 < GATES["b1_flag"]).sum()), "below_85": int((b1 < GATES["b1_floor"]).sum()),
            "b2": float(a["b2"].mean()), "b3_mean": float(a["b3_mean"].mean()), "b4": float(a["b4"].mean()),
            "nn_diff": float(a["nn_diff"].mean()), "nn_diff_used": used(a["nn_diff"])}


def build_groups(world: World, fams: dict, extra: str = "") -> dict[str, list]:
    """{group: keys sorted by map use (desc), key}: the material families, then ``extra`` items
    (``label=S1+S2``: every flat frame of those shapes)."""
    use = world.flat_use
    order = lambda ks: sorted(ks, key=lambda k: (-use.get(k, 0), k))  # noqa: E731
    by = defaultdict(list)
    for k in world.flat_keys:
        by[fams.get(k, "other")].append(k)
    groups = {f: order(by[f]) for f in family_order(by) if by.get(f)}
    for item in (x.strip() for x in extra.split(",")):
        if not item:
            continue
        label, sep, shapes = item.partition("=")
        if not sep:
            raise ValueError(f"bad group {item!r} (want label=S1+S2)")
        sset = {int(s) for s in shapes.split("+") if s.strip()}
        groups[label] = order(k for k in world.flat_keys if k[0] in sset)
    return groups


# ---------------------------------------------------------------------------- drawing helpers
def _font(size: int):
    from PIL import ImageFont
    try:
        return ImageFont.load_default(size=size)
    except Exception:  # noqa: BLE001 - Pillow without FreeType: fixed bitmap font
        return ImageFont.load_default()


def sparkle_rgb(pal8: np.ndarray, t: np.ndarray) -> np.ndarray:
    """RGB of an index tile with static pixels dimmed to grey and cycling pixels in their range colour."""
    t = np.asarray(t)
    g = (pal8[t].astype(np.float64).mean(-1) * 0.35).astype(np.uint8)
    out = np.repeat(g[..., None], 3, -1)
    cid = CYCLE_ID[t]
    m = cid >= 0
    out[m] = CYCLE_COLOURS[cid[m]]
    return out


def _b1_colour(b1: float):
    return RED if b1 < GATES["b1_floor"] else ORANGE if b1 < GATES["b1_flag"] else FG


def key_sheet(path: str, title: str, keys, world: World, packs: list[PackData], fams: dict, zoom: int = 2,
              per_row: int = 2, sparkle: bool = False) -> str:
    """One cell per key: 1x NN (at 6x) | each pack's tile, at ``zoom``; B1 under each panel."""
    from PIL import Image, ImageDraw
    f13, f11 = _font(13), _font(11)
    use = world.flat_use
    P, gap = 48 * zoom, 4
    npan = 1 + len(packs)
    cw = npan * P + (npan - 1) * gap + 18
    ch = 15 + P + 16 + 8
    rows = max(1, -(-len(keys) // per_row))
    top = 62
    img = Image.new("RGB", (per_row * cw + 10, top + rows * ch), BG_COL)
    d = ImageDraw.Draw(img)
    d.text((8, 5), title, font=f13, fill=FG)
    d.text((8, 24), "columns: 1x NN | " + " | ".join(p.title for p in packs), font=f11, fill=FG_DIM)
    note = "B1 under each panel (orange < 97 %, red < 85 %); empty frame: the pack has no tile for the key"
    if sparkle:
        note = ("static pixels dimmed; cycle ranges E0-E7 white, E8-EF cyan, F0-F3 yellow, F4-F7 orange, "
                "F8-FB magenta, FC-FE green; B1 as in the colour sheets")
    d.text((8, 40), note, font=f11, fill=FG_DIM)
    for i, k in enumerate(keys):
        r, c = divmod(i, per_row)
        x0, y0 = 8 + c * cw, top + r * ch
        d.text((x0, y0), f"{key_name(*k)}  {fams.get(k, 'other')}  map use {use.get(k, 0)}", font=f11, fill=FG)
        src = world.flat(*k)
        panels = [("1x NN", np.repeat(np.repeat(src, 6, 0), 6, 1), None)]
        panels += [(p.label, p.tiles.get(k), p.metrics.get(k)) for p in packs]
        for j, (lab, t, m) in enumerate(panels):
            px, py = x0 + j * (P + gap), y0 + 15
            if t is None:
                d.rectangle([px, py, px + P - 1, py + P - 1], outline=(80, 80, 80))
                d.text((px + 2, py + P + 2), f"{lab} -", font=f11, fill=FG_DIM)
                continue
            rgb = sparkle_rgb(world.pal8, t) if sparkle else world.pal8[np.asarray(t)]
            img.paste(Image.fromarray(np.repeat(np.repeat(rgb, zoom, 0), zoom, 1)), (px, py))
            if m is None:
                d.text((px + 2, py + P + 2), lab, font=f11, fill=FG_DIM)
            else:
                d.text((px + 2, py + P + 2), f"{lab} {100 * m['b1']:.1f}%", font=f11, fill=_b1_colour(m["b1"]))
    save_rgb(path, np.asarray(img))
    return path


def choose_crop(reg, fams: dict, ct: int, cover: np.ndarray | None = None) -> tuple[int, int]:
    """Top-left tile of the ct x ct crop of a view with the most material transitions (family changes
    between adjacent own flats, distinct families) and the fewest RLE cells and cells hidden by the
    overlay (``cover``: (th*8, tw*8) bool, the RLE overlay mask); ties: nearest the centre."""
    th, tw = reg.th, reg.tw
    hidden = np.zeros((th, tw), bool) if cover is None else cover.reshape(th, 8, tw, 8).mean((1, 3)) > 0.25
    ct = min(ct, th, tw)
    fam = np.full((th, tw), -1, np.int32)
    ids: dict[str, int] = {}
    for y in range(th):
        for x in range(tw):
            if reg.own[y, x] and reg.src_shape[y, x] >= 0:
                f = fams.get((int(reg.src_shape[y, x]), int(reg.src_frame[y, x])), "other")
                fam[y, x] = ids.setdefault(f, len(ids))
    best = None
    for y0 in range(th - ct + 1):
        for x0 in range(tw - ct + 1):
            s = fam[y0:y0 + ct, x0:x0 + ct]
            changes = int((s[:, 1:] != s[:, :-1]).sum() + (s[1:] != s[:-1]).sum())
            nf = len(set(s[s >= 0].ravel().tolist()))
            score = changes + 10 * nf - int((s < 0).sum()) - 3 * int(hidden[y0:y0 + ct, x0:x0 + ct].sum())
            cand = (-score, abs(2 * y0 + ct - th) + abs(2 * x0 + ct - tw), y0, x0)
            if best is None or cand < best:
                best = cand
    return best[2], best[3]


def view_renders(world: World, cache: FlatLayerCache, ch: dict, layers: list[tuple[str, dict]], scale: int = 6,
                 overlay: bool = True):
    """(region, [(label, RGB 6x render)], 1x overlay mask or None): NN first, then one render per
    (label, tiles) layer."""
    reg = view_region(world, cache, ch["cy"], ch["cx"], pad=0)
    imgs = [("nn", nn_render(reg, scale))] + [(lab, final_render(reg, t, world, scale)[0]) for lab, t in layers]
    mk = None
    if overlay:
        ov, mk = rle_overlay(world, reg)
        imgs = [(lab, composite(i, ov, mk, scale)) for lab, i in imgs]
    return reg, [(lab, world.pal8[i]) for lab, i in imgs], mk


def view_sheet(path: str, tag: str, ch: dict, reg, imgs, crop: tuple[int, int], ct: int, packs: list[PackData],
               scale: int = 6, cols: int = 3) -> str:
    from PIL import Image, ImageDraw
    f13, f11 = _font(13), _font(11)
    T = 8 * scale
    S = ct * T
    y0, x0 = crop
    n = len(imgs) + 1                                     # + legend panel
    rows = -(-n // cols)
    top = 30
    img = Image.new("RGB", (cols * (S + 8) + 8, top + rows * (S + 24)), BG_COL)
    d = ImageDraw.Draw(img)
    ty0, tx0 = reg.ty0 + y0, reg.tx0 + x0
    d.text((8, 7), f"{tag}: {ct}x{ct} tiles at world tile ({ty0}, {tx0}), 1:1 at 6x (each panel {S}x{S} px)",
           font=f13, fill=FG)
    titles = {"nn": "1x art, nearest neighbour (engine without overrides)"}
    titles.update({p.label: p.title for p in packs})
    for i, (lab, rgb) in enumerate(imgs):
        r, c = divmod(i, cols)
        px, py = 8 + c * (S + 8), top + r * (S + 24)
        d.text((px, py + 2), titles.get(lab, lab), font=f11, fill=FG)
        img.paste(Image.fromarray(np.ascontiguousarray(rgb[y0 * T:(y0 + ct) * T, x0 * T:(x0 + ct) * T])), (px, py + 18))
    r, c = divmod(len(imgs), cols)
    px, py = 8 + c * (S + 8), top + r * (S + 24) + 18
    lines = [f"view {tag}", f"terrain {ch.get('terrain', '?')}, chunk (cy {ch['cy']}, cx {ch['cx']})", ""]
    sh = ch.get("shares") or {}
    if sh:
        lines.append("own-cell shares of the chunk:")
        lines += [f"  {k} {100 * v:.0f} %" for k, v in sorted(sh.items(), key=lambda kv: (-kv[1], kv[0]))[:7]]
        lines.append("")
    lines.append("QA C1 seam ratio on this view (final / region):")
    for p in packs:
        row = next((v for v in ((p.qa_report or {}).get("C1") or {}).get("views", []) if v.get("view") == tag), None)
        if row is None:
            lines.append(f"  {p.label}: n/a")
        elif row.get("ratio") is None:
            lines.append(f"  {p.label}: final {row['final']:.3f}, no region baseline")
        else:
            lines.append(f"  {p.label}: {row['ratio']:.3f} (final {row['final']:.3f}, region {row['region']:.3f})")
    lines += ["", "RLE terrain tiles (shore pieces, swamp) are drawn", "from the 1x art, NN, in every panel",
              "(not part of the flat packs)."]
    for i, ln in enumerate(lines):
        d.text((px, py + 15 * i), ln, font=f11, fill=FG if i == 0 else FG_DIM)
    save_rgb(path, np.asarray(img))
    return path


# ---------------------------------------------------------------------------- report
def _pct(v) -> str:
    return "n/a" if v is None else f"{100 * v:.2f} %"


def _num(v, spec: str = ".4f") -> str:
    return "n/a" if v is None else format(v, spec)


def gate_rows(packs: list[PackData]) -> list[tuple[str, list[str]]]:
    """QA gate summary per pack, from each pack's QA ``report.json`` ("n/a" where a value is missing)."""
    def g(p, *path):
        o = p.qa_report
        for k in path:
            if not isinstance(o, dict) or k not in o:
                return None
            o = o[k]
        return o

    def row(name, fn):
        return name, [fn(p) if p.qa_report else "n/a" for p in packs]

    def c1(p):
        c = g(p, "C1") or {}
        if c.get("ratio") is None:
            return "n/a (no region baseline)"
        return f"{c['ratio']:.3f} (max view {c['ratio_max']:.3f}){'' if c.get('pass') else ' **> 1.2**'}"

    def c2(p):
        c = g(p, "C2") or {}
        return f"{c['pairs']} pairs; {c['excess_mean']:.4f} / {c['excess_p99']:.4f}" if c.get("pairs") else "n/a"

    def d1(p):
        c = g(p, "D1") or {}
        return f"{c['median']:.4f}; {c['n_flagged']}" if "median" in c else "n/a"

    return [
        row("overall", lambda p: "PASS" if g(p, "pass") else "**FAIL**"),
        row("tiles / flats without a tile", lambda p: f"{g(p, 'tiles')} / {g(p, 'missing_flats')}"),
        row("A1 format / A2 palette px / A3 P4 px",
            lambda p: f"{g(p, 'A1', 'format_findings')} / {g(p, 'A2', 'pixels')} / {g(p, 'A3', 'p4_pixels')}"),
        row("B1 aggregate (usage-weighted)",
            lambda p: f"{_pct(g(p, 'B1', 'aggregate'))} ({_pct(g(p, 'B1', 'usage_weighted'))})"),
        row("B1 min; tiles < 97 % / < 85 %",
            lambda p: f"{_pct(g(p, 'B1', 'min'))}; {g(p, 'B1', 'below_97')} / {g(p, 'B1', 'below_85')}"),
        row("B2 aggregate; bottom 5 % at",
            lambda p: f"{_pct(g(p, 'B2', 'aggregate'))}; <= {_pct(g(p, 'B2', 'bottom5_threshold'))}"),
        row("B3 mean / p99", lambda p: f"{_num(g(p, 'B3', 'mean'))} / {_num(g(p, 'B3', 'p99'))}"),
        row("B4 aggregate; tiles < 95 %", lambda p: f"{_pct(g(p, 'B4', 'aggregate'))}; {g(p, 'B4', 'below_95')}"),
        row("C1 seam ratio (gate <= 1.2)", c1),
        row("C2 pairs; excess mean / p99", c2),
        row("D1 median; flagged > 2x", d1),
    ]


def write_markdown(res: dict, packs: list[PackData], path: str) -> None:
    labs = [p.label for p in packs]
    L = ["# Pack comparison", "", "| label | pack | route | tiles | tree hash | QA per-tile cross-check |",
         "|---|---|---|---|---|---|"]
    for p in packs:
        x = res["packs"][p.label]
        cc = x["qa_crosscheck"]
        ccs = "n/a" if cc.get("ok") is None else (f"{cc['checked']} keys identical" if cc["ok"] else
                                                  f"**{cc['n_mismatched']} differ**")
        L.append(f"| {p.label} | `{p.path}`{' over ' + p.base if p.base else ''} | {p.route} | {x['tiles']} | "
                 f"`{x['tree_hash'][:16]}` | {ccs} |")
    L += ["", "## QA gates", "", "| gate | " + " | ".join(labs) + " |", "|---|" + "---|" * len(labs)]
    L += [f"| {name} | " + " | ".join(vals) + " |" for name, vals in gate_rows(packs)]
    fam = res["groups"]
    L += ["", "## Per family: B1 mean (usage-weighted)", "",
          "| group | keys | map use | " + " | ".join(labs) + " |", "|---|---|---|" + "---|" * len(labs)]
    for g, row in fam.items():
        cells = []
        for lab in labs:
            a = row["packs"].get(lab, {})
            cells.append(f"{100 * a['b1']:.2f} ({100 * a['b1_used']:.2f})" if a.get("tiles") else "-")
        L.append(f"| {g} | {row['keys']} | {row['use']} | " + " | ".join(cells) + " |")
    L += ["", "## Per family: tiles below 97 % / below 85 % B1", "",
          "| group | " + " | ".join(labs) + " |", "|---|" + "---|" * len(labs)]
    for g, row in fam.items():
        cells = [(f"{a['below_97']} / {a['below_85']}" if a.get("tiles") else "-")
                 for a in (row["packs"].get(lab, {}) for lab in labs)]
        L.append(f"| {g} | " + " | ".join(cells) + " |")
    L += ["", "## Per family: B2 round trip / B3 block-mean dE / B4 ramp identity", "",
          "| group | " + " | ".join(labs) + " |", "|---|" + "---|" * len(labs)]
    for g, row in fam.items():
        cells = [(f"{100 * a['b2']:.1f} % / {a['b3_mean']:.4f} / {100 * a['b4']:.2f} %" if a.get("tiles") else "-")
                 for a in (row["packs"].get(lab, {}) for lab in labs)]
        L.append(f"| {g} | " + " | ".join(cells) + " |")
    L += ["", "## Per family: share of 6x pixels that differ from the NN upscale of the 1x art (mean / usage-weighted)",
          "", "0 % means the route left the art as nearest neighbour; it measures how much a route changes, not "
          "quality.", "", "| group | " + " | ".join(labs) + " |", "|---|" + "---|" * len(labs)]
    for g, row in fam.items():
        cells = [(f"{100 * a['nn_diff']:.1f} % / {100 * a['nn_diff_used']:.1f} %" if a.get("tiles") else "-")
                 for a in (row["packs"].get(lab, {}) for lab in labs)]
        L.append(f"| {g} | " + " | ".join(cells) + " |")
    for lab, sub in res.get("subsets", {}).items():
        L += ["", f"## On the keys of subset pack `{lab}` ({sub['spec'] or 'by family'})", "",
              "B1 mean (usage-weighted); tiles < 97 % / < 85 %; B2; B3 mean.", "",
              "| group | keys | " + " | ".join(labs) + " |", "|---|---|" + "---|" * len(labs)]
        for g, row in sub["groups"].items():
            cells = []
            for l2 in labs:
                a = row["packs"].get(l2, {})
                cells.append(f"{100 * a['b1']:.1f} ({100 * a['b1_used']:.1f}); {a['below_97']} / {a['below_85']}; "
                             f"{100 * a['b2']:.1f} %; {a['b3_mean']:.4f}" if a.get("tiles") else "-")
            L.append(f"| {g} | {row['keys']} | " + " | ".join(cells) + " |")
    L += ["", "## Images", ""]
    L += [f"* `{p}`" for p in res["sheets"] + res["views"]]
    L.append("")
    atomic_write_bytes(path, "\n".join(L).encode())


# ---------------------------------------------------------------------------- driver
def run_compare(world: World, specs: list[str], out: str, qa_root: str | None = DEFAULT_QA_ROOT,
                groups_spec: str = "roads=21+24", per_group: int = 12, flag_b1: float = 0.90, crop: int = 10,
                chunks: list[dict] | None = None, previews: bool = True, scale: int = 6, log=None) -> dict:
    from .vote import tree_digests, tree_hash
    log = log or get_logger()
    prov = Provider.from_world(world)
    lut = ramp_lut(engine_ramps(world.pal6))
    qz = Quantizer(world.pal8, lut)
    fams = frame_families(world)
    use = world.flat_use
    packs = []
    for s in specs:
        lab, path, base = parse_pack_spec(s)
        p = load_pack(world, prov, lab, path, base, qa_root, scale, log)
        compute_metrics(world, p, qz, lut, scale)
        packs.append(p)
        log.info("pack %s: %d tiles from %s (route %s)", lab, len(p.tiles), path, p.route)
    by_label = {p.label: p for p in packs}
    if len(by_label) != len(packs):
        raise ValueError("duplicate pack labels")
    for p in packs:
        if p.base is not None and p.base not in by_label:
            raise ValueError(f"pack {p.label}: unknown base {p.base!r}")
    res = {"packs": {}, "groups": {}, "subsets": {}, "sheets": [], "views": [], "previews": [], "crops": {}}
    rel = lambda fn: os.path.relpath(fn, out)  # noqa: E731 - outputs are listed relative to OUT
    for p in packs:
        res["packs"][p.label] = {"path": p.path, "base": p.base, "route": p.route, "tiles": len(p.tiles),
                                 "subset_spec": p.subset_spec, "tree_hash": tree_hash(tree_digests(p.path)),
                                 "qa_dir": p.qa_dir, "qa_crosscheck": crosscheck_qa(p),
                                 "all": aggregate(sorted(p.metrics), p.metrics, use)}
    groups = build_groups(world, fams, groups_spec)
    for g, keys in groups.items():
        res["groups"][g] = {"keys": len(keys), "use": int(sum(max(use.get(k, 0), 0) for k in keys)),
                            "packs": {p.label: aggregate(keys, p.metrics, use) for p in packs}}
    for p in packs:
        if len(p.tiles) >= len(world.flat_keys):
            continue
        if p.subset_spec:
            sub = select_keys(world, p.subset_spec, fams)[1]
            sgroups = {g: [tuple(int(v) for v in n.split("_")) for n in names] for g, names in sub.items()}
        else:
            sgroups = {g: [k for k in ks if k in p.tiles] for g, ks in groups.items()}
            sgroups = {g: ks for g, ks in sgroups.items() if ks}
        allk = sorted({k for ks in sgroups.values() for k in ks})
        rows = {g: {"keys": len(ks), "packs": {q.label: aggregate(ks, q.metrics, use) for q in packs}}
                for g, ks in list(sgroups.items()) + [("all", allk)]}
        res["subsets"][p.label] = {"spec": p.subset_spec, "groups": rows}
    # sheets
    os.makedirs(out, exist_ok=True)
    sheets_dir = os.path.join(out, "sheets")
    for g, keys in groups.items():
        top = keys[:per_group]
        fn = os.path.join(sheets_dir, f"{g.replace('+', '-')}.png")
        res["sheets"].append(rel(key_sheet(fn, f"{g}: the {len(top)} most used of {len(keys)} keys", top, world,
                                           packs, fams)))
        if any((CYCLE_ID[world.flat(*k)] >= 0).any() for k in top):
            fn = os.path.join(sheets_dir, f"sparkles_{g.replace('+', '-')}.png")
            res["sheets"].append(rel(key_sheet(fn, f"{g} sparkles (cycling pixels): the {len(top)} most used keys",
                                               top, world, packs, fams, sparkle=True)))
    full = [p for p in packs if p.base is None]
    low = sorted({k for p in full for k, m in p.metrics.items() if m["b1"] < flag_b1}, key=lambda k: (-use.get(k, 0), k))
    if low:
        fn = os.path.join(sheets_dir, "flagged_low_b1.png")
        res["sheets"].append(rel(key_sheet(fn, f"flagged: the {min(per_group, len(low))} most used of {len(low)} keys "
                                               f"with B1 < {100 * flag_b1:.0f} % in some full pack", low[:per_group],
                                           world, packs, fams)))
    # views
    cache = FlatLayerCache(world)
    if chunks is None:
        chunks = pick_preview_chunks(world, fams)
    layers = []
    for p in packs:
        t = dict(by_label[p.base].tiles) if p.base else {}
        t.update(p.tiles)
        layers.append((p.label, t))
    res["chunks"] = chunks
    for ch in chunks:
        tag = f"{ch['category']}_{ch['cy']:03d}_{ch['cx']:03d}"
        reg, imgs, cover = view_renders(world, cache, ch, layers, scale)
        cy0, cx0 = choose_crop(reg, fams, crop, cover)
        res["views"].append(rel(view_sheet(os.path.join(out, "views", f"{tag}.png"), tag, ch, reg, imgs, (cy0, cx0),
                                           crop, packs, scale)))
        res["crops"][tag] = {"ty": reg.ty0 + cy0, "tx": reg.tx0 + cx0, "tiles": crop}
        if previews:
            for lab, rgb in imgs:
                fn = os.path.join(out, "previews", tag, f"{lab}_6x.png")
                save_rgb(fn, rgb)
                res["previews"].append(rel(fn))
        log.info("view %s: crop at tile (%d, %d)", tag, reg.ty0 + cy0, reg.tx0 + cx0)
    res["ok"] = all(x["qa_crosscheck"].get("ok") is not False for x in res["packs"].values())
    atomic_write_json(os.path.join(out, "metrics.json"), res)
    write_markdown(res, packs, os.path.join(out, "metrics.md"))
    return res


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="hirescompare.py", description="Side-by-side comparison of flat packs: "
                                 "per-family sheets, sparkle sheets, 6x view crops and previews, metrics.")
    ap.add_argument("--pack", action="append", required=True, metavar="LABEL=PATH[@BASE]",
                    help="pack root or candidate dir; @BASE: draw this subset pack over pack BASE in the views")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--static", default=None)
    ap.add_argument("--qa-root", default=DEFAULT_QA_ROOT, help="QA outputs per pack basename ('' to skip)")
    ap.add_argument("--groups", default="roads=21+24", help="extra key groups 'label=S1+S2,...' (shapes)")
    ap.add_argument("--per-group", type=int, default=12, help="keys per sheet (most used first)")
    ap.add_argument("--flag-b1", type=float, default=0.90)
    ap.add_argument("--crop", type=int, default=10, help="view crop side in tiles")
    ap.add_argument("--chunks", default=None, help="'cat:cy:cx,...' instead of the QA preview pick")
    ap.add_argument("--no-previews", action="store_true", help="no full-size per-pack view renders")
    a = ap.parse_args(argv)
    world = World.load(a.static)
    chunks = None
    if a.chunks:
        chunks = [{"category": c.split(":")[0], "cy": int(c.split(":")[1]), "cx": int(c.split(":")[2])}
                  for c in a.chunks.split(",")]
    r = run_compare(world, a.pack, a.out, a.qa_root or None, a.groups, a.per_group, a.flag_b1, a.crop, chunks,
                    not a.no_previews)
    print(json.dumps({lab: {"tiles": x["tiles"], "tree": x["tree_hash"][:16], "qa_crosscheck": x["qa_crosscheck"].get("ok")}
                      for lab, x in r["packs"].items()}), "OK" if r["ok"] else "QA CROSS-CHECK MISMATCH", "->", a.out)
    return 0 if r["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
