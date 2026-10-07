# 6x terrain art for the Exult hi-res override: recommendation and pipeline

Date: **2026-10-04**. This document combines the five research reports in this folder: `sr_models.md`, `diffusion.md`, `api_models.md`, `algorithmic.md` and `prior_art.md`. Each report ended with an adversarial fact-check section. A claim appears here as fact only if it survived that fact-check. Everything else carries a label.

Scope: the 3,885 flat 8x8 terrain frames of BG (shapes 0-149) become 48x48 frames, indexed against palette 0. RLE sprites come later and are covered only where a decision affects them.

**Labels**

| Label | Meaning |
|---|---|
| **[M]** | Measured on this machine on real BG data. Most of these numbers were re-run by the fact-check. |
| **[V]** | Checked against a primary source (repository, model card, PyPI, wheel index, live API) on 2026-10-04. |
| **[A]** | A design proposal or estimate from the reports. **Not yet measured on U7 art.** Validate before relying on it. |
| **[U]** | Unverified or unverifiable. Do not build on it. |

---

## 0. Decision in brief

1. **No ready-made tool covers this job.**
   - OpenModelDB lists 53 pixel-art SR models. All date from 2019-2022, 45 are non-commercial, and none is from 2025-2026 [V].
   - OpenRouter lists 57 image models. None of them is an upscaler; each one regenerates the whole frame [V].
   - So the pipeline is ours. Context assembly, palette return, cycling restore, seam control and QA are local, deterministic Python. Only the "detail layer" in the middle can be swapped between tools.
2. **Never upscale a lone 8x8 tile.**
   - Zero or reflect padding makes the step at the tile edge 4-18 times larger than a normal pixel step [M].
   - U7 frames do not tile with themselves either: the original art has a wrap-seam ratio of 1.4-1.8 [M].
   - Instead, upscale real map context or periodic macro-textures, cut the result into tiles, and keep one **consensus** tile per `(shape, frame)` [M].
3. **Render above 6x, then come down, and quantize last.** Use 12x→6x box or 8x→6x area resampling. This is the same "render high, downscale at the end" idea as the engine design. Prior art (Doom 8x→2x, BG 4x→1x, Moguri) converged on it too [V].
4. **The output stays 8-bit indexed against palette 0.**
   - Indices ≥ 0xE0 are copied from the source cycling mask. A model or a quantizer never produces them [V, `analysis/palette.md`].
   - Flats never use 0xFF.
5. **Ranked shortlist:**
   1. Best quality-faithful: **structure-locked diffusion refine** (SDXL + xinsir Tile ControlNet on an xBRZ base) on the Windows GPU, with human curation.
   2. Best automatic baseline: the **4x-NXbrz** SR model on chunk context windows, with local palette snap and consensus.
   3. Best fallback without a GPU: **xBRZ 6x + local snap**, which runs in WSL today.
6. **First action:** build route 3 and the QA harness end to end in WSL now. They produce a complete, valid `art_6x` set for the engine work, and they set the QA thresholds that every later route must pass.

---

## 1. Ranked shortlist for 6x terrain tiles

### 1.1 The three picks

| Role | Approach | Evidence | Weak points | Licence | Runs on |
|---|---|---|---|---|---|
| **1. Best quality-faithful** (curated, slowest) | **Structure-locked diffusion refine** (steps below this table) | **Licences** [V]: SDXL base 1.0 is CreativeML Open RAIL++-M; xinsir Tile CN is Apache-2.0. **Circular padding** [V]: the spinagon node patches the U-Net and VAE convolutions only, not the ControlNet. **Prior art** [V]: Moguri 9.0 (2024-08-11) used ESRGAN + SD, blended with the original, plus manual edge work. **Quality and settings** [A]: not yet run on U7. | Hallucination risk on floors and roads; style drift between shapes; needs human review. VRAM and speed claims ("8-10 GB, seconds per canvas") are [U]; measure them. xinsir says its own SR mode "may unstable" [V], so use it only as a refiner. | SDXL OpenRAIL++-M; xinsir Apache-2.0 | Windows GPU (ComfyUI) |
| **2. Best automatic baseline** (no curation needed) | **4x-NXbrz** (ESRGAN, 2021-06) on 1x chunk windows with 16-32 px of real neighbours, then crop, Lanczos 1.5x to 6x, local palette snap and per-frame consensus. **Look alternative:** 8x-Arzenal-v1-1 (2021-02) or 8x-MS-Unpainter (2021-03), boxed 8→6. They are more painterly and less faithful. | **Fidelity** [M]: index match after local quantization is 80.5 % (xbrz 82.1 %, UltraSharpV2 80.8 %, Arzenal 65.0 %, RealESRGAN x4plus 49.9 %). **Context stability** [M]: per-pixel std across instances is 1.3-1.5, against 4.75-8.74 for the other models. **Circular padding** [M]: exact (MAE 0.010/255). **GPU cost** [M]: 160 px window = 0.47 GB and 0.06 s on the 5070 Ti. | "Vector/blobby" look; blurs some grain. Round-trip metrics favour blur, so always pair them with visual review. | CC-BY-NC-SA-4.0. Build packs locally; do not distribute the model. | WSL CUDA (already working), Windows, or CPU at 5-15 s per window |
| **3. Best fallback without GPU** (deterministic, index-preserving) | **xBRZ 1.9 (2026-01-24) at native 6x** on region renders with real context, then **local snap**: re-index each output pixel to the OKLab-nearest index among the source pixel's 3x3 neighbourhood. Then per-frame **mode** consensus. **Variant:** the *material-boundary hybrid* applies xBRZ only to ramp/material boundaries and uses NN for texture interiors. | **Fidelity** [M]: 6x6 block-majority = source index on 98.9 % of blocks. **Colour** [M]: mean ΔE against the raw xBRZ RGB is 0.0017-0.0019. **Cycling** [M]: indices kept. **Seams** [M]: gradient ratio is 4.46 with consensus, against 3.77 for the full region and 7.80 for per-tile clamp. **Consistency** [M]: instances disagree on about 2 % of pixels. **Speed** [M]: 26-51 ms per 768² source; 1-2.2 ms per 160² window. | Turns 1-px grass and dirt noise into "worms" whatever the tolerance (10-120) [M]. The material hybrid avoids that but stays chunky inside materials (98.7 % of pixels = NN) [M]. | xBRZ is GPLv3. Fine offline. Linking it at runtime makes the Exult binary GPLv3 [V]. | WSL CPU, now |

Route 1 in steps:

1. Make a base image (the "mould"): xBRZ-6x, or the material-boundary hybrid.
2. Upscale it 2x with Lanczos, to 12x.
3. Refine with **SDXL base 1.0 + xinsir controlnet-tile-sdxl-1.0**: denoise 0.30-0.50, CN strength 0.6-0.9, fixed seed and prompt per material class.
4. Box-downscale 2:1 to 6x.
5. Back-project, quantize with the constraints in §2.6, and take the per-frame consensus.

Route 3 doubles as:

- the **base image** for route 1;
- the **label-safe scaler** for cycling masks in every route;
- a **runtime fallback** candidate for the engine.

**Index-exact alternatives for route 3** [M]: MMPX 2x then Scale3x (99.3 % block-majority) and Scale2x then Scale3x (98.9 %). Both copy source pixels only, and both are GPLv2-compatible: MMPX is MIT according to the port's README, and Scale2x/3x is our own NumPy code.

**Challengers for route 1:** run them on the same harness. Both are DiT models, so they need pad-and-crop because circular convolution does not apply.

- **SeedVR2 3B** (ByteDance, Apache-2.0, one-step) via the numz ComfyUI node v2.5.24 (2025-12-24) [V]. Its model card warns that it oversharpens lightly degraded inputs [V]. Its pixel-art behaviour is untested.
- **Z-Image-Turbo** (6B, 2025-11-27) with the **Fun-ControlNet Tile 2.1-2601-8steps** (2026-01-12; 6.7 GB, lite version 2 GB), all Apache-2.0 [V]. **Build the Tile graph by hand.** The official `utility_z_image_turbo_2k_upscaler` template contains no ControlNet [V].

**Long-term option for route 1:** distil the curated results into a **custom native-6x model**.

- Train SPAN or RealPLKSR with `scale: 6` in traiNNer-redux.
- At the architecture level this is possible: `SPAN(upscale=6)` and `RealPLKSR(upscaling_factor=6)` build and map 16→96 px [V]. **No training run has been done** [U].
- Base checkpoint: **4x-PBRify-UpscalerSPANV4** (CC0, pure-conv, 2024-04-11) [V]. It keeps the circular-padding trick available and has a clean licence.

### 1.2 Hosted models (OpenRouter): pilot and style references only

Hosted models are not on the bulk shortlist, for six reasons:

- they regenerate the frame instead of upscaling it;
- they offer only tier sizes, with no exact size [V];
- Gemini and OpenAI models take no seed [V];
- models get withdrawn (Gemini 2.5 Flash Image was shut down 2026-10-02) [V];
- they know nothing about the palette;
- the art is copyrighted, and uploading it to third parties is a grey area.

Their job is a cheap look pilot and a hand-approved golden set of 20-50 chunks that later serves as style references (§3.4).

### 1.3 Not recommended (with the reason)

| Option | Why not |
|---|---|
| RealESRGAN x4plus / x4plus_anime_6B as the main model | Round-trip index match is 47-50 % after local quantization. anime_6B wipes grass texture flat [M]. |
| 4x-UltraSharpV2 / 4x-PBRify-UpscalerV4 (DAT2, 2025-05) as the main model | They keep the pixel staircases on diagonals. Circular padding fails on DAT (MAE 15.3/255) [M]. Use them only as a second pass or blend partner. |
| A 2x model chained three times (2x-Faithful) | Colour drift; 41.4 % match [M]. |
| 4x-Struzan and other painterly models (UltraFArt, BigFArt) | Cross-hatch noise or an "oil painting" look [M]. Reviewers complain about this look [V]. |
| FLUX.1-dev, Kontext-dev, FLUX.2-dev, FLUX.2-klein-9B, SUPIR, HYPIR | Non-commercial licences [V] and photo priors. |
| Qwen-Image-Edit-2511 | Apache-2.0, but 20B parameters (about 40 GB in BF16), so it needs offload on 16 GB. Drift. Its upscale LoRA is GPL-3.0 and "not trained on 2d" [V]. |
| Pixel-art LoRAs, Retro Diffusion | They go the wrong way: they *make* low-res pixel art. |
| Per-tile self-wrap padding as the default | Only 33 of the 3,121 used tiles ever appear surrounded by copies of themselves [M]. Self-wrap invents neighbours that never occur on the map. |
| Pillow `quantize`, ImageMagick `-remap`, libimagequant for final art | Each one fails a hard requirement: approximate search, RGB-only matching, or palette alteration (see §2.6). |
| Upscayl | NCNN backend only, no wrap padding, last release v2.15.0 on 2024-12-25 [V]. |

---

## 2. End-to-end art pipeline

```
 shapes.vga / u7chunks / u7map / palettes.flx  (BG STATIC, read-only)
        │  Stage 0  extraction (done: art_original/)
        ▼
 indexed planes + class/ramp planes + world tile grid
        │  Stage 1  context assembly
        ▼
 context windows (chunk + apron) · macro-texture sheets · atlases
        │  Stage 2  upscale  (route 3 xBRZ │ route 2 SR │ route 1 diffusion │ pilot: hosted)
        ▼          always ≥6x, supersampled, then integer-friendly downscale to exactly 6x
 6x windows (RGB and/or index)
        │  Stage 3  seam handling: crop apron → cut instances → per-(shape,frame) consensus
        ▼
 one canonical 48x48 per frame (RGB or index)
        │  Stage 4  colour lock + quantization (cycling mask first, static 0x01-0xDF)
        ▼
 48x48 index tiles
        │  Stage 5  write-out: art_6x/shapes/flat/SSSS_FF.png + .json sidecar (+ optional 768² chunk overrides)
        ▼
        │  Stage 6  QA: format/palette compliance · downscale consistency · seams · consistency · human review
        ▼
 accept │ repair (border lock, regenerate) │ reject → back to Stage 2
```

### 2.1 Invariants (all routes)

1. **Carry the index plane end to end. RGB is only a working copy.**
   - Palette 0 contains exact duplicate colours: {0,252}, {15,224,232,248}, {73,194} and {225,231} [M, V].
   - So index → RGB → index is lossy: cycling whites turn into static whites, and the reverse.
   - Colour-copying scalers (xBRZ before the snap, MMPX, ScaleFX) must run on a **uniquified palette**, where each duplicate is nudged by 1-3 LSB, and be inverted by lookup [M].
2. **Convert 6-bit to 8-bit exactly as the engine does:** `c8 = v*255//63` [V].
3. **Allowed output indices:**
   - The quantizer emits **0x01-0xDF**, plus 0x00 only where the source frame already uses black.
   - **0xFF is never allowed** in flats.
   - Cycling indices are copied from the source, staying in the source pixel's cycle range: E0-E7, E8-EF, F0-F3, F4-F7, F8-FB, FC-FE (`gamewin.cc:1043-1048`) [V]. In BG flats only **E0-E7 and FE** occur: 369 frames in 48 shapes, 0.47 % of pixels [M].
4. **Context always.** No tile is processed without real or periodic neighbours.
5. **Reproducible.**
   - Every output gets a JSON sidecar: route, model id and SHA-256, seed, parameters, source hash.
   - Keep every raw model output, as the Doom pack author did [V].
6. **Per game.** BG and SI palettes differ (`verify.cc`), so build packs per game [V].

### 2.2 Stage 0: extraction (done)

`art_original/` already contains:

- **3,885 flats** as 8x8 mode-P PNGs (`shapes/flat/SSSS_FF.png`);
- **2,105** used chunk templates (`chunks/chunk_NNNN.png`, 128²);
- **144** superchunks (`superchunks/schunk_NNN.png`, 2048²);
- the palettes;
- `manifest.json`, with `map_uses` and `uses_reserved_indices` per frame.

369 flats use reserved indices; this count was recomputed independently from the raw `shapes.vga` [M, V].

The PNGs must keep **raw indices**, with palette 0 as PLTE, and must not go through `Export_png8`'s index rotation (`palette.md` §7.7).

Still to add:

- a per-frame class and cycling-mask sidecar;
- the **world tile-id grid**: 3072×3072 tiles, with frame = `(b1>>2)&31`, as in `objs/chunkter.cc:117-118` [V]. The reader code is in `algorithmic_code/u7algo.py` (`load_chunks`, `load_map`) and in `diffusion.md` Appendix A.

### 2.3 Stage 1: context assembly

Three kinds of context, in order of priority:

1. **Real-map windows. This is the primary context for every route.**
   - For each used chunk template, render 128² at 1x plus an **apron of 16-32 px** (2-4 tiles) taken from its **most frequent real neighbours**. That gives a 160-192 px window.
   - Alternatively, for routes 3 and 2, process whole superchunks (2048²) with overlapping tiles.
   - Store a per-pixel **instance id** (shape, frame, world position) so that Stage 3 can cut the result back into tiles.
   - Facts [M]: 3,121 distinct flat frames are used on the map. Each sees a median of 13 distinct 4-neighbour contexts (p90: 177). The 3x3 neighbourhood count is a median of 16 per tile id.
2. **Periodic macro-texture sheets.** These feed the diffusion route, and also any frames never used on the map.
   - **77 shapes** are 8x4-frame macro-textures (frame f's right neighbour is f+1, its lower neighbour f+8), forming a 64x32 px periodic unit [M].
   - Caveats: only 74 of them have all 32 frames, and the count of 77 depends on a >60 % threshold [V].
   - Pad the unit toroidally to 2x2 periods.
   - Edges that are identical (20.1 %) or follow the macro layout (46.5 %) make up 66.6 % of flat-to-flat edges [M].
3. **Self-wrap.** Use it only for frames that really self-tile (source wrap-seam ratio ≤ about 1.1 [A]), and for the roughly 765 frames never used on the map when no macro sheet covers them.

**Planes per window:** index, RGB (palette 0), class (static or cycling-range id), and ramp id (Exult's 16 static ramps plus the 6 cycling ranges). The ramp-id plane feeds the material hybrid and ramp-constrained quantization.

**Atlases** (diffusion and hosted routes): pack 3x3-neighbourhood cells with gutters, so that neighbouring cells do not bleed into each other [V, Doom lesson].

### 2.4 Stage 2: upscale (every route ends at exactly 6x)

| Route | Input | Native step | To exactly 6x | Notes |
|---|---|---|---|---|
| **3. xBRZ + snap** | Uniquified-RGB window, at least 2 source px of context | `xbrz::scale(6, ColorFormat::rgb, default ScalerCfg)` | native | Then local snap to indices (§2.6). Index-exact variants: MMPX2x→Scale3x, Scale2x→Scale3x. |
| **2. SR model** | 1x RGB window (raw pixels; pixel-art models expect them) | spandrel 0.4.2 (2026-02-21, MIT) + 4x-NXbrz, or 8x-Arzenal | 4x → **Lanczos 1.5x**; 8x → **box 0.75** | For pure-conv models on periodic sheets, set `Conv2d.padding_mode='circular'`: exact infinite wrap [M]. 8x models: keep inputs ≤ 384 px or use fp16; a 512 px input to Arzenal needs 16.6 GB [M]. |
| **1. Diffusion** | Route 3 (or material hybrid) at 6x → Lanczos 2x = **12x**, so 1 source px = 1.5 latent cells [A] | SDXL + xinsir Tile, about 1-1.5 MP canvases, 28 steps, cfg 5, dpmpp_2m karras [A] | **box 2:1** | Per-class denoise / CN strength [A]: water 0.30-0.40 / 0.8-0.9; grass, dirt, sand, swamp 0.40-0.55 / 0.6-0.8; floors and roads 0.25-0.35 / 0.85-1.0; transition tiles 0.30-0.45 (inpainting, §2.5). Fixed seed and prompt per class. Put all frames of a shape on one canvas. |
| **Pilot: hosted** | 256 px window pre-upscaled to the 2K tier (8x) | Image edit with a "keep every shape, edge and colour region" prompt [A] | **area 8→6** | Pilot and golden set only (§3.4). |

### 2.5 Stage 3: seam handling

1. **Crop:** throw away the apron and keep only the chunk, or the centre period of a macro sheet.
2. **Cut instances and take a consensus per `(shape, frame)`.**
   - Index routes: per-pixel **mode**.
   - RGB routes: per-pixel **OKLab median or medoid**, weighted by occurrence, followed by Stage 4.
   - Context effects sit only in the border band: 2 source px for xBRZ, about 6 output px for the SR models [M]. Interior pixels do not depend on context.
3. **Diffusion macro sheets** [A]:
   - Check the seam with a half-period roll.
   - If a seam shows: roll by half a period, inpaint a band 1-2 source px wide across the seam cross at low denoise, and roll back.
4. **Diffusion transition tiles** [A]:
   - Inpaint each one inside its most frequent 3x3 context, using finished hi-res neighbours (`DifferentialDiffusion` plus a soft mask).
   - Go in dependency order: tiles whose neighbours are already finished come first.
5. **Optional border lock** [A]:
   - For generative tiles, blend the outer **k ≤ 3** px towards the route-3 consensus tile, or towards an xBRZ result computed with the real neighbour.
   - Larger k creates a visible frame.
6. **Optional chunk-template overrides.**
   - Keep the 768² chunk result whole. That removes every seam inside the chunk; only chunk borders (12.5 % of tile edges) can still seam.
   - Size: about 1.24 GB raw for 2,105 chunks [A].
   - It fits Exult's per-terrain `rendered_flats` cache. The cache key must include a hash of the chunk's 256 ShapeIDs, so that map edits invalidate the override.

### 2.6 Stage 4: palette quantization that respects the reserved indices

1. **Colour lock (RGB routes), before quantizing** [A, not yet measured on U7]:
   - **Back-projection:** `x' = x + U(y − D(x))` in linear RGB.
     - `y` = the source tile, `x` = the 6x result, `D` = the 6x6 box mean, `U` = nearest-neighbour upsampling (iterate with bicubic `U` if blocks show).
     - Afterwards every 6x6 block averages exactly to its source colour. Low-frequency seams then equal the 1x seams, and an engine downscale reproduces the original art.
   - **Alternative:** wavelet colour fix against an NN-6x copy of the original.
2. **Cycling pixels first.**
   - Mask `M` = source pixels with index ≥ 0xE0.
   - Upscale the *index plane* with a label-safe method:
     - route 3's xBRZ + snap on a uniquified palette, which turns sparkles into round glints [M];
     - or MMPX2x→Scale3x;
     - plain NN only as a last resort, because it makes 6x6 blinking squares.
   - Write those indices into the tile.
   - Exclude these pixels from back-projection and from the static quantizer.
3. **Static pixels.** Candidates are the indices present in the source pixel's 3x3 neighbourhood (**local snap**), chosen by OKLab distance.
   - Option: **ramp-constrained.** Allow any index from the ramps of those candidates. That gives more shading steps while staying in the same material and hue (`Palette::get_ramps`, about 17 ramps).
   - Fallback: global OKLab nearest over 0x01-0xDF.
   - Local quantization raised round-trip agreement by 1.6-9.4 points for every pixel-art SR model, though it fell for 4x-Struzan (−7.8) and PBRify (−1.1) [M, `sr_models.md` table 4.1]. It also keeps the region to the original's 56 colours instead of spreading over 110-200 [M].
4. **Dithering.** None by default.
   - If banding appears, use **ordered** dithering (Bayer 8/16, or tileable CC0 blue noise). Both divide 48, so align the pattern to the tile.
   - Keep the dither inside the source pixel's ramp.
   - **Never use Floyd-Steinberg** across tiles: it is order-dependent and creates seams [V, A].
5. **Implementation.** Use our own NumPy mapper: exact nearest over unique colours, or a 64³ LUT.
   - `algorithmic_code/u7algo.py` already has `nearest_index`, `local_snap`, `unique_rgb_palette`, `xbrz_rgb`, `scale2x/3x` and `mmpx2x_idx`.
   - Do **not** use Pillow `quantize` for final art. Its search is approximate: 97.1 % agreement with exact nearest on real data and 91.5 % on synthetic data. With a 256-entry palette, 0.33 % of pixels land on index ≥ 224 [M].
   - Do not use ImageMagick `-remap`: it matches in RGB only [V].
   - Do not use libimagequant: it may change the palette during remap, and it is GPLv3 [V].

### 2.7 Stage 5: cutting back to per-frame 48x48 PNGs

Files to write:

- `art_6x/shapes/flat/SSSS_FF.png`: **48x48, mode P, PLTE = palette 0, raw indices, no tRNS.** This mirrors the `art_original` layout.
- `art_6x/shapes/flat/SSSS_FF.json`: hash of the 64 source indices, route, model id and SHA-256, seed, parameters, number of instances in the consensus, and the QA metrics.
- Optional: `art_6x/chunks/chunk_NNNN.png` (768²) plus the hash of the chunk's 256 ShapeIDs.
- `art_6x_raw/`: the unquantized 6x RGB consensus tiles and the raw 8x/12x model outputs.

Pack key: `(game, shape, frame)`, guarded by the source-index hash. Identity keys follow the Mesen CHR-ROM and Daggerfall Unity pattern; the hash guard follows diablo1-4k and Dolphin [V]. The guard stops a BG pack from being applied silently to SI or to a modded `shapes.vga`.

Size: 3,885 × 48² ≈ 8.9 MP, about 9 MB raw [M].

### 2.8 Stage 6: QA

**Gates are [A] proposals, calibrated against the measured baselines [M].** Compare a metric only within its own definition. Round-trip metrics favour blur (bicubic scores 87.6 % on metric B2), so they act as **gates, not rankings**.

| # | Check | Definition | Baseline [M] | Proposed gate [A] |
|---|---|---|---|---|
| A1 | Format | 48x48, mode P, PLTE == palette 0 (via `v*255//63`), no tRNS | — | Hard fail |
| A2 | Static palette compliance | Every non-cycling pixel ∈ 0x01-0xDF (0x00 only if the source uses it); no 0xFF | — | Hard fail |
| A3 | Cycling compliance | Every pixel ≥ 0xE0 lies inside the 6x cycling mask and is in the same cycle range as its source pixel. Every source cycling pixel still has ≥ 1 cycling pixel in its 6x6 block. | xBRZ + snap keeps 6,955 of 9,468 NN cycling pixels (the sparkles are reshaped, not dropped) | Hard fail on range violation; warn on a dropped sparkle |
| B1 | Block-majority consistency | 6x6 block majority index == source index | Scale2x→3x 98.9 %; MMPX→3x 99.3 %; xBRZ + snap 98.9 %; xBRZ + global remap 97.7 % | ≥ 97 % per tile |
| B2 | Box-mean round trip | Box-downscale the 6x RGB, nearest index == source (% of non-cycling pixels) | NXbrz 80.5 %; xbrz 82.1 %; UltraSharpV2 80.8 %; Arzenal 65.0 % (local quantize) | Per route; flag the bottom 5 % of tiles |
| B3 | Block colour error | Block-mean OKLab ΔE against source, mean / p99 | xBRZ + snap 0.0076 / 0.057; MMPX→3x 0.0046 / 0.049; xBRZ global 0.0086 / 0.089 | p99 ≤ 0.10 |
| B4 | Ramp identity | Share of blocks whose dominant ramp == the source pixel's ramp | — | ≥ 95 %; hard fail when a block drifts to a different ramp |
| C1 | Seam gradient ratio | Mean OKLab ΔE across tile-boundary pixel pairs ÷ mean ΔE across other neighbour pairs, on world renders built from the **final tiles**, compared with the same route's region-level render | xBRZ, window A (B): region 3.77 (5.00), consensus 4.46 (5.90), per-tile wrap 6.51 (7.74), per-tile clamp 7.80 (9.36) | ≤ 1.2 × the region value (xBRZ consensus scores 1.18×) |
| C2 | Edge-pair check | Edge-strip ΔE for every (A right / B left) and (top / bottom) pair that **occurs on the map** (60,737 unique cross-shape tile pairs [M]) | — | Repair or regenerate the worst 1 % |
| D1 | Instance disagreement | % of 48x48 pixels where instances differ from the consensus (index routes), or per-pixel std (RGB routes) | xBRZ 2.1-2.4 %; NXbrz std 1.3-1.5; others 4.75-8.74 | Flag frames above 2× the route median: possible hallucination or context dependence |
| E1 | Registration (generative routes only) | Phase-correlation shift of the box-downscaled output against the source | — | ≤ 0.25 source px |
| F1 | Human review | Contact sheets per material family (1x NN, route 3, candidate); 6x world previews of about 50 representative chunks (coast, town, roads, swamp, floors) shown at 6x and downscaled to the real display target; half-period roll sheets for macro-textures | — | Accept or override per frame |

Prior art says to budget manual work for two groups first: text-like tiles, and the shoreline and water families [V].

---

## 3. Where each part runs

Windows work folder: `E:\Dati\Ultima7_Upscale`, which is `/mnt/e/Dati/Ultima7_Upscale` from WSL.

### 3.1 WSL, CPU only: ready now

| Task | Tool / location | Status / speed |
|---|---|---|
| Extraction, world tile grid, context windows, macro sheets | Python + NumPy (`tmp/algo/venv`, Python 3.11) + `u7algo.py` | Extraction done. Reader code exists. |
| xBRZ 1.9 at 6x | `tmp/algo/xbrz19/libxbrz19.so` (built with GCC 9.4 after the one-line `[&]() -> bool` patch) [M] | 26-51 ms per 768² source; 1-2.2 ms per 160² window [M]. The whole map takes minutes. |
| MMPX 2x, Scale2x/3x | `tmp/algo/mmpx/libmmpx.so` (clamp bug fixed); NumPy Scale2x/3x [M] | 0.04-0.14 s per 768² [M] |
| Local snap, material-boundary hybrid | `u7algo.local_snap`, `algorithmic_code/exp9_material.py` | About 9 s per 768² in NumPy [M]. Vectorize it or port it to C for the full set. |
| Quantizer, consensus, QA harness, contact sheets | NumPy / Pillow (Pillow only for PNG I/O) | To be written |
| SR models on CPU (route 2) | `tmp/sr_bakeoff/venv` (torch 2.14.1+cpu, spandrel 0.4.2), models in `tmp/sr_bakeoff/models/` (SHA-256 checked) | 5-15 s per 128 px window [M]. About 3-9 h for 2,105 chunks [A]. Prefer the GPU route in 3.2. |

### 3.2 WSL with CUDA: already working, no Windows install needed for route 2

`tmp/factcheck/venv_cu130` (torch **2.14.1+cu130**) sees the **RTX 5070 Ti (sm 12.0)**. Its arch list includes sm_120, and CUDA matmul and conv run [M].

So route 2 (spandrel + NXbrz/Arzenal) can run on the GPU from WSL straight away. At about 0.06-0.3 s per 160 px window [M], all 2,105 chunks take minutes.

Keep large model files on the WSL ext4 disk. Reading multi-GB files over `/mnt/e` is slow [A].

Driver facts [V]: the Windows driver is **617.14** and the CUDA UMD is 13.4. "615.78" is the version of WSL's `nvidia-smi` userspace, not the driver.

### 3.3 Windows GPU: route 1 (diffusion) and the GUI tools

**PyTorch facts for the RTX 5070 Ti** [V, wheel indexes fetched 2026-10-04]:

- Blackwell sm_120 has been supported since torch 2.7 (cu128, April/May 2025).
- The newest torch is **2.14.1**, published on the **cu130** and **cu132** indexes.
- The **cu128** index is frozen at 2.11.0.
- **cu126 has no sm_120**, even though it carries 2.14.1.
- Use `--index-url https://download.pytorch.org/whl/cu130`.

**(a) ComfyUI (route 1).** Windows portable; the README says the nvidia build is Python 3.13 + CUDA 13.0 [V].

```bat
:: 0) Folders
mkdir E:\Dati\Ultima7_Upscale\work\in E:\Dati\Ultima7_Upscale\work\out E:\Dati\Ultima7_Upscale\work\workflows E:\Dati\Ultima7_Upscale\work\qa
:: 1) Download ComfyUI_windows_portable_nvidia.7z from https://github.com/Comfy-Org/ComfyUI/releases
::    and extract to E:\Dati\Ultima7_Upscale\ComfyUI_windows_portable
::    Do NOT use ComfyUI_windows_portable_nvidia_cu126.7z (10-series only, no Blackwell).
:: 2) Check that the build supports Blackwell
cd /d E:\Dati\Ultima7_Upscale\ComfyUI_windows_portable
.\python_embeded\python.exe -c "import torch;print(torch.__version__, torch.cuda.get_device_name(0), torch.cuda.get_arch_list())"
::    expect a +cu130 build and 'sm_120' in the list
:: 3) Models (ComfyUI\models\...)
::    checkpoints\sd_xl_base_1.0.safetensors     <- huggingface.co/stabilityai/stable-diffusion-xl-base-1.0
::    vae\sdxl_vae_fp16_fix.safetensors           <- huggingface.co/madebyollin/sdxl-vae-fp16-fix (optional)
::    controlnet\xinsir_tile_sdxl.safetensors     <- huggingface.co/xinsir/controlnet-tile-sdxl-1.0 (diffusion_pytorch_model.safetensors, renamed)
::    upscale_models\4x-NXbrz.pth, 8x-Arzenal-v1-1.pth  <- copy from WSL tmp/sr_bakeoff/models
:: 4) Custom nodes (git clone or download the zip), then install each requirements.txt if present
cd ComfyUI\custom_nodes
git clone https://github.com/spinagon/ComfyUI-seamless-tiling
git clone https://github.com/numz/ComfyUI-SeedVR2_VideoUpscaler
cd ..\..
.\python_embeded\python.exe -m pip install -r ComfyUI\custom_nodes\ComfyUI-SeedVR2_VideoUpscaler\requirements.txt
:: 5) Start, listening for the WSL driver script
.\python_embeded\python.exe -s ComfyUI\main.py --windows-standalone-build --listen 0.0.0.0 --port 8188 ^
    --input-directory E:\Dati\Ultima7_Upscale\work\in --output-directory E:\Dati\Ultima7_Upscale\work\out
```

Notes on ComfyUI:

- **Reaching it from WSL.**
  - Default NAT networking: use `WINHOST=$(ip route | awk '/default/{print $3}')` and add a firewall rule for TCP 8188.
  - Or set `networkingMode=mirrored` in `%UserProfile%\.wslconfig` and call `localhost:8188`.
  - Automation uses the HTTP API: `POST /prompt`, `GET /history/{id}`, `GET /view`, `POST /upload/image`, `ws /ws` [V].
- **Optional models:**
  - Z-Image-Turbo plus `Z-Image-Turbo-Fun-Controlnet-Tile-2.1-2601-8steps.safetensors` from `alibaba-pai/Z-Image-Turbo-Fun-Controlnet-Union-2.1` [V].
  - SeedVR2 3B weights through the numz node (`numz/SeedVR2_comfyUI`) [V]. VRAM: only tiers are published, "12-16 GB with BlockSwap/VAE tiling/GGUF" [V]; there is no FP8-specific figure [U].
- **Stability on Blackwell** [V, A]:
  - Pin torch once the setup works. Portable updates have replaced torch in the past.
  - Do not mix cu126 and cu13x wheels.
  - Leave SageAttention, `torch.compile` and NVFP4 off until the plain SDPA pipeline gives correct images.
  - Custom nodes that ship CUDA extensions built before 2025 fail on sm_120.
- **Licence:** the spinagon seamless node is GPL-3.0 [V].

**(b) chaiNNer (interactive look development; optional).**

1. Install **v0.25.1** (2025-10-23, still the latest release; the last commit on main was 2026-07-31) from https://github.com/chaiNNer-org/chaiNNer/releases into `E:\Dati\Ultima7_Upscale\chaiNNer`.
2. Open the *Dependency Manager* and install or update PyTorch. The v0.25.0 notes say this is required for the RTX 50 series [V].
3. Useful nodes:
   - *Upscale Image*, with Padding = **Wrap** for periodic sheets;
   - *Quantize to Reference*;
   - *Separate Alpha*, for sprites later.
4. Limits: it has **no xBRZ or MMPX** (issue #2422 is open). Its CLI (`run <chain>.chn --override <json>`) is experimental [V].

**(c) spandrel venv on Windows.** Optional, since WSL CUDA already works.

```bat
py -3.13 -m venv E:\Dati\Ultima7_Upscale\venv
E:\Dati\Ultima7_Upscale\venv\Scripts\python -m pip install torch torchvision --index-url https://download.pytorch.org/whl/cu130
E:\Dati\Ultima7_Upscale\venv\Scripts\python -m pip install spandrel pillow numpy safetensors
```

**(d) traiNNer-redux (later, for the custom model).**

- Apache-2.0; last commit 2026-10-03; needs `torch>=2.11`; Python 3.13 recommended, 3.12 supported, **3.14 not supported** [V].
- Install: `git clone https://github.com/the-database/traiNNer-redux` → `install.bat`. The script uses the cu128 index, which gives torch 2.11.0; that still satisfies the requirement.
- Train: `python train.py --auto_resume -opt options\train\<ARCH>\<cfg>.yml`.
- Loss: add `averageloss` (an AvgPool downscale-consistency loss) to enforce the round-trip constraint [V].

**First GPU session (checklist)** [A, from `diffusion.md` §11]:

- Shapes: 19 (water), 147 (grass), 149 (dirt), one floor shape, and 10 (sand + shore).
- Base image: NN-12x against xBRZ-6x upscaled 2x.
- Model: SDXL + Tile, SeedVR2-3B, and Z-Image + Tile.
- Denoise: {0.3, 0.4, 0.5}; 2 seeds each.
- Run the same post-processing and QA harness on all of them.
- **Measure VRAM and seconds per canvas.** That replaces the [U] estimates.

### 3.4 OpenRouter (hosted): pilot and golden set

**The MCP connector cannot do the batch work.**

- In this session it exposes only `authenticate` and `complete_authentication`, so it is not authorized.
- Its `generate-image` tool is documented as text-to-image. Whether it accepts reference images is undocumented [V, docs].
- Its key expires after 7 days and has a $10 default spend cap [V].
- Use it for catalogue lookups at most.

**For batch work, use the REST API:** `POST https://openrouter.ai/api/v1/images` [V].

- Input goes in `input_references` as data-URL PNGs; output comes back as `b64_json`.
- Billing is all-or-nothing: a failed generation is not billed.
- Use your own API key with a hard credit limit. A client sketch is in `api_models.md` §7.
- **There is no exact output size.** You get tiers only; `size` is declared only by `inclusionai/ming-*`. So send an 8x pre-upscaled input at the tier size (256 px window → 2K) and area-downsample 8→6 locally.

**Model ids** (live catalogue, 2026-10-04 [V]):

| Model id (date added to OR) | Seed | Refs | Price | Role |
|---|---|---|---|---|
| `google/gemini-3.1-flash-image` "Nano Banana 2" (2026-06-18) | no | 0-14 (Google: at most **3 style** refs) | 512 $0.045 · 1K $0.067 · 2K $0.101 | Main pilot candidate |
| `google/gemini-3-pro-image` "Nano Banana Pro" (2026-06-18) | no | 0-14 (Google: ≤ 6 high-fidelity object refs) | 1K/2K $0.134 · 4K $0.24 | Golden or style set only |
| `black-forest-labs/flux.2-klein-4b` (2026-01-14) | **yes** | 0-4 | $0.014/MP | **Bridge model**: Apache-2.0 open weights, about 13 GB, so the same model can run locally |
| `black-forest-labs/flux.2-pro` (2025-11-25) | yes | 0-8 | $0.03/MP | Pilot |
| `bytedance-seed/seedream-4.5` (2025-12-23) | yes | 0-14 | $0.04 flat, input free | Pilot |
| `bytedance-seed/seedream-5-0-flash` (2026-10-01) | yes | 0-14 | $0.018 flat | Cheapest seeded editor |
| `qwen/qwen-image-3` (2026-08-05) | yes | 0-4 | $0.03 output + $0.003 per input image | Pilot |

**Avoid:**

- `google/gemini-2.5-flash-image`: Google shut it down on 2026-10-02 [V].
- `openai/gpt-image-2` for sprites: it has **no transparent background**. Only `gpt-image-2.5-sunburst` and `-flare` have one [V].
- `bytedance-seed/seedream-5-0-pro` at 2K: it costs **$0.09**, not $0.045 [V].

**Estimated cost for about 2,000 tiles.** These are list prices from the OR endpoints and exclude retries; expect 1.5-3x more.

| Model | (a) Atlas: 64 tiles in 3x3 context per 2K request, about 32 requests | (b) One 1K request each, about 2,000 requests | (c) One 2K request each (chunk + 64 px margin), about 2,000 requests |
|---|---|---|---|
| gemini-3.1-flash-image | ≈ $3.2 | ≈ $134 | ≈ $202 |
| gemini-3-pro-image | ≈ $4.3 | ≈ $268 | ≈ $268 |
| flux.2-klein-4b | ≈ $1.9 | ≈ $29 | ≈ $118 |
| flux.2-pro | ≈ $4.0 | ≈ $63 | ≈ $252 |
| seedream-4.5 | ≈ $1.3 | ≈ $80 | ≈ $80 |
| seedream-5-0-flash | ≈ $0.6 | ≈ $36 | ≈ $36 |
| qwen-image-3 (1 input image) | ≈ $1.1 | ≈ $66 | ≈ $66 |

Reading the table:

- Option (a) is cheap, but packing makes registration and cell bleed harder to control.
- Option (b) means one request per chunk at 8x. A single tile per request is not useful: the minimum output is 512-1024 px, and a lone tile has no context.
- Option (c) gives the best seams.
- Pilot: 20 windows × 6 models × 3 prompts = 360 requests at 1K. That costs $5 (klein) to $49 (Nano Banana Pro) per model; cap the whole pilot at **≤ $25**.
- Cost is not the obstacle. The obstacles are faithfulness, geometry drift, seams, consistency between requests and reproducibility.
- All Gemini outputs carry SynthID [V]. Uploading copyrighted U7 art to third parties is a grey area; it is acceptable for a private pilot only.

---

## 4. Claims to avoid

### 4.1 Refuted by the fact-check: use the corrected version

| Claim seen in the reports or elsewhere | Correction (evidence) |
|---|---|
| "traiNNer-redux trains only scales 1/2/3/4/8, so there is no native 6x" | `scale` is a free integer. SPAN and RealPLKSR build at 6x. Only ESRGAN (power-of-2 upsampling) and Real-CUGAN are restricted. Training at 6x has not been run. |
| "ESRGAN handles 512-1024 px tiles easily on 16 GB"; "16 GB is never the constraint" | 8x-Arzenal with a 512 px input peaks at 16.6 GB, spills to shared memory and takes 6.5 s. 1024 px fp32 tiles fit no model. Use ≤ 384 px inputs or fp16 for 8x models. Chunk-sized windows are trivial (< 2 GB, < 0.3 s). |
| "`utility_z_image_turbo_2k_upscaler` is the Z-Image + Tile ControlNet path" | The template runs RealESRGAN_x4plus → Lanczos 0.5x → Z-Image img2img (5 steps, cfg 1, denoise 0.33). It has **no** ControlNet. |
| "Exult's `scale_xbr.cc` is 2x-only" and "no Exult scaler reaches 6x" | Exult registers 2xBR, 3xBR and 4xBR. **Point, Interlaced and Bilinear accept any factor, including 6x**, so a 6x point fallback already exists. No *edge-aware* Exult scaler goes beyond 4x. |
| "Qwen-Image-Edit is reachable through OpenRouter" | It is not. OpenRouter offers only `qwen/qwen-image-3` and `qwen/qwen-image-3-pro`. |
| "Qwen-Image 2.1 is Apache-2.0" | It is under the Qwen Research License. Qwen-Image-Edit-2511 is Apache-2.0, but it is 20B. |
| "gpt-image-2 supports a transparent background" | Only `gpt-image-2.5-sunburst` and `-flare` do. |
| "Seedream 5.0 Pro is $0.045 at 1K and 2K" | It costs $0.09 above about 2.36 MP, so at 2K. (The threshold comes from a third-party source.) |
| "xBRZ is the only mature scaler with native 6x" | OmniScale, cleanEdge, xbrz-freescale and super-xbr scale by any factor, but they exist only as GPU shaders. xBRZ is the only maintained **CPU library** with a dedicated 6x kernel. `xbrz-rs` (GPL-3.0) is a 6x CLI route. |
| "didder v1.3.0 is from 2025-10; `sudo apt install didder` works" | v1.3.0 was released 2023-12-20. The apt package exists only from Debian trixie and Ubuntu 25.10, and this WSL is Ubuntu 20.04. |
| "4x-PixelPerfectV4 is the only permissive pixel-art model" | 4x-Skyrim-Alpha (CC0) and 1x-ArtClarity (WTFPL) are permissive too, and some GPL-3.0 models allow commercial use. PixelPerfectV4 is the only permissive 4x **sprite** model, and it was trained on anime images. |
| "No pixel-art-tagged model after 2021" | 4x-HDCube (2022) carries the tag but says it is unsuitable for pixel art. The substance holds: nothing *aimed at* pixel art since 2021. |
| "chaiNNer is active in Oct 2026" | Its last release is v0.25.1 (2025-10-23) and its last commit on main is 2026-07-31. |
| "Install torch from the cu128 index" | It works, but it caps torch at 2.11.0. Use cu130 (2.14.1). |
| "This PC has driver 615.78" | The Windows driver is 617.14. 615.78 is the version of WSL's `nvidia-smi`. |
| Minor | Real-ESRGAN's last master commit is 2024-04-03. The ScaleFX preset has 6 passes. The ImageMagick magnify commit was cited wrongly; the feature itself is confirmed by the ChangeLog after 7.0.8-53. mesen.ca still shows HD-pack `<ver>105`, while 106 appears only in the GitHub docs. `randomBackground` and `<addition>` exist only in the mkwong98 fork. spandrel's README does not say "50+ architectures". |

### 4.2 Unverified or unverifiable: measure or confirm before relying on these

- VRAM and speed of SDXL + xinsir Tile on the 5070 Ti ("8-10 GB, 3-6 s per canvas"), and every diffusion throughput estimate (77 macro sheets in about 5-8 min per seed; 2,105 chunk canvases in about 3-5 h).
- FP8-specific VRAM for SeedVR2. Only tiers are published.
- Every diffusion parameter: per-class denoise and CN strength, 12x working scale, back-projection, seam-healing by inpainting. These are design proposals, not results.
- That a native-6x custom model trains well; training time ("hours to about a day").
- Retro Diffusion RD Tile's X/Y wrap, palette control and pricing.
- That wrap padding is the "documented" xBRZ workaround for tileable textures. A user suggested it, not the author. That cleanEdge adds no new colours.
- The original MMPX licence. Only a port's README says MIT, and the JCGT paper quotes were not re-checked.
- That GCC ≥ 11 builds xBRZ 1.9 without the patch.
- That OpenRouter's MCP `generate-image` accepts reference images. That `provider.data_collection` and `zdr` are honoured by the Images endpoint.
- That Qwen-Image 3.0 is closed weights. This rests on third-party blogs only.
- Magnific pricing.
- The minimum image size of gpt-image-2 (655,360 px).
- That the "Universal Seamless Tiles" node works for DiTs. It has a single commit, dated 2026-07-14.
- That Raze recolours indexed hightiles in its renderer. Only the parser was traced.

### 4.3 Caveats on the measured numbers

- "77 macro shapes" depends on a > 60 % threshold; it becomes 80 at > 50 %. Only 74 of them have all 32 frames.
- "33 self-surrounded tiles" counts all 8 neighbours. With the 4 orthogonal neighbours only, it is 51.
- `manifest.json` gives 3,120 used flats and the statistics script gives 3,121. One used tile id points to a frame that does not exist in `shapes.vga`.
- The SR bake-off ran on CPU on a single 96x96 mixed-terrain window, plus one water window and 6 tiles × 40 instances. That is indicative, not a full-tileset ranking.
- The xBRZ seam and consistency figures come from two 4x4-chunk windows, A and B.
- `sr_models.md` §0 says that local quantization helped "every model". Its own table 4.1 shows a drop for 4x-Struzan and PBRify. The fact-check missed this, and the corrected wording is in §2.6.

---

## 5. Licensing notes

- **Offline tools** do not affect Exult's licence.
- **Runtime xBRZ** makes the combined binary GPLv3. Exult is GPLv2+, so this is allowed, but it is a project decision [V].
- **GPLv2-compatible runtime alternatives:**
  - Exult's own xBR 2-4x (GPLv2+);
  - Exult's Scale2x and hq scalers (LGPL v2+);
  - MMPX + Scale3x;
  - Exult's Point and Bilinear, which already accept 6x [V].
- **Models:**
  - 45 of the 53 pixel-art SR models are non-commercial [V].
  - SDXL is CreativeML Open RAIL++-M.
  - xinsir Tile, SeedVR2, Z-Image and FLUX.2-klein-4B are Apache-2.0 [V].
  - FLUX.1-dev, FLUX.2-dev, klein-9B, SUPIR and HYPIR are non-commercial [V].
- **The art itself** is derived from EA/Origin data. Distribute the pipeline, not the generated pack; users build packs locally from their own files. This is what diablo1-4k and OpenRA do [V].

---

## 6. Next steps (in order)

1. **WSL, now.** Stage 1 tooling: the world tile-id grid, chunk windows with real-neighbour aprons, class and ramp planes, and macro sheets.
2. **WSL, now.** Route 3 end to end on all 3,885 frames: xBRZ 6x + local snap + mode consensus, plus the material-hybrid variant. Then the QA harness of §2.8.
   - Result: a complete, valid `art_6x/` baseline that the engine work can use and test against.
   - The same run calibrates the QA gates.
3. **WSL + CUDA, now.** Route 2: NXbrz, and Arzenal for comparison, on the 2,105 chunk windows. Then consensus, local quantization, and QA against route 3.
4. **Engine side.** Load `art_6x/` through the override path. Check that a 6x render downscaled to the actual window still matches the 1x art; back-projection guarantees this for RGB routes.
5. **Windows.** Install ComfyUI as in §3.3 and run the first GPU session on 5 shapes. Measure VRAM and time, and pick the route-1 stack.
6. **Optional.** OpenRouter pilot for ≤ $25 with a REST key, then a golden set of 20-50 chunks for IP-Adapter or style-LoRA references.
7. **Curation.** Train a style LoRA, or a custom native-6x SPAN/RealPLKSR model, so the curated look applies deterministically to the whole set.

---

## Sources

- The reports in this folder, each with its own source list and a "Fact-check" section dated 2026-10-04: `sr_models.md`, `diffusion.md`, `api_models.md`, `algorithmic.md`, `prior_art.md`.
- OpenRouter catalogue snapshot: `openrouter_image_models_2026-10-04.json`.
- Experiment code and images: `algorithmic_code/`, `algorithmic_img/`; `/home/simonea/ultima7_exult/tmp/sr_bakeoff/` (`bakeoff.py`, `seams.py`, `out/*_metrics.json`); `/home/simonea/ultima7_exult/tmp/algo/` (built `libxbrz19.so` and `libmmpx.so`); fact-check scratch in `/home/simonea/ultima7_exult/tmp/factcheck/`.
- Engine constraints: `docs-hires/analysis/palette.md` (allowed indices, cycling ranges, PNG I/O caveat) and `docs-hires/analysis/world.md` (chunk-terrain cache).
