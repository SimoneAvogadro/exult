#!/usr/bin/env python3
"""Generate the engine-side hi-res test fixtures (synthetic palette and content only, no EA data).

Writes, next to this script:

* ``editor/<case>/<SSSS_FF>.png`` and ``editor/expected.txt``: PNGs laid out like real editor
  exports (DESIGN.md section 6.2, test_editor_fixtures): Aseprite indexed with and without a
  transparent index, GIMP indexed with "remove unused colors" (depth 4, compacted colormap), a
  full-palette GIMP export with GIMP's ancillary chunks, an Adam7-interlaced export, a template
  whose tEXt chunks an editor dropped, the guard after IDAT, and RGB / RGBA exports.
  ``expected.txt`` uses the format of ``rules/expected.txt``: ``<case>/<file> <RULE|OK> <severity>``.
* The ``t1`` and ``pal8_crc32`` lines of ``hash_vectors.txt`` (the other lines are kept):
  ``t1 <own: 64 hex digits>/<a>.<b>.<c>.<d> <key>``: own tile t's flat pixel (x, y) is
  ``(a*t + b*x + c*y + d) & 0xff``, every other cell zero; ``pal8_crc32 <768 6-bit values as hex>
  <CRC32 of Pal8.rgb>`` (Get_color8 clamp).

The provider is the synthetic game in ``rules/game`` (palettes.flx, shapes.vga), written by
tools/hires/tests/make_rule_fixtures.py. Python 3 standard library only; deterministic.
usage: make_fixtures.py
"""

import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SCALE = 6
SIG = b"\x89PNG\r\n\x1a\n"


# ---------------------------------------------------------------------------- synthetic game
def read_flex(path):
    with open(path, "rb") as f:
        data = f.read()
    count = struct.unpack_from("<I", data, 0x54)[0]
    out = []
    for i in range(count):
        off, length = struct.unpack_from("<II", data, 0x80 + 8 * i)
        out.append(data[off:off + length])
    return out


def get_color8(v):
    c = v * 100 * 255 // (100 * 63)
    return c if c <= 255 else 255


def load_game():
    pal6 = read_flex(os.path.join(HERE, "rules", "game", "palettes.flx"))[0][:768]
    pal8 = bytes(get_color8(v) for v in pal6)
    shapes = read_flex(os.path.join(HERE, "rules", "game", "shapes.vga"))
    return pal6, pal8, shapes


def flat(shapes, shape, frame):
    data = shapes[shape]
    return data[frame * 64:(frame + 1) * 64]


def crc32(data):
    return zlib.crc32(bytes(data)) & 0xFFFFFFFF


# ---------------------------------------------------------------------------- PNG writer
def chunk(ctype, data):
    return struct.pack(">I", len(data)) + ctype + data + struct.pack(">I", zlib.crc32(ctype + data) & 0xFFFFFFFF)


def text(key, value):
    return chunk(b"tEXt", key.encode("latin-1") + b"\0" + value.encode("latin-1"))


def itxt(key, value):
    return chunk(b"iTXt", key.encode("latin-1") + b"\0\0\0" + b"\0" + b"\0" + value.encode("utf-8"))


def pack_row(row, depth):
    if depth == 8:
        return bytes(row)
    per = 8 // depth
    out = bytearray()
    for i in range(0, len(row), per):
        b = 0
        for k in range(per):
            v = row[i + k] if i + k < len(row) else 0
            b |= v << (8 - depth * (k + 1))
        out.append(b)
    return bytes(out)


def raw_rows(rows, depth):
    return b"".join(b"\0" + pack_row(r, depth) for r in rows)


ADAM7 = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))


def adam7_rows(rows, depth):
    h, w = len(rows), len(rows[0])
    out = b""
    for x0, y0, dx, dy in ADAM7:
        sub = [[rows[y][x] for x in range(x0, w, dx)] for y in range(y0, h, dy)]
        if sub and sub[0]:
            out += raw_rows(sub, depth)
    return out


def png(rows, depth, ctype, plte=None, before=(), after=(), interlace=0, pixel_bytes=None):
    h, w = len(rows), len(rows[0]) // (pixel_bytes or 1)
    out = [SIG, chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace))]
    if plte is not None:
        out.append(chunk(b"PLTE", plte))
    out.extend(before)
    raw = adam7_rows(rows, depth) if interlace else raw_rows(rows, depth)
    out.append(chunk(b"IDAT", zlib.compress(raw, 9)))
    out.extend(after)
    out.append(chunk(b"IEND", b""))
    return b"".join(out)


def nn(tile, s=SCALE):
    return [[tile[(y // s) * 8 + x // s] for x in range(8 * s)] for y in range(8 * s)]


# GIMP 2.10's ancillary chunks for an indexed export with the default options: background
# colour, resolution (72 dpi = 2835 px/m), modification time, comment; and XMP as iTXt.
def gimp_chunks(bkgd):
    return [chunk(b"bKGD", bkgd), chunk(b"pHYs", struct.pack(">IIB", 2835, 2835, 1)),
            chunk(b"tIME", struct.pack(">HBBBBB", 2026, 10, 4, 12, 0, 0)), text("Comment", "Created with GIMP")]


XMP = ('<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?><x:xmpmeta xmlns:x="adobe:ns:meta/">'
       '<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"/></x:xmpmeta><?xpacket end="w"?>')


def build_editor(pal8, shapes):
    out = os.path.join(HERE, "editor")
    cases = []

    def put(case, name, data, rule, sev):
        d = os.path.join(out, case)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name), "wb") as fh:
            fh.write(data)
        cases.append((f"{case}/{name}", rule, sev))

    base = nn(flat(shapes, 4, 0))
    # Aseprite: IHDR, PLTE (the whole sprite palette), [tRNS], IDAT, IEND; sRGB colour profile.
    put("aseprite_indexed", "0004_00.png", png(base, 8, 3, pal8, [chunk(b"sRGB", b"\0")]), "OK", "-")
    put("aseprite_transparent", "0004_00.png", png(base, 8, 3, pal8, [chunk(b"tRNS", b"\0")]), "F4", "warning")
    # GIMP "remove unused colors": the colormap shrinks to the used colours, in index order, and
    # the pixels are renumbered; with <= 16 colours GIMP writes 4 bit per pixel.
    used = sorted({v for r in base for v in r})
    remap = {v: i for i, v in enumerate(used)}
    plte = b"".join(pal8[3 * v:3 * v + 3] for v in used)
    rows = [[remap[v] for v in r] for r in base]
    put("gimp_remove_unused", "0004_00.png", png(rows, 4, 3, plte, gimp_chunks(b"\0")), "F2", "reject")
    # GIMP with the full 256-entry colormap kept: accepted.
    put("gimp_full_palette", "0004_00.png", png(base, 8, 3, pal8, gimp_chunks(b"\0") + [itxt("XML:com.adobe.xmp", XMP)]),
        "OK", "-")
    put("gimp_interlaced", "0004_00.png", png(base, 8, 3, pal8, gimp_chunks(b"\0"), interlace=1), "OK", "-")
    # A guarded template re-saved by an editor that drops unknown tEXt: accepted, unguarded.
    put("text_dropped", "0004_01.png", png(nn(flat(shapes, 4, 1)), 8, 3, pal8, gimp_chunks(b"\0")), "OK", "-")
    # The guard after IDAT (libpng's end_info): accepted, guarded.
    t = flat(shapes, 4, 2)
    put("text_after_idat", "0004_02.png",
        png(nn(t), 8, 3, pal8, after=[text("Exult-Src-CRC32", f"{crc32(t):08x}"), text("Exult-Origin", "editor")]),
        "OK", "-")
    # RGB and RGBA exports: F1.
    rgb = [b"".join(pal8[3 * v:3 * v + 3] for v in r) for r in base]
    put("gimp_rgb", "0004_00.png", png(rgb, 8, 2, None, gimp_chunks(b"\0\0\0\0\0\0"), pixel_bytes=3), "F1", "reject")
    rgba = [b"".join(pal8[3 * v:3 * v + 3] + b"\xff" for v in r) for r in base]
    put("gimp_rgba", "0004_00.png", png(rgba, 8, 6, None, gimp_chunks(b"\0\0\0\0\0\0"), pixel_bytes=4), "F1", "reject")

    lines = ["# <case>/<file> <expected primary rule or OK> <severity>; generated by make_fixtures.py"]
    lines += [f"{c} {r} {s}" for c, r, s in cases]
    with open(os.path.join(out, "expected.txt"), "w") as fh:
        fh.write("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------- hash vectors
def fnv1a64(data, h=0xCBF29CE484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def t1_key(own, a, b, c, d):
    pix = bytearray(128 * 128)
    for t in range(256):
        if own[t >> 3] & (1 << (t & 7)):
            ty, tx = divmod(t, 16)
            for y in range(8):
                for x in range(8):
                    pix[(ty * 8 + y) * 128 + tx * 8 + x] = (a * t + b * x + c * y + d) & 0xFF
    return fnv1a64(b"U7TK\x01" + bytes(own) + bytes(pix))


def update_vectors(pal6):
    path = os.path.join(HERE, "hash_vectors.txt")
    with open(path) as f:
        keep = [ln for ln in f.read().splitlines()
                if not ln.startswith(("t1 ", "pal8_crc32 ", "# t1 ", "# pal8_crc32 "))]
    own_sets = [bytes(32), bytes([0xFF] * 32), bytes([0x55] * 32), bytes((i * 37 + 11) & 0xFF for i in range(32))]
    patterns = [(0, 0, 0, 0), (1, 3, 11, 7), (7, 1, 16, 0), (13, 5, 17, 200)]
    lines = ["# t1 <own: 64 hex digits>/<a>.<b>.<c>.<d> <key>: own tile t's pixel (x, y) = (a*t + b*x + c*y + d) & 0xff;"
             " other cells 0 (make_fixtures.py)"]
    for own, (a, b, c, d) in zip(own_sets, patterns):
        lines.append(f"t1 {own.hex()}/{a}.{b}.{c}.{d} {t1_key(own, a, b, c, d):016x}")
    pal8 = bytes(get_color8(v) for v in pal6)
    lines.append("# pal8_crc32 <768 6-bit values> <CRC32 of the 8-bit palette with the Get_color8 clamp>; the synthetic"
                 " palette of rules/game (index 255 = (250, 64, 1))")
    lines.append(f"pal8_crc32 {bytes(pal6).hex()} {crc32(pal8):08x}")
    with open(path, "w") as f:
        f.write("\n".join(keep + lines) + "\n")


def main():
    pal6, pal8, shapes = load_game()
    build_editor(pal8, shapes)
    update_vectors(pal6)
    return 0


if __name__ == "__main__":
    sys.exit(main())
