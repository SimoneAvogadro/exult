"""Route 2 on a machine with transient faults: redundant_call, worker fault injection and repair,
representative key subsets (families.select_keys), and a GPU subset run (CUDA venv only)."""

import importlib.util
import os
from dataclasses import asdict

import numpy as np
import pytest

from u7hires import families, route2
from u7hires.util import key_name, redundant_call


def test_redundant_call_retries_and_votes():
    calls = iter([RuntimeError("flip"), 5])

    def f():
        v = next(calls)
        if isinstance(v, Exception):
            raise v
        return v

    r, st = redundant_call(f, 1)
    assert r == 5 and st["attempts"] == 2 and st["errors"] == 1 and st["mismatches"] == 0
    seq = iter([b"A", b"B", b"A"])
    r, st = redundant_call(lambda: next(seq), 2, digest=lambda x: x)
    assert r == b"A" and st["attempts"] == 3 and st["mismatches"] == 1 and st["errors"] == 0
    cnt = iter(range(100))
    with pytest.raises(RuntimeError):
        redundant_call(lambda: str(next(cnt)).encode(), 2, digest=lambda x: x)   # never two equal results
    with pytest.raises(RuntimeError):
        redundant_call(lambda: 1 / 0, 1)                                          # a real bug still fails


def _xbrz_or_skip():
    from u7hires.scalers import xbrz_available
    if not xbrz_available():
        pytest.skip("libxbrz19.so not built")


def _window(world):
    from u7hires.ctx import selfwrap_window
    win = selfwrap_window(world, 2, 4, 8)                       # water-like flat with glints, 24x24 window
    sr = np.repeat(np.repeat(world.pal8[win.idx].astype(np.float32), 4, 0), 4, 1)
    return win, sr


def test_post_job_recovers_from_exception_and_silent_flip(world, monkeypatch):
    _xbrz_or_skip()
    win, sr = _window(world)
    route2._init(world.pal8, world.pal6, asdict(route2.R2Params(redundancy=2)))
    ref, st = route2._post_job((win, sr, 4))
    assert (st["attempts"], st["errors"], st["mismatches"], st["rebuilds"]) == (2, 0, 0, 0)
    real = route2.post_window
    faults = iter(["raise", "flip"])

    def flaky(idx, sr_, ms, k, p):
        final, rgb6, r3 = real(idx, sr_, ms, k, p)
        what = next(faults, None)
        if what == "raise":
            raise IndexError("index 1157 is out of bounds for axis 0 with size 256")
        if what == "flip":
            final = final.copy()
            final[50, 50] ^= 4                                  # inside the own cell (48..95)
        return final, rgb6, r3

    monkeypatch.setattr(route2, "post_window", flaky)
    out, st = route2._post_job((win, sr, 4))
    assert st["errors"] == 1 and st["mismatches"] == 1 and st["attempts"] == 4
    assert route2.instances_digest(out) == route2.instances_digest(ref)
    # redundancy 1: the exception is retried, the flip would go through (whole runs are voted instead)
    route2._init(world.pal8, world.pal6, asdict(route2.R2Params(redundancy=1)))
    faults = iter(["raise"])
    out1, st1 = route2._post_job((win, sr, 4))
    assert st1["attempts"] == 2 and st1["errors"] == 1
    assert route2.instances_digest(out1) == route2.instances_digest(ref)


def test_check_kernel_rebuilds_corrupted_tables(world):
    route2._init(world.pal8, world.pal6, asdict(route2.R2Params()))
    assert route2._check_kernel() == 0
    route2._K.qz.lab_u[7, 0] += 0.25                            # a flipped bit in a long-lived table
    assert route2._check_kernel() == 1
    assert route2._check_kernel() == 0


def test_select_keys_ranking_and_groups(world):
    fams = families.frame_families(world)
    use = world.flat_use
    fam = sorted(set(fams.values()), key=lambda f: (-sum(v == f for v in fams.values()), f))[0]
    spec = f"{fam}:5,roads=1+2:4"
    keys, groups = families.select_keys(world, spec)
    rank = lambda pool: sorted(pool, key=lambda k: (-use.get(k, 0), k))  # noqa: E731
    pool1 = rank(k for k, v in fams.items() if v == fam)[:5]
    pool2 = rank(k for k in world.flat_keys if k[0] in (1, 2) and k not in set(pool1))[:4]
    assert groups[fam] == [key_name(*k) for k in pool1]
    assert groups["roads"] == [key_name(*k) for k in pool2]
    assert keys == sorted(set(pool1) | set(pool2)) and len(keys) == len(pool1) + len(pool2)
    assert families.select_keys(world, spec) == (keys, groups)
    for bad in ("water", "water:x", ":3"):
        with pytest.raises(ValueError):
            families.select_keys(world, bad)


def test_known_models_are_complete():
    for name, info in route2.KNOWN_MODELS.items():
        assert len(info["sha256"]) == 64 and info["scale"] in (4, 8) and info["file"].endswith(".pth")
        assert route2.model_path_for(route2.R2Params(model=name))[0].endswith(info["file"])
    assert route2.to_6x(np.zeros((80, 64, 3), np.float32), 8, 10, 8).shape == (60, 48, 3)   # box 8->6


class _FakeSR:
    """CPU stand-in for route2.SRModel (NN 4x of the window RGB), so that run()'s main-process path
    is tested without torch."""
    scale, device, fp16 = 4, "cpu", False

    def __init__(self, path, fp16=True):
        pass

    def __call__(self, batch_rgb):
        return np.repeat(np.repeat(np.asarray(batch_rgb, np.float32), 4, 1), 4, 2)

    def vram(self):
        return {"device": self.device}


@pytest.mark.parametrize("consensus", ["mode", "medoid"])
def test_run_main_process_retries(tmp_path, world, monkeypatch, consensus):
    """A transient exception in a main-process step (GPU batch, consensus add, consensus result,
    save) is recomputed: the output equals an undisturbed run, the retries are counted in
    faults.main_retries, and a persistent error still ends the run."""
    _xbrz_or_skip()
    from u7hires import pack
    from u7hires.consensus import MedoidConsensus, ModeConsensus
    left = {"model": 0, "add": 0, "result": 0, "save": 0}
    raised = []

    def maybe_raise(what):
        if left[what]:
            left[what] -= 1
            raised.append(what)
            raise IndexError("index 1157 is out of bounds for axis 0 with size 256")

    class Flaky(_FakeSR):
        def __call__(self, batch_rgb):
            maybe_raise("model")
            return super().__call__(batch_rgb)

    cls = ModeConsensus if consensus == "mode" else MedoidConsensus
    orig_add, orig_res, orig_save = cls.add, cls.result, route2.save_candidates

    def add(self, *a, **k):
        maybe_raise("add")
        return orig_add(self, *a, **k)

    def result(self, *a, **k):
        maybe_raise("result")
        return orig_res(self, *a, **k)

    def save(*a, **k):
        maybe_raise("save")
        return orig_save(*a, **k)

    monkeypatch.setattr(route2, "SRModel", Flaky)
    monkeypatch.setattr(cls, "add", add)
    monkeypatch.setattr(cls, "result", result)
    monkeypatch.setattr(route2, "save_candidates", save)
    model_file = tmp_path / "fake.pth"
    model_file.write_bytes(b"not a model")
    p = route2.R2Params(model="fake", model_path=str(model_file), batch=4, chunk=8, consensus=consensus)
    ref = route2.run(world, str(tmp_path / "ref"), p, workers=1, limit=3)
    assert ref["faults"]["main_retries"] == 0 and ref["torch"] is None and not raised
    left.update(model=1, add=1, result=1, save=1)
    meta = route2.run(world, str(tmp_path / "flaky"), p, workers=1, limit=3)
    assert sorted(raised) == ["add", "model", "result", "save"] and not any(left.values())
    assert meta["faults"]["main_retries"] == 4 and meta["faults"]["errors"] == 0
    a, ma = pack.load_candidates(str(tmp_path / "ref"))
    b, mb = pack.load_candidates(str(tmp_path / "flaky"))
    assert a and sorted(a) == sorted(b) and all(np.array_equal(a[k], b[k]) for k in a)
    assert ma["keys"] == mb["keys"]
    left["model"] = 99                                          # not transient: the run still fails
    with pytest.raises(RuntimeError):
        route2.run(world, str(tmp_path / "dead"), p, workers=1, limit=3)


def _gpu_ready():
    if importlib.util.find_spec("torch") is None or importlib.util.find_spec("spandrel") is None:
        return False
    return os.path.exists(os.path.join(route2.DEFAULT_MODEL_DIR, route2.KNOWN_MODELS["4x-NXbrz"]["file"]))


@pytest.mark.skipif(not _gpu_ready(), reason="torch/spandrel/4x-NXbrz not available (use the CUDA venv)")
def test_route2_gpu_subset_meta(tmp_path, world):
    from u7hires import pack, rules
    fams = families.frame_families(world)
    fam = sorted(set(fams.values()))[0]
    spec = f"{fam}:3"
    keys, _ = families.select_keys(world, spec)
    p = route2.R2Params(batch=4, chunk=8, subset=spec, redundancy=2)
    meta = route2.run(world, str(tmp_path / "s"), p, workers=2, limit=6)
    tiles, m = pack.load_candidates(str(tmp_path / "s"))
    assert tiles and set(tiles) <= set(keys)
    assert m["subset"]["keys"] == len(keys) and m["subset"]["windows"] == meta["windows"]["processed"]
    assert m["faults"]["attempts"] >= 2 * meta["windows"]["processed"]
    assert m["gpu"]["max_allocated_mb"] > 0 and m["gpu"]["seconds"] >= 0
    import torch
    assert m["torch"] == torch.__version__ and m["faults"]["main_retries"] == 0
    for k, t in tiles.items():
        assert rules.p4_violations(t, world.flat(*k)) == 0 and not (t == 0xFF).any()
