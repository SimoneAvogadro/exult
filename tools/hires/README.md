# Hi-res art tools (`tools/hires`)

Offline tooling for the 6x terrain-flat overrides of the Exult hi-res fork
(`docs-hires/design/DESIGN.md` §5 and §8). The package is `u7hires/`; the scripts here are thin
wrappers around each module's `main()`. EA-derived pixels (work data, packs) never go into the
repository: work data lives in `art_work/`, packs in `/home/simonea/ultima7_exult/packs/<name>`.

## Setup

```sh
VIRTUAL_ENV=/home/simonea/ultima7_exult/tools-venv uv pip install -r tools/hires/requirements.txt
tools/hires/third_party/build_xbrz.sh          # libxbrz19.so (xBRZ 1.9, SHA-checked, g++ 9 patch)
```

Input: the BG `STATIC` dir (`--static`, else `$U7_BG_STATIC`, else the ext4 cache
`art_work/static_cache`, else `/mnt/e/Games/RolePlayingGames/ultima7/static`).

## Pipeline

| Step | Command | Output |
|---|---|---|
| A1 context | `mkctx.py --apron 16 [--png]` | `art_work/ctx/a16` (terrain windows with real-neighbour aprons, macro sheets, self-wrap; instance maps; T1 keys) |
| A2 route 3 | `route3.py --variant xbrz\|hybrid\|mixed [--ctx DIR]` | `art_work/raw/r3-<variant>/tiles.npz` + `tiles.json` |
| B1 route 2 | `<venv_cu130>/bin/python route2.py [--consensus mode\|medoid]` | `art_work/raw/r2-4x-nxbrz-<consensus>/` |
| A3 pack | `mkpack.py CANDIDATES NAME [--edge nn3] [--bundle]` | `packs/NAME/{pack.txt,x6/flats/SSSS/SSSS_FF.png+.json,x6/.reload}` |
| check | `hirescheck.py packs/NAME [--strict] [--restamp] [--json F]` | engine rules N1 F1 F2 F3 F4 P0 P4 G1 G2 R1 B0, offline P2 E1 |
| QA | `hiresqa.py packs/NAME [--baseline OTHER]` | `art_work/qa/NAME/{report.json,report.md,per_tile.json,sheets/,previews/}` |
| publish | `publish.sh [--delete] [--dry-run] NAME` | mirror to `/mnt/e/Dati/Ultima7_Upscale/packs/NAME`, touch `.reload` on both sides |
| parity | `mkctx.py --parity-dump DUMP_DIR` | compares the engine's `--dump-art` `terrain/<t1>.png` with the Python fill port |

All steps are deterministic; workers are capped at 8 processes.

## Tests

```sh
tools-venv/bin/python -m pytest tools/hires/tests          # synthetic data; real-data smoke test skips without STATIC
tmp/factcheck/venv_cu130/bin/python -m pytest tools/hires/tests   # also runs the GPU route-2 smoke test
tools/hires/tests/make_rule_fixtures.py                    # regenerate tests/data/rules (synthetic)
```

`tests/data/rules/` holds crafted PNGs with their expected rule IDs (`expected.txt`) and the synthetic
game they are checked against; `tests/data/hash_vectors.txt` holds CRC32/FNV/Get_color8 vectors. Both
are meant to be shared with the engine's doctest suite.
