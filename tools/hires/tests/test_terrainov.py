"""u7hires.terrainov on the synthetic test world (tests/data/hires/world, no game data): the T1 key of a
terrain equals the generator's (and so the engine's), the cut of a window plane, the offline rules and
the pack file."""

import os
import subprocess
import sys

import numpy as np
import pytest

from u7hires import S_ART
from u7hires.io import World
from u7hires.pngio import read_indexed
from u7hires.terrainov import LAYER, SIDE, check_terrain, cut_terrain, terrain_t1, write_pack_terrain

TOP = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
WORLD = os.path.join(TOP, "tests", "data", "hires", "world")


@pytest.fixture(scope="module")
def world(tmp_path_factory):
    static = tmp_path_factory.mktemp("world") / "static"
    subprocess.run([sys.executable, os.path.join(WORLD, "make_world.py"), "--assemble", str(static)], check=True)
    return World.load(str(static))


def world_keys():
    keys = {}
    with open(os.path.join(WORLD, "world.txt")) as f:
        for line in f:
            if line.startswith("terrain "):
                cols = line.split()
                keys[int(cols[1])] = int(cols[3].split("=")[1], 16)
    return keys


def test_t1_matches_the_generator(world):
    keys = world_keys()
    assert len(keys) == 6
    for tnum, key in keys.items():
        assert terrain_t1(world, tnum) == key


def test_cut_and_rules():
    rng = np.random.default_rng(3)
    idx = rng.integers(0x10, 0xC0, size=(160, 160), dtype=np.uint8)
    idx[40:60, 40:60] = 0xE2                                    # water in the 1x layer
    final = np.repeat(np.repeat(idx, S_ART, 0), S_ART, 1)
    tile, layer = cut_terrain(final, idx, 16, 16)
    assert tile.shape == (SIDE, SIDE) and layer.shape == (LAYER, LAYER)
    assert np.array_equal(layer, idx[16:144, 16:144])
    assert check_terrain(tile, layer)["ok"]
    bad = tile.copy()
    bad[0, 0] = 0xFF                                            # P0
    assert check_terrain(bad, layer)["p0"] == 1 and not check_terrain(bad, layer)["ok"]
    cyc = tile.copy()
    cyc[600:768, 600:768] = 0xE3                                # cycling far from any water: P4
    r = check_terrain(cyc, layer)
    assert r["p4"] > 0 and not r["ok"]
    with pytest.raises(ValueError):
        cut_terrain(final, idx, 40, 16)                         # the terrain is not inside the window


def test_pack_file(world, tmp_path):
    key = terrain_t1(world, 2)
    tile = np.full((SIDE, SIDE), 0x20, np.uint8)
    path = write_pack_terrain(str(tmp_path), tile, world, key, "test")
    assert os.path.basename(path) == f"{key:016x}.png"
    px, info = read_indexed(path)
    assert np.array_equal(px, tile)
    assert info.texts["Exult-Terrain-Key"] == f"{key:016x}" and info.texts["Exult-Origin"] == "test"
    assert "palette_crc32=" in (tmp_path / "pack.txt").read_text()
