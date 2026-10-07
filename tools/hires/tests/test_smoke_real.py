"""Smoke test on the real BG data; skipped when the STATIC directory is not available.

Reads EA data at test time only; nothing derived from it is written into the repository.
"""

import os

import numpy as np
import pytest

from conftest import real_static_dir

STATIC = real_static_dir()
pytestmark = pytest.mark.skipif(STATIC is None, reason="BG static dir not available (set U7_BG_STATIC)")
ART_ORIGINAL = os.environ.get("U7_ART_ORIGINAL", "/home/simonea/ultima7_exult/art_original")


@pytest.fixture(scope="module")
def bg():
    from u7hires.io import World
    return World.load(STATIC)


def test_bg_counts_and_palette(bg):
    from u7hires.hashing import BG_PALETTE0_CRC32, palette_crc32
    assert palette_crc32(bg.pal8) == BG_PALETTE0_CRC32
    assert tuple(bg.pal6[255]) == (250, 64, 1) and tuple(bg.pal8[255]) == (255, 255, 4)
    assert len(bg.flat_keys) == 3885
    assert len(bg.used_terrains) == 2105 and bg.num_terrains == 3072
    assert not bg.v2_chunks
    assert len(bg.flat_use) == 3120


def test_bg_ramps_match_research_table(bg):
    from u7hires.rules import engine_ramps
    r = engine_ramps(bg.pal6)
    assert len(r) == 23 and r[0] == (1, 0x0E) and r[15] == (0xD1, 0xDF) and r[-1] == (0xFF, 0xFF)


@pytest.mark.skipif(not os.path.isdir(os.path.join(ART_ORIGINAL, "chunks")), reason="art_original missing")
def test_fill_parity_with_existing_extraction_on_own_cells(bg):
    """art_original chunk renders (u7art.py, no under-RLE fill) equal our layer on every own flat cell."""
    from PIL import Image
    from u7hires.fill import render_flat_layer, own_flat_mask
    for t in [int(x) for x in bg.used_terrains[::97]]:
        p = os.path.join(ART_ORIGINAL, "chunks", f"chunk_{t:04d}.png")
        if not os.path.exists(p):
            continue
        ref = np.asarray(Image.open(p))
        mine, _ = render_flat_layer(bg, t)
        own = np.repeat(np.repeat(own_flat_mask(bg.terrain_kinds(t)).reshape(16, 16), 8, 0), 8, 1)
        assert np.array_equal(ref[own], mine[own])


def test_route3_kernel_and_checks_on_one_window(bg):
    from u7hires import check, ctx
    from u7hires.fill import FlatLayerCache
    from u7hires.route3 import R3Kernel, R3Params
    from u7hires.scalers import xbrz_available
    if not xbrz_available():
        pytest.skip("libxbrz19.so not built")
    cache = FlatLayerCache(bg)
    t = int(bg.used_terrains[0])
    w = ctx.terrain_window(bg, cache, t, ctx.choose_anchors(bg)[t], 16)
    k = R3Kernel(bg.pal8, bg.pal6, R3Params())
    out = k.window(w.idx, False)
    prov = check.Provider.from_world(bg)
    n = 0
    for s, f, ty, tx, tile in ctx.cut_instances(w, out, 6):
        fixed, _ = k.qz.enforce(tile, bg.flat(s, f), 6)
        assert not [x for x in check.check_tile_array(fixed, s, f, prov, offline=False) if x.severity == "reject"]
        n += 1
    assert n > 0
