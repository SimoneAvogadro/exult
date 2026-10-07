import json
import os

import numpy as np
import pytest

from u7hires import compare, pack, qa, rules
from u7hires.vote import compare_trees


def _cands(path, world, keys, damage=False):
    """NN candidate tiles; ``damage`` changes one 6x6 block per tile (B1 = 63/64)."""
    tiles = {}
    for k in keys:
        t = rules.nn_upscale(world.flat(*k), 6).copy()
        if damage:
            v = int(t[0, 0])
            t[:6, :6] = v + 1 if 1 <= v < 0xDF else 1
        tiles[k] = t
    pack.save_candidates(str(path), tiles, {"route": "test", "params": {}})
    return tiles


def test_parse_pack_spec():
    assert compare.parse_pack_spec("r3=/a/b") == ("r3", "/a/b", None)
    assert compare.parse_pack_spec("r2a=/a/sub@r2") == ("r2a", "/a/sub", "r2")
    for bad in ("r3", "=/a", "r3="):
        with pytest.raises(ValueError):
            compare.parse_pack_spec(bad)


def test_sparkle_rgb_marks_cycle_ranges(world):
    t = np.array([[0x10, 0xE0], [0xE8, 0xFC]], np.uint8)
    rgb = compare.sparkle_rgb(world.pal8, t)
    assert tuple(rgb[0, 1]) == (255, 255, 255) and tuple(rgb[1, 0]) == (0, 255, 255)
    assert tuple(rgb[1, 1]) == (0, 255, 0)
    assert rgb[0, 0].max() <= 0.35 * 255 and rgb[0, 0][0] == rgb[0, 0][1] == rgb[0, 0][2]


def test_compare_outputs_metrics_and_determinism(tmp_path, world):
    keys = world.flat_keys
    _cands(tmp_path / "a", world, keys)
    _cands(tmp_path / "b", world, keys, damage=True)
    _cands(tmp_path / "s", world, keys[:10])
    # a QA run for pack "a" (its per_tile.json is the cross-check reference)
    qa.run_qa(str(tmp_path / "a"), world, str(tmp_path / "qa" / "a"), previews=False, sheets=False, c1=False, c2=False)
    specs = [f"a={tmp_path / 'a'}", f"b={tmp_path / 'b'}", f"s={tmp_path / 's'}@a"]
    ch = [{"category": "test", "cy": 10, "cx": 10}]
    kw = dict(qa_root=str(tmp_path / "qa"), groups_spec="first=1+2", per_group=4, chunks=ch)
    r = compare.run_compare(world, specs, str(tmp_path / "out1"), **kw)
    assert r["ok"]
    pa, pb, ps = (r["packs"][x] for x in "abs")
    assert pa["qa_crosscheck"]["ok"] and pa["qa_crosscheck"]["checked"] == len(keys)
    assert pb["qa_crosscheck"]["ok"] is None                      # no QA run for b
    assert pa["all"]["b1"] == 1.0 and abs(pb["all"]["b1"] - 63 / 64) < 1e-12
    assert pa["all"]["nn_diff"] == 0.0 and abs(pb["all"]["nn_diff_used"] - 1 / 64) < 1e-12
    assert pa["tiles"] == pb["tiles"] == len(keys) and ps["tiles"] == 10 and ps["base"] == "a"
    assert "first" in r["groups"] and r["groups"]["first"]["keys"] == sum(1 for k in keys if k[0] in (1, 2))
    assert set(r["subsets"]) == {"s"} and r["subsets"]["s"]["groups"]["all"]["keys"] == 10
    out = tmp_path / "out1"
    for p in r["sheets"] + r["views"] + r["previews"]:
        assert not os.path.isabs(p) and os.path.exists(out / p)
    assert {os.path.basename(p) for p in r["previews"]} == {"nn_6x.png", "a_6x.png", "b_6x.png", "s_6x.png"}
    c = r["crops"]["test_010_010"]
    assert c["tiles"] == 10 and (out / "views" / "test_010_010.png").exists()
    md = (out / "metrics.md").read_text()
    assert "## QA gates" in md and "## On the keys of subset pack `s`" in md and "differ from the NN upscale" in md
    # the subset pack is drawn over its base: same render as the base where it has no tile of its own
    from PIL import Image
    ia = np.asarray(Image.open(out / "previews" / "test_010_010" / "a_6x.png"))
    isub = np.asarray(Image.open(out / "previews" / "test_010_010" / "s_6x.png"))
    assert np.array_equal(ia, isub)                               # s holds a's NN tiles for its 10 keys
    # deterministic, path-independent output
    compare.run_compare(world, specs, str(tmp_path / "out2"), **kw)
    assert compare_trees(str(out), str(tmp_path / "out2"))["identical"]
    # the cross-check catches a changed QA value
    pt = tmp_path / "qa" / "a" / "per_tile.json"
    q = json.loads(pt.read_text())
    first = sorted(q)[0]
    q[first]["b1"] = q[first]["b1"] - 2 ** -20
    pt.write_text(json.dumps(q))
    p = compare.load_pack(world, compare.Provider.from_world(world), "a", str(tmp_path / "a"), None,
                          str(tmp_path / "qa"))
    from u7hires.quant import Quantizer
    lut = rules.ramp_lut(rules.engine_ramps(world.pal6))
    compare.compute_metrics(world, p, Quantizer(world.pal8, lut), lut)
    cc = compare.crosscheck_qa(p)
    assert cc["ok"] is False and cc["mismatched"] == [first]


def test_choose_crop_prefers_transitions_and_avoids_overlay():
    from types import SimpleNamespace
    th, tw = 25, 40
    reg = SimpleNamespace(th=th, tw=tw, own=np.ones((th, tw), bool), src_shape=np.ones((th, tw), np.int16),
                          src_frame=np.zeros((th, tw), np.int16))
    fams = {(1, 0): "grass", (1, 1): "dirt"}
    assert compare.choose_crop(reg, fams, 10) == (7, 15)          # uniform: the centred crop
    cover = np.zeros((th * 8, tw * 8), bool)
    cover[:, : tw * 4] = True                                     # the overlay hides the left half
    assert compare.choose_crop(reg, fams, 10, cover) == (7, 20)
    reg.src_frame[0:4, 30:34] = 1                                 # a dirt patch in the top right corner
    y0, x0 = compare.choose_crop(reg, fams, 10)
    assert y0 <= 0 + 3 and x0 <= 30 and x0 + 10 >= 34             # the crop contains the patch
