# Synthetic test world

A tiny Exult game with **original, procedurally generated art** (`make_world.py`), plus a 6x
hi-res pack for it. It holds no EA data, so it may be committed and redistributed. It lets the
hi-res engine tests run anywhere, without the Ultima VII files: `make check-world`
(`tests/world/world_tests.sh`).

## What is in it

| Path | Content |
|---|---|
| `static/` | The game files of the DEVEL game `hirestest`. `make_world.py --assemble DIR` lays them over Exult Studio's free template (`data/estudio/new`: fonts, faces, gumps, text) to give a complete static directory |
| `static/palettes.flx` | 11 palettes; water uses the cycling range E0-E7, so it shimmers |
| `static/shapes.vga` | Flats 1 grass, 2 dirt, 3 sand, 4 water, 5 road (4 frames each), 6 cobble (2 frames); RLE objects 150 rock and 151 bush |
| `static/u7chunks`, `static/u7map` | 6 terrains (meadow, path, pond, beach, plaza, road), tiled 3 x 2 over the whole 192 x 192 chunk map |
| `base/SSSS_FF.png` | The 1x flats as PNG (raw indices, palette 0): the "original" tiles |
| `pack/` | A hi-res pack root (`hires_path`), DESIGN.md section 5.1 |
| `pack/x6/flats/` | 6x tiles of grass, dirt and water (loose), road (the strict group `road/`), cobble (the disabled group `0006.off/`). Sand has no file |
| `pack/x6/terrain/<t1>.png` | A whole-terrain override of the pond (768 x 768): a round shore that per-tile art cannot draw |
| `world.txt` | Terrains and their T1 keys, shapes, counts (read by the tests) |

How the art relates: every tile is drawn at 6x first, and each 1x pixel is the palette colour
nearest to the mean of its 6 x 6 block. So the pack is real hi-res art, not NN, and the 1x game
is what an engine without the pack shows. With the pack, the engine paints:

* the pond chunks from the terrain override (`TERRAIN`);
* grass, dirt, water and road from the tile overrides (`TILE`);
* sand (no file) and cobble (disabled group) as NN of the 1x tiles.

## Playing it

```bash
python3 tests/data/hires/world/make_world.py --assemble /home/you/hirestest/static
```

Then in `exult.cfg` (absolute paths):

```xml
<config><disk><game><hirestest>
  <path>/home/you/hirestest</path>
  <hires_path>/path/to/exult/tests/data/hires/world/pack</hires_path>
  <editing>yes</editing>
</hirestest></game></disk></config>
```

and run `exult --game hirestest --nomenu`. The world has no NPCs and no usecode: `editing=yes`
(map-editing mode) lets Exult start without an `npc.dat` and puts the avatar in the middle of
the map, on a meadow. Checked headless: the game runs, the default `render_scale=art` gives the
6x world texture, and the store loads the pack (16 tiles, 1 terrain, no reject). With `dev=yes`
under `config/video/hires`, Ctrl-Alt-O toggles the pack and Ctrl-Alt-I names what paints the tile
under the mouse (cheats on).

## Changing it

Edit `make_world.py`, run it (it rewrites `static/`, `base/`, `pack/` and `world.txt`), then run
`WORLD_RECORD=1 tests/world/world_tests.sh <build>` to record the sample render's new digest
and commit everything together. `make_world.py --check` (run by the tests) fails when the
committed files and the generator disagree. Python 3.8+, standard library only.
