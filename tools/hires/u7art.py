#!/usr/bin/env python3
"""
u7art.py - Read Ultima VII art (shapes.vga, palettes.flx, u7chunks, u7map)
and export it as palette-indexed PNGs for the hi-res art pipeline.

It needs a Python 3 with numpy and Pillow. The system python3 of the WSL dev
box (3.8) has neither, so run it with the tools venv (Python 3.11):
  /home/simonea/ultima7_exult/tools-venv/bin/python tools/hires/u7art.py STATIC OUT
or activate that venv first (source .../tools-venv/bin/activate). The
"#!/usr/bin/env python3" line then finds the venv's python3, and
tools/hires/u7art.py STATIC OUT works as a command.

Formats follow Exult's own readers:
  - Flex container: files/Flex.cc (title 0x50 bytes, magic, count, table at 0x80)
  - Shape frames:   shapes/vgafile.cc  Shape_frame::read / get_rle_shape
  - Chunks / map:   gamemap.cc         Game_map::read_terrain / Game_map::init
  - Palette:        imagewin/iwin8.cc  Get_color8 (6-bit VGA -> 8-bit)
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from collections import Counter
from dataclasses import dataclass, field

try:
    import numpy as np
    from PIL import Image
except ImportError as exc:  # e.g. the system python3, which has neither module
    sys.exit(f"u7art.py: {exc}. It needs numpy and Pillow: run it with "
             "/home/simonea/ultima7_exult/tools-venv/bin/python, or activate that venv first.")

TILE = 8                    # c_tilesize
TILES_PER_CHUNK = 16        # c_tiles_per_chunk
CHUNK_PX = TILE * TILES_PER_CHUNK
NUM_SCHUNKS = 12            # c_num_schunks (per axis)
CHUNKS_PER_SCHUNK = 16
FIRST_OBJ_SHAPE = 0x96      # shapes 0..149 are 8x8 flat tiles
TRANSPARENT = 255           # index used for "no pixel" in exported RLE frames
V2_CHUNK_HDR = b"\xff\xff\xff\xffexlt\x00\x00"   # gamemap.cc v2hdr (V2_CHUNK_HDR_SIZE = 10)


# ---------------------------------------------------------------- Flex
def read_flex(path: str) -> list[bytes]:
    with open(path, "rb") as f:
        data = f.read()
    count = struct.unpack_from("<I", data, 0x54)[0]
    entries = []
    for i in range(count):
        off, size = struct.unpack_from("<II", data, 0x80 + 8 * i)
        entries.append(data[off:off + size] if off and size else b"")
    return entries


# ---------------------------------------------------------------- Palette
def load_palette(palettes_flx: str, index: int = 0) -> list[tuple[int, int, int]]:
    raw = read_flex(palettes_flx)[index]
    pal = []
    for i in range(256):
        r, g, b = raw[3 * i:3 * i + 3]
        pal.append(tuple(min(255, (c * 255) // 63) for c in (r, g, b)))
    return pal


def flat_palette(pal) -> list[int]:
    return [c for rgb in pal for c in rgb]


# ---------------------------------------------------------------- Shapes
@dataclass
class Frame:
    shape: int
    frame: int
    rle: bool
    pixels: np.ndarray            # uint8 [h, w]; TRANSPARENT where empty (RLE)
    mask: np.ndarray | None       # bool [h, w]; True where a pixel exists (RLE)
    xleft: int = TILE             # extents as in Shape_frame
    xright: int = -1
    yabove: int = TILE
    ybelow: int = -1


def _decode_rle_frame(buf: bytes) -> tuple[np.ndarray, np.ndarray, tuple[int, int, int, int]]:
    xright, xleft, yabove, ybelow = struct.unpack_from("<hhhh", buf, 0)
    w, h = xleft + xright + 1, yabove + ybelow + 1
    pix = np.full((h, w), TRANSPARENT, dtype=np.uint8)
    mask = np.zeros((h, w), dtype=bool)
    p = 8
    while True:
        scanlen = struct.unpack_from("<H", buf, p)[0]
        p += 2
        if scanlen == 0:
            break
        encoded = scanlen & 1
        scanlen >>= 1
        scanx, scany = struct.unpack_from("<hh", buf, p)
        p += 4
        x0, y = scanx + xleft, scany + yabove
        if not encoded:
            pix[y, x0:x0 + scanlen] = np.frombuffer(buf, np.uint8, scanlen, p)
            mask[y, x0:x0 + scanlen] = True
            p += scanlen
            continue
        b = 0
        while b < scanlen:
            bcnt = buf[p]
            p += 1
            repeat = bcnt & 1
            bcnt >>= 1
            if repeat:
                pix[y, x0 + b:x0 + b + bcnt] = buf[p]
                p += 1
            else:
                pix[y, x0 + b:x0 + b + bcnt] = np.frombuffer(buf, np.uint8, bcnt, p)
                p += bcnt
            mask[y, x0 + b:x0 + b + bcnt] = True
            b += bcnt
    return pix, mask, (xleft, xright, yabove, ybelow)


def decode_shape(shnum: int, data: bytes) -> list[Frame]:
    """Decode all frames of one shapes.vga entry."""
    if not data:
        return []
    dlen = struct.unpack_from("<I", data, 0)[0]
    shapelen = len(data)
    is_rle = dlen == shapelen or (not (shapelen & 1) and dlen == shapelen - 1)
    frames = []
    if is_rle:
        hdrlen = struct.unpack_from("<I", data, 4)[0]
        nframes = (hdrlen - 4) // 4
        offs = [struct.unpack_from("<I", data, 4 + 4 * i)[0] for i in range(nframes)]
        for fn, off in enumerate(offs):
            end = offs[fn + 1] if fn + 1 < nframes else dlen
            pix, mask, (xl, xr, ya, yb) = _decode_rle_frame(data[off:end])
            frames.append(Frame(shnum, fn, True, pix, mask, xl, xr, ya, yb))
    else:
        n = shapelen // (TILE * TILE)
        for fn in range(n):
            raw = np.frombuffer(data, np.uint8, TILE * TILE, fn * TILE * TILE).reshape(TILE, TILE).copy()
            frames.append(Frame(shnum, fn, False, raw, None))
    return frames


# ---------------------------------------------------------------- Chunks / map
def load_chunks(path: str) -> list[list[tuple[int, int]]]:
    """Return list of chunks; each is 256 (shape, frame) pairs, row-major."""
    with open(path, "rb") as f:
        data = f.read()
    v2 = data[:len(V2_CHUNK_HDR)] == V2_CHUNK_HDR
    hdr, per = (len(V2_CHUNK_HDR), 3) if v2 else (0, 2)
    n = (len(data) - hdr) // (256 * per)
    chunks = []
    for c in range(n):
        base = hdr + c * 256 * per
        tiles = []
        for t in range(256):
            o = base + t * per
            if v2:
                tiles.append((data[o] | data[o + 1] << 8, data[o + 2]))
            else:
                tiles.append((data[o] | (data[o + 1] & 3) << 8, (data[o + 1] >> 2) & 0x1F))
        chunks.append(tiles)
    return chunks


def load_map(path: str) -> np.ndarray:
    """Return [192, 192] array of chunk numbers indexed [cy, cx]."""
    with open(path, "rb") as f:
        data = f.read()
    m = np.zeros((NUM_SCHUNKS * CHUNKS_PER_SCHUNK,) * 2, dtype=np.int32)
    for s in range(NUM_SCHUNKS * NUM_SCHUNKS):
        scy, scx = 16 * (s // 12), 16 * (s % 12)
        vals = struct.unpack_from("<256H", data, s * 512)
        m[scy:scy + 16, scx:scx + 16] = np.array(vals, dtype=np.int32).reshape(16, 16)
    return m


# ---------------------------------------------------------------- Export helpers
@dataclass
class Art:
    static_dir: str
    palette: list = field(default_factory=list)
    shapes: list = field(default_factory=list)     # list[list[Frame]]
    chunks: list = field(default_factory=list)
    worldmap: np.ndarray | None = None

    @classmethod
    def load(cls, static_dir: str, palette_index: int = 0) -> "Art":
        a = cls(static_dir)
        a.palette = load_palette(os.path.join(static_dir, "palettes.flx"), palette_index)
        entries = read_flex(os.path.join(static_dir, "shapes.vga"))
        a.shapes = [decode_shape(i, e) for i, e in enumerate(entries)]
        a.chunks = load_chunks(os.path.join(static_dir, "u7chunks"))
        a.worldmap = load_map(os.path.join(static_dir, "u7map"))
        return a

    def flat_tile(self, shnum: int, frnum: int) -> np.ndarray | None:
        if shnum >= len(self.shapes):
            return None
        frames = self.shapes[shnum]
        if not frames or frames[0].rle:
            return None
        return frames[frnum & 31].pixels if (frnum & 31) < len(frames) else None

    def render_chunk(self, cnum: int) -> np.ndarray:
        """Flat (non-RLE) terrain of one chunk, 128x128 indices (like Chunk_terrain::render_flats)."""
        out = np.zeros((CHUNK_PX, CHUNK_PX), dtype=np.uint8)
        for t, (sh, fr) in enumerate(self.chunks[cnum]):
            tile = self.flat_tile(sh, fr)
            if tile is not None:
                ty, tx = divmod(t, TILES_PER_CHUNK)
                out[ty * TILE:(ty + 1) * TILE, tx * TILE:(tx + 1) * TILE] = tile
        return out

    def usage(self) -> tuple[Counter, Counter]:
        """(flat tile usage weighted by map occurrences, chunk usage on the map)."""
        chunk_use = Counter(self.worldmap.flatten().tolist())
        tile_use: Counter = Counter()
        for cnum, n in chunk_use.items():
            if cnum >= len(self.chunks):
                continue
            for sh, fr in self.chunks[cnum]:
                if sh < FIRST_OBJ_SHAPE:
                    tile_use[(sh, fr & 31)] += n
        return tile_use, chunk_use


def save_indexed(path: str, pixels: np.ndarray, pal_flat: list[int], transparent: int | None = None) -> None:
    img = Image.fromarray(pixels, mode="P")
    img.putpalette(pal_flat)
    kw = {"transparency": transparent} if transparent is not None else {}
    img.save(path, optimize=True, **kw)


def export(art: Art, out: str, superchunks: bool = True) -> dict:
    pal = flat_palette(art.palette)
    os.makedirs(out, exist_ok=True)
    tile_use, chunk_use = art.usage()

    # Palette swatch + machine-readable palettes.
    pdir = os.path.join(out, "palettes")
    os.makedirs(pdir, exist_ok=True)
    pal_entries = read_flex(os.path.join(art.static_dir, "palettes.flx"))
    for pi in range(len(pal_entries)):
        if len(pal_entries[pi]) < 768:
            continue
        p = load_palette(os.path.join(art.static_dir, "palettes.flx"), pi)
        sw = np.arange(256, dtype=np.uint8).reshape(16, 16).repeat(16, 0).repeat(16, 1)
        save_indexed(os.path.join(pdir, f"palette_{pi:02d}.png"), sw, flat_palette(p))
        with open(os.path.join(pdir, f"palette_{pi:02d}.json"), "w") as f:
            json.dump(p, f)

    manifest = {"source": art.static_dir, "tile_px": TILE, "flats": [], "rle": [], "chunks": []}

    fdir = os.path.join(out, "shapes", "flat")
    rdir = os.path.join(out, "shapes", "rle")
    os.makedirs(fdir, exist_ok=True)
    os.makedirs(rdir, exist_ok=True)
    for frames in art.shapes:
        for fr in frames:
            name = f"{fr.shape:04d}_{fr.frame:02d}.png"
            if not fr.rle:
                save_indexed(os.path.join(fdir, name), fr.pixels, pal)
                manifest["flats"].append({
                    "shape": fr.shape, "frame": fr.frame, "file": f"shapes/flat/{name}",
                    "map_uses": tile_use.get((fr.shape, fr.frame), 0),
                    "uses_reserved_indices": bool((fr.pixels >= 0xE0).any()),
                })
            else:
                save_indexed(os.path.join(rdir, name), fr.pixels, pal, TRANSPARENT)
                manifest["rle"].append({
                    "shape": fr.shape, "frame": fr.frame, "file": f"shapes/rle/{name}",
                    "w": int(fr.pixels.shape[1]), "h": int(fr.pixels.shape[0]),
                    "xleft": fr.xleft, "xright": fr.xright, "yabove": fr.yabove, "ybelow": fr.ybelow,
                    "uses_index_255": bool((fr.mask & (fr.pixels == 255)).any()),
                })

    # Flat tile atlas: one row per shape, 32 frame columns, 1px gap of index 0.
    rows = [s for s in range(min(FIRST_OBJ_SHAPE, len(art.shapes))) if art.shapes[s] and not art.shapes[s][0].rle]
    atlas = np.zeros((len(rows) * (TILE + 1), 32 * (TILE + 1)), dtype=np.uint8)
    atlas_index = []
    for r, s in enumerate(rows):
        for fr in art.shapes[s]:
            if fr.frame < 32:
                y, x = r * (TILE + 1), fr.frame * (TILE + 1)
                atlas[y:y + TILE, x:x + TILE] = fr.pixels
                atlas_index.append({"shape": s, "frame": fr.frame, "x": x, "y": y})
    save_indexed(os.path.join(out, "flat_atlas.png"), atlas, pal)
    with open(os.path.join(out, "flat_atlas.json"), "w") as f:
        json.dump({"tile": TILE, "gap": 1, "tiles": atlas_index}, f)

    # Chunk terrain renders (flat tiles only).
    cdir = os.path.join(out, "chunks")
    os.makedirs(cdir, exist_ok=True)
    for cnum in range(len(art.chunks)):
        uses = chunk_use.get(cnum, 0)
        if uses == 0:
            continue
        save_indexed(os.path.join(cdir, f"chunk_{cnum:04d}.png"), art.render_chunk(cnum), pal)
        manifest["chunks"].append({"chunk": cnum, "map_uses": uses, "file": f"chunks/chunk_{cnum:04d}.png"})

    # Superchunk terrain renders (2048x2048 each): context for upscalers.
    if superchunks:
        sdir = os.path.join(out, "superchunks")
        os.makedirs(sdir, exist_ok=True)
        cache: dict[int, np.ndarray] = {}
        for s in range(NUM_SCHUNKS * NUM_SCHUNKS):
            scy, scx = divmod(s, NUM_SCHUNKS)
            img = np.zeros((CHUNKS_PER_SCHUNK * CHUNK_PX,) * 2, dtype=np.uint8)
            for cy in range(16):
                for cx in range(16):
                    cnum = int(art.worldmap[scy * 16 + cy, scx * 16 + cx])
                    if cnum not in cache:
                        cache[cnum] = art.render_chunk(cnum)
                    img[cy * CHUNK_PX:(cy + 1) * CHUNK_PX, cx * CHUNK_PX:(cx + 1) * CHUNK_PX] = cache[cnum]
            save_indexed(os.path.join(sdir, f"schunk_{s:03d}.png"), img, pal)

    manifest["flats"].sort(key=lambda e: -e["map_uses"])
    manifest["summary"] = {
        "flat_frames": len(manifest["flats"]),
        "flat_frames_used_on_map": sum(1 for e in manifest["flats"] if e["map_uses"] > 0),
        "flat_frames_with_reserved_indices": sum(1 for e in manifest["flats"] if e["uses_reserved_indices"]),
        "rle_frames": len(manifest["rle"]),
        "chunks_used_on_map": len(manifest["chunks"]),
    }
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    return manifest["summary"]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("static_dir", help="game STATIC directory (contains shapes.vga, u7chunks, u7map, palettes.flx)")
    ap.add_argument("out", help="output directory (e.g. art_original)")
    ap.add_argument("--palette", type=int, default=0, help="palettes.flx entry used for PNGs (0 = daylight)")
    ap.add_argument("--no-superchunks", action="store_true", help="skip 2048x2048 superchunk terrain renders")
    a = ap.parse_args(argv)
    art = Art.load(a.static_dir, a.palette)
    summary = export(art, a.out, superchunks=not a.no_superchunks)
    json.dump(summary, sys.stdout, indent=1)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
