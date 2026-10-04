"""Majority voting over redundant runs (u7hires.vote): candidate sets, trees, pack verification."""

import json
import os

import numpy as np
import pytest

from u7hires import pack, rules, vote
from u7hires.pngio import encode_indexed


def _cands(provider, n=4):
    tiles = {(1, f): rules.nn_upscale(provider.flat(1, f), 6) for f in range(n)}
    keys = {f"0001_{f:02d}": {"instances": 3 + f, "disagreement": 0.01 * f} for f in range(n)}
    return tiles, {"route": "r-test", "params": {"a": 1}, "keys": keys, "seconds": 1.0}


def _save(path, tiles, meta):
    pack.save_candidates(str(path), tiles, meta)
    return str(path)


def test_vote_candidates_majority(tmp_path, provider):
    tiles, meta = _cands(provider)
    a = _save(tmp_path / "a", tiles, meta)
    t2 = dict(tiles)
    t2[(1, 1)] = tiles[(1, 1)].copy()
    t2[(1, 1)][5, 5] ^= 4                                  # a flipped bit in one run's tile
    b = _save(tmp_path / "b", t2, {**meta, "seconds": 2.0})
    m3 = json.loads(json.dumps(meta))
    m3["keys"]["0001_02"]["disagreement"] = 7.0            # a corrupted stat in another run
    c = _save(tmp_path / "c", tiles, m3)
    cmp = vote.compare_candidates(a, b)
    assert not cmp["identical"] and cmp["tiles_differing"] == ["0001_01"] and cmp["meta_equal"]
    assert vote.compare_candidates(a, _save(tmp_path / "a2", tiles, {**meta, "seconds": 9.0}))["identical"]
    with pytest.raises(vote.VoteError):
        vote.vote_candidates([a, b])                       # two runs: any difference is unresolved
    r = vote.vote_candidates([a, b, c], str(tmp_path / "out"))
    assert r["unanimous"] == 2 and {o["key"] for o in r["outvoted"]} == {"0001_01", "0001_02"}
    got, gm = pack.load_candidates(str(tmp_path / "out"))
    assert all(np.array_equal(got[k], tiles[k]) for k in tiles) and gm["keys"] == meta["keys"]
    assert gm["route"] == "r-test" and gm["vote"]["runs"] == 3


def test_vote_candidates_meta_and_missing_key(tmp_path, provider):
    tiles, meta = _cands(provider)
    a = _save(tmp_path / "a", tiles, meta)
    short = {k: v for k, v in tiles.items() if k != (1, 3)}
    b = _save(tmp_path / "b", short, meta)
    c = _save(tmp_path / "c", tiles, meta)
    r = vote.vote_candidates([a, b, c], str(tmp_path / "out"))
    assert r["outvoted"][0]["key"] == "0001_03" and (1, 3) in pack.load_candidates(str(tmp_path / "out"))[0]
    d = _save(tmp_path / "d", tiles, {**meta, "params": {"a": 2}})
    e = _save(tmp_path / "e", tiles, {**meta, "params": {"a": 3}})
    with pytest.raises(vote.VoteError):
        vote.vote_candidates([a, d, e])                    # run-level meta without a majority


def test_vote_trees(tmp_path):
    def tree(name, files):
        for rel, data in files.items():
            p = tmp_path / name / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        return str(tmp_path / name)

    good = {"pack.txt": b"game=BG\n", "x6/flats/0001/0001_00.png": b"A", "x6/flats/0001/0001_01.png": b"B"}
    r1 = tree("r1", good)
    r2 = tree("r2", {**good, "x6/flats/0001/0001_00.png": b"X"})
    r3 = tree("r3", {**good, "x6/flats/0001/0001_01.png": b"Y", "x6/extra.json": b"{}", "x6/.reload": b""})
    assert vote.compare_trees(r1, r2)["differing"] == ["x6/flats/0001/0001_00.png"]
    r = vote.vote_trees([r1, r2, r3], str(tmp_path / "out"))
    assert r["files"] == 3 and len(r["outvoted"]) == 3     # two flipped files + one extra file
    assert vote.compare_trees(r1, str(tmp_path / "out"))["identical"]
    r4 = tree("r4", {**good, "x6/flats/0001/0001_00.png": b"Z"})
    with pytest.raises(vote.VoteError):
        vote.vote_trees([r1, r2, r4], str(tmp_path / "out2"))   # A, X, Z: no majority
    with pytest.raises(vote.VoteError):
        vote.vote_trees([r1, r2, r3], str(tmp_path / "out"))    # output exists


def test_verify_pack(tmp_path, provider):
    tiles, meta = _cands(provider)
    cand = _save(tmp_path / "cand", tiles, meta)
    for edge in ("none", "nn3"):
        s = pack.build_pack(cand, f"p-{edge}", provider, str(tmp_path / "packs"), edge=edge)
        assert s["written"] == 4
        r = vote.verify_pack(cand, str(tmp_path / "packs" / f"p-{edge}"), provider)
        assert r["ok"] and r["checked"] == 4 and r["edge"] == edge
    root = tmp_path / "packs" / "p-none"
    p = root / "x6" / "flats" / "0001" / "0001_02.png"
    bad = tiles[(1, 2)].copy()
    bad[10, 10] ^= 4
    p.write_bytes(encode_indexed(bad, provider.pal8))
    os.remove(root / "x6" / "flats" / "0001" / "0001_03.png")
    r = vote.verify_pack(cand, str(root), provider)
    assert not r["ok"] and r["mismatched"] == ["0001_02"] and r["missing"] == ["0001_03"]


def test_cli_compare(tmp_path, provider, capsys):
    tiles, meta = _cands(provider)
    a = _save(tmp_path / "a", tiles, meta)
    b = _save(tmp_path / "b", tiles, meta)
    assert vote.main(["compare", a, b]) == 0
    assert vote.main(["cand", str(tmp_path / "o"), a]) == 1        # one run is not a vote
    assert "error" in capsys.readouterr().out
