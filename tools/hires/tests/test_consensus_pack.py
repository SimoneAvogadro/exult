import hashlib
import os

import numpy as np
import pytest

from u7hires import check, pack, pngio, rules
from u7hires.consensus import MedoidConsensus, ModeConsensus


def test_mode_weighted_and_tiebreak():
    c = ModeConsensus()
    a = np.full((2, 2), 5, np.uint8)
    b = np.full((2, 2), 9, np.uint8)
    c.add((1, 0), a, 1.0)
    c.add((1, 0), b, 2.0)
    t, st = c.result((1, 0))
    assert (t == 9).all() and st["instances"] == 2 and st["unique_instances"] == 2
    assert abs(st["disagreement"] - 1 / 3) < 1e-9
    # equal weight: the parent's own index wins the tie, then the smaller index
    c = ModeConsensus()
    c.add((1, 1), a, 1.0)
    c.add((1, 1), b, 1.0)
    assert (c.result((1, 1), parent=np.array([[9]], np.uint8))[0] == 9).all()
    assert (c.result((1, 1))[0] == 5).all()


def test_mode_dedupe_and_tiers():
    c = ModeConsensus()
    a = np.arange(36, dtype=np.uint8).reshape(6, 6)
    for _ in range(5):
        c.add((2, 3), a, 1.0, tier=1)
    assert c.entries[(2, 3)].instances == 5 and len(c.entries[(2, 3)].tiles) == 1
    b = a.copy()
    b[0, 0] = 99
    c.add((2, 3), b, 1.0, tier=0)            # a better tier replaces the macro instances
    t, st = c.result((2, 3))
    assert st["tier"] == 0 and st["instances"] == 1 and t[0, 0] == 99
    c.add((2, 3), a, 100.0, tier=2)          # worse tier ignored
    assert c.result((2, 3))[1]["instances"] == 1


def test_mode_per_pixel():
    c = ModeConsensus()
    t1 = np.array([[1, 2], [3, 4]], np.uint8)
    t2 = np.array([[1, 7], [3, 8]], np.uint8)
    t3 = np.array([[6, 7], [3, 8]], np.uint8)
    for t in (t1, t2, t3):
        c.add((0, 0), t)
    assert np.array_equal(c.result((0, 0))[0], [[1, 7], [3, 8]])


def test_medoid():
    c = MedoidConsensus(cap=8)
    base = np.zeros((2, 2, 3), np.uint8)
    for v, w in ((100, 1.0), (110, 1.0), (120, 1.0), (250, 0.5)):
        c.add((0, 1), base + v, w)
    t, st = c.result((0, 1))
    assert (t == 110).all() and st["used_instances"] == 4


def test_medoid_prune_keeps_heaviest_and_is_atomic(monkeypatch):
    """Past 4*cap distinct tiles the 2*cap heaviest are kept (ties: earliest); an add that fails in
    the prune leaves the consensus unchanged, so route 2 may retry it."""
    from u7hires import consensus
    base = np.zeros((1, 1, 3), np.uint8)
    weights = [3.0, 1.0, 5.0, 2.0, 5.0, 0.5, 4.0, 1.5]           # 8 = 4*cap distinct tiles: no prune yet
    c = MedoidConsensus(cap=2)
    for v, w in enumerate(weights):
        c.add((0, 0), base + v, w)
    e = c.entries[(0, 0)]
    assert len(e.tiles) == 8 and e.seq == 8
    state = (dict(e.tiles), e.seq, e.instances, e.weight)

    def boom(*a, **k):
        raise MemoryError("flip")

    monkeypatch.setattr(consensus, "sorted", boom, raising=False)
    with pytest.raises(MemoryError):
        c.add((0, 0), base + 8, 4.5)
    assert (dict(e.tiles), e.seq, e.instances, e.weight) == state
    monkeypatch.delattr(consensus, "sorted")
    c.add((0, 0), base + 8, 4.5)                                 # 9th tile: prune to the 4 heaviest
    kept = sorted(int(r[0][0, 0, 0]) for r in e.tiles.values())
    assert kept == [2, 4, 6, 8] and e.seq == 9 and e.instances == 9 and e.weight == sum(weights) + 4.5


def test_pack_writer_layout_and_determinism(tmp_path, provider):
    def build(root):
        w = pack.PackWriter(str(root), provider, 6, route="unit", title="t")
        for f in range(3):
            assert w.add(1, f, rules.nn_upscale(provider.flat(1, f), 6), {"instances": 3, "disagreement": 0.0})
        bad = rules.nn_upscale(provider.flat(1, 3), 6).copy()
        bad[:4, :4] = 0xFF
        assert not w.add(1, 3, bad, {})               # refused: P0
        return w.finish()

    s = build(tmp_path / "a")
    assert s["written"] == 3 and s["refused"][0]["rules"] == ["P0"]
    x6 = tmp_path / "a" / "x6"
    assert (x6 / ".reload").exists() and (x6 / "flats" / "0001" / "0001_00.png").exists()
    assert (x6 / "flats" / "0001" / "0001_00.json").exists()
    assert not list(x6.rglob("*.tmp"))
    txt = (tmp_path / "a" / "pack.txt").read_text().splitlines()
    assert txt[:2] == ["game=BG", "scale=6"] and txt[2].startswith("palette_crc32=") and "edge=none" in txt
    t, info = pngio.read_indexed(str(x6 / "flats" / "0001" / "0001_01.png"))
    assert np.array_equal(t, rules.nn_upscale(provider.flat(1, 1), 6))
    assert info.texts["Exult-Origin"] == "unit" and len(info.texts["Exult-Src-CRC32"]) == 8
    assert np.array_equal(info.plte, provider.pal8)
    build(tmp_path / "b")

    def tree_hash(root):
        h = hashlib.sha256()
        for p in sorted(root.rglob("*")):
            if p.is_file() and p.name != ".reload":
                h.update(str(p.relative_to(root)).encode() + p.read_bytes())
        return h.hexdigest()

    assert tree_hash(tmp_path / "a") == tree_hash(tmp_path / "b")
    assert check.check_root(str(tmp_path / "a"), provider).ok()


def test_candidates_roundtrip_and_build_pack(tmp_path, provider):
    tiles = {(1, f): rules.nn_upscale(provider.flat(1, f), 6) for f in range(4)}
    pack.save_candidates(str(tmp_path / "cand"), tiles, {"route": "r-test", "params": {"a": 1},
                                                         "keys": {"0001_00": {"instances": 7}}})
    t2, meta = pack.load_candidates(str(tmp_path / "cand"))
    assert sorted(t2) == sorted(tiles) and all(np.array_equal(t2[k], tiles[k]) for k in tiles)
    s = pack.build_pack(str(tmp_path / "cand"), "p", provider, str(tmp_path / "packs"), edge="nn3", bundle=True)
    assert s["written"] == 4 and s["edge"] == "nn3"
    root = tmp_path / "packs" / "p"
    assert (root / "x6" / "flats.bundle").exists()
    import json
    side = json.loads((root / "x6" / "flats" / "0001" / "0001_00.json").read_text())
    assert side["instances"] == 7 and side["route"] == "r-test" and side["params"] == {"a": 1}
    rep = check.check_root(str(root), provider)
    assert rep.ok(strict=True) and rep.loaded == 4


def test_palette_crc_from_ref(tmp_path):
    p = tmp_path / "ref.txt"
    p.write_text("game=BG\npalette_crc32=0xC9C2C0E7\n")
    assert pack.palette_crc_from_ref(str(p)) == 0xC9C2C0E7


def test_publish_script(tmp_path):
    import subprocess
    src = tmp_path / "src" / "pk" / "x6" / "flats"
    src.mkdir(parents=True)
    (src / "a.png").write_bytes(b"x")
    (tmp_path / "src" / "pk" / "pack.txt").write_text("game=BG\n")
    env = dict(os.environ, U7_PACKS_SRC=str(tmp_path / "src"), U7_PACKS_DST=str(tmp_path / "dst"))
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    r = subprocess.run(["bash", os.path.join(here, "publish.sh"), "pk"], env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert (tmp_path / "dst" / "pk" / "x6" / "flats" / "a.png").read_bytes() == b"x"
    assert (tmp_path / "dst" / "pk" / "x6" / ".reload").exists() and (tmp_path / "src" / "pk" / "x6" / ".reload").exists()
