"""Read Ultima VII static data the way Exult does.

Formats follow the engine at fork base 8b6ab6b43:

* Flex container: ``files/Flex.cc`` (title 0x50 bytes, magic, count at 0x54, table at 0x80).
* shapes.vga: ``shapes/vgafile.cc`` ``Shape_frame::read`` (RLE iff ``dlen == shapelen`` or
  ``dlen == shapelen - 1`` for even lengths; flats are 64-byte frames, ``framenum &= 31``;
  a frame number beyond the frame count yields no frame).
* u7chunks: ``gamemap.cc`` ``Game_map::init_chunks`` / ``read_terrain`` and
  ``objs/chunkter.cc`` constructor. v1 = 2 bytes per tile (shape = b0 + 256*(b1&3),
  frame = (b1>>2)&31); v2 = 10-byte header ``ff ff ff ff 'exlt' 00 00`` then 3 bytes per tile.
* u7map: ``Game_map::init``; 144 superchunks x 16x16 little-endian u16 terrain numbers.
* palettes.flx: ``palette.cc`` ``Palette::set_loaded`` (768 bytes = single palette, >= 1536 =
  interleaved pal1/pal2) and ``imagewin/iwin8.cc`` ``Get_color8`` (6 -> 8 bit with clamp at 255).

This module is a port of (and supersedes) ``tools/hires/u7art.py``; unlike that script, its v2 chunk
header check uses the engine's 10-byte header.
"""

from __future__ import annotations

import hashlib
import os
import struct
from dataclasses import dataclass
from functools import cached_property

import numpy as np

from . import NUM_CHUNKS, TILE, WORLD_TILES

FLEX_TABLE = 0x80
V2_CHUNK_HDR = b"\xff\xff\xff\xffexlt\x00\x00"   # gamemap.cc v2hdr, V2_CHUNK_HDR_SIZE = 10

# Tile kinds as seen by Chunk_terrain::paint_tile (objs/flat_source.h Tile_kind in the design).
KIND_NONE = 0        # get_shape() == nullptr (missing shape or frame)
KIND_FLAT = 1        # non-RLE frame
KIND_FLAT_VOID = 2   # shape 12 frame 0 (palette-cycling void) that resolves to a flat
KIND_RLE = 3         # RLE frame

DEFAULT_STATIC_CANDIDATES = (
    os.environ.get("U7_BG_STATIC", ""),
    "/home/simonea/ultima7_exult/art_work/static_cache",
    "/mnt/e/Games/RolePlayingGames/ultima7/static",
)
STATIC_FILES = ("shapes.vga", "u7chunks", "u7map", "palettes.flx")


def find_static_file(static_dir: str, name: str) -> str:
    """Case-insensitive lookup of ``name`` inside ``static_dir``."""
    p = os.path.join(static_dir, name)
    if os.path.exists(p):
        return p
    try:
        for e in os.listdir(static_dir):
            if e.lower() == name.lower():
                return os.path.join(static_dir, e)
    except FileNotFoundError:
        pass
    raise FileNotFoundError(f"{name} not found in {static_dir}")


def default_static_dir() -> str | None:
    """First candidate directory that holds all four BG static files, or None."""
    for d in DEFAULT_STATIC_CANDIDATES:
        if not d or not os.path.isdir(d):
            continue
        try:
            for n in STATIC_FILES:
                find_static_file(d, n)
            return d
        except FileNotFoundError:
            continue
    return None


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


# ---------------------------------------------------------------------------- Flex
def parse_flex(data: bytes) -> list[bytes]:
    """Entries of a Flex file; an entry with offset or size 0 is empty (``b""``)."""
    if len(data) < FLEX_TABLE:
        raise ValueError("not a Flex file (too short)")
    count = struct.unpack_from("<I", data, 0x54)[0]
    if FLEX_TABLE + 8 * count > len(data):
        raise ValueError("Flex table exceeds file size")
    out = []
    for i in range(count):
        off, size = struct.unpack_from("<II", data, FLEX_TABLE + 8 * i)
        out.append(bytes(data[off:off + size]) if off and size else b"")
    return out


def read_flex(path: str) -> list[bytes]:
    with open(path, "rb") as f:
        return parse_flex(f.read())


def build_flex(entries: list[bytes], title: bytes = b"synthetic flex") -> bytes:
    """Write a Flex file (used by tests to build synthetic game data)."""
    hdr = bytearray(FLEX_TABLE)
    hdr[:len(title)] = title[:0x50]
    struct.pack_into("<II", hdr, 0x50, 0xFFFF1A00, len(entries))
    table = bytearray()
    body = bytearray()
    off = FLEX_TABLE + 8 * len(entries)
    for e in entries:
        if e:
            table += struct.pack("<II", off + len(body), len(e))
            body += e
        else:
            table += struct.pack("<II", 0, 0)
    return bytes(hdr + table + body)


# ---------------------------------------------------------------------------- Palette
def palette6_from_entry(raw: bytes) -> np.ndarray:
    """6-bit palette (256,3) as ``Palette::set_loaded`` stores it in pal1."""
    if len(raw) == 768:
        return np.frombuffer(raw, np.uint8).reshape(256, 3).copy()
    if len(raw) >= 1536:  # double palette: pal1[i] = buf[2*i]
        return np.frombuffer(raw[:1536:2], np.uint8).reshape(256, 3).copy()
    raise ValueError(f"palette entry has {len(raw)} bytes (expected 768 or >= 1536)")


def get_color8(val: np.ndarray | int, maxval: int = 63, brightness: int = 100):
    """``Get_color8`` (imagewin/iwin8.cc): ``c = val*brightness*255 / (100*maxval)``, clamped at 255."""
    v = np.asarray(val, dtype=np.int64)
    c = (v * brightness * 255) // (100 * maxval)
    return np.minimum(c, 255).astype(np.uint8)


def palette8_from_6bit(pal6: np.ndarray) -> np.ndarray:
    """Effective 8-bit palette (256,3) uint8, exactly ``Pal8.rgb`` of DESIGN §3.5 / §5.4."""
    return get_color8(np.asarray(pal6, np.uint8).reshape(256, 3))


def load_palette8(palettes_flx: str, index: int = 0) -> np.ndarray:
    return palette8_from_6bit(palette6_from_entry(read_flex(palettes_flx)[index]))


# ---------------------------------------------------------------------------- Shapes
@dataclass
class RleFrame:
    pixels: np.ndarray   # (h, w) uint8, 255 where empty
    mask: np.ndarray     # (h, w) bool, True where a pixel exists
    xleft: int
    xright: int
    yabove: int
    ybelow: int


def is_rle_entry(data: bytes) -> bool:
    """``Shape_frame::read``: RLE iff dlen == shapelen, or shapelen even and dlen == shapelen - 1."""
    if len(data) < 4:
        return False
    shapelen = len(data)
    dlen = struct.unpack_from("<I", data, 0)[0]
    return dlen == shapelen or (shapelen > 0 and not (shapelen & 1) and dlen == shapelen - 1)


def decode_rle_frame(buf: bytes) -> RleFrame:
    """Decode one RLE frame (``Shape_frame::get_rle_shape`` layout)."""
    xright, xleft, yabove, ybelow = struct.unpack_from("<hhhh", buf, 0)
    w, h = xleft + xright + 1, yabove + ybelow + 1
    pix = np.full((h, w), 255, dtype=np.uint8)
    mask = np.zeros((h, w), dtype=bool)
    p = 8
    while p + 2 <= len(buf):
        scanlen = struct.unpack_from("<H", buf, p)[0]
        p += 2
        if scanlen == 0:
            break
        encoded = scanlen & 1
        scanlen >>= 1
        scanx, scany = struct.unpack_from("<hh", buf, p)
        p += 4
        x0, y = scanx + xleft, scany + yabove
        if not (0 <= y < h):
            break
        if not encoded:
            n = max(0, min(scanlen, w - x0))
            pix[y, x0:x0 + n] = np.frombuffer(buf, np.uint8, n, p)
            mask[y, x0:x0 + n] = True
            p += scanlen
            continue
        b = 0
        while b < scanlen:
            bcnt = buf[p]
            p += 1
            repeat = bcnt & 1
            bcnt >>= 1
            n = max(0, min(bcnt, w - (x0 + b)))
            if repeat:
                pix[y, x0 + b:x0 + b + n] = buf[p]
                p += 1
            else:
                pix[y, x0 + b:x0 + b + n] = np.frombuffer(buf, np.uint8, n, p)
                p += bcnt
            mask[y, x0 + b:x0 + b + n] = True
            b += bcnt
    return RleFrame(pix, mask, xleft, xright, yabove, ybelow)


class ShapesFile:
    """shapes.vga with the engine's frame lookup semantics (no patch directory)."""

    def __init__(self, entries: list[bytes]):
        self.entries = entries
        self.num_shapes = len(entries)
        self._rle = [is_rle_entry(e) for e in entries]
        self._nframes = []
        for e, rle in zip(entries, self._rle):
            if not e:
                self._nframes.append(0)
            elif rle:
                hdrlen = struct.unpack_from("<I", e, 4)[0] if len(e) >= 8 else 4
                self._nframes.append(max(0, (hdrlen - 4) // 4))
            else:
                self._nframes.append(len(e) // (TILE * TILE))
        self._rle_cache: dict[tuple[int, int], RleFrame | None] = {}

    @classmethod
    def load(cls, path: str) -> "ShapesFile":
        return cls(read_flex(path))

    def is_rle(self, shape: int) -> bool:
        return 0 <= shape < self.num_shapes and self._rle[shape]

    def nframes(self, shape: int) -> int:
        return self._nframes[shape] if 0 <= shape < self.num_shapes else 0

    def is_flat_shape(self, shape: int) -> bool:
        return 0 <= shape < self.num_shapes and bool(self.entries[shape]) and not self._rle[shape]

    def flat(self, shape: int, frame: int) -> np.ndarray | None:
        """64-byte flat as (8,8) uint8 for ``(shape, frame & 31)``, or None (not a flat / missing)."""
        if not self.is_flat_shape(shape):
            return None
        f = frame & 31
        if f >= self._nframes[shape]:
            return None
        e = self.entries[shape]
        return np.frombuffer(e, np.uint8, 64, f * 64).reshape(TILE, TILE)

    def flat_bytes(self, shape: int, frame: int) -> bytes | None:
        t = self.flat(shape, frame)
        return None if t is None else t.tobytes()

    def kind(self, shape: int, frame: int) -> int:
        """KIND_NONE / KIND_FLAT / KIND_RLE as ``ShapeID::get_shape()`` would resolve it."""
        if not (0 <= shape < self.num_shapes) or not self.entries[shape]:
            return KIND_NONE
        n = self._nframes[shape]
        if not self._rle[shape]:
            return KIND_FLAT if (frame & 31) < n else KIND_NONE
        if 0 <= frame < n:
            return KIND_RLE
        if (frame & 32) and (frame & 31) < n:   # reflection of an existing frame
            return KIND_RLE
        return KIND_NONE

    def rle_frame(self, shape: int, frame: int) -> RleFrame | None:
        key = (shape, frame)
        if key in self._rle_cache:
            return self._rle_cache[key]
        out = None
        if self.is_rle(shape) and 0 <= frame < self._nframes[shape]:
            e = self.entries[shape]
            n = self._nframes[shape]
            offs = [struct.unpack_from("<I", e, 4 + 4 * i)[0] for i in range(n)]
            dlen = struct.unpack_from("<I", e, 0)[0]
            end = offs[frame + 1] if frame + 1 < n else dlen
            try:
                out = decode_rle_frame(e[offs[frame]:end])
            except (struct.error, IndexError, ValueError):
                out = None
        self._rle_cache[key] = out
        return out

    def all_flat_keys(self) -> list[tuple[int, int]]:
        """Every (shape, frame) flat in the file, frames 0..min(nframes,32)-1."""
        keys = []
        for s in range(self.num_shapes):
            if self.is_flat_shape(s):
                keys.extend((s, f) for f in range(min(self._nframes[s], 32)))
        return keys


# ---------------------------------------------------------------------------- Chunks / map
def parse_chunks(data: bytes) -> tuple[np.ndarray, np.ndarray, bool]:
    """u7chunks -> (shape [n,256] int32, frame [n,256] int32, v2). Tiles are row-major (16*ty+tx)."""
    v2 = data[:len(V2_CHUNK_HDR)] == V2_CHUNK_HDR
    if v2:
        body = np.frombuffer(data, np.uint8, offset=len(V2_CHUNK_HDR))
        n = len(body) // (256 * 3)
        a = body[:n * 768].reshape(n, 256, 3).astype(np.int32)
        shp = a[..., 0] + 256 * a[..., 1]
        frm = a[..., 2].astype(np.int8).astype(np.int32)   # ShapeID::framenum is a signed char
    else:
        n = len(data) // 512
        a = np.frombuffer(data, np.uint8, n * 512).reshape(n, 256, 2).astype(np.int32)
        shp = a[..., 0] + 256 * (a[..., 1] & 3)
        frm = (a[..., 1] >> 2) & 0x1F
    return shp, frm, v2


def load_chunks(path: str) -> tuple[np.ndarray, np.ndarray, bool]:
    with open(path, "rb") as f:
        return parse_chunks(f.read())


def parse_map(data: bytes) -> np.ndarray:
    """u7map -> terrain numbers [192, 192] indexed [cy, cx] (engine: terrain_map[cx][cy])."""
    need = 144 * 512
    if len(data) < need:
        data = data + bytes(need - len(data))
    m = np.frombuffer(data[:need], "<u2").reshape(12, 12, 16, 16)
    return m.transpose(0, 2, 1, 3).reshape(NUM_CHUNKS, NUM_CHUNKS).astype(np.int32)


def load_map(path: str) -> np.ndarray:
    with open(path, "rb") as f:
        return parse_map(f.read())


# ---------------------------------------------------------------------------- World
class World:
    """All static terrain data of one game (BG): palette 0, shapes, terrains, map."""

    def __init__(self, pal6: np.ndarray, shapes: ShapesFile, chunk_shape: np.ndarray,
                 chunk_frame: np.ndarray, tmap: np.ndarray, v2_chunks: bool = False,
                 static_dir: str | None = None, palette_index: int = 0):
        self.pal6 = np.asarray(pal6, np.uint8).reshape(256, 3)
        self.pal8 = palette8_from_6bit(self.pal6)
        self.shapes = shapes
        self.chunk_shape = chunk_shape
        self.chunk_frame = chunk_frame
        self.tmap = tmap
        self.v2_chunks = v2_chunks
        self.static_dir = static_dir
        self.palette_index = palette_index

    @classmethod
    def load(cls, static_dir: str | None = None, palette_index: int = 0) -> "World":
        static_dir = static_dir or default_static_dir()
        if not static_dir:
            raise FileNotFoundError("no BG static directory found (set U7_BG_STATIC)")
        pal6 = palette6_from_entry(read_flex(find_static_file(static_dir, "palettes.flx"))[palette_index])
        shapes = ShapesFile.load(find_static_file(static_dir, "shapes.vga"))
        cs, cf, v2 = load_chunks(find_static_file(static_dir, "u7chunks"))
        tmap = load_map(find_static_file(static_dir, "u7map"))
        return cls(pal6, shapes, cs, cf, tmap, v2, static_dir, palette_index)

    def input_hashes(self) -> dict[str, str]:
        if not self.static_dir:
            return {}
        return {n: sha256_file(find_static_file(self.static_dir, n)) for n in STATIC_FILES}

    @property
    def num_terrains(self) -> int:
        return self.chunk_shape.shape[0]

    # -- per-tile lookups
    def flat(self, shape: int, frame: int) -> np.ndarray | None:
        return self.shapes.flat(shape, frame)

    @cached_property
    def kind_lut(self) -> np.ndarray:
        """kind_lut[shape, frame & 0xFF] -> KIND_* (frames as stored in u7chunks)."""
        n = max(self.shapes.num_shapes, int(self.chunk_shape.max()) + 1 if self.chunk_shape.size else 1)
        lut = np.zeros((n, 256), np.uint8)
        for s in range(min(n, self.shapes.num_shapes)):
            for f in range(256):
                fr = f if f < 128 else f - 256   # signed-char frame numbers (v2)
                k = self.shapes.kind(s, fr)
                if k == KIND_FLAT and s == 12 and fr == 0:
                    k = KIND_FLAT_VOID
                lut[s, f] = k
        return lut

    def terrain_kinds(self, tnum: int) -> np.ndarray:
        """(256,) Tile_kind of each tile of terrain ``tnum`` in row-major order."""
        return self.kind_lut[self.chunk_shape[tnum], self.chunk_frame[tnum] & 0xFF]

    # -- usage
    @cached_property
    def terrain_usage(self) -> np.ndarray:
        """Number of map chunks that reference each terrain."""
        return np.bincount(self.tmap.ravel(), minlength=self.num_terrains)[:self.num_terrains]

    @cached_property
    def used_terrains(self) -> np.ndarray:
        return np.nonzero(self.terrain_usage)[0]

    @cached_property
    def tile_grid(self) -> tuple[np.ndarray, np.ndarray]:
        """World tile grid [3072, 3072] of (shape uint16, frame&31 uint8), indexed [ty, tx]."""
        t = self.tmap
        S = self.chunk_shape.reshape(-1, 16, 16)[t].transpose(0, 2, 1, 3).reshape(WORLD_TILES, WORLD_TILES)
        F = (self.chunk_frame.reshape(-1, 16, 16)[t] & 31).transpose(0, 2, 1, 3).reshape(WORLD_TILES, WORLD_TILES)
        return S.astype(np.uint16), F.astype(np.uint8)

    @cached_property
    def flat_keys(self) -> list[tuple[int, int]]:
        return self.shapes.all_flat_keys()

    @cached_property
    def flat_use(self) -> dict[tuple[int, int], int]:
        """Map occurrences of each flat (shape, frame&31) as an *own* tile on the map."""
        use: dict[tuple[int, int], int] = {}
        usage = self.terrain_usage
        for t in self.used_terrains:
            kinds = self.terrain_kinds(int(t))
            for i in np.nonzero((kinds == KIND_FLAT) | (kinds == KIND_FLAT_VOID))[0]:
                k = (int(self.chunk_shape[t, i]), int(self.chunk_frame[t, i]) & 31)
                use[k] = use.get(k, 0) + int(usage[t])
        return use
