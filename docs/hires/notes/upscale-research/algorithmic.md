# Algorithmic and hybrid pixel-art upscalers and palette tools for the Exult 6x override

Research date: 2026-10-04. Scope: the non-AI side of the 6x (8x8 to 48x48 per terrain tile) art pipeline. This covers deterministic pixel-art scalers, palette quantization and remapping to the fixed U7 palette, seamless tiling, and how to use the deterministic scalers as pre-passes or post-passes around AI models.

Constraints come from `docs-hires/analysis/palette.md`:

* The art is indexed against palette 0 of `palettes.flx`.
* The quantizer may only emit `0x01-0xDF`.
* Cycling indices `0xE0-0xFE` must be copied from the source mask, never produced by the quantizer.
* Flats must not use `0xFF`.
* Flats tile edge-to-edge.

Verification legend, used throughout:

* **[F]**: verified by fetching the page or file, or by querying PyPI/GitHub/SourceForge metadata.
* **[E]**: verified by a local experiment on the real BG data (section 3).
* **[S]**: search snippet only, not independently checked.

---

## 0. TL;DR

1. **There is a usable deterministic 6x path today, and it can be index-preserving.** Four candidates:
   * **xBRZ 6x**: native 6x. Blends colours, so it needs re-indexing.
   * **Scale2x then Scale3x (AdvMAME/EPX)**: 2x then 3x gives 6x. Rules use pixel equality only, so it runs directly on the index plane.
   * **MMPX 2x then Scale3x**: copies source pixels only.
   * **ScaleFX 3x then 2x**: copies source pixels only. Shader-based.

   The last three never invent colours or indices. They keep cycling indices, and they keep the 0/252 and 15/224/232/248 duplicate-colour indices apart. This last point matters because RGB-to-index round-trips are ambiguous in the U7 palette [E].
2. **Best deterministic quality: xBRZ 6x followed by "local-palette snap"** (my name for the step). Each 6x pixel is re-indexed to whichever source index in the 3x3 neighbourhood of its source pixel is nearest in OKLab.
   * You get xBRZ's smooth geometry (curved shorelines, rounded water glints).
   * The output uses only indices that already exist locally, including cycling ones.
   * It is closer to the xBRZ RGB result than a global remap: mean OKLab ΔE 0.0017-0.0019 versus 0.0024-0.0035 [E].
3. **The big weakness of every edge-directed scaler on U7 terrain:** grass and dirt are high-frequency 1-px noise textures. xBRZ, Scale2x/3x, MMPX and ScaleFX all turn that noise into "worms" or camouflage blobs.
   * xBRZ parameters do not fix this. `equalColorTolerance` 10-120 and the direction thresholds all give the same result [E].
   * About 86% of 1x terrain pixels sit inside a single colour ramp, so they are "texture interior", not edges [E].
   * **Recommendation:** use the algorithms for *structure* (material boundaries: shore, road, cliff, floor edges) and let the AI or diffusion path, or plain NN, own *texture interiors*. The "material-boundary xBRZ" prototype in section 3.6 does exactly this deterministically. It is the ideal *mould* or *init* for a diffusion img2img pass.
4. **Seams.** Upscaling each 8x8 frame in isolation (edge clamp) roughly doubles boundary contrast compared with upscaling the whole map region. Wrap padding helps only a little. Measured gradient ratio at tile borders against the interior, for xBRZ: region 3.8 / 5.0, per-tile wrap 6.5 / 7.7, per-tile clamp 7.8 / 9.4 [E].
   * **Per-frame consensus** fixes most of this: upscale whole map regions with real neighbours, slice, and take the per-pixel mode across all instances of a `(shape, frame)`. Result: ratio 4.5 / 5.9, mean ΔE against the context-true result 0.0008-0.0011 versus 0.0029-0.0031 for clamp [E].
   * Instances of the same frame disagree on only about 2% of their 48x48 pixels [E]. A single per-frame override is therefore viable.
   * The flats are very context-rich: 3121 distinct flat frames are used by the map, with a median of 13 and a p90 of 177 distinct 4-neighbour contexts [E].
5. **Palette mapping: write your own 30-line NumPy mapper.** Use OKLab, an explicit allowed-index list (`1..223`), and an exact nearest search over unique colours.
   * Pillow's `quantize(palette=…)` can be restricted to 224 entries (it then never emits >223) [E]. But its nearest-colour search is approximate: it agrees with exact RGB-nearest on only 97.1% of pixels [E]. It is also RGB-only and cannot exclude index 0.
   * ImageMagick `-remap` matches in RGB only, and `-quantize <colorspace>` does not affect remapping [F].
   * libimagequant (pngquant) is built to *create* palettes. With fixed colours it may still "improve" the palette, so remapped colours are not guaranteed exact [F]. It is also GPLv3/commercial.
6. **Dithering.** None is needed for the algorithmic/index-preserving paths. For AI or true-colour outputs, use **positional (ordered) dithering with a world-aligned threshold map** (Bayer or tileable CC0 blue noise) with an arbitrary-palette method (Knoll, Yliluoma, N-closest). That is seam-free by construction. Floyd-Steinberg per tile is not seam-free.
7. **Performance (CPU, WSL, single thread).**
   * xBRZ 6x of a 768x768 source (6x6 chunks) takes 0.13-0.4 s [E]. The whole 24576² world would take a few minutes.
   * Scale2x then Scale3x in NumPy: 0.05 s per 768² [E].
   * MMPX (C99): 0.13 s [E].
   * Local snap in NumPy: about 9 s per 768² [E]. It is trivial in C++.

   A **runtime** option is realistic: run xBRZ plus local snap per chunk when the S=6 terrain cache is built. Section 5.6 has the details and the license caveat (xBRZ is GPLv3).

---

## 1. What the algorithmic side must deliver (recap of the constraints)

* Input: 8x8 index tiles (3885 BG flat frames in shapes 0-149; 3121 of them are referenced by the map's 2105 used chunk terrains) [E].
* Output: 48x48 **index** tiles that:
  * use `0x01-0xDF` for static colours;
  * keep cycling pixels on the original cycling indices (`E0-E7` water sparkles and so on; 369 BG flat frames contain `>=0xE0`) [E];
  * never use `0xFF`;
  * tile seamlessly;
  * stay faithful (block-wise they should still "be" the original pixel).
* Palette 0 contains exact duplicate RGB values [E]:
  * `{0, 252}` (black; static vs cycling)
  * `{15, 224, 232, 248}` (white; static vs three cycling ranges)
  * `{73, 194}` (two static entries)
  * `{225, 231}` (cycling)

  **Any pipeline that goes index → RGB → index without carrying the index plane will silently convert cycling whites and blacks into static ones, or the other way round.** That is the main argument for index-plane algorithms and for local snap.

---

## 2. Algorithm survey

### 2.1 Summary table

| Algorithm (year) | Native factors | Path to 6x | New colours? | Runs on index plane? | Transparency | License | Linux/WSL CPU implementation | Verif. |
|---|---|---|---|---|---|---|---|---|
| **xBRZ** (Zenju; 1.0 2013, 6x since 1.4 2015-07, **1.9 2026-01-24**) | 2-6 | **native 6x** | yes (edge blending); about 91% of output px remain exact palette colours on U7 terrain | no (YCbCr colour distance); use local snap to re-index | ARGB mode; since 1.8 treats outside as transparent | GPLv3, plus a linking exception for MAME, FreeFileSync, Snes9x, ePSXe | C++ source from SourceForge, about 1k LOC; `xbrz.py` (ctypes, AGPL-3.0, 2021, wheels only cp36-cp39); `xbrzscale` CLI (GPL-3, SDL2); Hawkynt ImageResizer (.NET, has "xBRZ 1.9 2x…6x" since 2026-09-21) | [F][E] |
| **xBR** (Hyllian 2011), incl. **noblend** variants | 2, 3, 4 (shaders: any) | xBR 3x then 2x, or the `xbrz-freescale` shader | lv2/lv3: yes; **noblend: no** (noblend lv2 is correct only at odd factors) | noblend: yes in principle | shader dependent | MIT/GPL mix (libretro); Exult's own `imagewin/scale_xbr.cc` is GPLv2+ | libretro shaders through `ra-pixelart-scale`; Exult's built-in 2x/3x/4x code | [F] (Exult src), [S] (noblend) |
| **Super-xBR** (Hyllian, about 2015) | 2 per pass | 3 passes = 8x, then downsample; or 2 passes = 4x | yes (interpolating) | no | partial | varies by port (not checked) | `py-super-xbr` (archived 2020); GIMP plugin; libretro shaders | [F] (repo metadata), [S] (behaviour) |
| **hqx** (Stepin 2003) | 2, 3, 4 | hq3x then hq2x (non-canonical) | **yes**; MMPX paper: "does not preserve the palette or sharpness… does not support transparency" | no | no | LGPL-2.1 | `pip install hqx` (pure Python, LGPL-2.1, v1.0 2023-02, RGB only, slow); chaiNNer "Resize Pixel Art" node (hq2x/3x/4x, Eagle, AdvMAME, SaI) | [F] |
| **Scale2x/Scale3x = EPX / AdvMAME** (1992/2001) | 2, 3 (4 = 2x twice) | **2x then 3x, or 3x then 2x** | **no**: copies source pixels, rules are pure equality tests | **yes, natively** | yes (alpha is just another value; index 255 too) | Scale2x reference GPL; trivial to reimplement (about 40 lines NumPy here) | ImageMagick `-define magnify:method=scale2X/scale3X -magnify` (IM7, since July 2019); `scalenx` PyPI (Unlicense, 2026-08, pure Python, slow); this report's NumPy version | [F][E] |
| **MMPX** (McGuire & Gagiu, JCGT 2021) | 2 (4 = twice) | MMPX 2x then Scale3x [E] | **no** ("preserves the exact palette, transparency, and single-pixel features") | yes, with a uniquified palette (section 3.1) | yes | MIT | C++/JS/GLSL reference supplement; `ITotalJustice/mmpx` C99 (MIT, 2021, **has a clamp bug, see pitfalls**); `mmpx-rs` (`cargo install mmpx`, MIT, 2024) | [F][E] |
| **ScaleFX** (Sp00kyFox 2016-17) | 3 (9x = twice) | ScaleFX 3x then Scale2x/MMPX 2x | **no** ("The filtered picture will only consist of colours present in the original"); the `-hybrid` and `+rAA` variants add AA | not directly (GPU shader on RGB); copies colours, so it round-trips with a uniquified palette | needs alpha "split" mode | MIT | libretro `slang-shaders/edge-smoothing/scalefx` (scalefx, scalefx-9x, scalefx-hybrid, scalefx+rAA); `ra-pixelart-scale -m scalefx-*` (needs OpenGL; `xvfb-run` headless) | [F] |
| **OmniScale** (Lior Halphon / SameBoy) | any, incl. non-integer | direct 6x | yes (anti-aliased); "Legacy" variant "minimal new colors" | no | omniscale (alpha) / legacy (no alpha) | MIT | GLSL/Godot shaders; `ra-pixelart-scale -m omniscale` | [F] (SameBoy page), [S] (licence) |
| **cleanEdge** (torcado 2022) | arbitrary | direct 6x | no new colours per author/snippets (designed for rotation) | not directly | yes | MIT | gist shader; web tool; in `ra-pixelart-scale` list | [F] (blog), [S] (palette claim) |
| **Kopf-Lischinski depixelization** (SIGGRAPH 2011) | vector → any | rasterize SVG at 6x | yes (smooth shading regions) | no | n/a | libdepixelize GPLv2+ (Inkscape "Trace Pixel Art") | Inkscape GUI; several research C++ repos (incomplete or old) | [F] (repos), [S] (CLI) |
| PPSSPP "Hybrid" / deposterize | xBRZ plus bicubic in smooth areas | n/a | yes | no | yes | PPSSPP licence (GPL family; not checked) | inside PPSSPP only | [F] (docs) |
| 2xSaI / Eagle / Super Eagle | 2 | no | 2xSaI blends; Eagle copies | Eagle: yes | — | various | Exult `scale_2xSaI.cc`; chaiNNer | [F] (Exult src) |

Notes on the table:

* **Exult already contains** hq2x/3x/4x, xBR 2x/3x/4x (Zenju's HqMAME port, GPLv2+), 2xSaI, Scale2x, bilinear and point scalers in `exult-hires/imagewin/scale_*.cc` [F]. These are presentation-time scalers on the 8-bit buffer. `palette.md` §8 already rules them out for the S=6 buffer at runtime (too slow on the full screen, and blended RGB output). Their *code* can still be reused offline or per chunk.
* **chaiNNer** (GPL-3.0, very active, v0.25.x) has no xBRZ: issue #2422, opened 2023-12-24, is still open [F]. Its Rust pixel-art ops are only `adv_mame`, `eagle`, `sai`, `hqx` [F, repo tree].
* **`ra-pixelart-scale`** (Azq2, GPL-3.0, last push 2024-08-03) [F]:
  * Runs any libretro `.slangp` preset on a still image: `ra-pixelart-scale -m scalefx-9x -i in.png -o out.png --alpha split`.
  * Methods include scalefx 3x-9x, hqx, `xbrz-freescale-multipass`, xbr, scalenx, omniscale, eagle, sabr, cleanEdge, ddt, nnedi3.
  * Needs OpenGL. Use `xvfb-run` when headless. Whether WSLg's GL works for it is untested.
  * Practically the only CLI route to ScaleFX, OmniScale and cleanEdge on stills.

### 2.2 Per-algorithm notes that matter for U7 terrain

* **xBRZ** [F][E]
  * API: `xbrz::scale(factor 2..6, src, trg, w, h, ColorFormat::{rgb,argb,argbUnbuffered}, ScalerCfg, yFirst, yLast)`.
  * `ScalerCfg` defaults in 1.9: `equalColorTolerance=30`, `centerDirectionBias=4`, `dominantDirectionThreshold=3.6`, `steepDirectionThreshold=2.4` (2.2 before 1.9).
  * Slice processing (`yFirst/yLast`) lets you process a band of a large image with true neighbours, and it is thread-safe for non-overlapping slices.
  * Borders: "for pixels outside the input image, the xBRZ algorithm just assumes duplicates of the border, while looking as far as 2 pixels beyond" (Zenju, 2019-11-29). The documented workaround for tileable textures is to pad with wrapped pixels, scale, then crop [F].
  * **Look on U7:** excellent on shorelines and other material edges. Single-pixel water sparkles become small round glints, which is what `palette.md` §7.4 asks for. On noise textures it produces camouflage blobs (`algorithmic_img/A6_tol.png`, `A4_sheet.png`).
  * About 2.5-2.7k distinct RGB colours per 4x4-chunk window, with 90-91% of pixels exactly on palette colours [E].
* **Scale2x/Scale3x** [E]
  * Index-plane native, deterministic and fast.
  * Edges are stair-stepped at 45°/90° only; no curve reconstruction beyond 1:1 slopes.
  * On noise it makes small "maze" fragments (`A4_sheet.png`).
  * 2x→3x and 3x→2x differ slightly (gradient ratio 4.6 vs 5.0 region-level).
* **MMPX** [F][E]
  * Better slope handling than Scale2x (2:1 slopes, intersections), with the same palette guarantees.
  * It is 2x only. The paper: "MMPX can perform 4× scaling by simply running the 2× magnification process twice… we believe that a MMPX variant specifically designed for 3× is worth investigation". For 6x, combine it with Scale3x.
  * It breaks luminance ties with a "dark is foreground" heuristic and falls back to nearest when ambiguous.
  * The paper itself argues DNNs over-blur limited-palette single-pixel features. That is relevant to the AI comparison.
* **ScaleFX** [F]
  * "Interpolates edges up to level 6 and makes smooth transitions between different slopes. The filtered picture will only consist of colours present in the original."
  * The `scalefx.slangp` preset is 5 passes with the last at 3x. `scalefx-9x` runs the old pass chain twice (3x·3x).
  * For 6x: ScaleFX 3x, then Scale2x/MMPX 2x. Because only source colours are copied, you can recover indices exactly by feeding a uniquified palette (section 3.1).
  * GPU/GL shader only (no CPU port found).
* **hqx / Super-xBR / OmniScale / xBR-AA:** anti-aliasing scalers. They add colours, so they always need re-indexing and give no index guarantees. They offer no advantage over xBRZ 6x plus local snap for this use, and none is natively 6x except OmniScale.
* **Kopf-Lischinski:** designed for sprites with few colours and no AA. On noisy terrain it produces a "cartoon" vector look, which is unfaithful. It is a per-image vectorization, so tiling seams are not addressed. Not recommended for terrain. It might be worth a look later for large flat-shaded RLE objects (signs, furniture), but MMPX/xBRZ already cover that.

---

## 3. Local experiments on the real BG data

Data: `/mnt/e/Games/RolePlayingGames/ultima7/static` (read-only): `palettes.flx` entry 0, `shapes.vga` flats 0-149, `u7chunks` (3072 terrains × 512 B, 2 B/tile: `shape = b0 + 256*(b1&3)`, `frame = (b1>>2)&31`), and `u7map` (144 superchunks × 16×16 × u16).

Code: `docs-hires/upscale-research/algorithmic_code/`. `u7algo.py` holds the readers, Scale2x/3x on index planes, the OKLab mapper, local snap, and the xBRZ and MMPX ctypes wrappers.

* The xBRZ 1.9 C wrapper is `xbrz_cwrap.cpp`. Build: `g++ -O2 -std=c++2a -shared -fPIC -o libxbrz19.so xbrz.cpp cwrap.cpp`. On GCC 9 you must first patch `[&] -> bool` to `[&]() -> bool` in `xbrz.cpp:419`.
* MMPX: `gcc -O2 -shared -fPIC -o libmmpx.so mmpx.c`, after fixing the clamp bug.
* Images are in `algorithmic_img/`.

Windows: 4x4 chunks (512x512 px source) with one extra chunk of real context on each side. Window A is chunk (84,74); window B is chunk (146,44). Both were chosen automatically for maximum flat diversity and cycling content.

### 3.1 Index-preservation trick for colour-copying scalers

MMPX and ScaleFX run on RGB, but every output pixel is a copy of an input pixel. To recover indices exactly:

1. Build a "uniquified" palette in which each duplicate RGB is nudged by 1-3 LSB (for example white `15/224/232/248` becomes `FFFFFF/FFFFFE/FFFFFD/FFFFFC`).
2. Scale.
3. Invert with a dict lookup.

The decision logic is unaffected in practice (luma changes by less than 0.5%), and the round-trip is lossless.

Verified: MMPX output used only indices present in the source [E]. Implemented in `u7algo.unique_rgb_palette`. For sprites, pack index 255 with alpha so that MMPX and ScaleFX see it as transparent.

### 3.2 Seams and context (`exp3_seams.py`, `exp4_methods.py`)

**Gradient ratio** = mean OKLab ΔE across tile-boundary pixel pairs ÷ mean ΔE across other neighbour pairs. Compare it only within a method. The region-level value is the "correct" one, because the art itself has structure at tile edges.

| Window A (B in brackets) | region (true context) | per-tile, edge clamp | per-tile, wrap pad | per-frame consensus (mode of region instances) |
|---|---|---|---|---|
| xBRZ 6x gradient ratio | 3.77 (5.00) | 7.80 (9.36) | 6.51 (7.74) | 4.46 (5.90) |
| xBRZ mean ΔE vs region | 0 | 0.0029 (0.0031) | 0.0033 (0.0034) | **0.0008 (0.0011)** |

Further results for window A (per-tile is wrap-padded; ΔE is measured against each method's region result):

| Method | Region ratio | Per-tile ratio | ΔE in 6-px border band | ΔE interior | Index-preserving | Cycling px kept (NN = 9468) | Time per 768² source |
|---|---|---|---|---|---|---|---|
| nearest | 7.86 | 7.86 | 0 | 0 | yes | 9468 | 0.01 s |
| Scale2x→Scale3x | 4.61 | 6.82 | 0.0062 | 0.0001 | yes | 7152 | 0.04 s |
| Scale3x→Scale2x | 4.99 | 6.94 | 0.0062 | 0.0000 | yes | 7697 | 0.04 s |
| MMPX2x→Scale3x | 5.48 | 7.04 | 0.0041 | 0.0001 | yes | 8130 | 0.14 s |
| Scale3x→MMPX2x | 5.64 | 7.12 | 0.0059 | 0.0001 | yes | 7803 | 2.3 s (Python inverse map on 4608²) |
| xBRZ6→local snap | 3.78 | 6.54 | 0.0072 | 0.0004 | yes | 6955 | 9.2 s (NumPy snap) |

Other measurements:

* Per-frame instance disagreement with the per-frame mode: mean 2.1% (A) and 2.4% (B) of the 48x48 pixels; median 1.8% / 2.2%.
* All context effects sit in the border band, within the scaler's reach of 2 source pixels. Interior pixels are context-free.
* The fewer "cycling px kept" are expected: sparkles are re-shaped, not dropped.

### 3.3 Palette mapping of xBRZ output (`exp3`, `exp7_pillow.py`)

* OKLab nearest restricted to 1..223:
  * mean ΔE 0.0024 (A) / 0.0035 (B);
  * p99 0.034 / 0.115 (the large ones are blends between distant colours, such as water/shore midpoints);
  * **zero cycling pixels survive**, so a cycling mask is mandatory.
* RGB-Euclid and OKLab pick different indices on 1.4-2.0% of pixels.
* Local snap: mean ΔE 0.0017 / 0.0019, cycling indices kept, region gradient ratio unchanged (3.78 vs 3.77).
* Pillow 12.3.0:
  * With a full 256-entry palette image, 0.33% of pixels landed on indices ≥224. That is a bug source.
  * With `putpalette()` of only 224 entries, the max index is 223.
  * Agreement with exact RGB-nearest is 97.1%, and with OKLab-nearest 96.5%. Pillow's palette lookup is approximate (a cached search).
  * Floyd-Steinberg changed 1.8% of pixels.
  * About 1% of pixels map to index 0 (black). Fine for flats, but Pillow has no way to exclude a single index.

### 3.4 Downscale-consistency QA (`exp8_qa.py`)

A cheap fidelity gate for *any* 6x output, AI included: reduce each 6x6 block back to one pixel and compare it with the source.

| Method | 6x6 block-majority == source index | Block-mean OKLab ΔE vs source (mean / p99) |
|---|---|---|
| Scale2x→Scale3x | 98.9% | 0.0071 / 0.069 |
| MMPX2x→Scale3x | 99.3% | 0.0046 / 0.049 |
| xBRZ6→local snap | 98.9% | 0.0076 / 0.057 |
| xBRZ6→OKLab global (1..223) | 97.7% | 0.0086 / 0.089 |

Proposal: gate AI tiles at, for example, block-mean ΔE p99 ≤ 0.10, with a hard fail on any block whose mean drifts to a different ramp. Calibrate the thresholds on these deterministic baselines.

### 3.5 Visual findings (images in `algorithmic_img/`)

* `A5_sheet.png` (dirt/shore/water): xBRZ gives clean curved shorelines and round sparkles. The OKLab global remap is visually identical to raw xBRZ, apart from the sparkles losing their cycling. Local snap keeps both. Scale2x/3x and MMPX are blockier but faithful.
* `A4_sheet.png` and `A6_tol.png` (grass): every edge-directed method turns 1-px noise into vermicular blobs. xBRZ tolerances from 10 to 120 and direction thresholds 6/4 barely change this.
  * Unique colours: 3.9k, 4.2k, 4.1k and 3.7k for tolerances 10, 30, 60 and 120.
  * **This is the core limitation of the deterministic route for U7 terrain.**
* In these crops the black squares are tiles whose chunk entry references an RLE "flat object" (shape ≥150). My renderer draws those as index 0. They are not part of the test.

### 3.6 Prototype: material-boundary hybrid (`exp9_material.py`, `A9_material.png`)

1. Map every source index to its **ramp id**, using Exult's 16 static ramps from `palette.md` §9.1 plus the 6 cycling ranges. This is a "material map". About 86% of 1x pixels have all four neighbours in the same ramp.
2. xBRZ-6x the material map, using ramp mean colours as pseudo-colours, and local-snap back to ramp ids. This gives smooth material boundaries.
3. Start from NN-6x indices. Wherever the NN pixel's ramp disagrees with the upscaled material map, take the nearest source neighbour (3x3) whose ramp matches.

Result:

* Shorelines and grass/dirt borders become smooth curves.
* Texture interiors stay exactly the original pixels (6x6 blocks); 98.7% of pixels are equal to NN.
* No noise-to-worm artifacts.
* It is faithful but chunky inside materials.

This is the right **structure layer**: hand its interiors to AI or texture synthesis, and keep its boundaries as hard constraints (section 6).

---

## 4. Palette quantization and remapping

### 4.1 Recommended implementation (CPU, NumPy, no GPU)

```python
pal8 = (pal6 * 255 // 63)                  # engine's 6->8 bit conversion (palette.md 7.1)
allowed = np.arange(0x01, 0xE0)            # static colours only; add 0 for flats if you want black
# unique-colour trick: map each distinct RGB once
packed = (r<<16)|(g<<8)|b; uniq, inv = np.unique(packed, return_inverse=True)
d = ((oklab(uniq)[:,None,:] - oklab(pal8[allowed])[None])**2).sum(-1)
out = allowed[d.argmin(1)][inv]            # exact nearest, perceptual
```

* For speed on large batches, precompute a **6-bit-per-channel LUT** (64³ = 262,144 entries → index). The VGA palette is natively 6-bit, so this loses nothing meaningful. Alternatively use `scipy.spatial.cKDTree` in OKLab.
* **Distance metric:**
  * OKLab Euclidean (Ottosson 2020) is cheap and about as good as CIEDE2000 for nearest-palette work. CIEDE2000 is roughly 30× slower in at least one benchmark [S, ditherette issue #271].
  * For QA reports, `skimage.color.deltaE_ciede2000` (scikit-image 0.26.0, 2025-12) or `coloraide` (8.13, 2026-09-24) work [F, PyPI].
  * Exult itself uses squared RGB on 6-bit values (`palette.cc:486-503`). It is fine to differ, because this is offline art.
* **Always carry the index plane** and post-process with masks:
  * Cycling mask: source `>=0xE0` → keep the original index. The baseline is NN or a scaler that copies indices. The local snap gives "shrunken" glints for free.
  * Translucent-operator mask for RLE frames painted with TFA translucency (`0xEE-0xFE` as blend operators).
  * Transparent `0xFF` on RLE.
  * Static: quantize to `1..223`.

### 4.2 Tools evaluated

| Tool | Fixed palette | Reserved indices | Distance | Dither options | Verdict | Verif. |
|---|---|---|---|---|---|---|
| **Pillow 12.3.0** `Image.quantize(palette=P, dither=NONE\|FLOYDSTEINBERG)` | yes (palette image) | only by truncating the palette length (a 224-entry palette → indices 0..223); cannot exclude a single index (0) | RGB, **approximate** (97.1% exact) | none / FS only | OK for previews, not for final art | [F][E] |
| **ImageMagick 7** `magick in.png +dither -remap pal.png out.png` | yes | build `pal.png` with only the allowed colours, then re-index yourself (IM may reorder the colormap) | **RGB only**: "-quantize colorspace only affects -colors… can NOT define a color space for the color mapping or dithering phase" | `-dither None\|FloydSteinberg\|Riemersma`; ordered dither cannot use arbitrary palettes | Usable for quick checks; index order is not under your control | [F] |
| **libimagequant / pngquant** (Rust core since v4; `imagequant` PyPI 1.1.5, 2025-10-28) | `liq_image_add_fixed_color()` / `Histogram.add_fixed_color` | yes (≤256 fixed) | own perceptual-ish metric | `liq_set_dithering_level(0..1)` (FS) | "palette is improved during remapping": not guaranteed to keep your exact colours. Built to *create* palettes. GPLv3/commercial core. Not recommended for a fixed game palette | [F] |
| **hitherdither** (MIT, `pip install git+https://github.com/hbldh/hitherdither`) | yes, arbitrary palette | via the palette list | RGB | Bayer, cluster-dot, **Yliluoma 1**, error diffusion (FS, JJN, Stucki…) | Good reference for arbitrary-palette ordered dithering. Old, low activity | [F] |
| **didder** (GPL-3.0, v1.3.0, 2025-10; Debian/Ubuntu package) | `-p 'RRGGBB …'` | via the list | linearized RGB, luminance-weighted | many Bayer/cluster/custom JSON matrices; 11 error-diffusion kernels; `--serpentine` | Output is RGB PNG, **not paletted**, so re-index afterwards. No blue noise | [F] |
| **Own NumPy mapper** (section 4.1) | yes | exact control | OKLab / any | add ordered/blue-noise yourself | **Recommended** | [E] |
| `tilequant` (SkyTemple, PyPI 1.2.1, 2026-08) | per-8x8-tile sub-palettes | — | — | — | For NDS-style tile palette constraints. Not applicable to U7 (one global palette) | [F] (PyPI) |

### 4.3 Dithering policy

* **Algorithmic, index-preserving outputs: no dithering.** They are already exact palette pixels.
* **AI or true-colour outputs that must be indexed.**
  * Prefer **positional dithering with a world-aligned threshold**: threshold = `T[(world_x*6 + px) mod N, (world_y*6 + py) mod N]`. For a per-frame override that is shared across positions, align to *tile*-local coordinates with N dividing 48; Bayer 8x8/16x16 and blue noise 16² or 48² made tileable both work. Either way the pattern repeats identically on every tile edge, so it cannot create seams.
  * Methods that handle irregular palettes:
    * Knoll pattern dithering;
    * Yliluoma 1/2. A 2026 revisit (30fps.net, 2026-06-19, Python script included) shows that a near-constant mixing parameter loses almost no quality;
    * N-closest / N-convex / barycentric methods (matejlou, 2023-12-06, with the "Tetrapal" C library).
  * **Restrict the candidate set to the same Exult ramp** as the target colour, so dithering never mixes materials or hues and stays compatible with `PT_RampRemap`.
  * Blue-noise masks: Christoph Peters' tileable void-and-cluster textures, 16²-1024², **CC0** [F].
* **Error diffusion (Floyd-Steinberg and similar)** is order-dependent, so it will seam when frames are processed separately. If you need it, run it on a 3x3 toroidal tiling of the frame and keep the centre (cheap at 48 px). It is still not consistent between *different* neighbouring frames, so ordered dithering is preferred.
* `palette.md` §6.1: the engine presents through the palette *before* any RGB downscale, so 6x dithering averages out on smaller windows. Light ordered dithering is safe.

---

## 5. Seamless tiling techniques (ranked)

1. **Region-then-slice with per-frame consensus** (recommended for offline per-frame overrides) [E]:
   1. Render large map windows at 1x from `u7map` + `u7chunks`, with at least 2 source px (better 1 chunk) of true context.
   2. Upscale them.
   3. Slice by tile.
   4. For each `(shape, frame)`, take the **per-pixel mode** over all instances, or for RGB methods the OKLab medoid.
   * Interior pixels are identical across instances anyway. Only the border band varies (about 2% of pixels).
   * Cost: the whole world is 24576² px at 1x, so xBRZ takes a few minutes. The counting needs care: keep mode counters only for the 2-px-reach border band, or use local-snap indices, so that there are ≤9 candidates per pixel.
   * Frames used only in the editor (never on the map) fall back to option 2.
2. **Wrap padding** (pad 2 source px for xBRZ, 1 for Scale2x/3x, 3 for MMPX, cleanly cropped). It only helps for *self-tiling* frames such as plain grass or water. It is wrong for transition frames.
   * Better: **context-class padding**. Pad with the most frequent real neighbour on each side, taken from map statistics.
3. **Border lock as a post-constraint.** Force the outer k hi-res pixels of each edge to match a context-free rule (for example NN of the edge source pixel, or the consensus). This guarantees edge identity between any two tiles that share edge pixels. It can create a visible "frame" if k is large, so use k ≤ 3 at 6x. Useful as a final QA repair for AI tiles.
4. **Edge-consistency check across transition tiles.** For every pair `(A right edge, B left edge)` that actually occurs in the map (and likewise top/bottom), measure the hi-res gradient ratio. Repair or regenerate the worst pairs. This pairs well with the consensus method.
5. **Chunk-terrain-level overrides** (design alternative): 2105 used chunk terrains × 768×768 per-terrain images. This avoids intra-chunk tile seams completely, because each terrain is upscaled as one image with correct internal context. Only chunk-to-chunk seams remain, and those are fixable with the map neighbours.
   * Cost: about 1.2 GB raw at 8 bpp, much less as PNG.
   * Since `Chunk_terrain` already caches whole 128×128 flats per terrain (`objs/chunkter.cc`), the engine hook is natural: when present, the override replaces `rendered_flats` wholesale.
   * Downside: it no longer edits per frame, and map-editor changes to a chunk invalidate the override (detect with a hash of the 256 ShapeIDs).
6. **Runtime procedural terrain** (optional engine mode):
   * Build the S=6 terrain cache by running xBRZ 6x plus local snap on the 1x flats of the chunk, plus a 2-tile margin from neighbour chunks.
   * Cost: about 4-10 ms per chunk single-threaded, from the measured 0.2-0.6 µs per source px. Local snap in C++ is negligible.
   * Gives context-correct, seamless, index-preserving terrain with zero assets. Art overrides, where present, take precedence.
   * Caveats:
     * xBRZ is **GPLv3**, which forces the combined binary to GPLv3. Exult is GPLv2+, so this is allowed, but it is a project decision. Alternatives are Exult's own GPLv2+ xBR code (2x/3x/4x, combined with Scale2x/3x for 6x) or MMPX (MIT) + Scale3x.
     * The cache is per-terrain-number and shared across map positions. Neighbour-aware margins require a per-map-chunk cache, or accepting border approximations.
     * The grass "worm" look applies here too, so pair it with the material-boundary variant (section 3.6) if that look is rejected.

---

## 6. Hybrid pipelines with AI (how the deterministic tools plug in)

* **Pre-pass / mould.**
  * Give the AI model (SD-family img2img with ControlNet Tile, or an ESRGAN-type SR) an *xBRZ-6x or material-boundary-6x* image instead of NN-6x, so the model does not have to infer geometry from staircases.
  * Community practice with ESRGAN on sprites: "scaling the source image up with xBR/xBRZ before ESRGAN produces smooth edges… creates a 'mould'" [S, Doomworld thread]. OpenModelDB even hosts a "4x-xbrz" ESRGAN model trained to mimic xBRZ [S].
  * For SD img2img, use low denoise (about 0.2-0.4) on the xBRZ or material image.
  * Use the **material map as an inpainting mask**: AI may change texture interiors and must not move boundaries.
* **Post-pass / projection back to the game's constraints.** Order matters:
  1. Restore the cycling and translucent masks from the source (NN or local snap of the source indices).
  2. Re-index static pixels:
     * **local snap**, maximum fidelity: only colours present in the source neighbourhood, so no hallucinated colours;
     * or **ramp-constrained OKLab nearest**, with more freedom: allow any index in the ramps found in the 3x3 source neighbourhood. That lets the AI's new shading use more steps of the same material ramp.
  3. Optionally apply world-aligned ordered dithering within the ramp.
  4. Run the seam QA (section 5.4) and the block-consistency QA (section 3.4).
* **Consistency across the tileset.**
  * Algorithmic passes are deterministic by construction.
  * For AI passes, process *map regions* (not single frames) at a fixed seed and fixed prompt, then apply the per-frame consensus from section 5.1. This also averages away per-instance AI variance.
  * Frames whose instances disagree strongly after AI are a signal of hallucination. Flag them for review.

---

## 7. Install commands (Linux / WSL, CPU only)

```bash
# Python env (uv already present on this machine)
uv venv -p 3.11 venv && VIRTUAL_ENV=$PWD/venv uv pip install pillow numpy scipy scikit-image coloraide hqx imagequant
# xBRZ 1.9 (2026-01-24) from source -> shared lib for ctypes
curl -sL -o xBRZ_1.9.zip "https://sourceforge.net/projects/xbrz/files/xBRZ/xBRZ_1.9.zip/download"
unzip xBRZ_1.9.zip -d xbrz19 && cd xbrz19
# GCC >= 11 (or clang) compiles as-is with -std=c++23; on GCC 9 patch one C++23 lambda:
sed -i 's/\[&\] -> bool/[\&]() -> bool/' xbrz.cpp
g++ -O2 -std=c++2a -shared -fPIC -o libxbrz19.so xbrz.cpp cwrap.cpp   # cwrap.cpp = algorithmic_code/xbrz_cwrap.cpp
# xbrz.py alternative (old xBRZ, AGPL; wheels only cp36-cp39, sdist build is broken):
uv venv -p 3.9 venv39 && VIRTUAL_ENV=$PWD/venv39 uv pip install xbrz.py "pillow>=10" numpy
# MMPX C99 (MIT) -- fix clamp first (see pitfalls)
curl -sLO https://raw.githubusercontent.com/ITotalJustice/mmpx/master/mmpx.c && curl -sLO https://raw.githubusercontent.com/ITotalJustice/mmpx/master/mmpx.h
sed -i 's/min > max ? max : v;/v > max ? max : v;/' mmpx.c && gcc -O2 -shared -fPIC -o libmmpx.so mmpx.c
# or Rust CLI: cargo install mmpx   (mmpx input.png --output output.png)
# ImageMagick Scale2x/Scale3x/Eagle/xBR2x/hq2x one-liners (IM >= 7.0.8, July 2019)
magick in.png -define magnify:method=scale3X -magnify -define magnify:method=scale2X -magnify out6x.png
# ScaleFX / OmniScale / cleanEdge / xbrz-freescale via libretro shaders on stills (GPL-3, needs OpenGL)
git clone https://github.com/Azq2/ra-pixelart-scale && cd ra-pixelart-scale && make && sudo make install
xvfb-run ra-pixelart-scale -m scalefx-9x -i in.png -o out.png --alpha split   # list: --list-methods
# didder (ordered/error-diffusion with fixed palette; outputs RGB PNG)
sudo apt install didder    # Debian/Ubuntu packaged since 2025
```

ImageMagick: `-magnify` operates on RGB, so feed it a uniquified-palette RGB image and invert to indices, or use the NumPy index-plane version in `u7algo.py`.

---

## 8. Pitfalls (collected)

1. **Duplicate palette colours** (`0/252`, `15/224/232/248`, `73/194`, `225/231`) make index→RGB→index lossy. Carry indices, or uniquify the palette for colour-copying scalers [E].
2. **A global remap to `1..223` deletes all cycling pixels** (0 survivors in the test). Always restore from the source masks [E].
3. **Edge-directed scalers turn 1-px noise textures into worms or blobs**, independent of xBRZ tolerances [E]. Use them for boundaries, not for texture interiors.
4. **Per-frame isolated upscaling seams.** Default edge clamp is worst; wrap padding helps only self-tiling frames. Use region-then-consensus [E].
5. **xBRZ 1.9 needs C++23** (a lambda without `()`) and fails on GCC 9 [E]. Its enum names are lowercase (`ColorFormat::rgb/argb/argbUnbuffered`) [E].
6. **`xbrz.py` (PyPI 1.0.2, 2021-05)**: wheels only for CPython ≤3.9, the sdist misses `xbrz.h` and fails to build, and the `[pillow]` extra resolved to Pillow 7.2.0 under uv, which failed to build. It bundles a pre-1.9 xBRZ and is **AGPL-3.0** [E][F].
7. **`ITotalJustice/mmpx` C99 `clamp()` bug**: `return v < min ? min : min > max ? max : v;` never clamps the upper bound, so reads at the right and bottom edges go out of bounds. Fix it to `v > max ? max : v` [E, source read].
8. **`xbrzscale` CLI** README says factor 2-5 and "only tested on 32-bit RGBA PNGs" [F]. Use the library directly for 6x.
9. **Pillow quantize:**
   * a full 256-entry palette image lets the output hit reserved indices;
   * the nearest search is approximate (about 3% mismatch);
   * it is RGB-only;
   * the only dithers are FS and none;
   * converting `RGBA→P` ignores the `palette`/`dither` arguments [F][E].
10. **ImageMagick `-remap`** matches in RGB regardless of `-quantize` colourspace, and its ordered dither cannot use arbitrary palettes [F].
11. **libimagequant** can alter palette colours during remap ("not guaranteed exact"), is GPLv3 or commercial, and its Python binding has no fixed-palette example [F].
12. **MMPX is 2x-only and xBR-noblend is correct only at odd factors** [F][S]. Compose with Scale3x for 6x and measure. The order matters a little (2x→3x scored better in the test) [E].
13. **ScaleFX, OmniScale and cleanEdge exist only as GPU shaders.** `ra-pixelart-scale` needs an OpenGL context (`xvfb-run`) and is low-activity (last push 2024-08) [F].
14. **Engine 6-bit palette**: convert with `v*255//63` exactly as the engine does. Otherwise "exact palette colour" checks fail [E].
15. **License mixing:**
    * xBRZ GPLv3 (with an exception list that does not include Exult);
    * xbrz.py AGPL;
    * hqx LGPL;
    * Exult GPLv2+.

    Offline tools are unaffected. Runtime inclusion of xBRZ makes the fork GPLv3.

---

## 9. Sources

Verified by fetching ([F]):

* xBRZ project and files: https://sourceforge.net/projects/xbrz/ and https://sourceforge.net/projects/xbrz/files/xBRZ/ (1.9 = 2026-01-24 changelog; 6xBRZ added in 1.4); xBRZ 1.9 source zip (license and `ScalerCfg` read locally).
* xBRZ border-handling thread (Zenju, 2019-11-29): https://sourceforge.net/p/xbrz/forums/general/thread/f477dff7a1/
* xbrz.py: https://github.com/ioistired/xbrz.py ; PyPI JSON https://pypi.org/pypi/xbrz.py/json (1.0.2, 2021-05-04, AGPL-3.0-or-later)
* xbrzscale: https://github.com/atheros/xbrzscale (README)
* Hawkynt 2dimagefilter, xBRZ 1.9 entry (merged 2026-09-21): https://github.com/Hawkynt/2dimagefilter/pull/57
* MMPX paper, JCGT vol. 10 no. 2, 2021: https://jcgt.org/published/0010/02/04/paper.pdf (text extracted locally); C99 port https://github.com/ITotalJustice/mmpx ; Rust https://github.com/pierogis/mmpx-rs
* ScaleFX shader headers and presets: https://github.com/libretro/slang-shaders/tree/master/edge-smoothing/scalefx (pass4: "will only consist of colours present in the original", MIT, Sp00kyFox 2016-17)
* ra-pixelart-scale: https://github.com/Azq2/ra-pixelart-scale
* chaiNNer xBRZ request (open): https://github.com/chaiNNer-org/chaiNNer/issues/2422 ; chaiNNer-rs pixel-art ops tree: https://github.com/chaiNNer-org/chaiNNer-rs
* SameBoy scaling (OmniScale): https://sameboy.github.io/scaling/
* cleanEdge: https://torcado.com/blog/cleanEdge/
* libdepixelize (Kopf-Lischinski, GPLv2+): https://gitlab.com/inkscape/devel/libdepixelize
* Pixel-art scaling overview: https://en.wikipedia.org/wiki/Pixel-art_scaling_algorithms
* PPSSPP texture scaling: https://www.ppsspp.org/docs/settings/graphics/
* Pillow 12.3.0 `Image.quantize`: https://pillow.readthedocs.io/en/stable/reference/Image.html
* ImageMagick options: https://imagemagick.org/command-line-options/ ; quantize/remap colour-space limitation: https://usage.imagemagick.org/quantize/ ; magnify methods changelog (July 2019): https://github.com/ImageMagick/ImageMagick/commit/39f226a9c137f547e12afde972eeba7551124493
* libimagequant: https://pngquant.org/lib/ ; Python bindings: https://libimagequant-python.readthedocs.io/en/latest/ ; PyPI `imagequant` 1.1.5 (2025-10-28)
* hitherdither: https://github.com/hbldh/hitherdither
* didder manpage: https://github.com/makew0rld/didder/blob/main/MANPAGE.md (v1.3.0, 2025-10-20; GPL-3.0 LICENSE)
* Ordered dithering for arbitrary palettes (2023-12-06): https://matejlou.blog/2023/12/06/ordered-dithering-for-arbitrary-or-irregular-palettes/
* Revisiting Yliluoma's algorithm (2026-06-19): https://30fps.net/pages/revisiting-yliluoma-2/
* Blue noise textures (CC0): https://momentsingraphics.de/BlueNoise.html
* PixelArtScaling / `scalenx` (Unlicense): https://github.com/Dnyarri/PixelArtScaling
* PyPI metadata (versions and dates): hqx 1.0 (2023-02-12), coloraide 8.13 (2026-09-24), colour-science 0.4.7 (2025-12-06), scikit-image 0.26.0 (2025-12-20), Pillow 12.3.0 (2026-07-01), tilequant 1.2.1 (2026-08-03), scalenx 2026.8.6.34 (2026-08-06)
* Exult sources: `exult-hires/imagewin/scale_*.cc`, `gamemap.cc:200-245` (u7map layout), `objs/chunkter.cc:139-162` (u7chunks entry decoding)

Snippet only ([S]):

* xBR noblend characteristics: https://github.com/libretro/common-shaders/blob/master/xbr/xbr-lv3-noblend.cgp , https://docs.libretro.com/shader/xbr/
* OmniScale MIT licence / Godot port: https://github.com/nobuyukinyuu/godot-omniscale
* Super-xBR multi-pass usage: https://github.com/n0spaces/py-super-xbr (repo archived, per GitHub API [F])
* xBRZ as ESRGAN pre-pass: https://www.doomworld.com/forum/topic/106611-a-simple-method-of-scaling-up-2d-sprites-with-esrgan/ ; OpenModelDB 4x-xbrz: https://openmodeldb.info/models/4x-xbrz
* CIEDE2000 vs OKLab speed: https://github.com/mia-cx/ditherette/issues/271
* OKLab definition: https://bottosson.github.io/posts/oklab/

---

## Fact-check

Adversarial check run on 2026-10-04. Each claim was checked against a primary source: the upstream archive or source file, PyPI, crates.io or GitHub API metadata, the official docs, or a local rerun with independent code. Scratch files are in the session scratchpad (`fc_algo/`), not in the repo.

Verdicts: **CONFIRMED**, **REFUTED** (the claim is wrong as stated), **UNVERIFIABLE** (no primary evidence found).

| # | Claim | Verdict | Evidence (source, date) |
|---|---|---|---|
| 1 | xBRZ has had native 6x since 1.4 (2015-07); the latest release is 1.9 (2026-01-24). | **CONFIRMED** | `Changelog.txt` inside `xBRZ_1.9.zip` (downloaded today) has "xBRZ 1.9 [2026-01-24]" and "xBRZ 1.4 [2015-07-25] Added 6xBRZ scaler". `xbrz.h` has `SCALE_FACTOR_MAX = 6` and `scale(factor 2..6)`. The SourceForge file list shows upload dates one day later (1.9 on 2026-01-25, 1.4 on 2015-07-31). The `ScalerCfg` defaults 30 / 4 / 3.6 / 2.4 match `xbrz_config.h`. |
| 2 | xBRZ is GPLv3 with a linking exception only for MAME, FreeFileSync, Snes9x and ePSXe. Exult is GPLv2+, so linking xBRZ at runtime makes the binary GPLv3. | **CONFIRMED** | The header of `xbrz.h` and `xbrz_config.h` (1.9) lists exactly those four projects. `License.txt` is GPLv3. Exult `COPYING` is GPL v2, and the engine sources (`gamewin.cc`, `gamemap.cc`, `objs/chunkter.cc`) say "version 2 … or (at your option) any later version". |
| 3 | xBRZ 1.9 does not build on GCC 9 without a one-line lambda patch. | **CONFIRMED** (local) | This WSL box has Ubuntu 20.04 with GCC 9.4.0. `g++ -std=c++2a` on the unpatched `xbrz.cpp` fails with "419:38: error: expected '{' before '->' token". After the `[&]() -> bool` sed patch it compiles with 0 errors. The claim that GCC 11 or later builds it as-is was not tested. |
| 4 | `xbrz.py` is AGPL, from 2021, and installs only on CPython 3.9 or older. | **CONFIRMED** | PyPI JSON: 1.0.2 uploaded 2021-05-04/14, licence `AGPL-3.0-or-later`, wheels only `cp36`–`cp39` plus `pp36`/`pp37`. The sdist is broken: `lib/xbrz.cpp` includes `xbrz.h`, `xbrz_tools.h` and `dummy_module.cpp`, and none of them are in the tarball. The bundled core was "Modified by io mintz 2020-06-12", so it predates 1.9. |
| 5 | The `xbrzscale` CLI only does 2-5x, so 6x needs the library. | **CONFIRMED, with an omission** | The atheros/xbrzscale README says "an integer between 2 and 5 (inclusive)" and "only tested … on 32bit RGBA PNGs". The report misses two other 6x routes: (a) `ra-pixelart-scale -m rust-xbrz` uses the `xbrz-rs` crate (0.1.0, 2024-07-28, **GPL-3.0-only**), which does 2x-6x; (b) Hawkynt 2dimagefilter (row 7). |
| 6 | "xBRZ is the only mature scaler with native 6x." | **REFUTED as worded** | The ra-pixelart-scale README marks `omniscale`, `cleanEdge-scale`, `xbrz-freescale(-multipass)` and `super-xbr` as **any** scale. The SameBoy page says "OmniScale can scale an image by any factor". The report's own §2.1 table lists OmniScale and cleanEdge as "direct 6x". The defensible version: xBRZ is the only maintained **CPU library** with a dedicated 6x kernel. The others are GPU shaders. |
| 7 | Hawkynt ImageResizer has offered xBRZ 1.9 at 2x-6x since 2026-09-21. | **CONFIRMED** | GitHub API: PR #57 "+ xBRZ 1.9 is offered alongside the existing xBRZ entries" was merged 2026-09-21T05:39Z. The text says the GUI and CLI expose "Upscaler: xBRZ 1.9 2x … 6x". Release v1.1.3.86 came out 2026-09-21. Note: the repo licence is **LGPL-3.0**, while the xBRZ code inside it is GPLv3. |
| 8 | MMPX is MIT and 2x-only. The ITotalJustice C99 port's `clamp()` never clamps the upper bound. | **CONFIRMED** | `mmpx.c` lines 47-50 contain `return v < min ? min : min > max ? max : v;`. When `v > max` this returns `v`, so the read at line 70 goes out of bounds. The file header says "Available under the MIT license". crates.io `mmpx` 0.2.0 (2024-11-06) is MIT and ships the `mmpx` binary. ra-pixelart-scale lists `rust-mmpx` at 2x only. I could not fetch the JCGT paper page (empty response), so the paper quotes are unverified by me. |
| 9 | ScaleFX copies only original colours, is native 3x and MIT. ra-pixelart-scale is GPL-3.0, last pushed 2024-08. | **CONFIRMED** (one minor slip) | `scalefx-pass4.slang` has an MIT header, the comments "The filtered picture will only consist of colours present in the original" and "interpolates edges up to level 6", and is a 3x pass. `scalefx.slangp` has **6** passes (stock + 5 ScaleFX passes), not "5 passes". GitHub API for Azq2/ra-pixelart-scale: GPL-3.0, pushed 2024-08-03, needs OpenGL or `xvfb-run` per its README. |
| 10 | ImageMagick 7 has `-define magnify:method=scale2X/scale3X` (since July 2019), and `-remap` matches in RGB only. | **CONFIRMED** (citation wrong) | The ChangeLog entry between 7.0.8-53 (2019-07-05) and the next release reads: "magnify:method=… eagle2X, eagle3X, eagle3XB, epb2X, fish2X, hq2X, scale2X (default), scale3X, xbr2X". The commit cited in §9 (`39f226a9…`, 2019-07-18) is a JPEG heap-overflow fix, not the magnify change. The usage page says "-quantize colorspace setting is only used for the selection of colors, not its mapping" and that ordered dither cannot use arbitrary palettes. |
| 11 | Pillow 12.3 `quantize(palette=…)` is approximate and RGB-only; with a 224-entry palette it never emits an index above 223. | **CONFIRMED** (independent test) | I wrote a separate synthetic test: 224 random VGA colours and 200k blended pixels, with `dither=NONE`. Max index was 223. Agreement with exact RGB-nearest was **91.5%**, and 8.3% of pixels got a strictly farther colour. That is worse than the report's 97.1% on real data, so the advice to use Pillow for previews only stands. The 12.3.0 docs list only `Dither.NONE` and `FLOYDSTEINBERG`, applied only for "RGB to P". |
| 12 | libimagequant is GPLv3 or commercial and may change fixed colours during remap; the `imagequant` PyPI binding is BSD. | **CONFIRMED** | pngquant.org/lib says "GPL v3 or later … commercial license" and "palette is improved during remapping". PyPI `imagequant` 1.1.5 (2025-10-28) is BSD-3-Clause, but it compiles the GPLv3 libimagequant into its extension, so the shipped wheel is effectively GPLv3. |
| 13 | didder is "v1.3.0, 2025-10" and installs with `sudo apt install didder`. | **REFUTED** (date and install line) | GitHub API shows release v1.3.0 published **2023-12-20**; `CHANGELOG.md` gives 1.3.0 as 2022-12-20. "October 20, 2025" is only the date in `MANPAGE.md`. The GPL-3.0 licence is correct. Packaging: Debian has `didder 1.3.0-1` in trixie, forky and sid (accepted into unstable in 2025), and Ubuntu has it only from 25.10. **This WSL box runs Ubuntu 20.04, so the apt line in §7 fails.** Use `go install` or the release binary instead. |
| 14 | Palette 0 has the duplicate RGB groups {0,252}, {15,224,232,248}, {73,194} and {225,231}. | **CONFIRMED** (independent reader) | My own FLX parse of `palettes.flx` entry 0 (768 B, 6-bit), written without `u7algo.py`, gives exactly `[[0,252],[15,224,232,248],[73,194],[225,231]]`. Index 255 holds a non-VGA marker value (250,64,1). |
| 15 | xBRZ 6x takes 0.13-0.4 s per 768² source, and a runtime per-chunk mode costs about 4-10 ms per chunk. | **CONFIRMED as a conservative upper bound** | I rebuilt xBRZ 1.9 with GCC 9.4 `-O2` and timed the C call alone, single thread. 768² to 4608² took **26-51 ms**. A 160² window (one chunk plus a 2-tile margin) took **1.0-2.2 ms**. On ramp-noise input about 30-34% of pixels differ from NN, so this is real work. The report's figures probably include Python packing. Runtime feasibility is better than the report claims; C++ local snap was not timed. |
| 16 | Exult already ships GPLv2+ xBR 2-4x, hq2-4x, 2xSaI and Scale2x code. | **CONFIRMED, licence nuance** | `imagewin/scale_xbr.cc` is GPLv2+ ("xBR algorithm by Hyllian, based on HqMAME version by Zenju") with 2x, 3x and 4x kernels. `scale_2x.cc`, `scale_hq*.cc` and `scale_2xSaI.cc` are **LGPL v2+** ("GNU Library General Public License"). That is more permissive, so the conclusion holds. |
| 17 | Supporting items. | **CONFIRMED** | chaiNNer issue #2422 "Add xBRZ and ScaleNX pixel art upscalers" is still open (created 2023-12-24); latest release v0.25.1 is from 2025-10-23. `scalenx` 2026.8.6.34 (2026-08-06) is Unlicense and pure Python. `hqx` 1.0 (2023-02-12) is LGPL-2.1-only. The momentsingraphics blue-noise textures are CC0 and tileable, 16² to 1024². Zenju's 2019-11-29 border quote is verbatim. |
| 18 | Wrap padding is the "documented workaround" for tileable textures, and cleanEdge adds no new colours. | **Partly UNVERIFIABLE** | In the forum thread, the pad, scale and crop workaround (a 1-px pad, then crop 6 px) comes from a **user** (oneilmw, 2019-11-24), not from Zenju. Zenju's fix was to treat ARGB pixels outside the image as transparent in 1.8. The cleanEdge blog confirms MIT licensing and arbitrary scale but says nothing about palette preservation. The SameBoy page does not state the OmniScale licence. |

**What this changes for the decision:**

* Nothing on this list overturns the main recommendation: xBRZ 6x plus local snap, or the material-boundary hybrid, with Scale2x→3x or MMPX→Scale3x as index-exact fallbacks.
* Wording fixes:
  * say "only CPU library with a native 6x kernel";
  * fix the didder date and install line;
  * replace the IM commit citation with the ChangeLog entry;
  * describe the ScaleFX preset as 6 passes.
* Additions:
  * add `xbrz-rs` / `ra-pixelart-scale -m rust-xbrz` (GPL-3.0-only) as a CLI 6x route;
  * note that the per-chunk runtime cost is roughly 3-8x lower than §0.7 and §5.6 state.
* Licensing is unchanged. Every xBRZ derivative found (the original, xbrz.py, xbrz-rs, the Hawkynt port) is GPLv3 or AGPLv3. A GPLv2-compatible runtime needs Exult's own xBR (GPLv2+) or MMPX (MIT) plus Scale3x.
