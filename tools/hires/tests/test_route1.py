"""Route 1 CPU stages (diffusion itself needs torch + diffusers + SDXL and is not run here)."""

import numpy as np
import pytest

from u7hires import rules
from u7hires.quant import srgb_to_linear
from u7hires.route1 import (PILOT_WINDOWS, R1Params, backproject_smooth, detail_gain, gaussian_blur,
                            lowpass_lock, parse_settings, pick_consensus, post)
from u7hires.route3 import R3Kernel, R3Params
from u7hires.scalers import xbrz_available


def _noisy(world, src, sd=20.0, seed=3):
    rng = np.random.default_rng(seed)
    return np.clip(world.pal8[rules.nn_upscale(src, 6)].astype(np.float64) + rng.normal(0, sd, (*[6 * n for n in src.shape], 3)), 0, 255)


def test_smooth_backprojection_approaches_block_means(world):
    rng = np.random.default_rng(1)
    src = (6 + rng.integers(0, 5, (8, 8))).astype(np.uint8)
    rgb6 = _noisy(world, src)
    cyc = np.zeros((48, 48), bool)
    err0 = np.abs(srgb_to_linear(rgb6).reshape(8, 6, 8, 6, 3).mean((1, 3)) - srgb_to_linear(world.pal8[src])).mean()
    out = backproject_smooth(rgb6, src, world.pal8, cyc, 6, iters=6)
    err = np.abs(srgb_to_linear(out).reshape(8, 6, 8, 6, 3).mean((1, 3)) - srgb_to_linear(world.pal8[src])).mean()
    assert err < 0.5 * err0
    # factor 2 locks 2x2-parent means
    out2 = backproject_smooth(rgb6, src, world.pal8, cyc, 6, iters=6, factor=2)
    m2 = srgb_to_linear(out2).reshape(4, 12, 4, 12, 3).mean((1, 3))
    y2 = srgb_to_linear(world.pal8[src]).reshape(4, 2, 4, 2, 3).mean((1, 3))
    assert np.abs(m2 - y2).mean() < err0


def test_lowpass_lock_takes_low_frequencies_from_reference():
    rng = np.random.default_rng(4)
    x = np.full((48, 48, 3), 120.0) + rng.normal(0, 30, (48, 48, 3))
    ref = np.full((48, 48, 3), 60.0)
    out = lowpass_lock(np.clip(x, 0, 255), ref, np.zeros((48, 48), bool), 3.0)
    assert abs(srgb_to_linear(out).mean() - srgb_to_linear(ref).mean()) < 0.02
    assert out.std() > 5                       # texture kept
    cyc = np.zeros((48, 48), bool)
    cyc[:6, :6] = True
    out_c = lowpass_lock(np.clip(x, 0, 255), ref, cyc, 3.0)
    assert np.allclose(out_c[:6, :6], np.clip(x, 0, 255)[:6, :6], atol=1e-6)


def test_gaussian_blur_preserves_constant_and_mean():
    a = np.full((20, 30, 3), 0.25)
    assert np.allclose(gaussian_blur(a, 2.0), 0.25)


def test_detail_gain_keeps_block_means(world):
    src = np.full((4, 4), 8, np.uint8)
    x = _noisy(world, src, 10.0)
    g = detail_gain(x, 2.0)
    m0 = srgb_to_linear(x).reshape(4, 6, 4, 6, 3).mean((1, 3))
    m1 = srgb_to_linear(g).reshape(4, 6, 4, 6, 3).mean((1, 3))
    assert np.abs(m0 - m1).max() < 0.02
    assert g.std() > x.std()


def test_pick_consensus_is_medoid():
    rng = np.random.default_rng(5)
    base = rng.integers(0, 255, (48, 48, 3)).astype(np.float64)
    inst = [(np.full((48, 48), i, np.uint8), np.clip(base + d, 0, 255)) for i, d in enumerate((0, 3, 60))]
    tile, st = pick_consensus(inst)
    assert st["picked"] in (0, 1) and st["instances"] == 3
    single, st1 = pick_consensus(inst[:1])
    assert st1["picked"] == 0 and (single == 0).all()


@pytest.mark.skipif(not xbrz_available(), reason="libxbrz19.so not built")    # post() runs route 3
def test_post_is_deterministic_and_index_safe(world):
    rng = np.random.default_rng(6)
    idx = (6 + rng.integers(0, 5, (16, 16))).astype(np.uint8)
    k = R3Kernel(world.pal8, world.pal6, R3Params())
    out12 = np.clip(np.repeat(np.repeat(world.pal8[idx], 12, 0), 12, 1).astype(np.float32)
                    + rng.normal(0, 15, (192, 192, 3)), 0, 255).astype(np.float32)
    for lock in ("nn", "bicubic", "lp3"):
        ref = world.pal8[k.window(idx, True)] if lock == "lp3" else None
        f1, rgb6, r3, box6 = post(idx, out12, k, lock=lock, ref6=ref)
        f2 = post(idx, out12, k, lock=lock, ref6=ref)[0]
        assert f1.shape == (96, 96) and box6.shape == (96, 96, 3)
        assert np.array_equal(f1, f2)
        assert not (f1 == 0xFF).any() and not rules.IS_CYCLING[f1].any()


def test_settings_and_windows():
    s = parse_settings("soft:0.5:0.6:1000,xbrz:0.4:0.8:1001:painted", "grass")
    assert [p.tag for p in s] == ["soft_d0.50_c0.60_s1000", "xbrz_d0.40_c0.80_s1001_painted"]
    assert "pixel art" in s[1].prompts()[1] and "16-bit" in R1Params().prompts()[0]
    assert len({w.name for w in PILOT_WINDOWS}) == len(PILOT_WINDOWS)
