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
| compare | `hirescompare.py --pack LABEL=PATH[@BASE] ... [--out DIR]` | `art_work/qa/compare/{sheets/,views/,previews/,metrics.md,metrics.json}`: per-family sheets (1x NN \| pack 1 \| ...), sparkle sheets, 1:1 crops of the QA preview views, per-family metrics; `@BASE` draws a subset pack over a full one |
| publish | `publish.sh [--delete] [--dry-run] NAME` | mirror to `/mnt/e/Dati/Ultima7_Upscale/packs/NAME`, touch `.reload` on both sides |
| parity | `mkctx.py --parity-dump DUMP_DIR` | compares the engine's `--dump-art` `terrain/<t1>.png` with the Python fill port |
| vote | `vote.py cand OUT RUN1 RUN2 [RUN3]`, `vote.py tree OUT A B [C]`, `vote.py compare A B`, `vote.py verify CANDIDATES PACK` | per-key / per-file strict majority of redundant runs; pack PNGs checked against the candidates |

All steps are deterministic; workers are capped at 8 processes.

## Production runs on this machine (redundancy)

The WSL2 host (Ryzen 7 9700X) shows transient bit flips under load, always at the same bit (bit 26
of a 64-bit word: an int16 index 133 read as 1157, a uint32 xor `0x04000000`, an int64 off by 2^26).
Most of them raise an `IndexError` in `quant.local_snap` or a `ValueError` in the consensus, which
`route3.py` recomputes (`--retries`, default 2, counted in `tiles.json` `retries`); a flip in a uint8
plane could change a value silently. Production art is therefore never taken from a single pass:

1. build the context twice and `vote.py compare` the trees;
2. run the route at least twice (one pass with `--ctx`, one building its own context) and
   `vote.py compare` the candidate sets; if they differ, run a third pass and `vote.py cand`
   (route 2: `--redundancy N` repeats each window's CPU stage until N results agree);
3. build the pack twice from the voted candidates (`mkpack.py --packs DIR1|DIR2`), `vote.py compare`
   the trees (or `vote.py tree` over three), move the agreed copy into `packs/`, then
   `vote.py verify CANDIDATES packs/NAME`;
4. run `hirescheck.py` and `hiresqa.py` twice and compare their JSON output (`seconds` aside);
5. `hirescompare.py` re-derives every per-key metric and fails when one differs from the pack's QA
   `per_tile.json`; its output has no timestamps, so two runs can be checked with `vote.py compare`.

Load limits (the host crashed twice with bugcheck 0x1A MEMORY_MANAGEMENT): at most 4 CPU worker
processes in total (`--workers 4`, `OMP_NUM_THREADS=1` for the single-process tools), and never
route 2 (GPU) and route 3 at the same time. Measured on 2026-10-04: route 3 with 8 workers next to
route 2 with 8 workers raised 10 transient errors in its first 2,005 of 2,787 windows (route 2: 8 in
360 windows) before the host crashed; four route-3 passes with 4 workers alone raised none (xbrz
154-161 s, mixed 214-224 s per pass; the passes of a variant were identical).

Route 2 alone with 4 workers (RTX 5070 Ti, fp16; `art_work/r2prod`): a full 4x-NXbrz pass over the
2,787 windows takes 350-354 s (GPU 61 s; torch peak 834 MB allocated / 1,430 MB reserved; nvidia-smi
1.7 GB above the idle desktop); two passes, no faults, identical. The 8x-Arzenal-v1-1 look subset
(`--subset water:50,shore:50,grass:50,dirt:50,roads=21+24:50,floor:50 --batch 4`: 300 keys in 1,745
windows) takes 296-301 s (GPU 73-76 s; 1,633 MB allocated / 2,354 MB reserved; +2.6 GB). Two of its
three passes each had one key whose per-key stats (not the tile) differed silently from the other two,
with no fault reported, and the third pass also retried 2 `IndexError`s: a run that reports 0 faults is
still no proof, so always compare two passes. `vote.py` treats the per-run measurements of route 2
(`faults`, `gpu`, `gpu_seconds`) like `seconds` and keeps them per run under `vote.run_stats`.

## Tests

```sh
tools-venv/bin/python -m pytest tools/hires/tests          # synthetic data; real-data smoke test skips without STATIC
tmp/factcheck/venv_cu130/bin/python -m pytest tools/hires/tests   # also runs the GPU route-2 smoke test
tools/hires/tests/make_rule_fixtures.py                    # regenerate tests/data/rules (synthetic)
```

`tests/data/rules/` holds crafted PNGs with their expected rule IDs (`expected.txt`) and the synthetic
game they are checked against; `tests/data/hash_vectors.txt` holds CRC32/FNV/Get_color8 vectors. Both
are meant to be shared with the engine's doctest suite.
