#!/usr/bin/env python3
"""Checks an --dump-art tree against the game's static files (DESIGN.md sections 4.2, 6.4).

Usage: dump_art_check.py STATIC DUMP_DIR

Independent of the engine: the flats and palette 0 are read by tools/hires/mkpack_identity.py,
u7chunks and u7map here (v1 and v2 chunks, map 0 of the static directory; run with an empty
<PATCH>). Checked:
  * ref.txt has the format line and the keys of section 4.2; palette_crc32 and palette/pal0.act
    equal palette 0;
  * flats.txt lists exactly the flats of shapes.vga, each with its CRC32 and cycle_px, and every
    flats/ PNG and templates/x6 PNG exists, with that CRC in its Exult-Src-CRC32 text chunk, and
    nothing else is there;
  * terrain_tiles.bin and terrain_map.bin have valid headers and sizes, their own tiles equal
    u7chunks and their map 0 equals u7map;
  * parity with ports of the engine's rules written here: the tile kinds (ShapeID::get_shape),
    the fill under RLE tiles (find_flat_source, objs/flat_source.h, quirks included) against the
    effective-source half of terrain_tiles.bin, the T1 key of every terrain (FNV-1a-64 over
    "U7TK" | 0x01 | own[32] | pix[16384], DESIGN.md section 5.2) against terrain.txt, the uses
    and cell counts, the canonical terrain of each key (the first used one, else the first one),
    same_layer, the pixels of every terrain/<t1>.png against the layer painted from the fill, and
    map_uses / used_on_map of flats.txt;
  * manifest.txt lists every file of the dump but ref.txt and itself, with its size and CRC.
Standard library only (Python 3.8). Exit code 0: pass, 1: fail.
"""

import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "hires"))
sys.dont_write_bytecode = True

import mkpack_identity as mk  # noqa: E402

KIND_NONE, KIND_FLAT, KIND_FLAT_VOID, KIND_RLE = range(4)   # Tile_kind (objs/flat_source.h)
FLAT_KINDS = (KIND_FLAT, KIND_FLAT_VOID)
VOID_TILE = (12, 0)
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
DUMP_ENTRIES = ("palette", "flats", "flats.txt", "templates", "terrain", "terrain.txt", "terrain_tiles.bin",
                "terrain_map.bin")
REF_KEYS = ("format", "game", "game_variant", "game_title", "mod", "engine_version", "engine_rev", "palette_crc32",
            "png", "terrain_key", "crc", "template_scale", "shapes", "flats", "terrains", "terrain_keys", "maps")


def png_texts(path):
    """tEXt chunks of a PNG as a dict (first occurrence wins); checks signature and CRCs."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != mk.PNG_SIGNATURE:
        raise ValueError(f"{path}: not a PNG")
    texts = {}
    p = 8
    ihdr = None
    while p + 12 <= len(data):
        n, ctype = struct.unpack_from(">I4s", data, p)
        body = data[p + 8:p + 8 + n]
        crc = struct.unpack_from(">I", data, p + 8 + n)[0]
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != crc:
            raise ValueError(f"{path}: CRC error in {ctype!r}")
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif ctype == b"tEXt":
            k, v = body.split(b"\0", 1)
            texts.setdefault(k.decode("latin-1"), v.decode("latin-1"))
        elif ctype == b"IEND":
            break
        p += 12 + n
    return ihdr, texts


def read_ref(path):
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                out[k] = v
    return out


def read_header(data, typ):
    magic, ver, t, rsize, count = struct.unpack_from("<4sHHII", data, 0)
    if magic != b"U7HR" or ver != 1 or t != typ or len(data) != 16 + rsize * count:
        raise ValueError(f"bad U7HR header {magic!r} v{ver} type {t} record {rsize} x {count}, {len(data)} bytes")
    return rsize, count


def png_pixels(path):
    """Pixels of an 8-bit palette PNG (any filter, no interlace) and its PLTE."""
    with open(path, "rb") as f:
        data = f.read()
    p = 8
    idat = b""
    plte = None
    w = h = 0
    while p + 12 <= len(data):
        n, ctype = struct.unpack_from(">I4s", data, p)
        body = data[p + 8:p + 8 + n]
        if ctype == b"IHDR":
            w, h, depth, ctyp, _c, _f, inter = struct.unpack(">IIBBBBB", body)
            if (depth, ctyp, inter) != (8, 3, 0):
                raise ValueError(f"{path}: not an 8-bit palette PNG")
        elif ctype == b"PLTE":
            plte = body
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        p += 12 + n
    raw = zlib.decompress(idat)
    out = bytearray()
    prev = bytearray(w)
    for y in range(h):
        ftype = raw[y * (w + 1)]
        row = bytearray(raw[y * (w + 1) + 1:(y + 1) * (w + 1)])
        for x in range(w):
            a = row[x - 1] if x else 0
            b = prev[x]
            c = prev[x - 1] if x else 0
            if ftype == 1:
                row[x] = (row[x] + a) & 0xFF
            elif ftype == 2:
                row[x] = (row[x] + b) & 0xFF
            elif ftype == 3:
                row[x] = (row[x] + (a + b) // 2) & 0xFF
            elif ftype == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                row[x] = (row[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
            elif ftype != 0:
                raise ValueError(f"{path}: filter {ftype}")
        out += row
        prev = row
    return w, h, bytes(out), plte


def fnv1a64(data, h=FNV_OFFSET):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def t1_key(tiles, flats):
    """T1 key of a terrain (Hires::terrain_key_t1): tiles = 256 (shape, frame, kind)."""
    own = bytearray(32)
    pix = bytearray(128 * 128)
    for t, (shape, frame, kind) in enumerate(tiles):
        if kind in FLAT_KINDS:
            own[t >> 3] |= 1 << (t & 7)
            px = flats[(shape, frame & 31)]
            ty, tx = divmod(t, 16)
            for y in range(8):
                pix[(ty * 8 + y) * 128 + tx * 8:(ty * 8 + y) * 128 + tx * 8 + 8] = px[y * 8:y * 8 + 8]
    return fnv1a64(pix, fnv1a64(own, fnv1a64(b"U7TK\x01")))


def flat_source(tx, ty, tiles):
    """find_flat_source (objs/flat_source.h): the tile painted at (tx, ty), or -1."""
    me = 16 * ty + tx
    kind = tiles[me][2]
    if kind != KIND_RLE:
        return me if kind in FLAT_KINDS else -1
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            x, y = tx + dx, ty + dy
            if 0 <= x < 16 and 0 < y < 16:          # "y > 0": row 0 never fills a neighbour (quirk)
                t = 16 * y + x
                if tiles[t][:2] != VOID_TILE and tiles[t][2] in FLAT_KINDS:
                    return t
    for t in range(256):                              # the void tile is not skipped here (quirk)
        if tiles[t][2] in FLAT_KINDS:
            return t
    return -1


def shape_table(shapes_vga):
    """[(is_rle, frames)] of every shape (Shape_frame::read's RLE test)."""
    out = []
    for e in mk.read_flex(shapes_vga):
        n = len(e)
        dlen = struct.unpack_from("<I", e, 0)[0] if n >= 4 else -1
        rle = n >= 4 and (dlen == n or (n % 2 == 0 and dlen == n - 1))
        if not e:
            out.append((False, 0))
        elif rle:
            out.append((True, max(0, ((struct.unpack_from("<I", e, 4)[0] if n >= 8 else 4) - 4) // 4)))
        else:
            out.append((False, n // 64))
    return out


def tile_kind(table, shape, frame):
    """The Tile_kind ShapeID::get_shape gives (shape, frame as stored, signed in v2)."""
    if shape >= len(table) or table[shape][1] == 0:
        return KIND_NONE
    rle, n = table[shape]
    if not rle:
        if (frame & 31) >= n:
            return KIND_NONE
        return KIND_FLAT_VOID if (shape, frame) == VOID_TILE else KIND_FLAT
    if 0 <= frame < n or ((frame & 32) and (frame & 31) < n):
        return KIND_RLE
    return KIND_NONE


def find(static, name):
    for e in os.listdir(static):
        if e.lower() == name:
            return os.path.join(static, e)
    raise FileNotFoundError(name)


def main(argv):
    if len(argv) != 3:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    static, dump = argv[1], argv[2]
    errors = []

    def bad(msg):
        errors.append(msg)

    ref = read_ref(os.path.join(dump, "ref.txt"))
    missing = [k for k in REF_KEYS if k not in ref]
    if missing:
        bad(f"ref.txt lacks {missing}")
    if ref.get("format") != "exult-dump-art/1":
        bad(f"ref.txt format is {ref.get('format')!r}")
    pal = mk.palette0(find(static, "palettes.flx"))
    if ref.get("palette_crc32") != f"{zlib.crc32(pal) & 0xFFFFFFFF:08x}":
        bad(f"palette_crc32 {ref.get('palette_crc32')} != {zlib.crc32(pal) & 0xFFFFFFFF:08x}")
    with open(os.path.join(dump, "palette", "pal0.act"), "rb") as f:
        if f.read() != pal:
            bad("palette/pal0.act differs from palette 0")

    flats = mk.read_flats(find(static, "shapes.vga"))
    rows = {}
    with open(os.path.join(dump, "flats.txt")) as f:
        for line in f:
            if not line.startswith("#"):
                s, fr, crc, uses, cyc, used = line.split()
                rows[(int(s), int(fr))] = (crc, int(cyc), int(uses), int(used))
    if set(rows) != set(flats):
        bad(f"flats.txt keys differ from shapes.vga: {len(rows)} vs {len(flats)}")
    if ref.get("flats") != str(len(flats)):
        bad(f"ref.txt flats={ref.get('flats')}, shapes.vga has {len(flats)}")
    expect_flats = set()
    expect_tmpl = set()
    for key, px in sorted(flats.items()):
        crc = f"{zlib.crc32(px) & 0xFFFFFFFF:08x}"
        cyc = sum(1 for v in px if 0xE0 <= v <= 0xFE)
        if rows.get(key, ())[:2] != (crc, cyc):
            bad(f"flat {key}: flats.txt {rows.get(key)} != {(crc, cyc)}")
            continue
        name = f"{key[0]:04d}_{key[1]:02d}.png"
        expect_flats.add(name)
        expect_tmpl.add(os.path.join(f"{key[0]:04d}", name))
        for path, side in ((os.path.join(dump, "flats", name), 8),
                           (os.path.join(dump, "templates", "x6", "flats", f"{key[0]:04d}", name), 48)):
            try:
                ihdr, texts = png_texts(path)
            except (OSError, ValueError) as e:
                bad(str(e))
                continue
            if ihdr is None or ihdr[:4] != (side, side, 8, 3) or texts.get(mk.GUARD_KEY) != crc:
                bad(f"{path}: IHDR {ihdr} or guard {texts.get(mk.GUARD_KEY)} (expected {side}x{side}, {crc})")
    if set(os.listdir(os.path.join(dump, "flats"))) != expect_flats:
        bad("flats/ holds other files than the flats")
    tdir = os.path.join(dump, "templates", "x6", "flats")
    found = {os.path.join(d, n) for d in os.listdir(tdir) for n in os.listdir(os.path.join(tdir, d))}
    if found != expect_tmpl:
        bad("templates/x6/flats holds other files than the flats")

    with open(find(static, "u7chunks"), "rb") as f:
        chunks = f.read()
    v2 = chunks[:10] == b"\xff\xff\xff\xffexlt\x00\x00"
    per = 3 if v2 else 2
    body = chunks[10:] if v2 else chunks
    nter = len(body) // (256 * per)
    with open(os.path.join(dump, "terrain_map.bin"), "rb") as f:
        data = f.read()
    with open(find(static, "u7map"), "rb") as f:
        u7map = f.read()
    try:
        rsize, count = read_header(data, 2)
        if rsize != 4 + 2 * 192 * 192 or count < 1 or struct.unpack_from("<I", data, 16)[0] != 0:
            bad(f"terrain_map.bin: {count} records of {rsize} bytes, first map {struct.unpack_from('<I', data, 16)[0]}")
        else:
            tmap = struct.unpack_from("<36864H", data, 20)
            for sc in range(144):
                for cy in range(16):
                    for cx in range(16):
                        exp = struct.unpack_from("<H", u7map, sc * 512 + 2 * (16 * cy + cx))[0]
                        y = 16 * (sc // 12) + cy
                        x = 16 * (sc % 12) + cx
                        if tmap[192 * y + x] != exp:
                            bad(f"terrain_map.bin map 0 chunk {x},{y}: {tmap[192 * y + x]} != u7map {exp}")
                            break
    except ValueError as e:
        bad(f"terrain_map.bin: {e}")

    # Uses of the terrains on all maps of the dump (map 0 checked against u7map above).
    uses = [0] * nter
    nmaps = 0
    try:
        rsize, nmaps = read_header(data, 2)
        for m in range(nmaps if rsize == 4 + 2 * 192 * 192 else 0):
            for tnum in struct.unpack_from("<36864H", data, 16 + m * rsize + 4):
                if tnum < nter:
                    uses[tnum] += 1
    except ValueError:
        pass

    # The terrains: own tiles against u7chunks, kinds, fill, T1, counts, layers.
    table = shape_table(find(static, "shapes.vga"))
    with open(os.path.join(dump, "terrain_tiles.bin"), "rb") as f:
        tdata = f.read()
    try:
        rsize, count = read_header(tdata, 1)
        if rsize != 2048 or count != nter:
            bad(f"terrain_tiles.bin: {count} records of {rsize} bytes, expected {nter} of 2048")
            count = 0
    except ValueError as e:
        bad(f"terrain_tiles.bin: {e}")
        count = 0
    lines = {}
    with open(os.path.join(dump, "terrain.txt")) as f:
        for line in f:
            if not line.startswith("#"):
                p = line.split()
                lines[int(p[0])] = p
    if sorted(lines) != list(range(nter)) or any(len(p) != 8 for p in lines.values()):
        bad(f"terrain.txt does not list terrains 0-{nter - 1} with 8 columns")
    keys = {}
    layers = {}
    painted = {}
    own_use = {}
    for t in range(count):
        c = body[t * 256 * per:(t + 1) * 256 * per]
        if v2:
            ids = [(c[3 * i] + 256 * c[3 * i + 1], c[3 * i + 2] - (256 if c[3 * i + 2] > 127 else 0))
                   for i in range(256)]
        else:
            ids = [(c[2 * i] + 256 * (c[2 * i + 1] & 3), (c[2 * i + 1] >> 2) & 0x1F) for i in range(256)]
        tiles = [(s_, f_, tile_kind(table, s_, f_)) for s_, f_ in ids]
        rec = tdata[16 + t * 2048:16 + (t + 1) * 2048]
        eng = [struct.unpack_from("<HBB", rec, 4 * i) for i in range(512)]
        if [e[:2] for e in eng[:256]] != [(s_, f_ & 0xFF) for s_, f_ in ids]:
            bad(f"terrain_tiles.bin terrain {t}: own tiles differ from u7chunks")
            continue
        if [e[2] for e in eng[:256]] != [k for _s, _f, k in tiles]:
            bad(f"terrain_tiles.bin terrain {t}: tile kinds differ")
        src = [flat_source(i % 16, i // 16, tiles) for i in range(256)]
        exp = [(0xFFFF, 0xFF, 0xFF) if s_ < 0 else (tiles[s_][0], tiles[s_][1] & 31, s_) for s_ in src]
        if eng[256:] != exp:
            bad(f"terrain_tiles.bin terrain {t}: effective sources differ in "
                f"{sum(1 for a, b in zip(eng[256:], exp) if a != b)} cells")
        layer = bytearray(128 * 128)
        for i, s_ in enumerate(src):
            if s_ >= 0:
                px = flats[(tiles[s_][0], tiles[s_][1] & 31)]
                ty, tx = divmod(i, 16)
                for y in range(8):
                    layer[(ty * 8 + y) * 128 + tx * 8:(ty * 8 + y) * 128 + tx * 8 + 8] = px[y * 8:y * 8 + 8]
                if uses[t]:
                    k = (tiles[s_][0], tiles[s_][1] & 31)
                    painted[k] = painted.get(k, 0) + uses[t]
            if uses[t] and tiles[i][2] in FLAT_KINDS:
                k = (tiles[i][0], tiles[i][1] & 31)
                own_use[k] = own_use.get(k, 0) + uses[t]
        layers[t] = bytes(layer)
        keys[t] = f"{t1_key(tiles, flats):016x}"
        counts = (str(uses[t]), str(sum(1 for x in tiles if x[2] in FLAT_KINDS)),
                  str(sum(1 for x in tiles if x[2] == KIND_RLE)), str(src.count(-1)))
        p = lines.get(t, [""] * 8)
        if p[1] != keys[t]:
            bad(f"terrain {t}: T1 {p[1]} in terrain.txt, {keys[t]} here")
        if tuple(p[2:6]) != counts:
            bad(f"terrain {t}: uses/own/rle/missing {p[2:6]} in terrain.txt, {list(counts)} here")
    canon = {}
    for t in sorted(keys, key=lambda t: (uses[t] == 0, t)):
        canon.setdefault(keys[t], t)
    for t in keys:
        c = canon[keys[t]]
        exp = ["-", "-"] if c == t else [str(c), "1" if layers[t] == layers[c] else "0"]
        if lines.get(t, [""] * 8)[6:8] != exp:
            bad(f"terrain {t}: duplicate_of/same_layer {lines.get(t, [''] * 8)[6:8]} in terrain.txt, {exp} here")
    pngs = set(os.listdir(os.path.join(dump, "terrain")))
    if pngs != {k + ".png" for k in canon} or ref.get("terrain_keys") != str(len(canon)):
        bad(f"terrain/ holds {len(pngs)} files for {len(canon)} keys (ref.txt: {ref.get('terrain_keys')})")
    for key, c in sorted(canon.items()):
        path = os.path.join(dump, "terrain", key + ".png")
        try:
            ihdr, texts = png_texts(path)
            w, h, px, plte = png_pixels(path)
        except (OSError, ValueError, zlib.error) as e:
            bad(str(e))
            continue
        if ihdr[:4] != (128, 128, 8, 3) or texts.get("Exult-Terrain-Key") != key or plte != pal:
            bad(f"terrain/{key}.png: IHDR {ihdr}, key {texts.get('Exult-Terrain-Key')} or PLTE")
        elif px != layers[c]:
            bad(f"terrain/{key}.png: {sum(1 for a, b in zip(px, layers[c]) if a != b)} pixels differ from "
                f"the layer of terrain {c}")
    for key, row in sorted(rows.items()):
        if row[2:] != (own_use.get(key, 0), painted.get(key, 0)):
            bad(f"flat {key}: map_uses/used_on_map {row[2:]} in flats.txt, "
                f"{(own_use.get(key, 0), painted.get(key, 0))} here")
    if ref.get("terrains_used") != str(sum(1 for u in uses if u)) or \
            ref.get("flats_used_on_map") != str(sum(1 for v in painted.values() if v)):
        bad(f"ref.txt terrains_used {ref.get('terrains_used')} / flats_used_on_map {ref.get('flats_used_on_map')}")

    # manifest.txt: exactly the files of the dump's entries, with their size and CRC.
    listed = {}
    with open(os.path.join(dump, "manifest.txt")) as f:
        for line in f:
            if not line.startswith("#"):
                crc, size, rel = line.rstrip("\n").split(" ", 2)
                listed[rel] = (crc, int(size))
    found = {}
    for name in DUMP_ENTRIES:
        top = os.path.join(dump, name)
        paths = [top] if os.path.isfile(top) else \
            [os.path.join(d, n) for d, _s, ns in os.walk(top) for n in ns]
        for path in paths:
            with open(path, "rb") as f:
                content = f.read()
            found[os.path.relpath(path, dump).replace(os.sep, "/")] = (f"{zlib.crc32(content) & 0xFFFFFFFF:08x}",
                                                                       len(content))
    if found != listed:
        diff = sorted(set(found.items()) ^ set(listed.items()))
        bad(f"manifest.txt differs from the files in {len(diff)} entries, e.g. {diff[:3]}")

    for e in errors[:20]:
        print("FAIL:", e, file=sys.stderr)
    print(f"dump_art_check: {len(flats)} flats, {nter} terrains, {len(canon)} T1 keys, {nmaps} map(s), "
          f"palette_crc32 {ref.get('palette_crc32')}: {'FAIL' if errors else 'ok'} ({len(errors)} errors)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
