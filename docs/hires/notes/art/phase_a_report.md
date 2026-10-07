# Phase A art report: 6x BG terrain flats

2026-10-05. Scope: the four packs built on 2026-10-04 by the route-3 and route-2 production
runs, compared per material family. The default pack is published as `packs/bg`.

**Result.** `bg-r3h` is the only full pack that passes every QA gate, and it looks faithful. It is now
the active pack (`packs/bg`, mirrored to `E:`). By family, it is the best choice for water, stone,
roads, floors and the material transitions. Two caveats:

* Inside grass, dirt and sand, `bg-r3h` stays at the 1x pixel look (only 0.1-0.6 % of their 6x pixels
  differ from a nearest-neighbour (NN) upscale). With their transitions, those families are 35 % of
  map use and 88 % of the visible land (everything except water and void).
* On marsh and shore, the xBRZ result is the weakest. There `bg-r2` (4x-NXbrz) looks better.

The 8x-Arzenal subset is not usable as a default: its B1 is 89.7 %, it shows seams at tile borders
and it invents detail.

Phase B should therefore first refine grass, dirt, sand and their transitions with diffusion, then
marsh and shore.

## 1. Method

**Packs compared** (all under `/home/simonea/ultima7_exult/packs`, every one built by the redundancy
recipe of `tools/hires/README.md`):

| label | pack | route | tiles | tree hash | how each tile is confirmed |
|---|---|---|---|---|---|
| r3 | `bg-r3` | route 3, xBRZ 1.9 + local snap + mode consensus | 3,885 | `c859339551940a1b` | 3 runs, 3/3 unanimous |
| r3h | `bg-r3h` | route 3 `--variant mixed`: material-boundary hybrid on the grass, dirt and sand families (2,625 frames), xBRZ elsewhere | 3,885 | `91f97d1f1f171596` | 3 runs, 3/3 unanimous |
| r2 | `bg-r2` | route 2, 4x-NXbrz (fp16) + Lanczos 1.5x + colour lock + local snap + consensus | 3,885 | `a571aabbc6758555` | 2 runs, identical |
| r2a | `bg-r2a-subset` | route 2 with 8x-Arzenal-v1-1 (box 8 to 6), 300 keys: the 50 most used water, shore, grass, dirt, road (shapes 21, 24) and floor frames | 300 | `6ec9d31824889dba` | 3 runs, 298 unanimous, 2 by majority |

**Integrity checks before the comparison** (this session, after the reboot):

* the four `static_cache` files match the originals on `E:` (SHA-256);
* `vote.py verify` passes for every pack against its voted candidates (3,885/3,885 and 300/300);
* all four tree hashes match the values recorded at production time.

**Comparison tool.** `tools/hires/hirescompare.py` (`u7hires/compare.py`, commit `731d5cb3c` on
`hires-art`, with 4 pytest cases on synthetic data; the suite runs 114 passed, 2 skipped).
The command was:

```sh
hirescompare.py --pack r3=packs/bg-r3 --pack r3h=packs/bg-r3h --pack r2=packs/bg-r2 \
                --pack r2a=packs/bg-r2a-subset@r2 --out art_work/qa/compare
```

* Per-family sheets: the 12 most used keys of each family, plus the BG roads group (shapes 21 and 24).
  Each cell shows `1x NN | r3 | r3h | r2 | r2a` at zoom 2, with the B1 value under every panel.
* Sparkle sheets: the same cells for families whose top keys use cycling indices, with static pixels
  dimmed and each cycle range drawn in a signal colour.
* A sheet of the most used keys that some full pack has below 90 % B1.
* View crops: for each of the 12 representative views of the QA previews (2 each of coast, town,
  roads, forest, swamp and dungeon), a 10x10-tile crop at 1:1 (480x480 px at 6x), one panel per pack.
  The crop is chosen for material transitions and away from the approximate RLE overlay.
* Full 6x view renders (1920x1200) per pack.
* Per-family metrics. The Arzenal subset is drawn over `bg-r2` in the views, because a per-family
  promotion would look like that in the engine. In the tile sheets it shows only its own tiles.

The tool recomputes every per-key metric and checks it against each pack's QA `per_tile.json`. All
11,955 keys were identical, so the per-tile QA numbers below (A2, A3, B1-B4) are confirmed by a second,
independent computation. The tool ran twice, single-process with `OMP_NUM_THREADS=1` (about 21 s and 320 MB each).
Both output trees were identical (93 files, tree hash `2174f1a9d7719eb4`). No GPU work ran in this step.

**Visual review.** I looked at these images myself:

* the water, sparkle, shore, grass, dirt, sand, marsh, dirt+grass, stone, roads, floor and flagged
  sheets;
* the coast, roads, forest, swamp, town and dungeon view crops;
* extra 1:1 and 2x crops of grass, curbs, marsh pools and coast cliffs. These are ad hoc and stay in
  `tmp/compare_run/zoom`, not in the deliverable.

The criteria were faithfulness, worms and blobs, seams, water sparkles, and roads and floors.

## 2. QA gates per pack

From `art_work/qa/<pack>/report.json`. Each QA run was done twice with identical results at
production time, and confirmed again by the tool's cross-check.

| gate | r3 (xBRZ) | r3h (hybrid) | r2 (4x-NXbrz) | r2a (Arzenal subset) |
|---|---|---|---|---|
| **overall** | FAIL (C1 only) | **PASS** | FAIL (B1 floor only) | FAIL (B1) |
| tiles / flats left at NN | 3,885 / 0 | 3,885 / 0 | 3,885 / 0 | 300 / 3,585 |
| hirescheck: rejected / warnings / offline P2 | 0 / 0 / 141 | 0 / 0 / 281 | 0 / 0 / 60 | 0 / 0 / 27 |
| A1 format / A2 palette px / A3 P4 px | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| B1 aggregate (usage-weighted) | 98.65 % (99.00 %) | 99.61 % (99.71 %) | 97.53 % (99.03 %) | 89.74 % (96.55 %) |
| B1 min; tiles < 97 % / < 85 % | 85.94 %; 901 / 0 | 85.94 %; 195 / 0 | 67.19 %; 1,414 / **46** | 54.69 %; 242 / **74** |
| B2 aggregate; bottom 5 % at | 88.35 %; <= 68.97 % | 93.67 %; <= 73.02 % | 97.18 %; <= 87.50 % | 87.00 %; <= 50.00 % |
| B3 mean / p99 (gate p99 <= 0.10) | 0.0110 / 0.0893 | 0.0056 / 0.0913 | 0.0052 / 0.0455 | 0.0115 / 0.0924 |
| B4 aggregate; tiles < 95 % | 99.92 %; 5 | 99.85 %; 6 | 99.96 %; 5 | 99.55 %; 11 |
| C1 seam ratio (gate <= 1.2), max view | **1.213**, 1.530 | 1.104, 1.423 | 1.152, 1.276 | n/a (no region baseline) |
| C2 pairs; excess mean / p99 | 85,482; -0.0114 / 0.0197 | 85,482; -0.0043 / 0.0190 | 85,482; -0.0078 / 0.0167 | 10,776; -0.0087 / **0.0612** |
| D1 median; flagged > 2x median | 0.0146; 593 | 0.0031; 688 | 0.0157; 576 | 0.0514; 56 |

Notes:

* r2's 46 tiles below the 85 % floor are mostly transitions and stone: dirt+stone 15, stone 9,
  dirt+sand 7, grass+sand 6, grass 3, dirt+grass 3, shore 2, dirt 1. Seven of the dirt+stone and
  stone ones are road frames (shapes 21 and 24).
* r3h's lowest tile is `0065_17` (water, 85.9 %). Visually it is the NN tile with one rounded highlight.
* On the 300 subset keys, the means are r3h 99.4 %, r3 98.9 %, r2 97.4 % and r2a 89.7 %. Arzenal is
  worst in every group: floor 84.0 %, roads 83.0 %, shore 88.8 %.

## 3. Per family

Usage-weighted B1, tiles below 97 % / 85 %, and the share of 6x pixels that differ from the NN
upscale of the 1x art. That last share measures how much a route changes the art, not its quality.
Map use counts each flat's occurrences as an own tile on the BG map (9.27 M in all). Full tables are in
`art_work/qa/compare/metrics.md`.

| family | map use | B1 r3 / r3h / r2 / r2a | < 97 % / < 85 %: r3, r3h, r2 | px != NN: r3 / r3h / r2 |
|---|---|---|---|---|
| water | 54.4 % | 99.63 / 99.63 / 99.88 / 98.55 | 60/0, 60/0, 41/0 | 5.5 / 5.5 / 3.4 % |
| grass | 15.5 % | 98.70 / **99.95** / 97.54 / 92.89 | 410/0, 14/0, 354/3 | 16.2 / **0.6** / 12.0 % |
| dirt | 9.9 % | 98.39 / **99.79** / 98.65 / 91.51 | 258/0, 17/0, 262/1 | 13.0 / **0.5** / 8.5 % |
| void | 6.1 % | 100 / 100 / 100 / - | 0, 0, 0 | 0 / 0 / 0 % |
| sand | 5.5 % | **93.03** / 99.97 / 99.56 / - | 45/0, 0/0, 35/0 | 11.0 / **0.1** / 4.4 % |
| stone | 3.6 % | 99.18 / 99.18 / 96.15 / 84.41 | 21/0, 21/0, 98/9 | 14.2 / 14.2 / 13.2 % |
| roads (shapes 21, 24; part of stone) | 2.3 % | 99.22 / 99.22 / 95.36 / 84.41 | 5/0, 5/0, 44/7 | 14.4 / 14.4 / 13.9 % |
| dirt+grass | 3.2 % | 99.50 / 99.52 / 94.67 / - | 25/0, 11/0, 114/3 | 17.5 / 5.0 / 15.7 % |
| marsh | 0.74 % | 99.56 / 99.56 / 97.27 / - | 6/0, 6/0, 78/0 | 16.0 / 16.0 / 10.5 % |
| grass+sand | 0.42 % | 98.99 / 99.58 / 98.58 / - | 13/0, 8/0, 51/6 | 14.5 / 4.4 / 10.4 % |
| dirt+sand | 0.24 % | 99.88 / 99.91 / 93.19 / - | 5/0, 0/0, 55/7 | 15.3 / 1.3 / 14.1 % |
| shore | 0.19 % | 98.88 / 98.88 / 95.02 / 89.05 | 37/0, 37/0, 231/2 | 16.3 / 16.3 / 13.9 % |
| floor | 0.08 % | 98.06 / 98.06 / 99.15 / 92.94 | 19/0, 19/0, 25/0 | 11.1 / 11.1 / 6.8 % |
| grass+stone, dirt+stone, other | 0.09 % | 99.8-100 / same / 87.4-100 / 84.0 | 2/0, 2/0, 70/15 | r3 and r3h 0.7-16.7 %, r2 0.7-19.0 % |

r3h equals r3 exactly on every frame outside the hybrid families: water, shore, marsh, stone, floor,
void, other and the stone transitions. Only 3 of the 1,045 grass frames are identical in both.

## 4. Visual assessment

Paths are relative to `/home/simonea/ultima7_exult/art_work/qa/compare/` (copy on `E:` in
`E:\Dati\Ultima7_Upscale\art_compare\`).

**Faithfulness.**

* r3h is the most faithful. In grass, dirt and sand it is the original pixel art, and only the
  boundaries between materials are smoothed (`sheets/grass.png`, `sheets/dirt.png`, `sheets/sand.png`).
* r3 and r2 keep the layout of every tile. r3 reinterprets fine texture as long strokes. r2 rounds it
  into soft shapes.
* r2a (Arzenal) is not faithful:
  * it adds dark outlines and brush streaks that are not in the source: floors 0027_20/22/23 at
    69-77 % B1 in `sheets/floor.png`, and cobbles 0024_03 at 55 % in `sheets/roads.png`;
  * it changes the floor glyphs.

**Worms and blobs.**

* xBRZ turns the dithered 1x textures into "worms", networks of tubes. This is the dominant artefact
  of r3: grass, dirt, sand, marsh and the grey cobbles 0063_xx (`sheets/grass.png`, `sheets/marsh.png`,
  and the grass in `views/forest_051_069.png` and `views/coast_077_062.png`).
* At 1:1 on a 1920x1200 frame it reads as busy "spaghetti" grass.
* The hybrid removes the worms where it applies (grass, dirt, sand and their transitions). It leaves
  them in marsh and cobbles, which stay xBRZ.
* r2 replaces worms with softer blobs and short curved strokes. It is calmer and more natural on grass,
  dirt, sand, shore and marsh, and a little blurry overall.
* r2a is streaky.

**Seams.**

* r3 shows the tile grid in worm textures, because the strokes stop at tile borders: straight
  discontinuities in the grass of `views/coast_048_098.png` (C1 1.326 on that view) and in
  `views/forest_051_069.png` (1.480).
* Marsh has the worst seams in both r3 and r3h: `views/swamp_069_090.png` gives C1 1.392, and
  `views/swamp_020_035.png` gives 1.530 for r3 and 1.423 for r3h. r2 gives 1.175 and 1.225 on the
  same views.
* r3h's grass and dirt have no seams, because the original pixel grid is continuous.
* r2a has visible seams. In `views/roads_134_064.png` there is a dark line under every curb stone
  and the cobble grid shows. Its C2 p99 is 0.061, against 0.017-0.020 for the full packs.
* The `nn3` edge band is no fix. The pilot `bg-r3-mixed-nn3` holds the voted hybrid tiles plus the NN
  band (verified 3,885/3,885). Its C1 is 1.457 with a maximum of 2.944, re-run here with identical
  numbers. So `edge=none` stays.

**Water and sparkles.**

* Water is 54 % of map use and is good in every pack. The wave strokes become smooth curves and B1 is
  above 99 % weighted by use in the full packs, 98.6 % for r2a (`sheets/water.png`).
* Every pack keeps the sparkles. A3 finds 0 non-compliant and 0 dropped. Each 1x glint becomes a
  rounded dot of about 90 % of its 6x6 block, in the same place in every pack (`sheets/sparkles_water.png`).
* The cycling layout is identical in r3 and r2 on 365 of 369 cycling flats (6 px apart in total). r3h
  differs from r3 on 24 frames (190 px), all in the hybrid families.

**Roads, floors, stone.**

* xBRZ, shared by r3 and r3h, is at its best here:
  * the flagstones of `views/dungeon_070_115.png` look like hand-drawn hi-res art;
  * the curbs and cobbles of `views/roads_134_064.png` are crisp;
  * the floor glyphs (`sheets/floor.png`) are smooth and readable.
* r2 is softer and a little blurry there, with lower B1 (roads 95.4 % weighted, 7 tiles below 85 %).
* r2a damages roads and floors.

**Shore.** xBRZ draws the diagonal coast steps as a sawtooth (`sheets/shore.png`, tiles 0089_01,
0088_30, 0088_25). r2's coastlines are smoother and more natural. They are also less exact: 231 of 289
tiles are below 97 %, but only 2 below 85 %.

**Flagged tiles.** The 12 most used tiles below 90 % B1 in some pack are in `sheets/flagged_low_b1.png`.
The route-3 ones show no visible defect: water 0065_17 at 85.9 % and 0020_11 at 89.1 % in r3 and r3h,
and sand 0010_06 at 89.1 % in r3 only. r2's flagged tiles show shifted dirt/grass boundaries
(0137_27, 0143_10).

**Most telling images:**

* `sheets/grass.png`: the main choice. r3 worms, r3h at NN, r2 soft, r2a streaky.
* `views/coast_048_098.png`: water, shore, dirt and grass together; r3's seams; r3h's smooth material
  boundaries.
* `views/swamp_069_090.png`: marsh worms and seams with xBRZ; r2 is better.
* `views/roads_134_064.png`: cobbles and curbs (xBRZ crisp), grass, and r2a's seams.
* `views/dungeon_070_115.png`: xBRZ flagstones.
* `sheets/sparkles_water.png`: sparkles kept, the same in every pack.

## 5. Recommended default per family

The default pack is one pack, `bg-r3h`. The last column says which other source is a candidate for
promotion into a curated pack (DESIGN §8.4 B5), always subject to in-engine review.

| family | map use | default source (in `packs/bg`) | why | promotion candidate |
|---|---|---|---|---|
| water | 54.4 % | r3h (= xBRZ) | crisp, B1 99.6 % weighted, sparkles kept | none needed (r2 is equivalent) |
| grass, dirt, sand | 30.9 % | r3h (hybrid, about NN) | no worms, no seams, B1 99.8-99.97 % | **diffusion refine** (priority 1); r2 as the smooth interim look |
| transitions dirt+grass, grass+sand, dirt+sand | 3.8 % | r3h | smooth material boundaries, pixel interior | refine together with grass, dirt and sand |
| void, other | 6.1 % | any (identical) | B1 100 % | none |
| stone and roads, dirt+stone, grass+stone | 3.7 % | r3h (= xBRZ) | crisp flagstones and curbs, B1 99.2 % | optional refine of the cobble "links" (0024, 0063) |
| floor | 0.08 % | r3h (= xBRZ) | smooth, readable glyphs | none |
| marsh | 0.74 % | r3h (= xBRZ) | | **r2** (no worms; C1 on the swamp views 1.18-1.23 against 1.39-1.53; 0 tiles < 85 %), or refine |
| shore | 0.19 % | r3h (= xBRZ) | B1 98.9 % | r2 looks better but has 231/289 tiles flagged; refine (priority 2) |

* r3 (xBRZ everywhere) is not recommended for any family where it differs from r3h. It has worms in
  grass, dirt and sand, its sand B1 is 93.0 % weighted, and it fails C1.
* r2a (Arzenal) is not recommended for any family. It remains a look reference only.
* A curated pack that mixes routes (for example r3h plus r2 marsh) needs QA support first: C1 has
  only a single-route region baseline today.

## 6. Published

* **`packs/bg` = `bg-r3h`.** It was created with `vote.py tree packs/bg packs/bg-r3h
  /mnt/e/Dati/Ultima7_Upscale/packs/bg-r3h`: the ext4 copy and the `E:` mirror were read
  independently, and all 7,772 files agreed (0 outvoted). Checks:
  * tree hash `91f97d1f1f171596`;
  * `vote.py verify` against `raw/bg-r3h/voted` passes 3,885/3,885;
  * two `hirescheck.py` runs give identical results, with the same findings as `bg-r3h`
    (0 rejected, 0 warnings, 281 offline P2);
  * `hiresqa.py packs/bg` passes (`art_work/qa/bg`). Its `per_tile.json` and all 48 previews are
    identical to `bg-r3h`'s.
* `pack.txt` is the same as `bg-r3h`'s (`palette_crc32=c9c2c0e7`, `edge=none`, `route=r3-mixed`).
  The format is loose PNGs with sidecars and no `flats.bundle` (the bundle is not available yet).
* **Mirror.** `publish.sh` ran for `bg`, `bg-r3h`, `bg-r3`, `bg-r2` and `bg-r2a-subset` to
  `/mnt/e/Dati/Ultima7_Upscale/packs/`:
  * `bg` was new (121 s);
  * the others had no changed files (56 s each, 4.5 s for the subset).
  
  Every mirror was then re-read and compared with ext4: all five trees are identical (hashes above).
* **Comparison images for the user.** All of `art_work/qa/compare` (sheets, views, previews, metrics)
  and this report are in `/mnt/e/Dati/Ultima7_Upscale/art_compare/` (`E:\Dati\Ultima7_Upscale\art_compare\`).
* **Superseded pilot packs**, still on ext4 and partly on `E:`:
  * `bg-r3-mixed-none` has PNGs identical to `bg-r3h`;
  * `bg-r3-xbrz-none` has PNGs identical to `bg-r3`;
  * `bg-r2-nxbrz-none` has PNGs identical to `bg-r2`;
  * `bg-r3-mixed-nn3` is the hybrid with the `nn3` edge.
  
  The first three can be deleted. Keep `bg-r3-mixed-nn3` as the reference for the A4 edge decision.
  Nothing was deleted.

## 7. Known defects of the default (`packs/bg` = `bg-r3h`)

1. **No hi-res detail inside grass, dirt and sand** (0.1-0.6 % of pixels differ from NN). With the
   transitions, that is 88 % of the visible land: it will look like the original pixel art with
   smoothed material borders. This is by design of the hybrid; it is the first target for refinement.
2. **xBRZ artefacts where xBRZ is used:**
   * worms and visible tile seams in marsh: the C1 views swamp_069_090 at 1.392, swamp_020_035 at
     1.423 and dungeon_050_166 at 1.365 are above 1.2, although the aggregate 1.104 passes;
   * worm-like links in grey cobbles (0063_xx, 0024_xx);
   * a sawtooth on diagonal coastlines.
3. **195 tiles flagged** with B1 < 97 % (2 between 85 and 90 %, minimum 85.94 %, 0065_17). The most
   used ones show no visible defect, but the F1 review per Q8 is still open. The flagged lists are in
   `art_work/qa/bg/report.json`.
4. **Offline notes.** 281 P2 notes (one to five of a tile's 64 blocks change ramp) and 688 D1 flags. The
   D1 median is very low (0.0031), so "2x median" is a low bar.
5. **Not yet confirmed by the engine.** `palette_crc32` was computed by the tool, because there is no
   `--dump-art` / `ref.txt` yet. The Python fill port has not been cross-checked with an engine dump
   (A0), and there is no in-engine A/B for the edge (A4).
6. **RLE terrain tiles are not covered.** Shore pieces, swamp ground frames and walls are RLE shapes,
   not flats, so they stay 1x NN next to the 6x flats. This is visible in every view and needs
   per-terrain overrides (B2) or M2 sprite art.
7. **Slow to load from WSL.** The pack is loose PNGs, and a WSL engine reading them from `E:` takes
   about 17.6 s. Native Windows and the ext4 copy are fast.

## 8. Next steps

1. **Engine side.**
   * `--dump-art` parity (A0): fill port, T1 keys, `palette_crc32` from `ref.txt`.
   * Load `packs/bg` in the dev engine.
   * In-engine A/B of `none` against `nn3` (A4), which the metrics already decide for `none`.
2. **F1 review** of the flagged sheets and the per-family sheets with the user. Q8 accepted the tiles
   between 85 and 97 %; the per-tile floor stays.
3. **Diffusion refine** (Phase B route 1: SDXL + xinsir Tile ControlNet at low denoise on the xBRZ or
   hybrid base, back-projection, quantize, consensus; local only, per Q4), by priority:
   1. grass, dirt and sand plus their transitions dirt+grass, grass+sand and dirt+sand. This is about
      35 % of map use and 88 % of the visible land. Today it is either NN (r3h), worms (r3) or
      blobs (r2).
   2. marsh (worms and seams) and shore (sawtooth);
   3. optional: grey cobbles (shapes 24 and 63).

   Water, flagstones, curbs, floors, void and other need no refine.

   The first session should use grass 147, dirt 149, sand 10 and a marsh frame (shape 117) as test
   shapes. Water 19 is not needed.
4. **Curated interim pack** (optional, before diffusion): `bg-r3h` with r2 marsh, and r2 shore after
   review. First teach `qa.py` a per-route C1 baseline for mixed packs.
5. **`flats.bundle`** when the engine reader (B0) lands, then rebuild `packs/bg` as a bundle and
   republish.
6. **Clean-up** of the superseded pilot packs (section 6), when the user agrees.

## 9. Files

| what | where |
|---|---|
| this report | `/home/simonea/ultima7_exult/docs-hires/art/phase_a_report.md` (copy in `E:\Dati\Ultima7_Upscale\art_compare\`) |
| comparison sheets, views, previews, metrics | `/home/simonea/ultima7_exult/art_work/qa/compare/{sheets,views,previews}/`, `metrics.md`, `metrics.json` |
| QA per pack | `/home/simonea/ultima7_exult/art_work/qa/{bg,bg-r3,bg-r3h,bg-r2,bg-r2a-subset}/` |
| production logs | `art_work/prod/timeline.log` (route 3), `art_work/r2prod/timeline.log` and `qa/bg-r2/production_r2.json` (route 2) |
| active pack | `/home/simonea/ultima7_exult/packs/bg` and `E:\Dati\Ultima7_Upscale\packs\bg` |
| tool | `tools/hires/hirescompare.py`, `tools/hires/u7hires/compare.py`, `tools/hires/tests/test_compare.py` (branch `hires-art`, `731d5cb3c`) |
| this step's logs | `/home/simonea/ultima7_exult/tmp/compare_run/` (two compare runs, tree vote, hirescheck, QA, mirror comparisons) |
