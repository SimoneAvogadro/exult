"""Route 2 CPU stages (always) and a GPU smoke run (only where torch + spandrel + the model exist)."""

import importlib.util
import os

import numpy as np
import pytest

from u7hires import rules
from u7hires.quant import srgb_to_linear
from u7hires.route2 import DEFAULT_MODEL_DIR, KNOWN_MODELS, R2Params, backproject, post_window, to_6x
from u7hires.route3 import R3Kernel, R3Params


def test_to_6x_shapes():
    sr = np.random.default_rng(0).random((40, 48, 3)).astype(np.float32) * 255
    assert to_6x(sr, 4, 10, 12).shape == (60, 72, 3)
    sr8 = np.random.default_rng(0).random((80, 96, 3)).astype(np.float32) * 255
    assert to_6x(sr8, 8, 10, 12).shape == (60, 72, 3)


def test_backprojection_locks_block_means(world):
    rng = np.random.default_rng(2)
    src = (6 + rng.integers(0, 5, (8, 8))).astype(np.uint8)    # grey ramp, mid-range (no clipping at 0/255)
    rgb6 = np.clip(world.pal8[rules.nn_upscale(src, 6)].astype(np.float64) + rng.normal(0, 25, (48, 48, 3)), 0, 255)
    cyc = np.zeros((48, 48), bool)
    out = backproject(rgb6, src, world.pal8, cyc, 6, iters=6)
    mean = srgb_to_linear(out).reshape(8, 6, 8, 6, 3).mean((1, 3))
    assert np.abs(mean - srgb_to_linear(world.pal8[src])).max() < 2e-3
    # cycling pixels are untouched and excluded
    cyc[:6, :6] = True
    out2 = backproject(rgb6, src, world.pal8, cyc, 6, iters=2)
    assert np.allclose(out2[:6, :6], rgb6[:6, :6], atol=1e-6)


def test_post_window_rules(world):
    from u7hires.scalers import xbrz_available
    if not xbrz_available():
        pytest.skip("libxbrz19.so not built")
    k = R3Kernel(world.pal8, world.pal6, R3Params())
    idx = np.pad(world.flat(2, 4), 8, mode="wrap")              # water-like flat with glints
    sr = np.repeat(np.repeat(world.pal8[idx].astype(np.float32), 4, 0), 4, 1)
    final, rgb6, r3 = post_window(idx, sr, 4, k, R2Params())
    assert final.shape == (144, 144)
    cyc = rules.IS_CYCLING[final]
    assert np.array_equal(final[cyc], r3[cyc])                    # cycling only from route 3's plane
    assert not (final == 0xFF).any()
    tile = final[48:96, 48:96]
    fixed, _ = k.qz.enforce(tile, world.flat(2, 4), 6)
    assert rules.p4_violations(fixed, world.flat(2, 4)) == 0


def _gpu_ready():
    if importlib.util.find_spec("torch") is None or importlib.util.find_spec("spandrel") is None:
        return False
    return os.path.exists(os.path.join(DEFAULT_MODEL_DIR, KNOWN_MODELS["4x-NXbrz"]["file"]))


@pytest.mark.skipif(not _gpu_ready(), reason="torch/spandrel/4x-NXbrz not available (use the CUDA venv)")
def test_route2_gpu_smoke(tmp_path, world):
    from u7hires import pack, route2
    p = route2.R2Params(batch=4, chunk=16)
    cs = None
    meta = route2.run(world, str(tmp_path / "c"), p, workers=2, cs=cs, limit=3)
    tiles, _ = pack.load_candidates(str(tmp_path / "c"))
    assert meta["model_sha256"] == KNOWN_MODELS["4x-NXbrz"]["sha256"]
    for k, t in tiles.items():
        assert rules.p4_violations(t, world.flat(*k)) == 0 and not (t == 0xFF).any()
