"""Per-(shape, frame) consensus over the instances cut from context windows (recommendation §2.5).

* Index routes: weighted per-pixel **mode**. Ties go to the parent's own index (the NN value of the
  1x pixel), then to the smallest index.
* RGB routes: weighted per-pixel **OKLab medoid** (the instance colour minimising the weighted sum
  of OKLab distances to all instance colours at that pixel), over at most ``cap`` instances chosen by
  weight (ties by arrival order).

Only instances of the best (lowest) tier available for a key are kept: real map context (0) beats
macro sheets (1), which beat self-wrap (2). Identical instance tiles are merged (weights summed), so
memory stays small even with ~500k instances. Arrival order is deterministic when the caller feeds
windows in a fixed order.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field

import numpy as np

from .quant import srgb_to_oklab


@dataclass
class _Entry:
    tier: int
    tiles: dict = field(default_factory=dict)     # digest -> [tile, weight, count, seq]
    seq: int = 0
    instances: int = 0
    weight: float = 0.0


def _digest(a: np.ndarray) -> bytes:
    return hashlib.blake2b(np.ascontiguousarray(a).tobytes(), digest_size=16).digest()


class _Base:
    def __init__(self):
        self.entries: dict[tuple[int, int], _Entry] = {}

    def _entry(self, key, tier) -> _Entry | None:
        e = self.entries.get(key)
        if e is None or tier < e.tier:
            e = _Entry(tier)
            self.entries[key] = e
        elif tier > e.tier:
            return None
        return e

    def keys(self):
        return sorted(self.entries)

    def tier(self, key) -> int:
        return self.entries[key].tier


class ModeConsensus(_Base):
    """Weighted mode over index tiles."""

    def add(self, key: tuple[int, int], tile: np.ndarray, weight: float = 1.0, tier: int = 0) -> None:
        e = self._entry(key, tier)
        if e is None:
            return
        tile = np.asarray(tile, np.uint8)
        d = _digest(tile)
        rec = e.tiles.get(d)
        if rec is None:
            e.tiles[d] = [tile.copy(), float(weight), 1, e.seq]
            e.seq += 1
        else:
            rec[1] += float(weight)
            rec[2] += 1
        e.instances += 1
        e.weight += float(weight)

    def result(self, key: tuple[int, int], parent: np.ndarray | None = None) -> tuple[np.ndarray, dict]:
        e = self.entries[key]
        recs = sorted(e.tiles.values(), key=lambda r: r[3])
        tiles = np.stack([r[0] for r in recs]).reshape(len(recs), -1).astype(np.int64)
        w = np.array([r[1] for r in recs], np.float64)
        P = tiles.shape[1]
        shape = recs[0][0].shape
        comb = (np.arange(P)[None, :] * 256 + tiles).ravel()
        counts = np.bincount(comb, weights=np.repeat(w, P), minlength=P * 256).reshape(P, 256)
        best = counts.argmax(1)
        if parent is not None:
            par = np.asarray(parent).reshape(-1).astype(np.int64)
            if par.size != P:
                scale = shape[0] // np.asarray(parent).shape[0]
                par = np.repeat(np.repeat(np.asarray(parent), scale, 0), scale, 1).reshape(-1).astype(np.int64)
            take_par = counts[np.arange(P), par] >= counts[np.arange(P), best]
            best = np.where(take_par, par, best)
        cons = best.astype(np.uint8)
        diff = (tiles != cons[None, :].astype(np.int64)).mean(1)
        stats = {"instances": e.instances, "unique_instances": len(recs), "weight": e.weight, "tier": e.tier,
                 "disagreement": float((diff * w).sum() / max(w.sum(), 1e-12)),
                 "disagreement_unweighted": float(diff.mean())}
        return cons.reshape(shape), stats


class MedoidConsensus(_Base):
    """Weighted per-pixel OKLab medoid over RGB tiles (H, W, 3) uint8."""

    def __init__(self, cap: int = 48):
        super().__init__()
        self.cap = cap

    def add(self, key: tuple[int, int], tile_rgb: np.ndarray, weight: float = 1.0, tier: int = 0) -> None:
        e = self._entry(key, tier)
        if e is None:
            return
        t = np.clip(np.rint(np.asarray(tile_rgb, np.float64)), 0, 255).astype(np.uint8)
        d = _digest(t)
        rec = e.tiles.get(d)
        if rec is None:
            e.tiles[d] = [t, float(weight), 1, e.seq]
            e.seq += 1
            if len(e.tiles) > 4 * self.cap:      # prune lightest, keep memory bounded
                keep = sorted(e.tiles.items(), key=lambda kv: (-kv[1][1], kv[1][3]))[:2 * self.cap]
                e.tiles = dict(keep)
        else:
            rec[1] += float(weight)
            rec[2] += 1
        e.instances += 1
        e.weight += float(weight)

    def result(self, key: tuple[int, int]) -> tuple[np.ndarray, dict]:
        e = self.entries[key]
        recs = sorted(e.tiles.values(), key=lambda r: (-r[1], r[3]))[:self.cap]
        recs.sort(key=lambda r: r[3])
        shape = recs[0][0].shape
        rgb = np.stack([r[0] for r in recs]).reshape(len(recs), -1, 3)
        w = np.array([r[1] for r in recs], np.float64)
        lab = srgb_to_oklab(rgb)                                   # (n, P, 3)
        n, P, _ = lab.shape
        cost = np.zeros((n, P))
        for i in range(n):
            cost += w[i] * np.sqrt(((lab - lab[i][None]) ** 2).sum(-1))
        pick = cost.argmin(0)                                      # first minimum: earliest instance
        cons = rgb[pick, np.arange(P)]
        mean = (rgb.astype(np.float64) * w[:, None, None]).sum(0) / w.sum()
        var = ((rgb.astype(np.float64) - mean[None]) ** 2 * w[:, None, None]).sum(0) / w.sum()
        stats = {"instances": e.instances, "unique_instances": len(e.tiles), "used_instances": n,
                 "weight": e.weight, "tier": e.tier, "rgb_std": float(np.sqrt(var).mean()),
                 "disagreement": float(np.sqrt(var).mean())}
        return cons.reshape(shape), stats
