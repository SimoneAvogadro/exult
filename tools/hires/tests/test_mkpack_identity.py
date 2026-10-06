"""mkpack_identity.py on the synthetic game of tests/data/hires/rules/game (no game data).

The identity and marker packs are the inputs of the engine's override oracles O4a and O4b
(DESIGN.md section 6.4); these tests pin their contents: NN_S of every flat, the marker at the
top-left of every S x S block, palette 0 with the engine's clamp, the source guard, and the
bundle layout of section 5.7.
"""

import os
import struct
import sys
import zlib

import numpy as np
import pytest
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import mkpack_identity  # noqa: E402

GAME = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))), "tests", "data", "hires", "rules", "game")


def flex_entries(path):
    data = open(path, "rb").read()
    count = struct.unpack_from("<I", data, 0x54)[0]
    out = []
    for i in range(count):
        off, size = struct.unpack_from("<II", data, 0x80 + 8 * i)
        out.append(data[off:off + size] if off and size else b"")
    return out


def expected_flats():
    """Non-RLE entries of shapes.vga, frames 0-31, read independently of the tool."""
    flats = {}
    for shape, data in enumerate(flex_entries(os.path.join(GAME, "shapes.vga"))):
        if len(data) < 8:
            continue
        dlen = struct.unpack_from("<I", data, 0)[0]
        if dlen == len(data) or (len(data) % 2 == 0 and dlen == len(data) - 1):
            continue
        for frame in range(min(len(data) // 64, 32)):
            flats[(shape, frame)] = np.frombuffer(data, np.uint8, 64, 64 * frame).reshape(8, 8)
    return flats


def expected_palette():
    raw = flex_entries(os.path.join(GAME, "palettes.flx"))[0]
    six = raw[:768] if len(raw) == 768 else bytes(raw[2 * i] for i in range(768))
    return bytes(min(255, v * 255 // 63) for v in six)


@pytest.fixture(scope="module")
def flats():
    f = expected_flats()
    assert f, "the synthetic game has no flats"
    return f


def make(tmp_path, *args):
    out = str(tmp_path / "pack")
    assert mkpack_identity.main([GAME, out, *args]) == 0
    return out


def read_png(path):
    with Image.open(path) as im:
        assert im.mode == "P"
        return np.asarray(im).copy(), bytes(im.getpalette()[:768]), dict(im.text)


@pytest.mark.parametrize("scale", [2, 3, 6])
def test_identity_tiles_are_nn_of_the_flats(tmp_path, flats, scale):
    out = make(tmp_path, "--scales", str(scale))
    pal = expected_palette()
    names = sorted(os.listdir(os.path.join(out, f"x{scale}", "flats")))
    assert names == sorted({f"{s:04d}" for s, _ in flats})
    for (shape, frame), flat in flats.items():
        px, plte, text = read_png(os.path.join(out, f"x{scale}", "flats", f"{shape:04d}", f"{shape:04d}_{frame:02d}.png"))
        assert px.shape == (8 * scale, 8 * scale)
        assert np.array_equal(px, np.repeat(np.repeat(flat, scale, 0), scale, 1))
        assert plte == pal
        assert text["Exult-Src-CRC32"] == f"{zlib.crc32(flat.tobytes()) & 0xFFFFFFFF:08x}"
        assert text["Exult-Origin"] == "identity"
    meta = dict(line.split("=", 1) for line in open(os.path.join(out, "pack.txt")).read().splitlines())
    assert meta["palette_crc32"] == f"{zlib.crc32(pal) & 0xFFFFFFFF:08x}"
    assert meta["scale"] == str(scale)


def test_marker_tiles(tmp_path, flats):
    out = make(tmp_path, "--kind", "marker", "--marker", "0x05", "--scales", "3")
    for (shape, frame), flat in flats.items():
        px, _, text = read_png(os.path.join(out, "x3", "flats", f"{shape:04d}", f"{shape:04d}_{frame:02d}.png"))
        expect = np.repeat(np.repeat(flat, 3, 0), 3, 1)
        expect[::3, ::3] = 5
        assert np.array_equal(px, expect)
        assert text["Exult-Origin"] == "marker:5"


@pytest.mark.parametrize("subset", ["even", "odd"])
def test_subset_keeps_one_parity(tmp_path, flats, subset):
    out = make(tmp_path, "--kind", "marker", "--scales", "2", "--subset", subset)
    parity = 0 if subset == "even" else 1
    written = set()
    for root, _, files in os.walk(os.path.join(out, "x2", "flats")):
        for name in files:
            shape, frame = name[:-4].split("_")
            written.add((int(shape), int(frame)))
    expect = {k for k in flats if (k[0] + k[1]) % 2 == parity}
    assert written == expect
    assert expect and expect != set(flats), "the synthetic game needs tiles of both parities"
    for shape, frame in expect:
        px, _, _ = read_png(os.path.join(out, "x2", "flats", f"{shape:04d}", f"{shape:04d}_{frame:02d}.png"))
        nn = np.repeat(np.repeat(flats[(shape, frame)], 2, 0), 2, 1)
        nn[::2, ::2] = 1
        assert np.array_equal(px, nn)


def test_bundle_layout(tmp_path, flats):
    out = make(tmp_path, "--bundle", "--scales", "2,6")
    pal_crc = zlib.crc32(expected_palette()) & 0xFFFFFFFF
    assert not os.path.exists(os.path.join(out, "x6", "flats"))
    for scale in (2, 6):
        data = open(os.path.join(out, f"x{scale}", "flats.bundle"), "rb").read()
        side = 8 * scale
        magic, version, bscale, crc, count = struct.unpack_from("<4sHHII", data, 0)
        assert (magic, version, bscale, crc, count) == (b"U7HB", 1, scale, pal_crc, len(flats))
        assert len(data) == 16 + count * (8 + side * side)
        pos = 16
        keys = []
        for _ in range(count):
            shape, frame, flags, guard = struct.unpack_from("<HBBI", data, pos)
            px = np.frombuffer(data, np.uint8, side * side, pos + 8).reshape(side, side)
            flat = flats[(shape, frame)]
            assert flags == 1 and guard == zlib.crc32(flat.tobytes()) & 0xFFFFFFFF
            assert np.array_equal(px, np.repeat(np.repeat(flat, scale, 0), scale, 1))
            keys.append((shape, frame))
            pos += 8 + side * side
        assert keys == sorted(flats)


def test_output_is_deterministic(tmp_path):
    a = make(tmp_path / "a", "--scales", "2")
    b = make(tmp_path / "b", "--scales", "2")
    for root, _, files in os.walk(a):
        for name in files:
            rel = os.path.relpath(os.path.join(root, name), a)
            assert open(os.path.join(a, rel), "rb").read() == open(os.path.join(b, rel), "rb").read()


def test_rejects_cycling_marker_and_existing_output(tmp_path):
    with pytest.raises(SystemExit):
        mkpack_identity.main([GAME, str(tmp_path / "m"), "--kind", "marker", "--marker", "0xE0"])
    out = make(tmp_path, "--scales", "2")
    with pytest.raises(SystemExit):
        mkpack_identity.main([GAME, out, "--scales", "2"])
    assert mkpack_identity.main([GAME, out, "--scales", "3", "--force"]) == 0
    assert sorted(os.listdir(out)) == ["pack.txt", "x3"]


def test_force_refuses_foreign_directories(tmp_path):
    foreign = tmp_path / "foreign"
    foreign.mkdir()
    (foreign / "keep.txt").write_text("not a pack")
    with pytest.raises(SystemExit):
        mkpack_identity.main([GAME, str(foreign), "--scales", "2", "--force"])
    assert (foreign / "keep.txt").exists()
    # A real pack (route other than identity/marker) is not replaced either.
    real = tmp_path / "real"
    real.mkdir()
    (real / "pack.txt").write_text("game=BG\nscale=6\nroute=xbrz-hybrid\n")
    with pytest.raises(SystemExit):
        mkpack_identity.main([GAME, str(real), "--scales", "2", "--force"])
    assert (real / "pack.txt").exists()


@pytest.mark.parametrize("where", ["same", "inside", "parent"])
def test_refuses_output_overlapping_static(tmp_path, where):
    static = tmp_path / "static"
    static.mkdir()
    for name in ("shapes.vga", "palettes.flx"):
        (static / name).write_bytes(open(os.path.join(GAME, name), "rb").read())
    out = {"same": static, "inside": static / "pack", "parent": tmp_path}[where]
    before = sorted(os.listdir(static))
    with pytest.raises(SystemExit):
        mkpack_identity.main([str(static), str(out), "--scales", "2", "--force"])
    assert sorted(os.listdir(static)) == before
