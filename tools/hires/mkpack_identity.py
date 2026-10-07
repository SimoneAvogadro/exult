#!/usr/bin/env python3
"""Identity and marker packs for the override oracles (DESIGN.md sections 5, 6.4).

Writes a pack root whose per-tile overrides are made from the game's own 1x
flats, so that the engine's output with overrides on can be predicted exactly:

  identity   every tile is the nearest-neighbour upscale NN_S of its 1x flat
             (the "template"). With these overrides the S render must equal
             the render with overrides off (oracle O4a).
  marker     the identity tile with the top-left sub-pixel of every S x S block
             set to the marker index M (default 0x01). With passes=flats the S
             render is predictable pixel for pixel (oracle O4b).

Layout (section 5.1), for every scale S given:

  <out>/pack.txt                               game, scale, palette_crc32, edge, route, title
  <out>/x<S>/flats/<SSSS>/<SSSS>_<FF>.png      8S x 8S, colour type 3, raw indices,
                                               PLTE = palette 0 (v*255/63 clamped at 255),
                                               tEXt Exult-Src-CRC32 (CRC32 of the 1x flat), Exult-Origin
  <out>/x<S>/flats.bundle                      instead of the PNGs with --bundle (section 5.7)

With --subset even (odd) only the tiles whose shape + frame is even (odd) get
an override, so a flats cache mixes overridden and NN cells (the oracles'
coverage=partial).

With --terrain DUMP, per-terrain overrides (WP-17) are written as well, made
from the 1x terrain layers of a --dump-art directory (terrain/<t1>.png, the
engine's own fill included):

  <out>/x<S>/terrain/<t1>.png                  128S x 128S, NN_S of the layer (identity), or
                                               with the marker (--terrain-kind marker),
                                               tEXt Exult-Terrain-Key, Exult-Origin

--terrain-kind (default: --kind) lets the two differ: tile identity plus
terrain marker is the precedence oracle (terrain over tile). A key is skipped
when another terrain shares it with a different 1x layer (terrain.txt
same_layer 0: one override cannot be the identity of both), and when its layer
holds index 0xFF (rule P0). --no-flats writes the terrains only.

The flats are read from shapes.vga and palette 0 from palettes.flx of the
game's static directory, the way the engine reads them (shapes/vgafile.cc
Shape_frame::read, palette.cc Palette::set_loaded): an entry is RLE when its
first u32 equals its length (or its length - 1 for an even length), otherwise
it holds length / 64 raw 8x8 frames, of which frames 0-31 are keys. A patch
directory is not consulted: run the oracles with an empty <PATCH>.

palette_crc32 is computed here (CRC32 of the 768 converted bytes). There is no
--dump-art yet (WP-11) to copy it from; the engine checks it (rule R1) and
disables the root on a mismatch, which the oracles notice as missing overrides.

Only the Python 3 standard library is used (Python 3.8 or later), so the game
tests need no virtual environment. The output is derived from the game's art:
keep it in scratch directories, never commit it.

Usage:
  mkpack_identity.py STATIC OUT [--kind identity|marker] [--marker M]
                     [--scales 2,3,6] [--bundle] [--subset all|even|odd] [--force]
                     [--terrain DUMP [--terrain-kind identity|marker] [--no-flats]]
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import sys
import zlib

TILE = 8
TILE_BYTES = TILE * TILE
TERRAIN = 128                         # a terrain layer is 128 x 128 game px
TERRAIN_KEY = "Exult-Terrain-Key"
MAX_FRAMES = 32                       # keys are (shape, frame & 31)
FIRST_CYCLING = 0xE0                  # indices 0xE0-0xFE cycle (gamewin.cc rotatecolours)
GUARD_KEY = "Exult-Src-CRC32"
ORIGIN_KEY = "Exult-Origin"
BUNDLE_MAGIC = b"U7HB"
BUNDLE_VERSION = 1
BUNDLE_HDR = struct.Struct("<4sHHII")
BUNDLE_ENTRY = struct.Struct("<HBBI")
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def read_flex(path: str) -> list:
    """Entries of a Flex file (files/Flex.cc): count at 0x54, table at 0x80."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 0x80:
        raise ValueError(f"{path}: not a Flex file")
    count = struct.unpack_from("<I", data, 0x54)[0]
    entries = []
    for i in range(count):
        off, size = struct.unpack_from("<II", data, 0x80 + 8 * i)
        entries.append(data[off:off + size] if off and size else b"")
    return entries


def color8(v: int) -> int:
    """Get_color8(v, 63, 100) with the engine's clamp (iwin8.cc)."""
    return min(255, v * 255 // 63)


def palette0(palettes_flx: str) -> bytes:
    """Palette 0 as 768 bytes of 8-bit RGB, as hires_glue.cc converts it."""
    raw = read_flex(palettes_flx)[0]
    if len(raw) == 768:
        six = raw
    elif len(raw) >= 1536:            # double palette: the even bytes
        six = bytes(raw[2 * i] for i in range(768))
    else:
        raise ValueError(f"{palettes_flx}: palette 0 has {len(raw)} bytes")
    return bytes(color8(v) for v in six)


def read_flats(shapes_vga: str) -> dict:
    """{(shape, frame): 64-byte flat} of every non-RLE entry, frames 0-31."""
    flats = {}
    for shape, data in enumerate(read_flex(shapes_vga)):
        if len(data) < 8:
            continue
        dlen = struct.unpack_from("<I", data, 0)[0]
        n = len(data)
        if dlen == n or (n % 2 == 0 and dlen == n - 1):
            continue                  # RLE
        for frame in range(min(n // TILE_BYTES, MAX_FRAMES)):
            flats[(shape, frame)] = data[frame * TILE_BYTES:(frame + 1) * TILE_BYTES]
    return flats


def upscale(flat: bytes, scale: int, marker: int | None, n: int = TILE) -> bytes:
    """NN_S of the n x n image (an 8x8 flat by default); with a marker,
    sub-pixel (0, 0) of every block is it."""
    side = n * scale
    rows = []
    for y in range(n):
        src = flat[y * n:(y + 1) * n]
        row = bytearray(b"".join(bytes([c]) * scale for c in src))
        rows.append(bytes(row))
        if marker is None:
            rows.extend([bytes(row)] * (scale - 1))
        else:
            first = bytearray(row)
            for x in range(n):
                first[x * scale] = marker
            rows[-1] = bytes(first)
            rows.extend([bytes(row)] * (scale - 1))
    out = b"".join(rows)
    assert len(out) == side * side
    return out


def decode_png8(data: bytes) -> tuple:
    """(width, height, pixels) of a palette PNG of depth 8, non-interlaced
    (as the engine's Export_png8 writes them); raw indices."""
    if data[:8] != PNG_SIGNATURE:
        raise ValueError("not a PNG file")
    pos, idat, ihdr = 8, [], None
    while pos + 8 <= len(data):
        length, ctype = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif ctype == b"IDAT":
            idat.append(body)
        elif ctype == b"IEND":
            break
        pos += 12 + length
    if ihdr is None:
        raise ValueError("no IHDR")
    w, h, depth, ctype, _, _, interlace = ihdr
    if depth != 8 or ctype != 3 or interlace != 0:
        raise ValueError(f"not an 8-bit palette PNG (depth {depth}, colour type {ctype}, interlace {interlace})")
    raw = zlib.decompress(b"".join(idat))
    out = bytearray(w * h)
    prev = bytearray(w)
    for y in range(h):
        ftype = raw[y * (w + 1)]
        line = bytearray(raw[y * (w + 1) + 1:(y + 1) * (w + 1)])
        for x in range(w):
            a = line[x - 1] if x else 0
            b = prev[x]
            c = prev[x - 1] if x else 0
            if ftype == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ftype == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ftype == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 0xFF
            elif ftype != 0:
                raise ValueError(f"bad PNG filter {ftype}")
        out[y * w:(y + 1) * w] = line
        prev = line
    return w, h, bytes(out)


def read_terrain_layers(dump: str) -> tuple:
    """({t1: 128x128 layer} of the dump's usable keys, {reason: count} of the skipped ones).
    Skipped: a key that another terrain shares with a different layer (terrain.txt
    same_layer 0), a layer with index 0xFF."""
    differing = set()
    with open(os.path.join(dump, "terrain.txt"), encoding="ascii") as f:
        for line in f:
            cols = line.split()
            if not cols or cols[0].startswith("#") or len(cols) < 8:
                continue
            if cols[7] == "0":
                differing.add(cols[1].lower())
    layers, skipped = {}, {"shared_differing_layer": 0, "index_ff": 0}
    tdir = os.path.join(dump, "terrain")
    for name in sorted(os.listdir(tdir)):
        key = name[:-4]
        if len(name) != 20 or not name.endswith(".png") or any(c not in "0123456789abcdef" for c in key):
            continue
        if key in differing:
            skipped["shared_differing_layer"] += 1
            continue
        with open(os.path.join(tdir, name), "rb") as f:
            w, h, px = decode_png8(f.read())
        if (w, h) != (TERRAIN, TERRAIN):
            raise ValueError(f"{name}: {w}x{h}, expected {TERRAIN}x{TERRAIN}")
        if 0xFF in px:
            skipped["index_ff"] += 1
            continue
        layers[key] = px
    return layers, skipped


def png_chunk(ctype: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + ctype + data + struct.pack(">I", zlib.crc32(ctype + data) & 0xFFFFFFFF)


def encode_png(pixels: bytes, side: int, pal: bytes, texts: list) -> bytes:
    """Palette PNG, depth 8, filter none, deterministic (no time chunk)."""
    raw = b"".join(b"\x00" + pixels[y * side:(y + 1) * side] for y in range(side))
    out = [PNG_SIGNATURE, png_chunk(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 3, 0, 0, 0)),
           png_chunk(b"PLTE", pal)]
    for key, value in texts:
        out.append(png_chunk(b"tEXt", key.encode("latin-1") + b"\x00" + value.encode("latin-1")))
    out.append(png_chunk(b"IDAT", zlib.compress(raw, 9)))
    out.append(png_chunk(b"IEND", b""))
    return b"".join(out)


def write_file(path: str, data: bytes) -> None:
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def in_subset(shape: int, frame: int, subset: str) -> bool:
    """True when the tile (shape, frame) belongs to the subset all, even or odd (of shape + frame)."""
    return subset == "all" or (shape + frame) % 2 == (0 if subset == "even" else 1)


def write_pack(static: str, out: str, kind: str, marker: int, scales: list, bundle: bool,
               subset: str = "all", terrain: str | None = None, terrain_kind: str | None = None,
               flats_too: bool = True) -> dict:
    flats = read_flats(os.path.join(static, "shapes.vga"))
    pal = palette0(os.path.join(static, "palettes.flx"))
    pal_crc = zlib.crc32(pal) & 0xFFFFFFFF
    mark = marker if kind == "marker" else None
    origin = "identity" if mark is None else f"marker:{marker}"
    os.makedirs(out, exist_ok=True)
    keys = sorted(k for k in flats if in_subset(k[0], k[1], subset)) if flats_too else []
    layers, skipped = read_terrain_layers(terrain) if terrain else ({}, {})
    t_kind = terrain_kind or kind
    t_mark = marker if t_kind == "marker" else None
    t_origin = "identity" if t_mark is None else f"marker:{marker}"
    for scale in scales:
        if not layers:
            break
        tdir = os.path.join(out, f"x{scale}", "terrain")
        os.makedirs(tdir, exist_ok=True)
        for key, layer in sorted(layers.items()):
            texts = [(TERRAIN_KEY, key), (ORIGIN_KEY, t_origin)]
            data = encode_png(upscale(layer, scale, t_mark, TERRAIN), TERRAIN * scale, pal, texts)
            write_file(os.path.join(tdir, f"{key}.png"), data)
    if not flats_too:
        origin = t_origin
    for scale in scales:
        side = TILE * scale
        sdir = os.path.join(out, f"x{scale}")
        os.makedirs(sdir, exist_ok=True)
        if bundle and keys:
            parts = [BUNDLE_HDR.pack(BUNDLE_MAGIC, BUNDLE_VERSION, scale, pal_crc, len(keys))]
            for shape, frame in keys:
                flat = flats[(shape, frame)]
                guard = zlib.crc32(flat) & 0xFFFFFFFF
                parts.append(BUNDLE_ENTRY.pack(shape, frame, 1, guard))
                parts.append(upscale(flat, scale, mark))
            write_file(os.path.join(sdir, "flats.bundle"), b"".join(parts))
            continue
        for shape, frame in keys:
            flat = flats[(shape, frame)]
            gdir = os.path.join(sdir, "flats", f"{shape:04d}")
            os.makedirs(gdir, exist_ok=True)
            texts = [(GUARD_KEY, f"{zlib.crc32(flat) & 0xFFFFFFFF:08x}"), (ORIGIN_KEY, origin)]
            data = encode_png(upscale(flat, scale, mark), side, pal, texts)
            write_file(os.path.join(gdir, f"{shape:04d}_{frame:02d}.png"), data)
    lines = ["game=BG", f"scale={max(scales)}", f"palette_crc32={pal_crc:08x}", "edge=none",
             f"route={origin}", f"title={kind} pack from the game's 1x flats (oracles, never commit)"]
    write_file(os.path.join(out, "pack.txt"), ("\n".join(lines) + "\n").encode("ascii"))
    reserved = sum(1 for k in keys if 0xFF in flats[k])
    return {"flats": len(keys), "scales": scales, "palette_crc32": f"{pal_crc:08x}",
            "with_index_ff": reserved, "bundle": bundle, "kind": kind, "subset": subset,
            "terrains": len(layers), "terrain_kind": t_kind, "terrains_skipped": skipped}


def parse_scales(text: str) -> list:
    scales = sorted({int(s) for s in text.replace(":", ",").split(",") if s.strip()})
    if not scales or any(s < 1 or s > 16 for s in scales):
        raise argparse.ArgumentTypeError("scales must be values 1..16")
    return scales


def overlaps(a: str, b: str) -> bool:
    """True when the directories a and b are the same, or one lies inside the other."""
    a, b = os.path.realpath(a), os.path.realpath(b)
    return os.path.commonpath([a, b]) in (a, b)


def generated_by_us(out: str) -> bool:
    """True when out holds a pack.txt this tool wrote (route=identity or route=marker:<n>)."""
    try:
        with open(os.path.join(out, "pack.txt"), encoding="ascii") as f:
            lines = f.read().splitlines()
    except (OSError, UnicodeDecodeError):
        return False
    for line in lines:
        if line.startswith("route="):
            route = line[len("route="):]
            return route == "identity" or (route.startswith("marker:") and route[7:].isdigit())
    return False


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("static", help="the game's static directory (shapes.vga, palettes.flx)")
    ap.add_argument("out", help="pack root to write")
    ap.add_argument("--kind", choices=("identity", "marker"), default="identity")
    ap.add_argument("--marker", type=lambda s: int(s, 0), default=0x01,
                    help="marker index for --kind marker (0..0xDF, default 0x01)")
    ap.add_argument("--scales", type=parse_scales, default=[2, 3, 6], help="folders to write (default 2,3,6)")
    ap.add_argument("--bundle", action="store_true", help="write x<S>/flats.bundle instead of loose PNGs")
    ap.add_argument("--subset", choices=("all", "even", "odd"), default="all",
                    help="only the tiles whose shape + frame is even (odd); default all")
    ap.add_argument("--terrain", metavar="DUMP",
                    help="also write per-terrain overrides from this --dump-art directory")
    ap.add_argument("--terrain-kind", choices=("identity", "marker"),
                    help="kind of the terrain overrides (default: --kind)")
    ap.add_argument("--no-flats", action="store_true", help="write the terrain overrides only (needs --terrain)")
    ap.add_argument("--force", action="store_true",
                    help="replace an existing output directory that holds a pack this tool wrote")
    args = ap.parse_args(argv)
    if not 0 <= args.marker < FIRST_CYCLING:
        ap.error("the marker must be 0..0xDF (a cycling index would break rule P4)")
    if (args.no_flats or args.terrain_kind) and not args.terrain:
        ap.error("--no-flats and --terrain-kind need --terrain")
    if args.terrain and overlaps(args.out, args.terrain):
        ap.error(f"{args.out} is, contains or lies inside the dump {args.terrain}")
    if overlaps(args.out, args.static):
        ap.error(f"{args.out} is, contains or lies inside the static directory {args.static}")
    if os.path.exists(args.out) and os.listdir(args.out):
        if not args.force:
            ap.error(f"{args.out} exists and is not empty (use --force)")
        if not generated_by_us(args.out):
            ap.error(f"--force replaces only a pack this tool wrote (pack.txt with route=identity "
                     f"or route=marker:<n>), and {args.out} is not one")
        shutil.rmtree(args.out)
    try:
        info = write_pack(args.static, args.out, args.kind, args.marker, args.scales, args.bundle, args.subset,
                          args.terrain, args.terrain_kind, not args.no_flats)
    except (OSError, ValueError, zlib.error) as exc:
        print(f"mkpack_identity.py: {exc}", file=sys.stderr)
        return 1
    subset = "" if info["subset"] == "all" else f" ({info['subset']} subset)"
    print(f"mkpack_identity.py: {info['kind']} pack{subset}, {info['flats']} flats, scales "
          f"{','.join(map(str, info['scales']))}{' (bundle)' if info['bundle'] else ''}, palette_crc32 "
          f"{info['palette_crc32']}, {info['with_index_ff']} flats with index 0xFF -> {args.out}")
    if args.terrain:
        skipped = ", ".join(f"{n} {why}" for why, n in info["terrains_skipped"].items())
        print(f"mkpack_identity.py: {info['terrain_kind']} terrains: {info['terrains']} keys (skipped: {skipped})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
