"""Synthetic game data for the tests: palette, shapes.vga, u7chunks, u7map. No EA data.

The palette has grey/colour ramps with brightness jumps, the six cycling ranges, exact duplicate
colours (to exercise the uniquified palette) and an out-of-range 6-bit value at index 255 (to exercise
the Get_color8 clamp). Shapes 0-5 and 12 are flats; shape 7 is RLE (solid 8x8 frames).
"""

from __future__ import annotations

import os
import struct

import numpy as np

from u7hires.io import ShapesFile, World, build_flex

NUM_SHAPES = 16
FLAT_SHAPES = (0, 1, 2, 3, 4, 5, 12)
RLE_SHAPE = 7


def synth_palette6() -> np.ndarray:
    p = np.zeros((256, 3), np.int32)
    # 14 static ramps of 16 entries (indices 0x00-0xDF), each a brightness ramp of one hue
    hues = [(1, 1, 1), (1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0), (1, 0, 1), (0, 1, 1), (2, 1, 0),
            (0, 2, 1), (1, 0, 2), (2, 2, 1), (1, 2, 2), (2, 1, 2), (1, 1, 2)]
    for r, (a, b, c) in enumerate(hues):
        for k in range(16):
            v = 4 + k * 3
            p[r * 16 + k] = (min(63, v * a // max(1, max(a, b, c)) + (k if a else 0)),
                             min(63, v * b // max(1, max(a, b, c)) + (k if b else 0)),
                             min(63, v * c // max(1, max(a, b, c)) + (k if c else 0)))
    p[0] = (0, 0, 0)
    # cycling ranges E0-FE: whites/blues/reds
    for i in range(0xE0, 0xFF):
        p[i] = ((i * 5) % 64, (i * 11) % 64, 63)
    p[0xE0] = (63, 63, 63)          # duplicate of a static white
    p[0x0F] = (63, 63, 63)
    p[0x30] = p[0x31]               # exact static duplicate
    p[255] = (250, 64, 1)           # out of 6-bit range: Get_color8 clamps to 255
    return p.astype(np.int64)


def pal6_bytes(p6: np.ndarray) -> bytes:
    return bytes(int(v) & 0xFF for v in np.asarray(p6).ravel())


def flat_frames(shape: int, n: int = 32, seed: int = 0) -> np.ndarray:
    rng = np.random.default_rng(1000 + shape + seed)
    base = 0x10 * (1 + shape % 13)
    fr = (base + rng.integers(0, 6, (n, 8, 8))).astype(np.uint8)
    if shape == 2:                       # water-like: sparse cycling glints E0-E7
        m = rng.random((n, 8, 8)) < 0.08
        fr[m] = 0xE0 + rng.integers(0, 8, m.sum())
    if shape == 3:                       # some FC-FE and black
        m = rng.random((n, 8, 8)) < 0.05
        fr[m] = 0xFC + rng.integers(0, 3, m.sum())
        fr[:, 0, 0] = 0
    return fr


def rle_shape_bytes(nframes: int = 2, color: int = 0x55) -> bytes:
    """RLE shape with solid 8x8 frames (xleft = yabove = 7, hot spot bottom-right)."""
    frames = []
    for f in range(nframes):
        b = bytearray(struct.pack("<hhhh", 0, 7, 7, 0))      # xright, xleft, yabove, ybelow
        for y in range(8):
            b += struct.pack("<Hhh", (8 << 1) | 0, -7, y - 7)  # unencoded scan of 8
            b += bytes([color + f]) * 8
        b += struct.pack("<H", 0)
        frames.append(bytes(b))
    hdr = 4 + 4 * nframes
    offs, pos = [], hdr
    for fr in frames:
        offs.append(pos)
        pos += len(fr)
    body = struct.pack("<I", pos) + b"".join(struct.pack("<I", o) for o in offs) + b"".join(frames)
    assert len(body) == pos
    return body


def shapes_entries() -> list[bytes]:
    ents = [b""] * NUM_SHAPES
    for s in FLAT_SHAPES:
        n = 4 if s == 12 else 32
        ents[s] = flat_frames(s, n).tobytes()
    ents[RLE_SHAPE] = rle_shape_bytes(2)
    return ents


def chunk_bytes(chunks: list[list[tuple[int, int]]]) -> bytes:
    out = bytearray()
    for tiles in chunks:
        for s, f in tiles:
            out += bytes([s & 0xFF, ((s >> 8) & 3) | ((f & 31) << 2)])
    return bytes(out)


def synth_terrains(n: int = 8, seed: int = 3) -> list[list[tuple[int, int]]]:
    rng = np.random.default_rng(seed)
    out = []
    for t in range(n):
        tiles = []
        for i in range(256):
            ty, tx = divmod(i, 16)
            if t == 0:                                   # macro-texture terrain of shape 1
                tiles.append((1, (ty % 4) * 8 + tx % 8))
            elif t == 1:
                tiles.append((2, (ty % 4) * 8 + tx % 8))
            else:
                s = int(rng.choice([0, 1, 2, 3, 4]))
                tiles.append((s, int(rng.integers(0, 32))))
        if t >= 2:                                       # sprinkle RLE tiles, a 12/0 and a missing frame
            for i in rng.choice(256, 12, replace=False):
                tiles[int(i)] = (RLE_SHAPE, int(rng.integers(0, 2)))
            tiles[17] = (12, 0)
            tiles[200] = (12, 9)                         # frame 9 of a 4-frame flat: missing
        out.append(tiles)
    return out


def synth_map(nterr: int, seed: int = 4) -> np.ndarray:
    rng = np.random.default_rng(seed)
    m = np.zeros((192, 192), np.int32)
    m[:] = rng.integers(0, nterr, (192, 192))
    m[:96, :96] = 0
    m[96:, 96:] = 1
    return m


def map_bytes(tmap: np.ndarray) -> bytes:
    out = bytearray()
    for sc in range(144):
        scy, scx = 16 * (sc // 12), 16 * (sc % 12)
        out += np.asarray(tmap[scy:scy + 16, scx:scx + 16], "<u2").tobytes()
    return bytes(out)


def write_static(d: str, nterr: int = 8) -> str:
    os.makedirs(d, exist_ok=True)
    pal = synth_palette6()
    with open(os.path.join(d, "palettes.flx"), "wb") as f:
        f.write(build_flex([pal6_bytes(pal), pal6_bytes(pal)]))
    with open(os.path.join(d, "shapes.vga"), "wb") as f:
        f.write(build_flex(shapes_entries()))
    terr = synth_terrains(nterr)
    with open(os.path.join(d, "u7chunks"), "wb") as f:
        f.write(chunk_bytes(terr))
    with open(os.path.join(d, "u7map"), "wb") as f:
        f.write(map_bytes(synth_map(nterr)))
    return d


def synth_world(tmpdir: str, nterr: int = 8) -> World:
    return World.load(write_static(tmpdir, nterr))


def synth_shapes() -> ShapesFile:
    return ShapesFile(shapes_entries())
