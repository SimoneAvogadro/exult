import numpy as np
import pytest

import synth
from u7hires import hashing, io


def test_crc32_vectors():
    assert hashing.crc32(b"") == 0
    assert hashing.crc32(b"123456789") == 0xCBF43926          # CRC-32/ISO-HDLC check value
    assert hashing.crc32(b"The quick brown fox jumps over the lazy dog") == 0x414FA339
    assert hashing.crc32_hex(bytes(64)) == f"{hashing.crc32(bytes(64)):08x}"


def test_fnv1a64_vectors():
    assert hashing.fnv1a64(b"") == 0xCBF29CE484222325
    assert hashing.fnv1a64(b"a") == 0xAF63DC4C8601EC8C
    assert hashing.fnv1a64(b"foobar") == 0x85944171F73967E8


def test_get_color8_clamps_like_engine():
    # Get_color8(val, 63, 100) = val*25500/6300, clamped at 255 (iwin8.cc)
    assert io.get_color8(0) == 0
    assert io.get_color8(63) == 255
    assert io.get_color8(32) == 32 * 255 // 63
    assert io.get_color8(250) == 255             # not (250*255//63) & 0xFF
    assert io.get_color8(64) == 255
    v = np.arange(64)
    assert np.array_equal(io.get_color8(v), (v * 255 // 63).astype(np.uint8))


def test_palette_crc_clamp_vs_wrap():
    p6 = synth.synth_palette6()
    pal8 = io.palette8_from_6bit(p6 & 0xFF)
    assert tuple(pal8[255]) == (255, 255, 4)
    wrapped = ((p6 & 0xFF) * 255 // 63) & 0xFF
    assert hashing.palette_crc32(pal8) != hashing.palette_crc32(wrapped.astype(np.uint8))
    assert hashing.BG_PALETTE0_CRC32 == 0xC9C2C0E7 and hashing.BG_PALETTE0_CRC32_WRAPPED == 0x78D19732


def test_palette_double_entry_interleaved():
    p6 = (np.arange(768) % 64).astype(np.uint8)
    dbl = np.zeros(1536, np.uint8)
    dbl[0::2] = p6
    dbl[1::2] = 7
    assert np.array_equal(io.palette6_from_entry(dbl.tobytes()).ravel(), p6)
    with pytest.raises(ValueError):
        io.palette6_from_entry(bytes(100))


def test_flex_roundtrip():
    ents = [b"abc", b"", b"x" * 100]
    assert io.parse_flex(io.build_flex(ents)) == ents


def test_shapes_semantics():
    sf = synth.synth_shapes()
    assert sf.is_flat_shape(1) and not sf.is_flat_shape(synth.RLE_SHAPE) and not sf.is_flat_shape(6)
    assert sf.nframes(12) == 4 and sf.nframes(synth.RLE_SHAPE) == 2
    assert sf.flat(12, 3) is not None and sf.flat(12, 4) is None          # beyond the frame count
    assert np.array_equal(sf.flat(1, 33), sf.flat(1, 1))                    # framenum &= 31 for flats
    assert sf.kind(1, 5) == io.KIND_FLAT
    assert sf.kind(12, 9) == io.KIND_NONE
    assert sf.kind(synth.RLE_SHAPE, 1) == io.KIND_RLE
    assert sf.kind(synth.RLE_SHAPE, 5) == io.KIND_NONE
    assert sf.kind(6, 0) == io.KIND_NONE and sf.kind(999, 0) == io.KIND_NONE
    fr = sf.rle_frame(synth.RLE_SHAPE, 1)
    assert fr.pixels.shape == (8, 8) and fr.mask.all() and (fr.pixels == 0x56).all()
    assert len(sf.all_flat_keys()) == 6 * 32 + 4


def test_chunks_v1_and_v2_header():
    tiles = [[(i % 300, i % 32) for i in range(256)]]
    v1 = synth.chunk_bytes(tiles)
    s, f, v2 = io.parse_chunks(v1)
    assert not v2 and s[0, 5] == 5 and f[0, 33] == 1
    raw = bytearray(io.V2_CHUNK_HDR)
    for sh, fr in tiles[0]:
        raw += bytes([sh & 0xFF, sh >> 8, fr])
    s2, f2, isv2 = io.parse_chunks(bytes(raw))
    assert isv2 and np.array_equal(s, s2) and np.array_equal(f, f2)
    assert len(io.V2_CHUNK_HDR) == 10


def test_world_load_and_grid(world):
    tm = synth.synth_map(8)
    assert np.array_equal(world.tmap, tm)
    S, F = world.tile_grid
    assert S.shape == (3072, 3072)
    cy, cx, ty, tx = 100, 7, 3, 9
    t = tm[cy, cx]
    assert S[cy * 16 + ty, cx * 16 + tx] == world.chunk_shape[t, ty * 16 + tx]
    assert F[cy * 16 + ty, cx * 16 + tx] == world.chunk_frame[t, ty * 16 + tx] & 31
    assert hashing.palette_crc32(world.pal8) == hashing.palette_crc32(io.palette8_from_6bit(synth.synth_palette6() & 0xFF))
    assert set(world.used_terrains.tolist()) == set(range(8))


def test_t1_key_properties(world):
    t = 2
    kinds = world.terrain_kinds(t)
    own = (kinds == io.KIND_FLAT) | (kinds == io.KIND_FLAT_VOID)
    flats = [world.flat(int(world.chunk_shape[t, i]), int(world.chunk_frame[t, i])) for i in range(256)]
    k = hashing.t1_key(own, flats)
    # independent of what non-own cells hold (the fill)
    flats2 = list(flats)
    for i in np.nonzero(~own)[0]:
        flats2[i] = np.full((8, 8), 7, np.uint8)
    assert hashing.t1_key(own, flats2) == k
    # dependent on own pixels and on the own bitmap
    i0 = int(np.nonzero(own)[0][0])
    flats3 = list(flats)
    flats3[i0] = flats[i0].copy()
    flats3[i0][0, 0] ^= 1
    assert hashing.t1_key(own, flats3) != k
    own2 = own.copy()
    own2[i0] = False
    assert hashing.t1_key(own2, flats) != k
    assert len(hashing.t1_hex(k)) == 16


def test_hash_vectors_file():
    """tests/data/hash_vectors.txt: shared vectors (engine doctest can read the same file)."""
    import os
    from conftest import DATA
    vec = {}
    with open(os.path.join(DATA, "hash_vectors.txt")) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                kind, inp, out = line.split()
                vec.setdefault(kind, []).append((inp, out))
    for inp, out in vec["crc32"]:
        assert f"{hashing.crc32(bytes.fromhex(inp) if inp != '-' else b''):08x}" == out
    for inp, out in vec["fnv1a64"]:
        assert f"{hashing.fnv1a64(bytes.fromhex(inp) if inp != '-' else b''):016x}" == out
    for inp, out in vec["get_color8"]:
        assert int(io.get_color8(int(inp))) == int(out)
