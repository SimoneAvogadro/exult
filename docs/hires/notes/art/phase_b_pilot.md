# Phase B pilot: diffusion refine (route 1) for grass, dirt and sand

2026-10-06. Question: does generative refinement (SDXL + xinsir Tile ControlNet) add believable
detail to the 6x grass, dirt and sand flats, where the algorithmic routes leave either the original
pixels (`packs/bg` = r3h) or xBRZ worms / NXbrz blobs, and does it stay inside the engine art contract
(DESIGN §5.3, §5.5)?

**Verdict.**

* **As art, a qualified yes.** On a whole window, the best setting gives believable fine detail:
  * organic grass strokes instead of the 1x pixel grid;
  * rounded clods and pebbles in dirt;
  * shaded stones in sand;
  * smooth material boundaries.

  It invents no objects at denoise ≤ 0.5, and the low-frequency layout stays the original (block
  colour error B3 p99 ≤ 0.06 in grass, dirt and sand).

  The gain is real but moderate. Most of the raw generated micro-detail, such as blades, is erased by
  the palette snap. What survives reads as fine painted texture, a little busy.
* **As per-tile flats (one 48x48 per `(shape, frame)`, today's contract), no.**
  * Two diffusion instances of the same key differ far more than for any algorithmic route (D1 0.10-0.13
    against 0.003-0.016), so tiles assembled per key seam.
  * C1 is 1.3-1.9 against the 1.2 gate, and the seams show as straight lines or a tile grid.
  * Macro-sheet canvases do not fix this, because BG grass mixes 4-6 macro shapes freely.
  * B1 falls to 83-93 %, against the 97 % aggregate gate and the 85 % floor.
* **Roads and marsh: no gain.** It blurs the drain grate and softens cobbles; marsh stays wormy.
  Keep xBRZ for roads and r2 for marsh.

**Recommendation.**

* Do **not** run route 1 over all grass/dirt/sand frames as a per-tile pack. `packs/bg` stays the
  default.
* If the look is wanted, the viable target is **per-terrain overrides** (768² per chunk template,
  DESIGN §5.1/§5.2, engine WP-17), generated from whole a16 windows. Details are in §7.

## 1. Method

**Windows.** Seven 12x12-tile crops (96x96 px at 1x) of the a16 terrain windows
(`art_work/ctx/a16`). The outer 2 tiles are context; the centre 8x8 tiles (the "eval region",
384x384 at 6x) are evaluated. Chosen by family shares (`scripts/pick_windows.py`) and checked by eye:

| name | terrain | crop (ty, tx) | content of the 64 eval cells | distinct keys |
|---|---|---|---|---|
| grass | 1825 | 0, 0 | grass 64 (shapes 146-148, 135, 136, 121, 46) | 55 |
| dirt | 2699 | 4, 0 | dirt 60, grass edges | 39 |
| sand | 25 | 0, 4 | sand 64 (shape 10 with stones) | 9 |
| dirt_grass | 163 | 8, 4 | dirt+grass 34, dirt 14, grass 12 | 54 |
| grass_sand | 1941 | 2, 6 | grass 26, sand 20, grass+sand 16 | 63 |
| marsh (control) | 1121 | 8, 2 | marsh 58, grass 5 | 51 |
| road (control) | 239 | 0, 4 | cobbles (shape 24) and a drain grate (shape 1) | 5 |

Every crop was checked to equal `ctx.world_region` (the engine fill) at the same world position.
Three extra full windows (terrains 1825, 1826 and 1827, 20x20 tiles, 1920² at 12x) measure timing
and chunk-border seams.

**Pipeline.** `tools/hires/u7hires/route1.py` (branch `hires-art`, commit `83707fac5`; 7 new CPU
tests, suite 121 passed / 2 skipped).

1. **Mould at 12x.** One of:
   * `mixed`: the route-3 plane of `packs/bg`, computed on the whole a16 window, Lanczos 2x;
   * `xbrz`: the same with the xBRZ plane;
   * `soft`: the 1x crop, Lanczos 12x;
   * `r2`: the `packs/bg-r2` tiles assembled over the crop, Lanczos 2x.
2. **Refine.** diffusers 0.40 `StableDiffusionXLControlNetImg2ImgPipeline`:
   * SDXL base 1.0 fp16 variant, xinsir controlnet-tile-sdxl-1.0, madebyollin sdxl-vae-fp16-fix;
   * fp16, DPM++ 2M Karras, 28 scheduled steps × strength, cfg 5;
   * ControlNet over the first 90 % of the steps; the mould is both the init image and the control
     image; fixed seed (`torch.Generator("cuda")`).
   * Prompt per material, e.g. grass: "top-down view of a grass meadow ground texture, 16-bit RPG game
     terrain, hand-painted, detailed grass blades, crisp, even lighting". Negative: "blurry, soft
     focus, text, letters, watermark, signature, objects, buildings, people, animals, perspective,
     horizon, sky, frame, border, photo, photorealistic, 3d render, jpeg artifacts, noise".
3. **Box 2:1 to 6x**, then a colour lock (§3) in linear RGB.
4. **Index and contract rules.**
   * Cycling pixels come from route 3's label-safe xBRZ+snap plane and are excluded from the lock and
     the snap.
   * Static pixels get a local snap to the OKLab-nearest index of the 1x parent's 3x3 neighbourhood
     (optionally ramp-expanded). This is `Quantizer.local_snap`, never 0xFF and never ≥ 0xE0.
   * `Quantizer.enforce` then applies P4, P0 and the 0x00 rule per tile.
5. **Cut and consensus.** The own cells are cut into 48x48 tiles, with one consensus per key:
   * `pick` (new): the whole-instance OKLab medoid, which keeps one coherent texture;
   * or `mode`: the per-pixel weighted mode, as in routes 2 and 3.
6. **Metrics.** QA metrics from `u7hires.qa`:
   * B1-B4 / A2 / A3 per tile;
   * **C1** = seam ratio of the eval region rendered from the per-key tiles ÷ seam ratio of the
     route's own window-level plane (the region baseline);
   * **C2** = edge-strip excess over the 1x seam for adjacent own pairs inside the eval region;
   * **D1** = instance disagreement;
   * detail: the share of 6x pixels ≠ NN and the mean OKLab-L standard deviation inside 6x6 blocks
     ("std"; r3h grass 0.000, r2 0.016).
7. **Mini packs and validation.** One cross-window `pick` consensus per setting over all 7 windows
   (263 keys, 58 with several instances), written with `PackWriter` to
   `art_work/phase_b_pilot/packs/pilot-{s1,s2}-{bc,lp3}`. Checked by `hirescheck.py` and
   `check_root`.

**Determinism.** A repeat of the first setting on every window was bit-identical (7 of 7), so the
GPU stage reproduces exactly. All CPU stages are deterministic and ran with at most 4 threads.

## 2. Timing and VRAM (RTX 5070 Ti 16 GB, WSL, torch 2.14.1+cu130)

| canvas | VAE | steps run | s / canvas | torch peak alloc | nvidia-smi max (desktop idle ≈ 1.0 GB) |
|---|---|---|---|---|---|
| 1152² (12x12 tiles at 12x, 1.33 MP) | full | d0.3: 8, d0.4: 11, d0.5: 14, d0.6: 17, d0.75: 21 | 3.4 / 4.5 / 5.6 / 6.4 / 8.2 | 12.4 GB | **15.7 GB of 16.3** (too tight) |
| 1152² | tiled | d0.5 | 5.8-6.4 | 10.0 GB | 12.5 GB |
| 1152x768 macro sheet | full | d0.5 | 3.6 | - | - |
| 1920² (whole a16 window, 3.7 MP) | full | d0.5 | 38-39 | **18.2 GB** (spills into shared memory) | 15.8 GB |
| 1920² | tiled | d0.5 | 18.7-19.3 | 10.9 GB | 14.0 GB |

* Pipeline load: 2.5-7.7 s.
* The first canvas after a load: 17.4 s (warm-up).
* Tiled against full VAE changes the output by a mean |Δ| of 0.87/255.
* The CPU post-processing takes about 1 s per crop.
* **Use `--vae-tiling` for every production run.**

## 3. What the sweep showed (grass window)

The prescribed grid was denoise {0.3, 0.4, 0.5} × CN {0.6, 0.8} × seeds {1000, 1001}. It ran on the
`mixed` and `xbrz` moulds and, after the first look, also on `soft` (36 canvases). An extension tried
denoise 0.6/0.75 with CN 0.4/0.6, the `r2` mould, and a "painted" prompt without "16-bit RPG" that
puts pixel art into the negative (30 more canvases). Sheets: `sheets/sweep/grass_{mixed,xbrz,soft}.png`
(raw | strict lock | bicubic lock per setting) and `sheets/extended/`.

1. **SDXL + tile CN copies the mould at denoise ≤ 0.5.**
   * `mixed` (NN inside grass): it reproduces the 6x6 blocks. After the lock 0.4-0.8 % of pixels
     differ from NN, so the result is r3h again.
   * `xbrz`: it keeps the worms, slightly softened.
   * `r2`: it renders the hard edges as pixel art, prompt or no prompt.
   * Only the blurred `soft` mould leaves room for new texture.
   * Denoise and CN within 0.3-0.5 / 0.6-0.8 change little. The mould dominates.
2. **Strong settings hallucinate.**
   * `soft` at d0.75/CN0.4 paints large cartoon grass blades at the wrong scale.
   * `xbrz` at d0.75 invents purple flowers (`sheets/extended/grass_strong_denoise_nnlock.png`).
   * d0.6 is the limit for grass.
3. **The strict colour lock re-imposes the pixel grid.** Route 2's back-projection
   (`x' = x + U(y − D(x))`, NN U) makes every 6x6 block average exactly to its 1x pixel. In U7 grass
   every 1x pixel is a grain of noise, so the 6x result is again a mosaic of 6x6 blocks with faint
   texture inside. Two alternative locks were added:
   * **`bc`**: iterative back-projection with a bicubic U, so the correction field is smooth and no
     block grid is stamped in;
   * **`lp3`**: low frequencies (Gaussian σ = 3 px at 6x) from the r3h 6x plane, high frequencies from
     diffusion. Material boundaries then follow r3h's smooth shapes and the interiors carry the
     diffusion texture.
4. **The palette snap is the detail bottleneck.**
   * Snapping each pixel to the 3x3-parent indices (typically 3-6 shades of one ramp) turns blades into
     rounded blobs.
   * Ramp expansion (`rx`) or a detail gain keeps more texture, but B1 drops to 55-80 %.
   * `sheets/extended/raw_locked_snap_rampexpand_r2_2x.png` shows, at 2x for grass, dirt and sand,
     raw | locked RGB | bc snap | bc + ramp-expanded snap | r2. It also shows back-projection
     overshoot (cyan halos round the berries) that the snap removes.

**The two settings carried forward** (2 seeds each on all windows):

* **S1** = `soft` mould, d0.50, CN0.60: the most detail.
* **S2** = `xbrz` mould, d0.40, CN0.80: the recommendation's mould, the most faithful.

## 4. Metrics per window (seed 1000, lock `lp3`; `bc` in brackets where it differs notably)

"window" is the window-level plane cut per cell (what a per-terrain override ships, with no per-key
consensus). "pick" is the per-key tile assembled back into the window. "alt" is the seam stress test:
cells taken alternately from seeds 1000 and 1001, as happens when a key's tile comes from another
context. Full tables: `art_work/phase_b_pilot/report_lp3.json` and `report_bc.json`.

| window | r3h B1 / std | r2 B1 / std | S1 window B1 / min / B3p99 / std | S1 pick C1 | S1 alt C1 | S2 window B1 / std | S2 pick C1 |
|---|---|---|---|---|---|---|---|
| grass | 100.0 / 0.000 | 97.6 / 0.016 | 82.8 / 67 / 0.062 / 0.030 (bc 86.5 / 77 / 0.031 / 0.034) | 1.40 (bc 1.34) | 1.71 (bc 1.40) | 96.2 / 0.021 | 1.16 |
| dirt | 99.9 / 0.001 | 98.9 / 0.009 | 89.2 / 75 / 0.053 / 0.018 | **1.73** (bc 1.77) | 1.54 | 97.1 / 0.013 | 1.32 |
| sand | 99.8 / 0.000 | 98.6 / 0.008 | 97.1 / 77 / 0.046 / 0.017 | 1.42 | 1.13 | 93.8 / 0.011 | 1.16 |
| dirt_grass | 99.6 / 0.009 | 95.5 / 0.018 | 77.5 / 64 / 0.104 / 0.035 (bc 80.4 / 62) | 1.44 | 1.31 | 92.5 / 0.025 | 1.15 |
| grass_sand | 99.7 / 0.003 | 98.9 / 0.013 | 90.4 / 77 / 0.061 / 0.024 | 1.10 | 1.60 | 97.7 / 0.017 | 1.04 |
| marsh | 99.5 / 0.021 | 97.3 / 0.014 | 83.9 / 70 / 0.127 / 0.027 | 1.34 | 1.12 | 95.1 / 0.024 | 1.20 |
| road | 100.0 / 0.028 | 90.6 / 0.024 | 74.4 / 66 / 0.075 / 0.041 (pick B3p99 **0.280**) | 1.60 | 1.00 | 97.7 / 0.030 | 1.28 |

* Per-key `mode` instead of `pick` raises B1 by 1-4 points and C1 slightly (dirt 2.02): a per-pixel
  vote over diffusion instances is a patchwork.
* Per-terrain plane against per-tile pack on the same windows (`sheets/macro/{grass,dirt}.png`), C1:
  per-key pick from the window 1.34-1.77; tiles from macro-sheet canvases (53/55 grass keys, 31/39
  dirt keys) 1.73-1.92.

**Mini packs** (cross-window pick, 263 keys):

| pack | B1 agg / min | tiles < 97 % / < 85 % | B2 | B3 mean / p99 | B4 | A2 / A3 P4 / dropped | D1 (multi-instance keys) | hirescheck |
|---|---|---|---|---|---|---|---|---|
| pilot-s1-bc | 89.0 % / 62.5 % | 224 / 70 | 94.7 % | 0.010 / 0.051 | 99.8 % | 0 / 0 / 0 | 0.104 | 0 rejected, 0 warnings, 20 P2 |
| pilot-s1-lp3 | 84.8 % / 64.1 % | 252 / 133 | 80.1 % | 0.018 / 0.103 | 98.9 % | 0 / 0 / 0 | 0.128 | 0 rejected, 0 warnings, 74 P2 |
| pilot-s2-bc | 95.1 % / 73.4 % | 174 / 8 | 95.8 % | 0.008 / 0.060 | 99.9 % | 0 / 0 / 0 | 0.058 | 0 rejected, 0 warnings, 15 P2 |
| pilot-s2-lp3 | 95.8 % / 79.7 % | 167 / 4 | 87.0 % | 0.013 / 0.098 | 99.6 % | 0 / 0 / 0 | 0.055 | 0 rejected, 0 warnings, 45 P2 |

Phase A packs for reference: r3h 99.61 % B1, r2 97.53 %. D1 medians were 0.003-0.016.

* Every candidate satisfies the hard engine contract: 48x48 mode-P PNGs, PLTE = palette 0, raw
  indices, no 0xFF, cycling only per P4, 0 rejects.
* Every S1 candidate fails the QA gates as calibrated in Phase A: B1 aggregate ≥ 97 % with a per-tile
  floor of 85 %, and C1 ≤ 1.2. S2 fails B1 and C1 more narrowly.
* B1 measures exactly what diffusion is meant to change: who holds the majority inside a 6x6 block.
  For a generative route it should become a guard against drift (B3/B4 and E1), not a ≥ 97 % gate.

## 5. Visual verdict (sheets looked at 1:1 and at 2x)

* **Grass** (`sheets/bc/grass.png`, `sheets/lp3/grass.png`, `sheets/terrain_override_grass_1825.png`).
  * S1 turns the pixel grain into fine, curved strokes with soft highlights. At 1:1 on the whole
    16x16-tile terrain it reads most like "hi-res grass" of all routes: r3h shows pixels, r2 shows
    calm blobs.
  * It is busier than r2 and more painted noise than distinct blades.
  * The weed and berry sprites stay in place, with rounder shapes.
  * S2 is xBRZ worms again.
* **Dirt** (`sheets/bc/dirt.png`). S1 gives rounded clods, pebbles and dark cracks, which is nice. The
  per-key tiles, however, show **straight seam lines** where a key repeats in the window (C1 1.77),
  clearly visible at 2x. S2 is close to r2 and xBRZ.
* **Sand** (`sheets/bc/sand.png`). The dot grain becomes round dots, and the stones gain shading and
  rounder outlines. This is a modest gain over r2; S2 is barely different from r2.
* **Transitions** (`sheets/lp3/dirt_grass.png`, `sheets/lp3/grass_sand.png`).
  * With the `bc` lock the dirt/grass boundary keeps the 1x staircase (B1 80 %, minimum 62 %),
    which is worse than r3h.
  * With `lp3` the boundary takes r3h's smooth shape and the interiors keep the texture.
  * `grass_sand` with lp3 is the best-looking image of the pilot: no pixel grid, a natural sand patch,
    painted grass.
* **Marsh** (`sheets/lp3/marsh.png`). The noisy raw output is pulled back to wormy blobs. There is no
  gain over r2.
* **Road** (`sheets/lp3/road.png`).
  * S1 softens the cobbles and blurs the drain grate (shape 1).
  * The per-key tiles seam (C1 1.60) and drift in colour (B3 p99 0.28).
  * Diffusion must not touch structured shapes. S2 is harmless but adds nothing.
* **Hallucination.** At d ≤ 0.5 nothing new appears. Grey 1x pixels in dirt become small round
  stones, which is consistent with the source. At d ≥ 0.6 shapes start to move, and at 0.75 objects
  appear.
* **Consistency.** Seeds 1000 and 1001 give the same look per window (C1 and B1 within about 2
  points). Inside one window the texture is coherent.
* **Chunk borders between two independently generated per-terrain overrides**
  (`sheets/chunk_border_1825_1826.png`; terrains 1825 | 1826 and 1825 / 1827).
  * The border edge dE is 2.3-2.7x that of the tiles' own borders, against 1.06 for r3h.
  * The cause is a straight cut through the strokes. It is faint at 1:1 and visible at 2x.

## 6. Answer to the question

| criterion | result |
|---|---|
| adds believable detail | yes for grass, dirt and sand at window level (S1 + `lp3` or `bc`), moderately. The palette snap removes most raw micro-detail. No gain for marsh or roads. |
| stays faithful | layout yes (B3 p99 0.05-0.06, B4 ≥ 99 %, no invented objects at d ≤ 0.5). Per-pixel majority no (B1 83-93 %). |
| no invented objects | yes at d ≤ 0.5; no at d ≥ 0.75 |
| consistent across tiles | **no.** Per-key instances differ (D1 0.10-0.13). Tiles assembled per key seam (C1 1.3-1.9), including macro-sheet tiles. |
| no seams | only inside one generated plane. Chunk borders need a fix (§7). |
| engine contract | always met (0 rejects, A2/A3 0) |

## 7. Recommendation

1. **No per-tile production run of route 1** for grass, dirt and sand. It would replace a seamless,
   faithful pack (r3h) with one that seams at tile borders and fails B1 and C1. Its gain shows only in
   continuous areas.
   * For reference, such a run over the 2,105 a16 windows would take about 2,105 × 19 s ≈ 11 h per
     seed with tiled VAE (or four 1152² crops per window, ≈ 6.5 h).
   * Keep `packs/bg` as the default, with r2 as the interim candidate for marsh.
2. **If the diffusion look is wanted, use per-terrain overrides** (`x6/terrain/<T1>.png`, 768², engine
   WP-17).
   * **Recipe.** Per used terrain whose 256 cells are ≥ 50 % (or ≥ 90 %) grass, dirt, sand or their
     transitions:
     * the whole a16 window (160² at 1x) as a `soft` mould at 12x = 1920², `--vae-tiling`;
     * d0.50, CN0.60, CN end 0.9, 28 steps, cfg 5, one fixed seed per material and a sober prompt per
       material;
     * box 2:1, then the `lp3` lock against the r3h plane of the same window, local snap (`rx`
       optional), cycling from route 3, `enforce`;
     * crop the centre 768², T1 key and P4 against the 128² flat layer.
   * **Volume.**
     * At ≥ 50 %: 1,103 of the 2,105 used terrains (34.9 % of the map's chunks), about 1,103 × 19 s
       ≈ **5.8 h GPU per seed**.
     * At ≥ 90 %: 594 terrains (25.7 % of chunks), about 3.1 h.
     * CPU post-processing: 1-2 s per terrain.
     * Output: about 0.2-0.3 GB of indexed PNGs.
     * Generate 2 seeds and let a human choose per terrain group.
   * **Risks.**
     * **Chunk-border seams** (2.3-2.7x). Fix: blend the outer 6-12 px of each override towards the
       r3h plane (low-pass already matches, only texture differs). Measure again before production.
     * T1 duplicates (terrains with identical flats share a key).
     * Under-RLE fill pixels are generated too. That is harmless, but they must not look like objects.
     * The QA gates need a generative profile: B1 as a drift guard, plus B3/B4, E1 and human review.
     * 16 GB VRAM needs tiled VAE (untiled 1920² spills to shared memory and halves the speed).
     * Human review of about 1,100 terrains: budget 2-4 h with contact sheets grouped by material.
     * Engine WP-17 is a prerequisite.
3. **Before committing to that, a short Phase B2 pilot (about half a day):**
   * 5-10 per-terrain overrides around one forest or meadow region, including the border blend;
   * then in-engine A/B against `packs/bg` at the real display size (1920x1200 and the user's
     window);
   * plus one quantizer variant that keeps more raw detail: ramp-expanded snap, or ordered dithering
     inside the ramp.
4. **Do not use diffusion** for roads, floors or stone (structure is damaged), or for marsh (no gain:
   use r2).
5. Long-term option (diffusion.md §7): to make a per-tile diffusion pack consistent, a style LoRA or
   IP-Adapter alone is not enough. The per-key variability must go, for example through Tier-2
   inpainting of each key in its most common context with finished neighbours. That is several days
   of work and was not tried here.

## 8. Files

| what | where |
|---|---|
| code | `exult-hires-art/tools/hires/u7hires/route1.py`, `tools/hires/route1.py`, `tools/hires/tests/test_route1.py` (branch `hires-art`, `83707fac5`) |
| raw runs (12x PNG + run.json; 92 crop canvases (68 on grass), 12 macro canvases, 3 full windows; the untiled-VAE full window is in `tmp/phase_b_pilot/keep`) | `/home/simonea/ultima7_exult/art_work/phase_b_pilot/runs/` |
| reports | `art_work/phase_b_pilot/report_bc.json`, `report_lp3.json`, `macro_eval.json`, `chunk_border_seam.json`, `metrics_grass_full.json`, `sweep_grass_metrics.json` |
| mini packs + hirescheck JSON | `art_work/phase_b_pilot/packs/pilot-{s1,s2}-{bc,lp3}` (+ `.check.json`) |
| ad hoc scripts and GPU logs | `art_work/phase_b_pilot/scripts/` |
| comparison sheets (copy on `E:`) | `art_work/phase_b_pilot/sheets/` → `E:\Dati\Ultima7_Upscale\art_compare\phase_b_pilot\` |

**Best images:**

* `sheets/lp3/grass_sand.png`: best look (S1 + lp3), with r3h and r2 beside it;
* `sheets/terrain_override_grass_1825.png`: a whole terrain at 1:1, r3h | r2 | S1 per-terrain;
* `sheets/bc/dirt.png`: per-key seams (S1 pick C1 1.77) against the window plane;
* `sheets/macro/grass.png`: per-terrain plane against per-key and macro-sheet tiles (tile grid);
* `sheets/lp3/dirt_grass.png`: smooth material boundaries with lp3, staircase with bc;
* `sheets/extended/raw_locked_snap_rampexpand_r2_2x.png`: where the detail is lost;
* `sheets/extended/grass_strong_denoise_nnlock.png`: hallucination at d0.75 and the NN-lock grid;
* `sheets/chunk_border_1825_1826.png`: the chunk-border seam;
* `sheets/lp3/road.png`: the road control.

Sheet panels: 1x NN | r3h (`packs/bg`) | r2 (`packs/bg-r2`, 4x-NXbrz), then per setting: raw (box 2:1,
no lock) | seed 1000 per-key pick | seed 1001 per-key pick | seed 1000 window plane | seam test. All panels
are at 1:1 6x and captioned with B1, B3 p99, C1, C2 excess and std.
