import numpy as np
import pytest

import synth
from u7hires import io, rules
from u7hires.quant import Quantizer, dropped_sparkles, srgb_to_oklab, unique_rgb_palette


@pytest.fixture(scope="module")
def pal():
    p6 = synth.synth_palette6() & 0xFF
    return p6, io.palette8_from_6bit(p6)


@pytest.fixture(scope="module")
def qz(pal):
    p6, p8 = pal
    return Quantizer(p8, rules.ramp_lut(rules.engine_ramps(p6)))


def test_cycle_ranges():
    assert [rules.cycle_range_of(i) for i in (0xDF, 0xE0, 0xE7, 0xE8, 0xEF, 0xF0, 0xF3, 0xF4, 0xF8, 0xFC, 0xFE, 0xFF)] == \
        [-1, 0, 0, 1, 1, 2, 2, 3, 4, 5, 5, -1]


def test_engine_ramps_port(pal):
    p6, _ = pal
    ramps = rules.engine_ramps(p6)
    starts = [a for a, _ in ramps]
    assert starts[0] == 1
    for c in (0xE0, 0xE8, 0xF0, 0xF4, 0xF8, 0xFC, 0xFF):
        assert c in starts
    assert ramps[-1] == (0xFF, 0xFF)
    assert all(b >= a for a, b in ramps) and all(ramps[i][1] + 1 == ramps[i + 1][0] for i in range(len(ramps) - 1))
    lut = rules.ramp_lut(ramps)
    assert lut[0] == -1 and lut[1] == 0
    # a hand-made palette: one brightness jump at 10, otherwise flat
    q = np.zeros((256, 3), np.int64)
    q[10:] = 40
    r = rules.engine_ramps(q)
    assert r[0] == (1, 9) and r[1][0] == 10


def test_p4_in_tile_clamped_neighbourhood():
    src = np.full((8, 8), 0x20, np.uint8)
    src[3, 7] = 0xE2                         # cycling pixel at the right edge
    tile = rules.nn_upscale(src, 6)
    assert rules.p4_violations(tile, src) == 0
    t2 = tile.copy()
    t2[3 * 6, 6 * 6] = 0xE5                  # parent (3,6): neighbour (3,7) in range -> compliant
    assert rules.p4_violations(t2, src) == 0
    t2[0, 0] = 0xE5                          # parent (0,0): no E0-E7 nearby -> non-compliant
    assert rules.p4_violations(t2, src) == 1
    t3 = tile.copy()
    t3[3 * 6, 7 * 6] = 0xE9                  # wrong range (E8-EF) next to an E0-E7 parent
    assert rules.p4_violations(t3, src) == 1
    # a pixel whose only in-range neighbour lies in the *next* tile is non-compliant
    src2 = np.full((8, 8), 0x20, np.uint8)
    t4 = rules.nn_upscale(src2, 6)
    t4[3 * 6, 7 * 6 + 5] = 0xE1
    assert rules.p4_violations(t4, src2) == 1


def test_p4_verdict_thresholds():
    assert rules.p4_verdict(0, 2304) == "ok"
    assert rules.p4_verdict(11, 2304) == "warning"    # 0.477 %
    assert rules.p4_verdict(12, 2304) == "reject"     # 0.52 %


def test_reduce_mode_nn_identity_and_class_preservation():
    rng = np.random.default_rng(0)
    for _ in range(20):
        x = rng.integers(0, 256, (8, 8)).astype(np.uint8)
        nn6 = rules.nn_upscale(x, 6)
        assert np.array_equal(rules.reduce_mode(nn6, 6, 3, x), rules.nn_upscale(x, 3))
        assert np.array_equal(rules.reduce_mode(nn6, 6, 2, x), rules.nn_upscale(x, 2))
        assert np.array_equal(rules.reduce_mode(nn6, 6, 1, x), x)
    # parent cycling: the cycling index survives even when outnumbered
    parent = np.full((8, 8), 0x20, np.uint8)
    parent[0, 0] = 0xE3
    t = rules.nn_upscale(np.full((8, 8), 0x20, np.uint8), 6)
    t[0, 0] = 0xE3
    r = rules.reduce_mode(t, 6, 3, parent)
    assert r[0, 0] == 0xE3 and r[0, 1] == 0x20
    # non-cycling parent: static majority wins, ties to the smaller index
    t2 = np.zeros((2, 2), np.uint8)
    t2[:] = [[5, 7], [7, 5]]
    assert rules.reduce_mode(t2, 2, 1, np.array([[5]], np.uint8))[0, 0] == 5


def test_unique_palette_is_bijective(pal):
    _, p8 = pal
    pu, packed = unique_rgb_palette(p8)
    assert len(set(packed.tolist())) == 256
    assert int(np.abs(pu.astype(int) - p8.astype(int)).max()) <= 3
    # earlier indices keep their colour
    assert tuple(pu[0x0F]) == tuple(p8[0x0F]) and tuple(pu[0xE0]) != tuple(p8[0xE0])


def test_local_snap_inverts_exact_colours(qz):
    src = np.array([[0x0F, 0xE0], [0x30, 0x31]], np.uint8)
    rgb = qz.pal_u[rules.nn_upscale(src, 6)]
    out = qz.local_snap(rgb, src, 6, lab_pal=qz.lab_u)
    assert np.array_equal(out, rules.nn_upscale(src, 6))


def test_local_snap_candidates_stay_local(qz):
    rng = np.random.default_rng(5)
    src = (0x20 + rng.integers(0, 4, (8, 8))).astype(np.uint8)
    rgb = rng.integers(0, 256, (48, 48, 3)).astype(np.uint8)
    out = qz.local_snap(rgb, src, 6)
    nb = np.pad(src, 1, mode="edge")
    for y in range(0, 48, 5):
        for x in range(0, 48, 7):
            py, px = y // 6, x // 6
            assert out[y, x] in set(nb[py:py + 3, px:px + 3].ravel().tolist())
    # ramp expansion only adds static members of the candidates' ramps
    out2 = qz.local_snap(rgb, src, 6, ramp_expand=True)
    allowed = set()
    for i in np.unique(src):
        allowed |= set(np.nonzero(qz.ramp_lut == qz.ramp_lut[i])[0].tolist())
    assert set(np.unique(out2).tolist()) <= allowed


def test_quantizer_never_emits_bad_indices(qz):
    """Random sources with cycling pixels, 0x00 and 0xFF neighbours, random hi-res colours: after the
    snap and the in-tile enforcement there is no 0xFF, no P4 violation, no 0x00 unless the source has it."""
    rng = np.random.default_rng(11)
    for trial in range(30):
        src = (0x10 + rng.integers(0, 0xC0, (8, 8))).astype(np.uint8)
        m = rng.random((8, 8)) < 0.15
        src[m] = rng.choice([0xE0, 0xE4, 0xEA, 0xF1, 0xF6, 0xF9, 0xFD], m.sum())
        if trial % 3 == 0:
            src[rng.integers(0, 8), rng.integers(0, 8)] = 0
        ctx = np.pad(src, 2, mode="constant", constant_values=0xFF if trial % 2 else 0xE8)  # hostile context
        rgb = rng.integers(0, 256, (ctx.shape[0] * 6, ctx.shape[1] * 6, 3)).astype(np.uint8)
        hi = qz.local_snap(rgb, ctx, 6)
        tile = hi[12:-12, 12:-12]
        fixed, st = qz.enforce(tile, src, 6)
        assert not (fixed == 0xFF).any()
        assert rules.p4_violations(fixed, src) == 0
        if not (src == 0).any():
            assert not (fixed == 0).any()
        stat = ~rules.IS_CYCLING[fixed]
        assert ((fixed[stat] >= 1) & (fixed[stat] <= 0xDF) | (fixed[stat] == 0)).all()
        # cycling pixels only where P4 allows them
        allowed = rules.p4_allowed_masks(src, 6)
        cid = rules.CYCLE_ID[fixed]
        for r in range(6):
            assert not ((cid == r) & ~allowed[r]).any()


def test_static_snap_with_forbidden_cycling(qz):
    src = np.full((4, 4), 0xE0, np.uint8)
    src[0, 0] = 0x22
    rgb = np.full((24, 24, 3), 255, np.uint8)
    out = qz.local_snap(rgb, src, 6, forbid=rules.IS_CYCLING)
    assert not rules.IS_CYCLING[out].any()
    assert ((out >= 1) & (out <= 0xDF)).all()        # parents without static candidates: global fallback


def test_restore_sparkles(qz):
    src = np.full((8, 8), 0x20, np.uint8)
    src[4, 4] = 0xE1
    tile = rules.nn_upscale(np.full((8, 8), 0x20, np.uint8), 6)
    assert dropped_sparkles(tile, src) == 1
    fixed, st = qz.enforce(tile, src, 6, restore_sparkles=True)
    assert st["sparkles_restored"] == 1 and dropped_sparkles(fixed, src) == 0
    assert rules.p4_violations(fixed, src) == 0


def test_oklab_reference_values():
    lab = srgb_to_oklab(np.array([[255, 255, 255], [0, 0, 0], [255, 0, 0]], np.uint8))
    assert abs(lab[0, 0] - 1.0) < 1e-3 and abs(lab[1]).max() < 1e-6
    assert abs(lab[2, 0] - 0.62796) < 1e-3 and abs(lab[2, 1] - 0.22486) < 1e-3
