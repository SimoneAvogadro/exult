"""Redundant runs and majority voting: a guard against transient hardware errors.

The production machine (WSL2, Ryzen 7 9700X) showed transient single-bit flips under load,
always at the same bit (bit 26 of a 64-bit word: an int16 palette index 133 read as 1157, a uint32
word with xor 0x04000000, an int64 bincount index off by 2**26). They surface as an ``IndexError``
in ``quant.local_snap`` or a ``ValueError`` in the consensus, as a changed SHA-256 of a constant
array, or not at all (a silently different value). The pipeline is deterministic, so every step
that produces art is run at least twice, independently, and only results that a strict majority of
the runs agree on are kept:

* ``vote_candidates``: per-key majority over N candidate sets (route outputs ``tiles.npz`` +
  ``tiles.json``). A key's value is its tile together with its per-key stats; the run-level meta
  (everything except ``VOLATILE_META``: keys, seconds, retries, vote) needs a majority too.
* ``tree_digests`` / ``vote_trees``: per-file majority over N copies of a directory tree (a pack,
  a QA output). A file missing from one copy counts as a distinct value.
* ``verify_pack``: decode every flat PNG of a pack and compare it with the candidate tiles it was
  built from (edge contract applied, refused keys excluded), so the files on disk are checked
  against data that was computed and stored independently.

Every writer re-reads what it wrote and compares it with the in-memory majority.

CLI (``tools/hires/vote.py``)::

    vote.py cand OUT DIR1 DIR2 [DIR3 ...]     per-key majority -> OUT (exit 1 if any key has none)
    vote.py tree OUT ROOT1 ROOT2 [ROOT3 ...]  per-file majority copied to OUT (exit 1 if any file has none)
    vote.py compare A B                       two candidate dirs or two trees (exit 1 if they differ)
    vote.py verify CANDIDATES PACK_ROOT       pack PNGs == candidate tiles (exit 1 on any mismatch)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
from collections import Counter

import numpy as np

from .util import get_logger, key_name

IGNORE_NAMES = (".reload",)
IGNORE_SUFFIXES = (".tmp",)
VOLATILE_META = ("keys", "seconds", "retries", "vote")   # run-level fields that may differ between runs


class VoteError(RuntimeError):
    pass


def _canon(o) -> str:
    return json.dumps(o, sort_keys=True, separators=(",", ":"))


def _majority(values: list) -> tuple[object, int]:
    """(value, count) of the strict majority among ``values`` (hashable), or (None, 0)."""
    c = Counter(values)
    v, n = c.most_common(1)[0]
    return (v, n) if n * 2 > len(values) else (None, 0)


# ---------------------------------------------------------------------------- candidate sets
def _load_run(path: str):
    with np.load(os.path.join(path, "tiles.npz")) as z:
        karr, tarr = np.array(z["keys"]), np.array(z["tiles"])
    with open(os.path.join(path, "tiles.json")) as f:
        meta = json.load(f)
    keys = [tuple(int(v) for v in k) for k in karr]
    return keys, tarr, meta


def _run_values(keys, tarr, meta) -> tuple[dict, str]:
    per_key = meta.get("keys", {})
    vals = {}
    for i, k in enumerate(keys):
        t = np.ascontiguousarray(tarr[i])
        h = hashlib.sha256(t.tobytes() + str(t.shape).encode()).hexdigest()
        vals[k] = (h, _canon(per_key.get(key_name(*k))))
    glob = _canon({k: v for k, v in meta.items() if k not in VOLATILE_META})
    return vals, glob


def compare_candidates(a: str, b: str) -> dict:
    """Per-key comparison of two candidate dirs (tiles and per-key stats; VOLATILE_META ignored)."""
    ka, ta, ma = _load_run(a)
    kb, tb, mb = _load_run(b)
    va, ga = _run_values(ka, ta, ma)
    vb, gb = _run_values(kb, tb, mb)
    keys = sorted(set(va) | set(vb))
    tile_diff = [key_name(*k) for k in keys if va.get(k, (None,))[0] != vb.get(k, (None,))[0]]
    stat_diff = [key_name(*k) for k in keys if va.get(k, (None, None))[1] != vb.get(k, (None, None))[1]]
    return {"keys_a": len(va), "keys_b": len(vb), "tiles_differing": tile_diff, "stats_differing": stat_diff,
            "meta_equal": ga == gb, "identical": not tile_diff and not stat_diff and ga == gb}


def vote_candidates(dirs: list[str], out: str | None = None, log=None) -> dict:
    """Strict per-key majority over the candidate sets in ``dirs`` (N >= 2); writes it to ``out``.

    Raises VoteError when the run-level meta or any key has no strict majority (with two runs that
    means any difference at all): run another independent pass and vote again."""
    log = log or get_logger()
    if len(dirs) < 2:
        raise VoteError("need at least two runs")
    runs = [_load_run(d) for d in dirs]
    vals, globs = zip(*(_run_values(*r) for r in runs))
    g, _ = _majority(list(globs))
    if g is None:
        raise VoteError("run-level meta (params, inputs, library hashes) has no majority")
    keys = sorted(set().union(*(set(v) for v in vals)))
    pos = [{k: i for i, k in enumerate(r[0])} for r in runs]
    tiles, per_key = {}, {}
    unanimous, outvoted, unresolved = 0, [], []
    for k in keys:
        col = [v.get(k) for v in vals]
        win, n = _majority(col)
        if n == 0:
            unresolved.append(key_name(*k))
            continue
        if n == len(col):
            unanimous += 1
        else:
            outvoted.append({"key": key_name(*k), "agree": n, "runs": len(col),
                             "dissent": [i for i, c in enumerate(col) if c != win]})
        if win is None:                                     # the majority of runs has no such key
            continue
        r = next(i for i, c in enumerate(col) if c == win)
        tiles[k] = np.array(runs[r][1][pos[r][k]])
        st = runs[r][2].get("keys", {}).get(key_name(*k))
        if st is not None:
            per_key[key_name(*k)] = st
    res = {"runs": list(dirs), "keys": len(keys), "unanimous": unanimous, "outvoted": outvoted,
           "unresolved": unresolved}
    if unresolved:
        raise VoteError(f"{len(unresolved)} keys without a majority: {unresolved[:10]}")
    if out is not None:
        from .pack import load_candidates, save_candidates
        base = next(r[2] for r, gl in zip(runs, globs) if gl == g)
        meta = {**{k: v for k, v in base.items() if k not in VOLATILE_META}, "keys": per_key,
                "seconds": base.get("seconds"),
                "vote": {"runs": len(dirs), "unanimous": unanimous, "outvoted": outvoted}}
        save_candidates(out, tiles, meta)
        t2, m2 = load_candidates(out)                      # read back: catches write-path errors
        if sorted(t2) != sorted(tiles) or any(not np.array_equal(t2[k], tiles[k]) for k in tiles) \
                or _canon(m2.get("keys")) != _canon(per_key):
            raise VoteError(f"read-back of {out} differs from the voted set")
        log.info("vote: %d keys, %d unanimous, %d outvoted -> %s", len(keys), unanimous, len(outvoted), out)
    return res


# ---------------------------------------------------------------------------- directory trees
def tree_digests(root: str) -> dict[str, str]:
    """{relative path: sha256} of every file under ``root`` (``.reload`` and ``*.tmp`` ignored)."""
    out = {}
    for dp, dns, fns in os.walk(root):
        dns.sort()
        for fn in sorted(fns):
            if fn in IGNORE_NAMES or fn.endswith(IGNORE_SUFFIXES):
                continue
            p = os.path.join(dp, fn)
            with open(p, "rb") as f:
                out[os.path.relpath(p, root)] = hashlib.sha256(f.read()).hexdigest()
    return out


def tree_hash(digests: dict[str, str]) -> str:
    return hashlib.sha256("".join(f"{k} {v}\n" for k, v in sorted(digests.items())).encode()).hexdigest()


def compare_trees(a: str, b: str) -> dict:
    da, db = tree_digests(a), tree_digests(b)
    diff = sorted(k for k in set(da) | set(db) if da.get(k) != db.get(k))
    return {"files_a": len(da), "files_b": len(db), "differing": diff, "tree_hash_a": tree_hash(da),
            "tree_hash_b": tree_hash(db), "identical": not diff}


def vote_trees(roots: list[str], out: str, log=None) -> dict:
    """Copy the per-file strict majority of the trees in ``roots`` to ``out`` (must not exist)."""
    log = log or get_logger()
    if len(roots) < 2:
        raise VoteError("need at least two trees")
    if os.path.exists(out):
        raise VoteError(f"{out} exists")
    ds = [tree_digests(r) for r in roots]
    files = sorted(set().union(*(set(d) for d in ds)))
    plan, outvoted, unresolved = {}, [], []
    for rel in files:
        col = [d.get(rel) for d in ds]
        win, n = _majority(col)
        if n == 0:
            unresolved.append(rel)
            continue
        if n != len(col):
            outvoted.append(rel)
        if win is not None:                                 # majority says "absent": skip the file
            plan[rel] = (next(i for i, c in enumerate(col) if c == win), win)
    if unresolved:
        raise VoteError(f"{len(unresolved)} files without a majority: {unresolved[:10]}")
    tmp = out + ".tmp-vote"
    if os.path.exists(tmp):
        shutil.rmtree(tmp)
    for rel, (i, h) in plan.items():
        dst = os.path.join(tmp, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(roots[i], rel), dst)
    got = tree_digests(tmp)
    if got != {rel: h for rel, (i, h) in plan.items()}:
        raise VoteError("copied tree does not match the voted digests")
    os.replace(tmp, out)
    log.info("tree vote: %d files, %d outvoted -> %s", len(plan), len(outvoted), out)
    return {"files": len(plan), "outvoted": outvoted, "tree_hash": tree_hash(got)}


# ---------------------------------------------------------------------------- pack vs candidates
def verify_pack(candidates: str, pack_root: str, provider, scale: int = 6) -> dict:
    """Every candidate tile (edge contract applied) must be in the pack as an identical PNG, except
    the keys the pack writer refused; the pack must hold no other flat PNG."""
    from .check import read_pack_txt
    from .pack import apply_edge_nn3, load_candidates
    from .pngio import read_indexed
    tiles, _ = load_candidates(candidates)
    meta = read_pack_txt(os.path.join(pack_root, "pack.txt"))
    edge = meta.get("edge", "none")
    flats = os.path.join(pack_root, f"x{scale}", "flats")
    refused = set()
    sp = os.path.join(flats, "_pack_summary.json")
    if os.path.exists(sp):
        with open(sp) as f:
            refused = {tuple(r["key"]) for r in json.load(f).get("refused", [])}
    on_disk = {}
    for dp, _, fns in os.walk(flats):
        for fn in fns:
            if fn.endswith(".png"):
                on_disk[fn[:-4]] = os.path.join(dp, fn)
    checked, missing, mismatched = 0, [], []
    for k in sorted(tiles):
        n = key_name(*k)
        if k in refused:
            if n in on_disk:
                mismatched.append(n + " (refused but present)")
            continue
        p = on_disk.get(n)
        if p is None:
            missing.append(n)
            continue
        want = tiles[k]
        if edge == "nn3":
            want = apply_edge_nn3(want, provider.flat(*k), scale)
        got, _ = read_indexed(p)
        checked += 1
        if got.shape != want.shape or not np.array_equal(got, want):
            mismatched.append(n)
    extra = sorted(set(on_disk) - {key_name(*k) for k in tiles})
    return {"checked": checked, "refused": len(refused), "missing": missing, "mismatched": mismatched,
            "extra": extra, "edge": edge, "ok": not missing and not mismatched and not extra}


# ---------------------------------------------------------------------------- CLI
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="vote.py", description="Majority voting over redundant runs.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("cand", help="per-key majority of candidate sets")
    c.add_argument("out")
    c.add_argument("dirs", nargs="+")
    t = sub.add_parser("tree", help="per-file majority of directory trees")
    t.add_argument("out")
    t.add_argument("roots", nargs="+")
    m = sub.add_parser("compare", help="compare two candidate dirs or two trees")
    m.add_argument("a")
    m.add_argument("b")
    v = sub.add_parser("verify", help="pack PNGs == candidate tiles")
    v.add_argument("candidates")
    v.add_argument("pack_root")
    v.add_argument("--static", default=None)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "cand":
            r = vote_candidates(a.dirs, a.out)
            ok = True
        elif a.cmd == "tree":
            r = vote_trees(a.roots, a.out)
            ok = True
        elif a.cmd == "compare":
            cand = all(os.path.exists(os.path.join(p, "tiles.npz")) for p in (a.a, a.b))
            r = compare_candidates(a.a, a.b) if cand else compare_trees(a.a, a.b)
            ok = r["identical"]
        else:
            from .check import Provider
            from .io import World
            r = verify_pack(a.candidates, a.pack_root, Provider.from_world(World.load(a.static)))
            ok = r["ok"]
    except VoteError as e:
        print(json.dumps({"error": str(e)}))
        return 1
    print(json.dumps(r, indent=1, default=str))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
