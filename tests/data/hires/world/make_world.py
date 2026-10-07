#!/usr/bin/env python3
"""The synthetic test world: a tiny Exult "DEVEL" game with original art, and a hi-res pack for it.

Everything here is generated procedurally by this script (palette, flats, objects, map), so it
holds no EA data and may be committed and redistributed. The engine loads it like any game
(``--game hirestest``), which lets the render oracles (``--render-test``), ``--dump-art`` and the
override store run without the original Ultima VII files (``tests/world/world_tests.sh``,
``make check-world``).

The art is made at 6x first and reduced to the 1x game tiles, as a modder's hi-res art would
relate to the originals: each 1x pixel is the palette colour nearest to the mean of its 6 x 6
block (among the cycling indices of the same range for water, among the static ones otherwise). So the sample pack
is "real" hi-res art (not NN), and the 1x game is what an engine without the pack shows.

Layout (all generated; ``--check`` verifies that the committed files are up to date):

  static/                 the game files of the world, laid over the free template of Exult Studio
                          (data/estudio/new: fonts, faces, gumps, text, ...) by --assemble:
    palettes.flx          11 palettes (0 = day; 1-10 darker), cycling ranges E0-FE in use
    shapes.vga            flats 1-6 (grass, dirt, sand, water, road, cobble; 4 frames each but
                          cobble 2), RLE objects 150 (rock) and 151 (bush), the template's 721
                          and 1028 kept (avatar and pointer shapes)
    u7chunks, u7map       6 terrains, tiled 3 x 2 over the whole 192 x 192 chunk map
    initgame.dat          IDENTITY "HIRESTEST" only
    usecode               one empty function, never called (an empty file makes the reader loop)
    mainshp.flx           32 one-pixel menu shapes (Game::Game loads it; entry 0 is the load screen)
    *_info.txt, bodies.txt, shape_files.txt  empty data sections (no shape info is needed)
  base/SSSS_FF.png        the 1x flats as PNG (raw indices, palette 0), for viewing
  pack/                   a hi-res pack root (DESIGN.md section 5.1):
    pack.txt              palette_crc32 of the world
    x6/flats/SSSS_FF.png  grass, dirt, water: loose tiles with the Exult-Src-CRC32 guard
    x6/flats/road/        the road tiles as a strict group
    x6/flats/0006.off/    the cobble tiles in a disabled group (cobble stays NN)
                          (sand has no override: it stays NN)
    x6/terrain/<t1>.png   a whole-terrain override of the pond terrain (768 x 768, seamless)
  world.txt               what is where: terrains, keys, tiles, map pattern (for the tests)
  README.md               (hand-written) what the world is and how to play it

Usage:
  make_world.py                 regenerate everything next to this script
  make_world.py --check         exit 1 if a committed file differs from a fresh generation
  make_world.py --assemble DIR  write a complete static directory (template + world) into DIR
Python 3.8+, standard library only; deterministic.
"""

from __future__ import annotations

import argparse
import math
import os
import shutil
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
TEMPLATE = os.path.join(TOP, "data", "estudio", "new")

S = 6                       # art scale
TILE = 8
HI = TILE * S               # 48
CHUNK = 16                  # tiles per chunk side
LAYER = CHUNK * TILE        # 128
NUM_CHUNKS = 192
IDENTITY = "HIRESTEST"
SIG = b"\x89PNG\r\n\x1a\n"

# ---------------------------------------------------------------------------- palette (6-bit)
# Index ranges. The cycling ranges (gamewin.cc rotatecolours) are E0-E7, E8-EF, F0-F3, F4-F7,
# F8-FB, FC-FE; water uses E0-E7, so it shimmers in the engine as in the original games.
GREY = (1, 15)
GRASS = (16, 32)
DIRT = (48, 24)
SAND = (72, 24)
STONE = (96, 24)
BRICK = (120, 24)
ROCK = (144, 24)
LEAF = (168, 24)
MISC = (192, 32)
WATER = (0xE0, 8)


def ramp(n, a, b):
    """n 6-bit RGB triples from a to b."""
    return [tuple(round(a[k] + (b[k] - a[k]) * i / max(1, n - 1)) for k in range(3)) for i in range(n)]


def make_palette0():
    pal = [(0, 0, 0)] * 256
    def put(rng, colours):
        for i, c in enumerate(colours):
            pal[rng[0] + i] = c
    put(GREY, ramp(15, (4, 4, 4), (60, 60, 60)))
    put(GRASS, ramp(16, (4, 14, 3), (18, 38, 9)) + ramp(16, (14, 30, 6), (34, 48, 16)))
    put(DIRT, ramp(24, (14, 9, 4), (42, 31, 18)))
    put(SAND, ramp(24, (36, 31, 18), (62, 57, 40)))
    put(STONE, ramp(24, (12, 13, 15), (46, 47, 50)))
    put(BRICK, ramp(24, (18, 8, 6), (52, 32, 24)))
    put(ROCK, ramp(24, (10, 10, 9), (40, 38, 34)))
    put(LEAF, ramp(24, (2, 10, 4), (16, 40, 14)))
    put(MISC, ramp(16, (63, 0, 63), (63, 40, 0)) + ramp(16, (0, 40, 63), (63, 63, 63)))
    put(WATER, ramp(8, (4, 12, 30), (18, 34, 52)))
    put((0xE8, 8), ramp(8, (40, 4, 0), (63, 40, 8)))       # lava-like, unused by flats
    put((0xF0, 15), ramp(15, (20, 0, 30), (50, 30, 63)))  # the other cycling ranges, unused
    pal[255] = (63, 0, 63)                                # reserved (P0)
    return pal


def make_palettes():
    """Palette 0 and ten darker ones (as day/night palettes are)."""
    p0 = make_palette0()
    out = []
    for i in range(11):
        f = 1.0 - 0.07 * i
        out.append(bytes(min(63, int(v * f)) for c in p0 for v in c))
    return out


def color8(v):
    """Get_color8(v, 63, 100) with the engine's clamp (iwin8.cc)."""
    return min(255, v * 255 // 63)


# ---------------------------------------------------------------------------- noise
def hash2(x, y, seed):
    h = (x * 374761393 + y * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFFFF) / float(0x1000000)


def value_noise(x, y, cell, seed, period=None):
    """Smooth value noise in [0, 1); lattice coordinates wrap at period (cells) when given."""
    gx, gy = x / cell, y / cell
    x0, y0 = math.floor(gx), math.floor(gy)
    fx, fy = gx - x0, gy - y0
    sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    def at(i, j):
        if period:
            i, j = i % period, j % period
        return hash2(i, j, seed)
    a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * sx
    b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * sx
    return a + (b - a) * sy


def fbm(x, y, seed, cells=(12, 6, 3), period_px=None):
    total, weight, w = 0.0, 0.0, 1.0
    for k, cell in enumerate(cells):
        per = period_px // cell if period_px else None
        total += w * value_noise(x, y, cell, seed * 31 + k, per)
        weight += w
        w *= 0.5
    return total / weight


def shade(rng, t):
    """Index in ramp rng (start, length) for t in [0, 1]."""
    start, n = rng
    return start + max(0, min(n - 1, int(t * n)))


# ---------------------------------------------------------------------------- materials (hi-res)
# A material paints hi-res pixel (x, y) in world hi-res coordinates; 'v' is the variant (frame).
# Per-tile art uses tile-local coordinates and a period of 48 px, so a tile is seamless with
# itself; the whole-terrain art uses chunk coordinates without a period (seamless everywhere).

def grass(x, y, v, period):
    n = fbm(x, y, 11 + v, (12, 6, 3), period)
    blade = hash2(x, y // 2, 101 + v)            # short vertical strokes
    t = 0.15 + 0.7 * n
    if blade > 0.86:
        t = min(1.0, t + 0.3)
    elif blade < 0.08:
        t = max(0.0, t - 0.3)
    idx = shade(GRASS, t)
    if v == 3 and hash2(x // 3, y // 3, 7) > 0.985:   # flowers on frame 3
        idx = MISC[0] + 20 + int(hash2(x, y, 3) * 6)
    return idx


def dirt(x, y, v, period):
    n = fbm(x, y, 21 + v, (16, 8, 4), period)
    t = 0.2 + 0.6 * n
    if hash2(x // 2, y // 2, 201 + v) > 0.93:        # pebbles
        t = 0.95
    return shade(DIRT, t)


def sand(x, y, v, period):
    n = fbm(x, y, 31 + v, (16, 8), period)
    ripple = 0.5 + 0.5 * math.sin((y + 6 * n) * (2 * math.pi / 12))
    return shade(SAND, 0.25 + 0.45 * n + 0.25 * ripple)


def water(x, y, v, period):
    n = fbm(x, y, 41 + v, (16, 8, 4), period)
    wave = 0.5 + 0.5 * math.sin((x + 2 * y + 10 * n) * (2 * math.pi / 24))
    return shade(WATER, 0.1 + 0.5 * n + 0.4 * wave)


def road(x, y, v, period):
    # Slabs 24 x 16 px with 1 px mortar, every other row offset by half a slab.
    row = y // 16
    ox = (x + (12 if row % 2 else 0) + 6 * v) % 24
    oy = y % 16
    if ox == 0 or oy == 0:
        return shade(STONE, 0.1)
    n = fbm(x, y, 51 + v, (8, 4), period)
    slab = hash2((x + (12 if row % 2 else 0)) // 24, row, 9 + v)
    return shade(STONE, 0.35 + 0.3 * slab + 0.3 * n)


def cobble(x, y, v, period):
    # Round stones on a 12 px grid with dark joints.
    cx, cy = x % 12 - 5.5, y % 12 - 5.5
    d = math.hypot(cx, cy)
    if d > 5.2:
        return shade(BRICK, 0.08)
    stone = hash2(x // 12 + 3 * v, y // 12, 61)
    light = 0.5 + 0.08 * (-cx - cy)              # lit from the top left
    return shade(BRICK, 0.3 + 0.35 * stone + 0.25 * light)


MATERIALS = {          # shape: (name, painter, frames)
    1: ("grass", grass, 4),
    2: ("dirt", dirt, 4),
    3: ("sand", sand, 4),
    4: ("water", water, 4),
    5: ("road", road, 4),
    6: ("cobble", cobble, 2),
}
ROCK_SHAPE, BUSH_SHAPE = 150, 151


def tile_art(shape, frame):
    """The 48 x 48 hi-res art of flat (shape, frame), row-major."""
    paint = MATERIALS[shape][1]
    return bytes(paint(x, y, frame, HI) for y in range(HI) for x in range(HI))


CYCLE_RANGES = [(0xE0, 0xE7), (0xE8, 0xEF), (0xF0, 0xF3), (0xF4, 0xF7), (0xF8, 0xFB), (0xFC, 0xFE)]


def cycle_range(i):
    for k, (a, b) in enumerate(CYCLE_RANGES):
        if a <= i <= b:
            return k
    return -1


def reduce_mean(px, side, m, rgb):
    """Per m x m block, the index nearest to the block's mean colour: among the indices of the
    block's majority cycling range when most pixels cycle, else among the static indices 1-223
    (ties: the smallest index)."""
    n = side // m
    static = list(range(1, 0xE0))
    out = bytearray(n * n)
    for by in range(n):
        for bx in range(n):
            block = [px[y * side + x] for y in range(by * m, by * m + m) for x in range(bx * m, bx * m + m)]
            ranges = [cycle_range(c) for c in block]
            major = max(set(ranges), key=lambda r: (ranges.count(r), -r))
            cands = static if major < 0 else list(range(CYCLE_RANGES[major][0], CYCLE_RANGES[major][1] + 1))
            mean = [sum(rgb[c][k] for c in block) / len(block) for k in range(3)]
            out[by * n + bx] = min(cands, key=lambda c: (sum((rgb[c][k] - mean[k]) ** 2 for k in range(3)), c))
    return bytes(out)


# ---------------------------------------------------------------------------- RLE objects
def blob(w, h, seed, rng, bumpy):
    """A w x h raster (255 = transparent) of a lit, irregular round object."""
    px = bytearray([255] * (w * h))
    for y in range(h):
        for x in range(w):
            dx, dy = (x + 0.5 - w / 2) / (w / 2), (y + 0.5 - h / 2) / (h / 2)
            r = math.hypot(dx, dy) + bumpy * (value_noise(x, y, 3, seed) - 0.5)
            if r < 0.95:
                light = 0.55 - 0.35 * (dx + dy) + 0.2 * value_noise(x, y, 2, seed + 1)
                px[y * w + x] = shade(rng, light)
    return bytes(px)


def rle_frame(px, w, h):
    """Exult RLE frame (vgafile.cc): xright, xleft, yabove, ybelow, then raw scans, 0 ends.
    The origin is the bottom-right pixel, as for the original objects."""
    xleft, yabove = w - 1, h - 1
    out = bytearray(struct.pack("<hhhh", 0, xleft, yabove, 0))
    for y in range(h):
        x = 0
        while x < w:
            if px[y * w + x] == 255:
                x += 1
                continue
            end = x
            while end < w and px[y * w + end] != 255:
                end += 1
            out += struct.pack("<Hhh", (end - x) << 1, x - xleft, y - yabove)
            out += px[y * w + x:y * w + end]
            x = end
    out += b"\0\0"
    return bytes(out)


def rle_shape(frames):
    """A shape entry of RLE frames: u32 total length, u32 frame offsets, frames."""
    hdr = 4 + 4 * len(frames)
    offs, pos = [], hdr
    for f in frames:
        offs.append(pos)
        pos += len(f)
    return struct.pack("<I", pos) + b"".join(struct.pack("<I", o) for o in offs) + b"".join(frames)


# ---------------------------------------------------------------------------- containers
def flex(entries, title=b"Exult synthetic test world (tests/data/hires/world)"):
    count = len(entries)
    head = title.ljust(80, b"\0")[:80] + struct.pack("<III", 0xFFFF1A00, count, 0xCC) + bytes(36)
    pos = 128 + 8 * count
    table, body = bytearray(), bytearray()
    for e in entries:
        if e:
            table += struct.pack("<II", pos, len(e))
            body += e
            pos += len(e)
        else:
            table += struct.pack("<II", 0, 0)
    return head + bytes(table) + bytes(body)


def read_flex(path):
    with open(path, "rb") as f:
        data = f.read()
    count = struct.unpack_from("<I", data, 0x54)[0]
    out = []
    for i in range(count):
        off, size = struct.unpack_from("<II", data, 0x80 + 8 * i)
        out.append(data[off:off + size] if off and size else b"")
    return out


def png_chunk(ctype, data):
    return struct.pack(">I", len(data)) + ctype + data + struct.pack(">I", zlib.crc32(ctype + data) & 0xFFFFFFFF)


def encode_png(px, w, h, plte, texts=()):
    raw = b"".join(b"\0" + px[y * w:(y + 1) * w] for y in range(h))
    out = [SIG, png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0)), png_chunk(b"PLTE", plte)]
    for k, v in texts:
        out.append(png_chunk(b"tEXt", k.encode("latin-1") + b"\0" + v.encode("latin-1")))
    out.append(png_chunk(b"IDAT", zlib.compress(raw, 9)))
    out.append(png_chunk(b"IEND", b""))
    return b"".join(out)


def fnv1a64(data, h=0xCBF29CE484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def t1_key(tiles, flats):
    """T1 (DESIGN.md section 5.2) of a terrain: tiles = 256 (shape, frame); flats = {key: 64 B}."""
    own, pix = bytearray(32), bytearray(LAYER * LAYER)
    for t, key in enumerate(tiles):
        f = flats.get((key[0], key[1] & 31))
        if f is None:
            continue
        own[t >> 3] |= 1 << (t & 7)
        tx, ty = t % CHUNK, t // CHUNK
        for y in range(TILE):
            pix[(ty * TILE + y) * LAYER + tx * TILE:(ty * TILE + y) * LAYER + tx * TILE + TILE] = f[y * 8:y * 8 + 8]
    return fnv1a64(b"U7TK\x01" + bytes(own) + bytes(pix))


# ---------------------------------------------------------------------------- terrains
def terrains():
    """Six 16 x 16 terrains of (shape, frame); variants picked by a hash, so deterministic."""
    def var(tx, ty, n, seed):
        return int(hash2(tx, ty, seed) * n)
    out = []
    # T0 meadow
    out.append([(1, var(x, y, 4, 1)) for y in range(16) for x in range(16)])
    # T1 path: a dirt path across rows 6-9 (continues into the next T1 east and west)
    out.append([(2, var(x, y, 4, 2)) if 6 <= y <= 9 else (1, var(x, y, 3, 3)) for y in range(16) for x in range(16)])
    # T2 pond: water disc, sand ring, grass
    t = []
    for y in range(16):
        for x in range(16):
            d = math.hypot(x - 7.5, y - 7.5)
            t.append((4, var(x, y, 4, 4)) if d < 5 else (3, var(x, y, 4, 5)) if d < 6.4 else (1, var(x, y, 4, 6)))
    out.append(t)
    # T3 beach: sand above, water below a straight shore
    out.append([(3, var(x, y, 4, 7)) if y < 9 else (4, var(x, y, 4, 8)) for y in range(16) for x in range(16)])
    # T4 plaza: cobble inside a road border, rocks and bushes as RLE tiles (fill under them)
    t = []
    for y in range(16):
        for x in range(16):
            if (x, y) in ((4, 4), (11, 5), (6, 11)):
                t.append((ROCK_SHAPE, 0))
            elif (x, y) in ((1, 13), (13, 12)):
                t.append((BUSH_SHAPE, 0))
            elif x in (0, 15) or y in (0, 15):
                t.append((5, var(x, y, 4, 9)))
            else:
                t.append((6, var(x, y, 2, 10)))
    out.append(t)
    # T5 road: a vertical road through grass (continues into the T2 below? no: layout keeps it local)
    out.append([(5, var(x, y, 4, 11)) if 6 <= x <= 9 else (1, var(x, y, 4, 12)) for y in range(16) for x in range(16)])
    return out


TERRAIN_NAMES = ["meadow", "path", "pond", "beach", "plaza", "road"]
PATTERN = [[0, 1, 2], [3, 4, 5]]      # terrain of chunk (cx, cy) = PATTERN[cy % 2][cx % 3]


def u7chunks(terrs):
    out = bytearray()
    for t in terrs:
        for shape, frame in t:
            out += bytes([shape & 0xFF, ((shape >> 8) & 3) | (frame << 2)])
    return bytes(out)


def u7map():
    out = bytearray()
    for sc in range(144):
        scy, scx = 16 * (sc // 12), 16 * (sc % 12)
        for cy in range(16):
            for cx in range(16):
                out += struct.pack("<H", PATTERN[(scy + cy) % 2][(scx + cx) % 3])
    return bytes(out)


def pond_art(terr, flats):
    """The pond terrain as one seamless 768 x 768 image, which per-tile art cannot draw: a round,
    wobbly shore instead of the tiles' staircase, and chunk-wide noise without tile seams.
    Rule P4: a water (cycling) pixel is drawn only where its 1x parent pixel or one of the
    parent's 8 neighbours is water; elsewhere the shore stays sand."""
    side = LAYER * S
    layer = bytearray(LAYER * LAYER)
    for t, key in enumerate(terr):
        f = flats[(key[0], key[1] & 31)]
        tx, ty = t % CHUNK, t // CHUNK
        for y in range(TILE):
            layer[(ty * TILE + y) * LAYER + tx * TILE:(ty * TILE + y) * LAYER + tx * TILE + TILE] = f[y * 8:y * 8 + 8]
    def wet_near(lx, ly):
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                x, y = min(LAYER - 1, max(0, lx + dx)), min(LAYER - 1, max(0, ly + dy))
                if cycle_range(layer[y * LAYER + x]) == 0:
                    return True
        return False
    centre = side / 2
    px = bytearray(side * side)
    for y in range(side):
        for x in range(side):
            d = math.hypot(x + 0.5 - centre, y + 0.5 - centre) / HI           # in tiles
            wob = 0.35 * (value_noise(x, y, 40, 99) - 0.5) + 0.12 * (value_noise(x, y, 9, 98) - 0.5)
            if d + wob < 4.45 and wet_near(x // S, y // S):
                paint = water
            elif d + 1.4 * wob < 5.85:
                paint = sand
            else:
                paint = grass
            px[y * side + x] = paint(x, y, 0, None)
    return bytes(px)


# ---------------------------------------------------------------------------- generation
def generate(out):
    pals = make_palettes()
    plte = bytes(color8(v) for v in pals[0])
    pal_crc = zlib.crc32(plte) & 0xFFFFFFFF
    template = read_flex(os.path.join(TEMPLATE, "shapes.vga"))

    rgb = [tuple(color8(v) for v in pals[0][i * 3:i * 3 + 3]) for i in range(256)]
    arts, flats = {}, {}
    for shape, (_, _, frames) in MATERIALS.items():
        for frame in range(frames):
            art = tile_art(shape, frame)
            arts[(shape, frame)] = art
            flats[(shape, frame)] = reduce_mean(art, HI, S, rgb)

    shapes = [b""] * max(len(template), BUSH_SHAPE + 1)
    for shape, (_, _, frames) in MATERIALS.items():
        data = b"".join(flats[(shape, f)] for f in range(frames))
        dlen = struct.unpack_from("<I", data, 0)[0]
        assert not (dlen == len(data) or (len(data) % 2 == 0 and dlen == len(data) - 1)), "a flat reads as RLE"
        shapes[shape] = data
    shapes[ROCK_SHAPE] = rle_shape([rle_frame(blob(14, 11, 5, ROCK, 0.35), 14, 11)])
    shapes[BUSH_SHAPE] = rle_shape([rle_frame(blob(16, 14, 8, LEAF, 0.6), 16, 14)])
    for keep in (721, 1028):                                   # the template's avatar and pointer
        if keep < len(template) and keep < len(shapes):
            shapes[keep] = template[keep]

    terrs = terrains()
    files = {}
    st = "static/"
    files[st + "palettes.flx"] = flex(pals)
    files[st + "shapes.vga"] = flex(shapes)
    files[st + "u7chunks"] = u7chunks(terrs)
    files[st + "u7map"] = u7map()
    dot = rle_shape([rle_frame(bytes([GREY[0] + 7]), 1, 1)])
    files[st + "mainshp.flx"] = flex([dot] * 32)
    files[st + "usecode"] = struct.pack("<HH", 0x0001, 0)
    files[st + "initgame.dat"] = flex([b"identity".ljust(13, b"\0") + IDENTITY.encode() + b"\x1a"])
    empty = b"%%section version\n:1\n%%endsection\n"
    for name in ("shape_info.txt", "bodies.txt", "paperdol_info.txt", "gump_info.txt", "shape_files.txt"):
        files[st + name] = empty

    for (shape, frame), f in sorted(flats.items()):
        files[f"base/{shape:04d}_{frame:02d}.png"] = encode_png(f, TILE, TILE, plte)

    def tile_png(key):
        guard = f"{zlib.crc32(flats[key]) & 0xFFFFFFFF:08x}"
        return encode_png(arts[key], HI, HI, plte, [("Exult-Src-CRC32", guard), ("Exult-Origin", "synthetic")])
    for key in sorted(arts):
        shape, frame = key
        name = f"{shape:04d}_{frame:02d}.png"
        if shape in (1, 2, 4):
            files[f"pack/x6/flats/{name}"] = tile_png(key)
        elif shape == 5:
            files[f"pack/x6/flats/road/{name}"] = tile_png(key)
        elif shape == 6:
            files[f"pack/x6/flats/0006.off/{name}"] = tile_png(key)
    keys = [t1_key(t, flats) for t in terrs]
    pond = 2
    files[f"pack/x6/terrain/{keys[pond]:016x}.png"] = encode_png(
            pond_art(terrs[pond], flats), LAYER * S, LAYER * S, plte,
            [("Exult-Terrain-Key", f"{keys[pond]:016x}"), ("Exult-Origin", "synthetic")])
    files["pack/pack.txt"] = (f"game=DEVEL\nscale=6\npalette_crc32={pal_crc:08x}\nedge=none\nroute=synthetic\n"
                              "title=Synthetic test world sample pack (original art, free to redistribute)\n").encode()

    lines = ["# The synthetic test world (make_world.py); read by tests/world/world_tests.sh.",
             f"identity={IDENTITY}", f"palette_crc32={pal_crc:08x}",
             "pattern=terrain of chunk (cx, cy) is " + ",".join(map(str, PATTERN[0])) + " / "
             + ",".join(map(str, PATTERN[1])) + " by cx % 3, cy % 2",
             f"flats={len(flats)}", "pack_tiles_loaded=" + str(sum(MATERIALS[s][2] for s in (1, 2, 4, 5))),
             "pack_tiles_nn=sand (no file), cobble (group 0006.off)"]
    for i, (name, key) in enumerate(zip(TERRAIN_NAMES, keys)):
        lines.append(f"terrain {i} {name} t1={key:016x}" + (" override" if i == pond else ""))
    for shape, (name, _, frames) in MATERIALS.items():
        lines.append(f"shape {shape} {name} flat frames={frames}")
    lines.append(f"shape {ROCK_SHAPE} rock rle frames=1")
    lines.append(f"shape {BUSH_SHAPE} bush rle frames=1")
    files["world.txt"] = ("\n".join(lines) + "\n").encode()

    for rel, data in files.items():
        path = os.path.join(out, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    return sorted(files)


def assemble(dst):
    """A complete static directory: the template of Exult Studio, then the world's files."""
    os.makedirs(dst, exist_ok=True)
    for name in sorted(os.listdir(TEMPLATE)):
        shutil.copyfile(os.path.join(TEMPLATE, name), os.path.join(dst, name))
    src = os.path.join(HERE, "static")
    for name in sorted(os.listdir(src)):
        shutil.copyfile(os.path.join(src, name), os.path.join(dst, name))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="compare a fresh generation with the committed files")
    ap.add_argument("--assemble", metavar="DIR", help="write a complete static directory into DIR")
    args = ap.parse_args(argv)
    if args.assemble:
        assemble(args.assemble)
        return 0
    if args.check:
        with tempfile.TemporaryDirectory() as tmp:
            bad = []
            for rel in generate(tmp):
                mine = os.path.join(HERE, rel)
                if not os.path.exists(mine) or open(mine, "rb").read() != open(os.path.join(tmp, rel), "rb").read():
                    bad.append(rel)
            if bad:
                print("make_world.py --check: out of date: " + ", ".join(bad), file=sys.stderr)
                return 1
        print("make_world.py --check: up to date")
        return 0
    for sub in ("static", "base", "pack"):
        shutil.rmtree(os.path.join(HERE, sub), ignore_errors=True)
    rels = generate(HERE)
    print(f"make_world.py: {len(rels)} files written under {HERE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
