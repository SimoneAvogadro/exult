import io as _io
import struct
import zlib

import numpy as np
import pytest
from PIL import Image

import synth
from u7hires import io, pngio


@pytest.fixture(scope="module")
def pal8():
    return io.palette8_from_6bit(synth.synth_palette6() & 0xFF)


def test_roundtrip_raw_indices_plte_text(pal8):
    rng = np.random.default_rng(1)
    idx = rng.integers(0, 256, (48, 48)).astype(np.uint8)
    data = pngio.encode_indexed(idx, pal8, [("Exult-Src-CRC32", "0123abcd"), ("Exult-Origin", "test route")])
    info = pngio.parse_png(data)
    assert (info.width, info.height, info.bit_depth, info.color_type) == (48, 48, 8, 3)
    assert info.plte.shape == (256, 3) and np.array_equal(info.plte, pal8)
    assert info.trns is None
    assert info.texts == {"Exult-Src-CRC32": "0123abcd", "Exult-Origin": "test route"}
    # tEXt before IDAT (DESIGN §5.3; libpng reads both, editors keep the first)
    ct = info.chunk_types
    assert ct.index("tEXt") < ct.index("IDAT") and ct[0] == "IHDR" and ct[-1] == "IEND"
    assert np.array_equal(pngio.decode_indexed(data), idx)
    # Pillow agrees on the raw indices (no rotation, no palette lookup)
    im = Image.open(_io.BytesIO(data))
    assert im.mode == "P" and np.array_equal(np.asarray(im), idx)
    # deterministic bytes
    assert data == pngio.encode_indexed(idx, pal8, [("Exult-Src-CRC32", "0123abcd"), ("Exult-Origin", "test route")])


@pytest.mark.parametrize("depth", [1, 2, 4])
def test_low_bit_depths(pal8, depth):
    rng = np.random.default_rng(depth)
    idx = rng.integers(0, 1 << depth, (13, 11)).astype(np.uint8)
    data = pngio.encode_indexed(idx, pal8[: 1 << depth], bit_depth=depth)
    assert np.array_equal(pngio.decode_indexed(data), idx)
    assert np.array_equal(np.asarray(Image.open(_io.BytesIO(data))), idx)


def _filter_rows(rows: np.ndarray, ftype: int) -> bytes:
    out = bytearray()
    prior = np.zeros(rows.shape[1], np.int32)
    for r in rows.astype(np.int32):
        left = np.concatenate([[0], r[:-1]])
        upleft = np.concatenate([[0], prior[:-1]])
        if ftype == 0:
            f = r
        elif ftype == 1:
            f = r - left
        elif ftype == 2:
            f = r - prior
        elif ftype == 3:
            f = r - (left + prior) // 2
        else:
            p = left + prior - upleft
            pa, pb, pc = abs(p - left), abs(p - prior), abs(p - upleft)
            pr = np.where((pa <= pb) & (pa <= pc), left, np.where(pb <= pc, prior, upleft))
            f = r - pr
        out += bytes([ftype]) + bytes((f & 255).astype(np.uint8))
        prior = r
    return bytes(out)


@pytest.mark.parametrize("ftype", [0, 1, 2, 3, 4])
def test_all_filter_types(pal8, ftype):
    rng = np.random.default_rng(ftype)
    idx = rng.integers(0, 256, (20, 17)).astype(np.uint8)
    data = pngio.build_png(17, 20, 8, 3, _filter_rows(idx, ftype), pal8.tobytes())
    assert np.array_equal(pngio.decode_indexed(data), idx)
    assert np.array_equal(np.asarray(Image.open(_io.BytesIO(data))), idx)


def test_crc_and_truncation_errors(pal8):
    data = bytearray(pngio.encode_indexed(np.zeros((4, 4), np.uint8), pal8))
    with pytest.raises(pngio.PngError):
        pngio.parse_png(bytes(data[:-20]))
    bad = bytearray(data)
    bad[8 + 8 + 2] ^= 1                                     # IHDR body
    with pytest.raises(pngio.PngError):
        pngio.parse_png(bytes(bad))
    with pytest.raises(pngio.PngError):
        pngio.parse_png(b"GIF89a" + bytes(20))


def test_text_after_idat_and_ztxt(pal8):
    z = pngio.chunk(b"zTXt", b"Exult-Origin\0\0" + zlib.compress(b"compressed"))
    data = pngio.build_png(2, 2, 8, 3, b"\0\0\0\0\0\0", pal8.tobytes(), after_idat=[pngio.text_chunk("Exult-Src-CRC32", "deadbeef"), z])
    info = pngio.parse_png(data)
    assert info.texts["Exult-Src-CRC32"] == "deadbeef" and info.texts["Exult-Origin"] == "compressed"


def test_set_texts_restamp_keeps_pixels(pal8):
    idx = np.arange(64, dtype=np.uint8).reshape(8, 8)
    data = pngio.encode_indexed(idx, pal8, [("Exult-Src-CRC32", "00000000"), ("Other", "x")])
    out = pngio.set_texts(data, {"Exult-Src-CRC32": "89abcdef"})
    info = pngio.parse_png(out)
    assert info.texts["Exult-Src-CRC32"] == "89abcdef" and info.texts["Other"] == "x"
    assert info.chunk_types.count("tEXt") == 2
    assert np.array_equal(pngio.decode_indexed(out), idx)
    plain = pngio.encode_indexed(idx, pal8)
    assert pngio.parse_png(pngio.set_texts(plain, {"K": "v"})).texts == {"K": "v"}


def test_ihdr_before_decoding_is_cheap(pal8):
    """parse_png reads the header without decoding pixels (F3 is decided on IHDR)."""
    huge = pngio.build_png(100000, 100000, 8, 3, b"", pal8.tobytes())
    info = pngio.parse_png(huge)
    assert (info.width, info.height) == (100000, 100000)
    assert struct.unpack(">I", huge[16:20])[0] == 100000
