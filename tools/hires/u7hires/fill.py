"""Python port of the engine's flat-layer fill (``Chunk_terrain::paint_tile``).

This is a parity cross-check only: the engine's ``--dump-art`` (``terrain/<t1>.png`` and
``terrain_tiles.bin``) is the source of truth once it exists. Compare ``find_flat_source`` with the
dump's "effective source" table tile by tile.
"""

from __future__ import annotations

import numpy as np

from . import CHUNK_PX, TILE, TILES_PER_CHUNK
from .io import KIND_FLAT, KIND_FLAT_VOID, KIND_NONE, KIND_RLE, World

N = TILES_PER_CHUNK


def find_flat_source(tx: int, ty: int, kinds) -> int:
    """Row-major index (0..255) of the tile whose flat frame the engine paints at (tx, ty), or -1.

    Byte-for-byte port of ``Chunk_terrain::paint_tile`` (objs/chunkter.cc:86-133 at 8b6ab6b43),
    including its quirks, with ``kinds[16*y + x]`` one of io.KIND_NONE / KIND_FLAT /
    KIND_FLAT_VOID (sid == 12/0 and a flat) / KIND_RLE, i.e. what ``get_shape()`` resolves to:

    1. A flat (or 12/0) tile paints itself. A tile whose shape is missing (``get_shape()`` is
       null) paints nothing (with P3, the cell stays index 0).
    2. An RLE tile searches its 3x3 neighbourhood, rows y = -1..1 outer, x = -1..1 inner,
       accepting a neighbour only if ``tilex+x >= 0 && tilex+x < 16 && tiley+y > 0 &&
       tiley+y < 16`` -- note ``> 0``: row 0 is never a neighbour (quirk P1). Neighbours whose
       ShapeID is 12/0 (palette-cycling void) are skipped; RLE or missing neighbours are skipped.
       The first acceptable flat wins.
    3. Otherwise the whole chunk is scanned row-major (y outer, x inner) for the first flat,
       **without** the 12/0 skip (quirk P2).
    4. If nothing is found the cell is not painted.
    """
    k = kinds[ty * N + tx]
    if k == KIND_FLAT or k == KIND_FLAT_VOID:
        return ty * N + tx
    if k != KIND_RLE:
        return -1
    for y in (-1, 0, 1):
        for x in (-1, 0, 1):
            nx, ny = tx + x, ty + y
            if nx >= 0 and nx < N and ny > 0 and ny < N:      # quirk P1: 'tiley + y > 0'
                nk = kinds[ny * N + nx]
                if nk == KIND_FLAT_VOID:                       # skip palette cycling void tile
                    continue
                if nk == KIND_FLAT:
                    return ny * N + nx
    for y in range(N):                                         # quirk P2: no 12/0 skip here
        for x in range(N):
            nk = kinds[y * N + x]
            if nk == KIND_FLAT or nk == KIND_FLAT_VOID:
                return y * N + x
    return -1


def flat_sources(kinds) -> np.ndarray:
    """(256,) int16 effective source tile of every cell (-1 = unpainted)."""
    kinds = [int(k) for k in kinds]
    return np.array([find_flat_source(t % N, t // N, kinds) for t in range(N * N)], np.int16)


def render_flat_layer(world: World, tnum: int) -> tuple[np.ndarray, np.ndarray]:
    """1x flat layer (128,128) uint8 of terrain ``tnum`` as the engine paints it with P3 zero-fill.

    Returns (pixels, sources) where sources is the (256,) effective-source table.
    """
    kinds = world.terrain_kinds(tnum)
    src = flat_sources(kinds)
    out = np.zeros((CHUNK_PX, CHUNK_PX), np.uint8)
    shp = world.chunk_shape[tnum]
    frm = world.chunk_frame[tnum]
    for t in range(N * N):
        s = int(src[t])
        if s < 0:
            continue
        f = world.flat(int(shp[s]), int(frm[s]))
        ty, tx = divmod(t, N)
        out[ty * TILE:(ty + 1) * TILE, tx * TILE:(tx + 1) * TILE] = f
    return out, src


class FlatLayerCache:
    """Memoised flat layers and effective sources per terrain."""

    def __init__(self, world: World):
        self.world = world
        self._px: dict[int, np.ndarray] = {}
        self._src: dict[int, np.ndarray] = {}

    def get(self, tnum: int) -> tuple[np.ndarray, np.ndarray]:
        if tnum not in self._px:
            self._px[tnum], self._src[tnum] = render_flat_layer(self.world, tnum)
        return self._px[tnum], self._src[tnum]


def terrain_t1(world: World, tnum: int) -> int:
    """T1 key (hashing.t1_key) of terrain ``tnum`` from its own flats only (independent of the fill)."""
    from .hashing import t1_key
    kinds = world.terrain_kinds(tnum)
    own = (np.asarray(kinds) == KIND_FLAT) | (np.asarray(kinds) == KIND_FLAT_VOID)
    flats = [world.flat(int(world.chunk_shape[tnum, i]), int(world.chunk_frame[tnum, i])) if own[i] else None
             for i in range(N * N)]
    return t1_key(own, flats)


def compare_with_dump(world: World, dump_dir: str, terrains=None) -> dict:
    """Parity of this port with the engine's ``--dump-art`` (DESIGN §4.2): for every terrain, the
    engine's ``terrain/<t1>.png`` (1x flat layer, paint_flats with overrides off, raw indices) must
    equal ``render_flat_layer``. Terrains whose T1 file is missing are counted, not failed (the dump
    writes one file per distinct key)."""
    import os
    from .hashing import t1_hex
    from .pngio import read_indexed
    res = {"compared": 0, "equal": 0, "missing": 0, "mismatch": []}
    for t in (world.used_terrains if terrains is None else terrains):
        t = int(t)
        p = os.path.join(dump_dir, "terrain", t1_hex(terrain_t1(world, t)) + ".png")
        if not os.path.exists(p):
            res["missing"] += 1
            continue
        ref, _ = read_indexed(p)
        mine, _ = render_flat_layer(world, t)
        res["compared"] += 1
        if ref.shape == mine.shape and np.array_equal(ref, mine):
            res["equal"] += 1
        else:
            diff = int((ref != mine).sum()) if ref.shape == mine.shape else -1
            res["mismatch"].append({"terrain": t, "file": p, "pixels": diff})
    return res


def own_flat_mask(kinds) -> np.ndarray:
    k = np.asarray(kinds)
    return (k == KIND_FLAT) | (k == KIND_FLAT_VOID)


__all__ = ["find_flat_source", "flat_sources", "render_flat_layer", "FlatLayerCache", "own_flat_mask",
           "terrain_t1", "compare_with_dump",
           "KIND_NONE", "KIND_FLAT", "KIND_FLAT_VOID", "KIND_RLE"]
