"""Validator: rule IDs on the shared crafted fixtures, and root-level rules (R1, G2, B0, .off)."""

import filecmp
import os
import shutil

import numpy as np
import pytest

import make_rule_fixtures
from conftest import DATA
from u7hires import check, io, pack, rules
from u7hires.hashing import crc32

RULES = os.path.join(DATA, "rules")


@pytest.fixture(scope="module")
def fx_provider():
    pal6 = io.palette6_from_entry(io.read_flex(os.path.join(RULES, "game", "palettes.flx"))[0])
    shapes = io.ShapesFile.load(os.path.join(RULES, "game", "shapes.vga"))
    return check.Provider(io.palette8_from_6bit(pal6), shapes.num_shapes, shapes.flat, pal6)


def expected():
    out = []
    with open(os.path.join(RULES, "expected.txt")) as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                out.append(tuple(line.split()))
    return out


def test_fixtures_are_up_to_date(tmp_path):
    make_rule_fixtures.build(str(tmp_path))
    for root, _, files in os.walk(tmp_path):
        for n in files:
            p = os.path.join(root, n)
            rel = os.path.relpath(p, tmp_path)
            assert filecmp.cmp(p, os.path.join(RULES, rel), shallow=False), f"stale fixture {rel}"


@pytest.mark.parametrize("path,rule,sev", expected())
def test_rule_ids_on_fixtures(fx_provider, path, rule, sev):
    fp = os.path.join(RULES, path)
    with open(fp, "rb") as f:
        data = f.read()
    r = check.check_png_bytes(data, os.path.basename(fp), fx_provider, 6, fp)
    if rule == "OK":
        assert not r.rejected and not [f for f in r.findings if f.severity in ("warning", "offline")]
    elif sev == "reject":
        assert r.rejected and r.primary == rule, [(f.rule, f.detail) for f in r.findings]
    else:
        assert not r.rejected
        assert any(f.rule == rule and f.severity == sev for f in r.findings), [(f.rule, f.severity) for f in r.findings]


def test_guard_flag(fx_provider):
    with open(os.path.join(RULES, "ok", "0001_02.png"), "rb") as f:
        assert check.check_png_bytes(f.read(), "0001_02.png", fx_provider).guarded
    with open(os.path.join(RULES, "ok_unguarded", "0001_03.png"), "rb") as f:
        assert not check.check_png_bytes(f.read(), "0001_03.png", fx_provider).guarded


def _write_root(tmp, provider, pal_crc=None, edge="none"):
    w = pack.PackWriter(str(tmp), provider, 6, route="test", edge=edge, pal_crc=pal_crc)
    for f in range(4):
        src = provider.flat(1, f)
        assert w.add(1, f, rules.nn_upscale(src, 6), {"note": "x"})
    w.add(2, 0, rules.nn_upscale(provider.flat(2, 0), 6), {})
    return w.finish()


def test_root_ok_and_r1(tmp_path, fx_provider):
    _write_root(tmp_path, fx_provider)
    rep = check.check_root(str(tmp_path), fx_provider)
    assert rep.ok() and rep.loaded == 5 and rep.unguarded == 0 and rep.rejected == 0
    txt = (tmp_path / "pack.txt").read_text()
    meta = check.read_pack_txt(str(tmp_path / "pack.txt"))
    meta_crc = check.parse_crc(meta["palette_crc32"])
    (tmp_path / "pack.txt").write_text(txt.replace(f"{meta_crc:08x}", f"{meta_crc ^ 1:08x}"))
    rep = check.check_root(str(tmp_path), fx_provider)
    assert rep.root_disabled and any(f.rule == "R1" and f.severity == "root-disabled" for f in rep.findings)
    assert not rep.ok()


def test_group_strictness_g2_and_off(tmp_path, fx_provider):
    _write_root(tmp_path, fx_provider)
    gdir = tmp_path / "x6" / "flats" / "0001"
    shutil.copy(os.path.join(RULES, "p0_ff", "0001_09.png"), gdir / "0001_09.png")
    rep = check.check_root(str(tmp_path), fx_provider)
    assert rep.groups_skipped == 1 and rep.loaded == 1          # only 0002_00 survives
    assert any(f.rule == "G2" and f.severity == "group-skipped" for f in rep.findings)
    os.rename(gdir, tmp_path / "x6" / "flats" / "0001.off")
    rep = check.check_root(str(tmp_path), fx_provider)
    assert rep.groups_skipped == 0 and rep.loaded == 1 and rep.ok()
    # a loose bad file only rejects itself
    shutil.copy(os.path.join(RULES, "p0_ff", "0001_09.png"), tmp_path / "x6" / "flats" / "0001_09.png")
    rep = check.check_root(str(tmp_path), fx_provider)
    assert rep.rejected == 1 and rep.groups_skipped == 0 and rep.loaded == 1
    # ignored entries: _work dirs and non-PNG files
    os.makedirs(tmp_path / "x6" / "flats" / "_work")
    shutil.copy(os.path.join(RULES, "p0_ff", "0001_09.png"), tmp_path / "x6" / "flats" / "_work" / "0001_09.png")
    assert check.check_root(str(tmp_path), fx_provider).rejected == 1


def test_restamp(tmp_path, fx_provider):
    d = tmp_path / "x6" / "flats"
    d.mkdir(parents=True)
    shutil.copy(os.path.join(RULES, "ok_unguarded", "0001_03.png"), d / "0001_03.png")
    rep = check.check_root(str(tmp_path), fx_provider, restamp=True)
    assert rep.unguarded == 1
    rep = check.check_root(str(tmp_path), fx_provider)
    assert rep.unguarded == 0 and rep.loaded == 1


def test_bundle_b0(tmp_path, fx_provider):
    w = pack.PackWriter(str(tmp_path), fx_provider, 6, route="test")
    for f in range(3):
        w.add(1, f, rules.nn_upscale(fx_provider.flat(1, f), 6), None, loose=False, bundle=True)
    w.finish()
    bp = tmp_path / "x6" / "flats.bundle"
    entries, hdr = pack.read_bundle(str(bp))
    assert hdr["count"] == 3 and entries[0]["guard"] == crc32(fx_provider.flat(1, 0).tobytes())
    rep = check.check_root(str(tmp_path), fx_provider)
    assert rep.loaded == 3 and rep.ok()
    data = bp.read_bytes()
    bp.write_bytes(data[:-1])
    rep = check.check_root(str(tmp_path), fx_provider)
    assert any(f.rule == "B0" for f in rep.findings) and rep.loaded == 0
    bp.write_bytes(b"XXXX" + data[4:])
    with pytest.raises(pack.BundleError):
        pack.read_bundle(str(bp))


def test_edge_nn3_offline_rule(tmp_path, fx_provider):
    _write_root(tmp_path, fx_provider, edge="nn3")
    rep = check.check_root(str(tmp_path), fx_provider)
    assert rep.ok(strict=True)
    # break the band of one tile: E1 (offline) appears
    from u7hires import pngio
    p = tmp_path / "x6" / "flats" / "0001" / "0001_00.png"
    t, _ = pngio.read_indexed(str(p))
    t = t.copy()
    t[0, 20] = t[24, 24] if t[24, 24] != t[0, 20] else (int(t[0, 20]) % 0xDF) + 1
    p.write_bytes(pngio.encode_indexed(t, fx_provider.pal8))
    rep = check.check_root(str(tmp_path), fx_provider)
    assert any(f.rule == "E1" and f.severity == "offline" for f in rep.findings) and not rep.ok(strict=True)


def test_check_tile_array(fx_provider):
    good = rules.nn_upscale(fx_provider.flat(1, 0), 6)
    assert check.check_tile_array(good, 1, 0, fx_provider) == []
    bad = good.copy()
    bad[0, 0] = 0xFF
    assert [f.rule for f in check.check_tile_array(bad, 1, 0, fx_provider)] == ["P0"]
    assert check.check_tile_array(good, 7, 0, fx_provider)[0].rule == "N1"
    assert check.check_tile_array(good[:40], 1, 0, fx_provider)[0].rule == "F3"
    assert np.array_equal(pack.apply_edge_nn3(good, fx_provider.flat(1, 0)), good)
