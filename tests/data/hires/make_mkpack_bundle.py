#!/usr/bin/env python3
"""Write bundle/ (a pack root) with the art tools' own writer, for the engine's bundle reader test.

``bundle/pack.txt`` and ``bundle/x6/flats.bundle`` come from tools/hires/u7hires/pack.py
(``pack_txt_text`` and ``write_bundle``), so test_hires_store checks that the engine reads exactly
what mkpack writes (DESIGN.md section 6.7, WP-12). Content: NN x6 of three flats of the synthetic
game in rules/game ((1, 2) and (2, 0) guarded, (1, 3) unguarded); no EA data.

usage: make_mkpack_bundle.py [TOOLS_DIR]   (default: ../../../tools/hires, the u7hires package)
Needs numpy (tools-venv).
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "..", "tools", "hires")
sys.path.insert(0, os.path.abspath(TOOLS))

from u7hires import io, pack, rules  # noqa: E402
from u7hires.hashing import crc32, palette_crc32  # noqa: E402


def main():
    game = os.path.join(HERE, "rules", "game")
    pal8 = io.palette8_from_6bit(io.palette6_from_entry(io.read_flex(os.path.join(game, "palettes.flx"))[0]))
    shapes = io.ShapesFile(io.read_flex(os.path.join(game, "shapes.vga")))
    entries = []
    for shape, frame, guarded in ((2, 0, True), (1, 3, False), (1, 2, True)):
        src = shapes.flat(shape, frame)
        entries.append((shape, frame, rules.nn_upscale(src, 6), crc32(src.tobytes()) if guarded else None))
    root = os.path.join(HERE, "bundle")
    os.makedirs(os.path.join(root, "x6"), exist_ok=True)
    pcrc = palette_crc32(pal8)
    pack.write_bundle(os.path.join(root, "x6", "flats.bundle"), entries, 6, pcrc)
    with open(os.path.join(root, "pack.txt"), "w") as f:
        f.write(pack.pack_txt_text("TEST", 6, pcrc, "none", "unit", "mkpack bundle fixture"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
