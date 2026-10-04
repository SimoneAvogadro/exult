import os

import numpy as np
import pytest

from u7hires import ctx, qa, rules
from u7hires.fill import FlatLayerCache, render_flat_layer
from u7hires.scalers import scale2x, scale3x, xbrz_available


def test_terrain_windows_reproduce_engine_layer(world):
    cs = ctx.build_context(world, 16)
    assert len(cs.terrain) == len(world.used_terrains)
    for w in cs.terrain:
        px, _ = render_flat_layer(world, w.ident)
        assert np.array_equal(w.idx[16:-16, 16:-16], px)
        assert w.idx.shape == (160, 160) and w.tile_role.shape == (20, 20)
        own = w.tile_role == ctx.ROLE_OWN
        assert not own[:2].any() and not own[:, :2].any() and not own[-2:].any()
        assert w.weight == world.terrain_usage[w.ident]
    # every flat key is covered by some tier
    assert set(cs.coverage) == set(world.flat_keys)


def test_apron_is_real_neighbour_context(world):
    cache = FlatLayerCache(world)
    anchors = ctx.choose_anchors(world)
    t = 2
    cy, cx = anchors[t]
    assert world.tmap[cy, cx] == t
    w = ctx.terrain_window(world, cache, t, (cy, cx), 24)
    reg = ctx.world_region(world, cache, cy * 16 - 3, cx * 16 - 3, 22, 22)
    assert np.array_equal(w.idx, reg.idx)
    im = w.instance_map()
    assert im["world_x"][0, 0] == ((cx * 16 - 3) % 3072) * 8 and im["shape"].shape == w.idx.shape


def test_macro_detection_and_sheet(world):
    ms = ctx.macro_shapes(world)
    assert 1 in ms and 2 in ms                # terrains 0/1 tile the 8x4 layouts of shapes 1 and 2
    w = ctx.macro_window(world, 1, 16)
    assert w.idx.shape == (32 + 32, 64 + 32)
    cells = w.own_cells()
    assert len(cells) == 32 and sorted(f for _, _, _, f in cells) == list(range(32))
    unit = w.idx[16:48, 16:80]
    assert np.array_equal(unit[8:16, 24:32], world.flat(1, 11))
    assert np.array_equal(w.idx[:16, 16:80], unit[16:, :])          # toroidal wrap


def test_selfwrap_and_cut(world):
    w = ctx.selfwrap_window(world, 3, 5, 16)
    assert w.own_cells() == [(2, 2, 3, 5)]
    out = rules.nn_upscale(w.idx, 6)
    (s, f, ty, tx, t), = list(ctx.cut_instances(w, out, 6))
    assert (s, f) == (3, 5) and np.array_equal(t, rules.nn_upscale(world.flat(3, 5), 6))


def test_context_save_load(tmp_path, world):
    cs = ctx.build_context(world, 16, terrains=world.used_terrains[:3])
    ctx.save_context(cs, str(tmp_path), world, png=True)
    cs2 = ctx.load_context(str(tmp_path))
    assert [w.name for w in cs2.all()] == [w.name for w in cs.all()]
    assert np.array_equal(cs2.terrain[1].idx, cs.terrain[1].idx)
    assert os.path.exists(tmp_path / "png" / "terrain" / (cs.terrain[0].name + ".png"))


def test_scale2x3x_index_exact():
    rng = np.random.default_rng(0)
    a = rng.integers(0, 5, (10, 12)).astype(np.uint8)
    s6 = scale3x(scale2x(a))
    assert s6.shape == (60, 72) and set(np.unique(s6)) <= set(np.unique(a))


@pytest.mark.skipif(not xbrz_available(), reason="libxbrz19.so not built")
def test_route3_on_synthetic_world(tmp_path, world, provider):
    from u7hires import pack, route3
    p = route3.R3Params(variant="mixed")
    meta = route3.run(world, str(tmp_path / "cand"), p, workers=1)
    tiles, m2 = pack.load_candidates(str(tmp_path / "cand"))
    assert set(tiles) == set(world.flat_keys)
    for k, t in tiles.items():
        src = world.flat(*k)
        assert t.shape == (48, 48)
        assert rules.p4_violations(t, src) == 0 and not (t == 0xFF).any()
        if not (src == 0).any():
            assert not (t == 0).any()
    assert meta["keys"]["0001_00"]["tier"] in (0, 1)
    # deterministic: same result with a second run (and with workers)
    route3.run(world, str(tmp_path / "cand2"), p, workers=2)
    t2, _ = pack.load_candidates(str(tmp_path / "cand2"))
    assert all(np.array_equal(tiles[k], t2[k]) for k in tiles)
    # pack + engine checks + QA end to end
    s = pack.build_pack(str(tmp_path / "cand"), "syn", provider, str(tmp_path / "packs"))
    assert s["written"] == len(tiles) and not s["refused"]
    rep = qa.run_qa(str(tmp_path / "packs" / "syn"), world, str(tmp_path / "qa"), previews=True, sheets=True,
                    chunks=[{"category": "test", "cy": 10, "cx": 10}])
    assert rep["A2"]["pass"] and rep["A3"]["pass"] and rep["A1"]["pass"]
    assert os.path.exists(tmp_path / "qa" / "report.md") and rep["sheets"]
    assert os.path.exists(tmp_path / "qa" / "previews" / "test_010_010_6x_1280x800.png")
    assert "ratio" in rep["C1"]["views"][0]


def test_qa_metrics_on_nn_tiles(world):
    from u7hires.quant import Quantizer
    keys = world.flat_keys[:40]
    tiles = [rules.nn_upscale(world.flat(*k), 6) for k in keys]
    lut = rules.ramp_lut(rules.engine_ramps(world.pal6))
    m = qa.tile_metrics(keys, tiles, [world.flat(*k) for k in keys], Quantizer(world.pal8, lut), lut)
    assert (m["b1"] == 1).all() and (m["b4"] == 1).all() and m["block_de"].max() < 1e-9
    assert (m["a2_bad"] == 0).all() and (m["a3_p4"] == 0).all() and (m["a3_dropped"] == 0).all()
    # B1 sees a damaged block
    t = tiles[0].copy()
    t[:6, :6] = (int(t[0, 0]) + 1) if t[0, 0] < 0xDF else 1
    m = qa.tile_metrics(keys[:1], [t], [world.flat(*keys[0])], Quantizer(world.pal8, lut), lut)
    assert abs(m["b1"][0] - 63 / 64) < 1e-9


def test_seam_excess_and_c2(world):
    from u7hires.quant import Quantizer
    rgb = np.zeros((96, 96, 3), np.uint8)
    rgb[:, 48:] = 200                                    # one hard vertical seam at a tile border
    r, b, n = qa.seam_excess(rgb, 48)
    assert r > 10 and n < 1e-9 + b
    tiles = {k: rules.nn_upscale(world.flat(*k), 6) for k in world.flat_keys}
    c2 = qa.c2_pairs(world, tiles, Quantizer(world.pal8))
    assert c2["pairs"] > 0 and abs(c2["excess_mean"]) < 1e-9      # NN tiles reproduce the 1x seams
