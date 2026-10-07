"""Index semantics shared by the engine, the validator, QA and the generators (DESIGN §5.5, §5.6).

* Cycle ranges: ``gamewin.cc`` ``rotatecolours`` rotates E0-E7, E8-EF, F0-F3, F4-F7, F8-FB, FC-FE.
  0xFF is not cycled; it is the border/transparent colour and never allowed in a flat (P0).
* **P4** (one definition everywhere): a pixel in cycle range R is compliant iff its 1x parent pixel
  or one of the parent's 8 neighbours is in R, the neighbours taken *inside the override's own 1x
  source, clamped at its edge* (the 8x8 flat for a tile, the 128x128 layer for a terrain).
  The engine rejects a file when ``200 * noncompliant > total`` (more than 0.5 %), and warns when
  ``0 < noncompliant``; generated art must have 0.
* Ramps: port of ``Palette::get_ramps`` (palette.cc) on the 6-bit palette 0.
* ``reduce_mode``: the class-preserving mode filter of §5.6.
"""

from __future__ import annotations

import numpy as np

CYCLE_RANGES: tuple[tuple[int, int], ...] = (
    (0xE0, 0xE7), (0xE8, 0xEF), (0xF0, 0xF3), (0xF4, 0xF7), (0xF8, 0xFB), (0xFC, 0xFE),
)
CYCLE_NAMES = tuple(f"{a:02X}-{b:02X}" for a, b in CYCLE_RANGES)
STATIC_MIN, STATIC_MAX = 0x01, 0xDF     # quantizer output range for static pixels
BORDER_INDEX = 0xFF

CYCLE_ID = np.full(256, -1, np.int8)
for _r, (_a, _b) in enumerate(CYCLE_RANGES):
    CYCLE_ID[_a:_b + 1] = _r
IS_CYCLING = CYCLE_ID >= 0
STATIC_INDICES = np.arange(STATIC_MIN, STATIC_MAX + 1)

P4_REJECT_NUM, P4_REJECT_DEN = 1, 200   # reject iff noncompliant / total > 1/200


def cycle_range_of(index: int) -> int:
    return int(CYCLE_ID[index])


def _dilate3_clamped(m: np.ndarray) -> np.ndarray:
    """3x3 dilation of a bool plane with edge clamping (neighbours outside are the edge itself)."""
    p = np.pad(m, 1, mode="edge")
    h, w = m.shape
    out = np.zeros_like(m)
    for dy in range(3):
        for dx in range(3):
            out |= p[dy:dy + h, dx:dx + w]
    return out


def p4_allowed_masks(src: np.ndarray, scale: int) -> np.ndarray:
    """(6, H*scale, W*scale) bool: where a pixel of cycle range r is P4-compliant."""
    src = np.asarray(src)
    cid = CYCLE_ID[src]
    masks = []
    for r in range(len(CYCLE_RANGES)):
        allowed = _dilate3_clamped(cid == r)
        masks.append(np.repeat(np.repeat(allowed, scale, 0), scale, 1))
    return np.stack(masks)


def p4_noncompliant(tile: np.ndarray, src: np.ndarray, scale: int | None = None) -> np.ndarray:
    """Bool plane of P4-non-compliant pixels of ``tile`` (hi-res indices) against its 1x ``src``."""
    tile = np.asarray(tile)
    src = np.asarray(src)
    if scale is None:
        scale = tile.shape[0] // src.shape[0]
    if tile.shape != (src.shape[0] * scale, src.shape[1] * scale):
        raise ValueError(f"tile {tile.shape} is not {scale}x src {src.shape}")
    allowed = p4_allowed_masks(src, scale)
    tid = CYCLE_ID[tile].astype(np.int16)
    bad = np.zeros(tile.shape, bool)
    for r in range(len(CYCLE_RANGES)):
        bad |= (tid == r) & ~allowed[r]
    return bad


def p4_violations(tile: np.ndarray, src: np.ndarray, scale: int | None = None) -> int:
    return int(p4_noncompliant(tile, src, scale).sum())


def p4_verdict(noncompliant: int, total: int) -> str:
    """'ok', 'warning' or 'reject' with the engine thresholds."""
    if noncompliant * P4_REJECT_DEN > total * P4_REJECT_NUM:
        return "reject"
    return "warning" if noncompliant > 0 else "ok"


# ---------------------------------------------------------------------------- ramps
def engine_ramps(pal6: np.ndarray) -> list[tuple[int, int]]:
    """Port of ``Palette::get_ramps`` (palette.cc:581-654) for palette 0 (6-bit values).

    Index 0 belongs to no ramp. A new ramp starts where the 6-bit brightness sum jumps by more
    than 48 (below 0xE0), and at every cycling range start 0xE0, 0xE8, 0xF0, 0xF4, 0xF8, 0xFC and
    at 0xFF (the ``std::find`` end-pointer quirk). At most 32 ramps; the last ends at 255.
    """
    p = np.asarray(pal6, np.int32).reshape(256, 3)
    starts_cyc = (0xE0, 0xE8, 0xF0, 0xF4, 0xF8, 0xFC)
    ramps = [[1, 0]]
    last = int(p[1].sum())
    full = False
    for c in range(2, 256):
        bright = int(p[c].sum())
        if (c < 0xE0 and abs(bright - last) > 48) or c in starts_cyc or c == 0xFF:
            ramps[-1][1] = c - 1
            if len(ramps) >= 32:
                full = True
                break
            ramps.append([c, 0])
        last = bright
    if not full:
        ramps[-1][1] = 255
    return [(a, b) for a, b in ramps]


def ramp_lut(ramps: list[tuple[int, int]]) -> np.ndarray:
    """(256,) int16 ramp id per index, -1 where no ramp (index 0)."""
    lut = np.full(256, -1, np.int16)
    for r, (a, b) in enumerate(ramps):
        lut[a:b + 1] = r
    return lut


# ---------------------------------------------------------------------------- reduction (§5.6)
def reduce_mode(tile: np.ndarray, s_from: int, s_to: int, parent: np.ndarray) -> np.ndarray:
    """Class-preserving mode reduction from scale ``s_from`` to ``s_to`` (``s_from % s_to == 0``).

    Per m x m block (m = s_from / s_to): if the 1x parent pixel is in cycle range R and the block
    holds indices in R, take the most frequent of those; otherwise the most frequent non-cycling
    index (the most frequent index if none). Ties go to the smallest index.
    """
    if s_from % s_to:
        raise ValueError("s_from must be a multiple of s_to")
    m = s_from // s_to
    tile = np.asarray(tile, np.uint8)
    H, W = tile.shape
    h, w = H // m, W // m
    blocks = tile.reshape(h, m, w, m).transpose(0, 2, 1, 3).reshape(h, w, m * m)
    out = np.empty((h, w), np.uint8)
    for by in range(h):
        for bx in range(w):
            b = blocks[by, bx]
            pr = CYCLE_ID[parent[by // s_to, bx // s_to]]
            cnt = np.bincount(b, minlength=256)
            if pr >= 0:
                sel = cnt * (CYCLE_ID == pr)
                if sel.any():
                    out[by, bx] = int(np.argmax(sel))
                    continue
            stat = cnt * ~IS_CYCLING
            out[by, bx] = int(np.argmax(stat)) if stat.any() else int(np.argmax(cnt))
    return out


def nn_upscale(a: np.ndarray, f: int) -> np.ndarray:
    return np.repeat(np.repeat(np.asarray(a), f, 0), f, 1)
