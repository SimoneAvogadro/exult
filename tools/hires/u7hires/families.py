"""Material families of flat frames (contact sheets, per-family routing, preview selection).

Each palette index gets a pixel class from its OKLab colour (void, water, grass, sand, dirt, stone,
other); cycling indices are ignored. A frame's family comes from its pixel-class shares:

* ``void``  >= 60 % void pixels;
* ``shore`` >= 15 % water and >= 15 % land (grass/sand/dirt/stone) -- coasts, river banks;
* ``water`` the rest with water dominant;
* a land class (``grass``, ``sand``, ``dirt``, ``stone``) when it dominates and the second land class
  is below 20 %; otherwise the transition ``<a>+<b>`` (alphabetical), e.g. ``dirt+grass``;
* ``other`` when unclassified colours dominate (letters, markers, ornaments).

Calibrated by eye on BG (tmp atlas of shapes 0-149). An optional JSON ``{"<shape>": fam}`` or
``{"<shape>_<frame>": fam}`` overrides entries.
"""

from __future__ import annotations

import json
import os

import numpy as np

from .quant import srgb_to_oklab
from .rules import IS_CYCLING

PIXEL_CLASSES = ("void", "water", "grass", "sand", "dirt", "stone", "other")
LAND = ("grass", "sand", "dirt", "stone")


def pixel_classes(pal8: np.ndarray) -> np.ndarray:
    """(256,) int8 index -> PIXEL_CLASSES position (-1 for cycling indices)."""
    lab = srgb_to_oklab(np.asarray(pal8, np.uint8).reshape(256, 3))
    L, a, b = lab[:, 0], lab[:, 1], lab[:, 2]
    C = np.hypot(a, b)
    h = np.degrees(np.arctan2(b, a)) % 360
    out = np.full(256, PIXEL_CLASSES.index("other"), np.int8)
    for i in range(256):
        if IS_CYCLING[i]:
            out[i] = -1
        elif L[i] < 0.12:
            out[i] = 0
        elif C[i] >= 0.09 and 225 <= h[i] < 300:
            out[i] = 1
        elif C[i] < 0.02:
            out[i] = 5
        elif 100 <= h[i] < 200:
            out[i] = 2
        elif 20 <= h[i] < 100:
            out[i] = 3 if L[i] >= 0.52 else 4
    return out


def frame_family(tile: np.ndarray, pcls: np.ndarray) -> str:
    c = pcls[np.asarray(tile).ravel()]
    c = c[c >= 0]
    if not len(c):
        return "water"                      # all-cycling frame (sparkle water)
    share = np.bincount(c, minlength=len(PIXEL_CLASSES)) / len(c)
    s = dict(zip(PIXEL_CLASSES, share))
    land = sum(s[k] for k in LAND)
    if s["void"] >= 0.6:
        return "void"
    if s["water"] >= 0.15 and land >= 0.15:
        return "shore"
    if s["water"] >= max(land, s["other"], s["void"]):
        return "water"
    if s["other"] > land:
        return "other"
    ranked = sorted(LAND, key=lambda k: (-s[k], k))
    if s[ranked[1]] >= 0.2:
        return "+".join(sorted(ranked[:2]))
    return ranked[0]


BG_OVERRIDES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "families_bg.json")


def default_overrides(world) -> str | None:
    """The shipped BG override file when the world's palette 0 is BG's."""
    from .hashing import BG_PALETTE0_CRC32, palette_crc32
    return BG_OVERRIDES if palette_crc32(world.pal8) == BG_PALETTE0_CRC32 else None


def frame_families(world, overrides: str | dict | None = "auto") -> dict[tuple[int, int], str]:
    """Family of every flat (shape, frame) of the world (``overrides="auto"``: BG file for BG)."""
    pcls = pixel_classes(world.pal8)
    fam = {k: frame_family(world.flat(*k), pcls) for k in world.flat_keys}
    if overrides == "auto":
        overrides = default_overrides(world)
    if overrides:
        if isinstance(overrides, str):
            with open(overrides) as f:
                overrides = json.load(f)
        for k, v in overrides.items():
            if k.startswith("_"):
                continue
            if "_" in k:
                s, f = (int(x) for x in k.split("_"))
                fam[(s, f)] = v
            else:
                for key in list(fam):
                    if key[0] == int(k):
                        fam[key] = v
    return fam


def shape_families(world, overrides="auto") -> dict[int, str]:
    """Most common frame family per shape (weighted by map use, then frame count)."""
    ff = frame_families(world, overrides)
    use = world.flat_use
    acc: dict[int, dict[str, float]] = {}
    for (s, f), v in ff.items():
        acc.setdefault(s, {})
        acc[s][v] = acc[s].get(v, 0.0) + 1.0 + use.get((s, f), 0)
    return {s: max(d.items(), key=lambda kv: (kv[1], kv[0]))[0] for s, d in acc.items()}


def family_order(fams) -> list[str]:
    base = ["water", "shore", "marsh", "grass", "sand", "dirt", "stone", "floor"]
    rest = sorted(set(fams) - set(base) - {"void", "other"})
    return base + rest + ["void", "other"]


def is_hybrid_family(fam: str, hybrid_families) -> bool:
    """True if every component of ``fam`` (``a+b`` transitions) is in ``hybrid_families``."""
    parts = fam.split("+")
    return all(p in hybrid_families for p in parts)
