"""The paint_tile port on crafted chunks, pinning the engine quirks P1 ('tiley + y > 0') and P2
(no 12/0 skip in the full-chunk scan)."""

import numpy as np

from u7hires.fill import find_flat_source, flat_sources, render_flat_layer
from u7hires.io import KIND_FLAT, KIND_FLAT_VOID, KIND_NONE, KIND_RLE

R, F, V, N = KIND_RLE, KIND_FLAT, KIND_FLAT_VOID, KIND_NONE


def chunk(default=R, **cells):
    k = [default] * 256
    for name, kind in cells.items():
        x, y = (int(v) for v in name[1:].split("_"))
        k[y * 16 + x] = kind
    return k


def idx(x, y):
    return y * 16 + x


def test_flat_paints_itself_and_none_paints_nothing():
    k = chunk(F, c3_4=N, c5_5=V)
    assert find_flat_source(2, 2, k) == idx(2, 2)
    assert find_flat_source(5, 5, k) == idx(5, 5)        # 12/0 paints itself
    assert find_flat_source(3, 4, k) == -1               # get_shape() null: nothing


def test_neighbour_order_rows_then_columns():
    k = chunk(R, c6_4=F, c4_6=F, c4_5=F)
    # y = -1 row first: (4,4) R, (5,4) R, (6,4) F
    assert find_flat_source(5, 5, k) == idx(6, 4)
    k = chunk(R, c4_5=F, c6_5=F)
    assert find_flat_source(5, 5, k) == idx(4, 5)


def test_quirk_p1_row_zero_is_never_a_neighbour():
    # RLE at (5,1); its only flat neighbour is (5,0) in row 0, which pass 1 skips ('> 0').
    # The full scan then finds the first flat row-major: (0,0), not (5,0).
    k = chunk(R, c5_0=F, c0_0=F)
    assert find_flat_source(5, 1, k) == idx(0, 0)
    # a neighbour in row 2 is accepted
    k = chunk(R, c5_0=F, c0_0=F, c5_2=F)
    assert find_flat_source(5, 1, k) == idx(5, 2)
    # at ty = 0 the row above is out of range anyway and row 0 itself is skipped too
    k = chunk(R, c4_0=F, c6_1=F)
    assert find_flat_source(5, 0, k) == idx(6, 1)


def test_void_skipped_in_pass1_but_not_in_full_scan():
    # quirk P2: (5,4) is 12/0 and skipped by pass 1; the full scan takes (0,0) = 12/0 first.
    k = chunk(R, c5_4=V, c0_0=V, c9_9=F)
    assert find_flat_source(5, 5, k) == idx(0, 0)
    # without the void at (0,0) the full scan finds (5,4) (void, row 4) before (9,9)
    k = chunk(R, c5_4=V, c9_9=F)
    assert find_flat_source(5, 5, k) == idx(5, 4)
    # a real flat neighbour wins over a void neighbour that comes first in loop order
    k = chunk(R, c4_4=V, c6_6=F)
    assert find_flat_source(5, 5, k) == idx(6, 6)


def test_missing_and_rle_neighbours_skipped():
    k = chunk(R, c4_4=N, c5_4=R, c6_6=F)
    assert find_flat_source(5, 5, k) == idx(6, 6)


def test_edges_clamped_in_x_and_no_flat_at_all():
    k = chunk(R, c15_8=F)
    assert find_flat_source(0, 8, k) == idx(15, 8)        # not a neighbour (x - 1 < 0): full scan
    k = chunk(R, c1_8=F)
    assert find_flat_source(0, 8, k) == idx(1, 8)
    assert find_flat_source(3, 3, chunk(R)) == -1
    assert find_flat_source(3, 3, chunk(N, c3_3=R)) == -1


def test_flat_sources_table():
    k = chunk(F, c3_3=R, c7_7=N)
    src = flat_sources(k)
    assert src[idx(3, 3)] == idx(2, 2)
    assert src[idx(7, 7)] == -1
    assert src[idx(0, 0)] == idx(0, 0)


def test_render_flat_layer_synthetic(world):
    t = 3
    px, src = render_flat_layer(world, t)
    kinds = world.terrain_kinds(t)
    for i in range(256):
        ty, tx = divmod(i, 16)
        cell = px[ty * 8:(ty + 1) * 8, tx * 8:(tx + 1) * 8]
        s = int(src[i])
        if s < 0:
            assert (cell == 0).all()          # P3 zero fill
            continue
        exp = world.flat(int(world.chunk_shape[t, s]), int(world.chunk_frame[t, s]))
        assert np.array_equal(cell, exp)
        if kinds[i] in (KIND_FLAT, KIND_FLAT_VOID):
            assert s == i
    assert (kinds == KIND_RLE).sum() > 0 and (kinds == KIND_NONE).sum() == 1      # tile 200 = 12/9
    assert src[200] == -1


def test_compare_with_engine_dump(tmp_path, world):
    """Parity harness for the engine's --dump-art terrain/<t1>.png (here a fake dump from our port)."""
    from u7hires.fill import compare_with_dump, terrain_t1
    from u7hires.hashing import t1_hex
    from u7hires.pngio import encode_indexed
    d = tmp_path / "terrain"
    d.mkdir()
    for i, t in enumerate([2, 3, 4]):
        px, _ = render_flat_layer(world, t)
        if i == 2:
            px = px.copy()
            px[5, 5] ^= 1
        (d / (t1_hex(terrain_t1(world, t)) + ".png")).write_bytes(encode_indexed(px, world.pal8))
    r = compare_with_dump(world, str(tmp_path))
    assert r["compared"] == 3 and r["equal"] == 2 and len(r["mismatch"]) == 1
    assert r["mismatch"][0]["terrain"] == 4 and r["mismatch"][0]["pixels"] == 1
    assert r["missing"] == len(world.used_terrains) - 3
