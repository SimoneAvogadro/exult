# Dedicated super-resolution (SR) models for 6x Ultima VII tiles

*Research date: 2026-10-04. Angle: dedicated SR networks (ESRGAN family and newer architectures), the OpenModelDB catalogue, runners (chaiNNer, spandrel, Upscayl, Real-ESRGAN), and how to feed 8x8 tiles to them. Diffusion and API models are covered elsewhere and appear here only as pointers.*

**Verification legend**
- **[V-fetch]**: I fetched the page, repository or API and read it myself.
- **[V-src]**: I read the source code or data in a cloned repository (commit date given).
- **[V-run]**: I measured it myself on real U7 art in this session (local bake-off, section 4).
- **[S]**: search-engine snippet only. Not independently confirmed.

---

## 0. TL;DR

1. **No current pixel-art SR model exists.** OpenModelDB lists 53 models tagged *Pixel Art*. All of them date from 2019 to 2022, 52 are classic ESRGAN (RRDB), and 45 carry non-commercial CC-BY-NC(-SA) licences [V-src]. The 2024-2026 models (DAT2, RealPLKSR, SPAN, MoSR, FDAT) target photos, anime, video and modern game textures. None is a 2025-2026 pixel-art model. The diffusion route is where the community moved.
2. **Best off-the-shelf picks for U7 terrain** (from my bake-off on real U7 map renders, section 4):
   - **4x-NXbrz** (ESRGAN, 2021, CC-BY-NC-SA-4.0) gives the cleanest and most faithful result. It "depixelizes" staircases into smooth contours, adds little invented content, and is the *least context-sensitive* model (most deterministic per tile). Its look is somewhat "vector/blobby".
   - **8x-Arzenal-v1-1 / 8x-MS-Unpainter** (ESRGAN, 2021, CC-BY-NC-SA-4.0) give the best painterly look: flowing strokes on dirt and grass, smooth shorelines. They are native 8x, then **box-downscale 8 to 6** (a supersampled result). They change the art more than NXbrz does.
   - **4x-PixelPerfectV4** (ESRGAN, 2020, **WTFPL**, the only permissively licensed pixel-art model worth using) gives a smooth painterly look but smears fine speckle (grass loses texture).
   - **4x-UltraSharpV2** (DAT2, May 2025, CC-BY-NC-SA-4.0) is the best *general* model on fidelity. It adds plausible grass-blade texture, **but it keeps the pixel staircases** on diagonals and shorelines, because it was not trained on nearest-neighbour inputs. It is a good second pass or blend partner, not a primary model.
   - **Reject:** RealESRGAN_x4plus_anime_6B flattens all texture. RealESRGAN_x4plus shows low fidelity and noisy invented detail. 4x-Struzan produces cross-hatch noise. Chaining a 2x model three times (2x-Faithful) gives strong colour drift.
3. **6x strategy:** run the model at its native scale on a **context window**, crop, then resample to exactly 6x. With an 8x model, box-downscale (clean). With a 4x model, Lanczos-upscale by 1.5x or run it twice (16x) and box-downscale, which is what chaiNNer's "custom scale" does [V-src]. No public 3x pixel-art model exists, and traiNNer-redux trains only scales **1/2/3/4/8**, so it has no native 6x [V-src]. For a custom model, train **8x and box to 6x**, or **3x on 2x-nearest-prescaled input**.
4. **Never upscale a lone 8x8 tile.** With zero or mirror padding, the seam step at the tile edge is **4-18 times** larger than a normal pixel step [V-run]. U7 terrain frames are **not** self-tiling either: even the original art has a wrap-seam ratio of 1.4-1.8 [V-run]. So the context must be the **real map neighbourhood**. Upscale unique 128x128 **chunks** (2105 used on the map) or larger map windows with real neighbours as padding. Then either keep chunk-level overrides (768x768, seamless inside the chunk) or cut tiles and **average each (shape, frame) over many instances** (consensus) to get one canonical 48x48 tile.
5. **The circular-padding trick:** for pure-convolution models (ESRGAN, Compact, SPAN, RealPLKSR), setting every `Conv2d.padding_mode='circular'` makes a periodic input produce an *exactly periodic* output. That is identical to infinite wrap context (MAE 0.001-0.17/255 against 5x5 wrap) [V-run]. It does **not** work for DAT/HAT/SwinIR-type window-attention models (MAE 2-9/255) [V-run].
6. **Palette back-quantization:** use a **local palette** ("Quantize to Reference": candidates are the original indices in the 3x3 low-res neighbourhood). It keeps colours where they were, prevents palette drift, and raised round-trip index agreement by 2-10 points for every model [V-run]. Never emit 0xE0-0xFF from the quantizer. Restore cycling pixels with a label-preserving upscaler (xBRZ/MMPX/nearest on the index or mask), **not** with the SR net. A plain nearest copy looks blocky next to smooth SR output [V-run].
7. **The best path to consistent, faithful 6x:** train a small custom model with **traiNNer-redux** (active Oct 2026, Apache-2.0, PyTorch cu128+, works on Windows). Use (a) synthetic pairs: painted HR textures area-downscaled 6x and quantized to the U7 palette, and (b) later, human-curated diffusion outputs for a few hundred U7 tiles as HR targets. Add `averageloss` (down-scale consistency) for fidelity. This distils one style into a deterministic, fast, seam-friendly network.
8. **Runners:** **chaiNNer** (GUI plus experimental CLI, Torch updated for RTX 50 in v0.25.0, Oct 2025, nightlies to Feb 2026, repo active) or a **20-line Python script with spandrel 0.4.2** (Feb 2026, MIT). **Upscayl** is not suitable: NCNN only, last release v2.15.0 in Dec 2024, no wrap padding, no palette tools [V-fetch]. Real-ESRGAN upstream is dormant (last push Aug 2024) [V-fetch].
9. **RTX 5070 Ti:** current PyTorch is **2.14.1**. Wheels exist for **cu130 and cu132** (also cu126, but cu126 has no sm_120). The **cu128 index tops out at torch 2.11.0** [V-fetch]. Install with `--index-url https://download.pytorch.org/whl/cu130`. All candidate models are ≤ 140 MB and run comfortably in 16 GB, even untiled, at chunk sizes.

---

## 1. Why U7 terrain is a hard SR input

| Property | Consequence for SR models |
|---|---|
| 8x8 tiles, 3-12 colours each (most-used grass frames use only 3-4 indices [V-run]) | There is no information in a tile alone. The network's receptive field mostly sees padding. Context must come from neighbours. |
| Hand-pixelled 1:1 art, hard staircase edges, scattered single-pixel speckle | Photo-trained models (bicubic or "real" degradations) treat staircases as real structure and **keep them** [V-run]. Pixel-art models (trained on nearest-neighbour downscales) smooth them. |
| Terrain frames are pieces of larger patterns (frames 0..31 of a shape tile with *each other*, plus transition frames) | Self-wrap context is **wrong** for most frames (section 5.2). Real-map context is right. |
| 8-bit palette, 0xE0-0xFE cycling, 0xFF transparent key (see `docs-hires/analysis/palette.md`) | SR nets output RGB. You must re-quantize, protect the reserved ranges, and handle cycling separately. |
| Same (shape, frame) is used up to ~800k times on the map with different neighbours | Per-tile overrides need one canonical output. The model's context dependence decides how much the instances disagree (section 4.4). |

---

## 2. State of the ecosystem (October 2026)

### 2.1 OpenModelDB catalogue [V-src]
I cloned `github.com/OpenModelDB/open-model-database`, the data repository behind openmodeldb.info (last commit **2026-08-14**). It holds 671 models.

- **Pixel Art tag:** 53 models. Dates range from **2019-02-21 to 2022-05-10**. 52 are ESRGAN and one is SPSR. Scales: 39 at 4x, 6 at 8x, 6 at 2x, 2 at 1x.
- **Licences of Pixel Art models:** 25 CC-BY-NC-SA-4.0, 20 CC-BY-NC-4.0, 3 GPL-3.0, 2 CC0, 2 WTFPL, 1 unknown. **Only PixelPerfectV4 and ArtClarity (WTFPL) and HDCube and Skyrim-Alpha (CC0) are permissive.** HDCube's own card says it is *not* suitable for pixel art under 16 px.
- **Newest models with a "game" angle** (2024-2026): 4x-PBRify-UpscalerV4 (DAT2, CC0, 2025-05), 4x-UltraSharpV2 (DAT2, CC-BY-NC-SA, 2025-05), 2x-AoMR-mosr (CC-BY-4.0, 2024-09), 4x-TextureDAT2-otf (CC-BY-4.0, 2023-12), and NES/Genesis composite-cleanup models (1x, MIT, 2025). The last group are video-capture restorers, not upscalers of hand-pixelled art.
- **3x models:** only research or photo models (3x-DAT-2, 3x-OmniSR, 3x SwinIR; Apache-2.0, bicubic-trained). There is no 3x pixel-art model.
- The website's tag filter (`/?t=pixel-art`) did not render the list through WebFetch. The repository JSON is the source of truth.

### 2.2 Runners and frameworks

| Tool | Status (verified) | Licence | Fit |
|---|---|---|---|
| **chaiNNer** | v0.25.0 (2025-10-19) "Update Torch & Torchvision to support RTX 50 series". v0.25.1 (2025-10-23). Nightlies up to 2026-02-23. Repo pushed 2026-10-01 [V-fetch, GitHub API] | GPL-3.0 | **Recommended GUI.** Has wrap/mirror/replicate padding in *Upscale Image*, custom scale, *Quantize to Reference*, *Dither (Palette)*, *Wavelet Color Fix*, *Interpolate Models*, *Resize Pixel Art* (hqx, Eagle, AdvMAME, SaI; **no xBRZ/MMPX**, issue #2422 still open), and an experimental CLI with input overrides [V-src, V-fetch]. |
| **spandrel** | v0.4.2 (2026-02-21), adds FDAT and AuraSR, bf16 for DAT/HAT [V-fetch] | MIT (each architecture keeps its own licence. Non-commercial architectures are in `spandrel_extra_arches`) | **Recommended for scripting.** One loader for .pth/.safetensors of about 40 architectures. Used by chaiNNer, ComfyUI and traiNNer-redux. |
| **traiNNer-redux** | Repo pushed 2026-10-03. `torch>=2.11`, Python 3.12/3.13 (not 3.14). `install.bat` uses the cu128 index. Scales 1/2/3/4/8. Templates for about 60 architectures (ESRGAN, RealPLKSR, SPAN, DAT, HAT, ATD, DRCT, FDAT, MoSR, RCAN, Compact, ...), fidelity and GAN, paired or OTF-degradation [V-src] | Apache-2.0 | **Recommended for a custom model.** |
| **Upscayl** | Last release v2.15.0 (2024-12-25). Repo active (2026-09-28). NCNN/Vulkan backend only. Bundled models are general (HFA2k "High Fidelity", Remacri, UltraSharp, Ultramix) [V-fetch] | AGPL-3.0 | **Not suitable.** No wrap padding, no palette tools, needs pth-to-ncnn conversion for pixel-art models. A "Upscayl 2.5 (2026)" article in search results contradicts GitHub and looks auto-generated [S, disregarded]. |
| **Real-ESRGAN** (xinntao) | Last push 2024-08-06 [V-fetch]. Models: x4plus, x2plus, anime_6B, realesr-general-x4v3, realesr-animevideov3 (x1-x4) [V-fetch] | BSD-3 | Baseline only (it did poorly on U7, section 4). |
| **Real-ESRGAN-lp** | "Local padding" fork that removes tile seams in patch inference (arXiv 2309.02340) [V-fetch] | BSD-3 | Interesting reference. Our chunk sizes fit in one pass, so we do not need patching. |
| **Real-CUGAN** (bilibili) | Native 2x/3x/4x anime models, NCNN port [S] | MIT [S] | Only public native-3x option for a 2x-then-3x chain, but anime-trained. Not tested. |
| **waifu2x / nunif** | swin_unet "art" models, scale 1/2/4 [S] | MIT [S] | Anime art. Not tested. |

### 2.3 PyTorch for the RTX 5070 Ti (Blackwell, sm_120) [V-fetch: download.pytorch.org index, 2026-10-04]
- Newest torch on each index for cp313 win_amd64: **cu130 = 2.14.1, cu132 = 2.14.1**, cu126 = 2.14.1 (no Blackwell kernels), **cu128 = 2.11.0** (frozen there), cu129 = 2.9.0.
- Recommendation: `pip install torch torchvision --index-url https://download.pytorch.org/whl/cu130`. traiNNer-redux's `install.bat` pins the cu128 index and therefore gets torch 2.11.0, which still satisfies its `torch>=2.11.0`. Either works on a 5070 Ti.
- WSL2 on this machine already sees the GPU (`nvidia-smi`: RTX 5070 Ti, 16303 MiB, driver 617.14) [V-run]. Linux-side CUDA inference is therefore also possible.

---

## 3. Candidate models (details)

All downloads below are from the OpenModelDB JSON. SHA-256 prefixes match OpenModelDB for the files I downloaded [V-run]. Most ESRGAN RRDB 64nf/23nb models are 16.7 M parameters and about 64 MB. DAT2 models are 11.2 M parameters and about 140 MB.

| Model | Arch / scale | Date | Licence | Training data / purpose | Download | Fit for U7 tiles |
|---|---|---|---|---|---|---|
| **4x-NXbrz** | ESRGAN 4x | 2021-06 | CC-BY-NC-SA-4.0 | Mix of xBRZ- and ScaleNX-upscaled images as HR ("simpler style") | `objectstorage.us-phoenix-1.oraclecloud.com/n/ax6ygfvpvzka/b/open-modeldb-files/o/4x-NXbrz.pth` | **Top pick for fidelity.** Smooth contours, low invention, most context-stable. |
| **4x-xbrz** / **4x-scalenx** / 4x-xbrz-dd | ESRGAN 4x | 2019-06 | CC-BY-NC-4.0 | Mimic xBRZ / ScaleNx. "dd" adds de-dithering | Google Drive (**files are RAR archives named .pth**. Extract first.) | Like NXbrz. xbrz-dd removes too much (dither looks like texture). |
| **8x-Arzenal-v1-1** | ESRGAN 8x | 2021-02 | CC-BY-NC-SA-4.0 | Interpolation of NMKD pixel-art models plus de-dither versions. "Smooth general pixel art, Minecraft textures" | oracle mirror `.../o/8x-Arzenal-v1-1.pth` | **Top pick for a painterly look.** 8 to 6 box is clean. |
| **8x-MS-Unpainter** (+ De-Dither) | ESRGAN 8x | 2021-03 | CC-BY-NC-SA-4.0 | Mix of Arzenal, MS-Unpainter, NMKD-Sphax, YanderePixelArt4 | oracle mirror `.../o/8x-MS-Unpainter.pth` | Nearly identical to Arzenal on U7. |
| **4x-PixelPerfectV4** | ESRGAN 4x | 2020-11 | **WTFPL** | 300 anime images, 137k iterations. "Sprite upscaler" | Google Drive `1x583PgpY5O4T2603gJK8kwrGiIc4aFne` | Smooth and painterly. Erases speckle. Best permissive licence. |
| **4x-Fatal-Pixels** | ESRGAN 4x | 2020-06 | CC-BY-NC-SA-4.0 | Anime/manga, pixel art and sprites | oracle mirror | Busy hatching, more invention. |
| 4x-deviantPixelHD | ESRGAN 4x | 2019-09 | CC-BY-NC-4.0 | DeviantArt digital art. Used by AlbionHiRes | Google Drive | Grainy. Albion project needed waifu2x denoise after it. |
| 4x-Struzan | ESRGAN 4x | 2020-10 | CC-BY-NC-SA-4.0 | Airbrush/pencil posters. Basis of Doom "pencil style" chaiNNer preset [S] | oracle mirror | Cross-hatch noise on U7. Reject. |
| 2x-Faithful(-Lite) | ESRGAN 2x | 2019/2020 | CC-BY-NC(-SA) | Minecraft default to Faithful32 | Google Drive / oracle | Needs three passes for 6x: colour drift. Reject. |
| 4x-PocketMonsters-Alpha, 8x-Sphax-Alpha-NN, 2x-Gen5-Alpha | ESRGAN, **RGBA in/out** | 2021-02 | CC-BY-NC-SA-4.0 | Pixel art with alpha, LR = nearest downscale | pCloud only | Candidates for **RLE sprites** later (native alpha). Not tested (pCloud). |
| 1x-PixelSharpen | ESRGAN 1x | 2021-03 | CC-BY-NC-SA-4.0 | Restores blurry or upscaled pixel art (PixelJoint) | OneDrive | Possible pre-pass. Not tested. |
| 4x-UltraFArt-v3 (Fine/Smooth/Photo) | ESRGAN 4x, 128nf | 2021-05 | CC-BY-NC-SA-4.0 | ArtStation tiles | OneDrive | Not tested (OneDrive). |
| **4x-UltraSharpV2** (+ Lite = RealPLKSR) | **DAT2** 4x | **2025-05-23** | CC-BY-NC-SA-4.0 | Private multi-domain dataset | `huggingface.co/Kim2091/UltraSharpV2` (safetensors, ONNX) | **Best general model.** Adds blade texture but keeps staircases. Use as blend or second pass. |
| **4x-PBRify-UpscalerV4** | **DAT2** 4x | **2025-05-19** | **CC0-1.0** | 2000s-era game textures | `github.com/Kim2091/Kim2091-Models/releases/download/4x-PBRify_UpscalerV4/4x-PBRify_UpscalerV4.pth` | Similar to UltraSharpV2, slightly noisier. **CC0** makes it the cleanest licence for a fine-tune base. |
| 4x-TextureDAT2-otf | DAT2 4x | 2023-12 | CC-BY-4.0 | GTA V textures, OTF degradations | GitHub Phhofm/models release | Not tested. Permissive, possible fine-tune base. |
| RealESRGAN_x4plus / anime_6B | ESRGAN 4x | 2021-22 | BSD-3 | Real-world / anime | GitHub xinntao releases | Poor on U7 (section 4). |

Other permissive bases for fine-tuning: the research pretrains (DAT-2 x2/x3/x4, SwinIR, OmniSR, all Apache-2.0) and the traiNNer-redux "pretrained-models" release [V-src].

---

## 4. Local bake-off on real U7 art [V-run]

**Set-up.** Everything is in `/home/simonea/ultima7_exult/tmp/sr_bakeoff/`: `bakeoff.py` and `seams.py`, models in `models/`, results in `out/`.
- CPU inference (16 threads), torch 2.14.1+cpu, spandrel 0.4.2.
- Input: `art_original/superchunks/schunk_030.png`, a 96x96 window at (624,32) with **16 px of real map context** on every side. It holds grass, dirt bank, a river channel and the shoreline.
- Each model runs at native scale, then the window is cropped. 8x and 16x results are box-downscaled to 6x; 4x results are Lanczos-upscaled to 6x. 2x models are applied three times, then box-downscaled.
- Quantization: (a) global nearest in OKLab over indices 0x00-0xDF; (b) local "quantize-to-reference" (3x3 low-res neighbourhood). Cycling pixels are copied with nearest.
- **Round-trip fidelity:** box-downscale the 6x result back to 1x, then (i) mean ΔE·100 in OKLab against the original, and (ii) % of non-cycling pixels whose nearest palette index equals the original index. This rewards *faithfulness*, not beauty. Bicubic scores well on it but looks blurry.

### 4.1 Fidelity metrics (mixed-terrain window)

| Model | Arch | CPU time | rt ΔE (RGB) | rt index match (RGB) | match after local quantize | colours after global quantize |
|---|---|---|---|---|---|---|
| nearest (reference) | – | – | 0.00 | 100 % | 100 % | 56 |
| bicubic | – | – | 1.51 | 85.2 % | 87.6 % | 109 |
| **4x-UltraSharpV2** | DAT2 | 12.9 s | **1.70** | **80.1 %** | 80.8 % | 128 |
| **4x-NXbrz** | ESRGAN | 7.2 s | 2.06 | 72.2 % | **80.5 %** | 114 |
| 4x-xbrz | ESRGAN | 9.4 s | 1.98 | 72.7 % | **82.1 %** | 127 |
| 4x-Struzan | ESRGAN | 8.6 s | 1.84 | 81.7 % | 73.9 % | 207 |
| 4x-PBRify-UpscalerV4 | DAT2 | 3.3 s | 1.90 | 78.2 % | 77.1 % | 138 |
| 4x-scalenx | ESRGAN | 14.8 s | 2.13 | 73.1 % | 76.0 % | 116 |
| 4x-PixelPerfectV4 | ESRGAN | 12.7 s | 2.61 | 65.7 % | 71.4 % | 137 |
| 4x-Fatal-Pixels | ESRGAN | 7.6 s | 2.89 | 59.3 % | 65.8 % | 198 |
| 8x-MS-Unpainter | ESRGAN | 4.8 s | 2.99 | 63.3 % | 65.6 % | 144 |
| 8x-Arzenal-v1-1 | ESRGAN | 6.1 s | 3.02 | 63.4 % | 65.0 % | 141 |
| 4x-deviantPixelHD | ESRGAN | 11.0 s | 2.80 | 62.3 % | 64.8 % | 163 |
| 4x-xbrz-dd | ESRGAN | 14.1 s | 3.61 | 51.7 % | 59.5 % | 117 |
| RealESRGAN_x4plus | ESRGAN | 4.8 s | 3.83 | 42.7 % | 49.9 % | 149 |
| RealESRGAN_x4plus_anime_6B | ESRGAN-6B | 5.0 s | 4.14 | 43.6 % | 47.3 % | 176 |
| 2x-Faithful (applied 3 times) | ESRGAN | 74.2 s | 5.92 | 20.2 % | 41.4 % | 139 |

(A first run on a mostly-water window ranked the models the same way. Times are noisy because jobs shared the CPU.)

### 4.2 Visual findings
See `out/E1_sheet.png` (raw RGB beside palette-local, 48x48 low-res crop at 6x for every model) and `out/E1_zoom.png` (1:1 crop of dirt bank and shoreline, 8 models).
- **Pixel-art-trained models** (NXbrz, xbrz, scalenx, Arzenal, MS-Unpainter, PixelPerfect) **turn the staircase shoreline into smooth curves** and build coherent "strokes" on the dirt bank.
  - NXbrz and xbrz look like blob/vector art with very little invention.
  - Arzenal and MS-Unpainter give long flowing painterly strokes. This is the most "hand-painted hi-res" feel, but it re-interprets the art most strongly. The round-trip match drops to about 65 %.
  - PixelPerfectV4 smooths speckle into soft strokes, so grass loses its texture.
- **General models** (UltraSharpV2, PBRify, RealESRGAN) **keep the pixel staircases** on diagonal edges, which reads as "sharp but still pixelated". UltraSharpV2 and PBRify add plausible grass-blade micro-texture. UltraSharpV2 has the best fidelity of all real SR models.
- **RealESRGAN_x4plus_anime_6B** wipes the grass texture into flat colour: a meaning change. **4x-Struzan** adds a pencil cross-hatch. **2x-Faithful** chained three times shifts green hue and saturation.
- **Cycling sparkles** (water glints 0xE0-0xE7) copied by nearest stay as 6x6 blocks next to smooth SR content. They need a label-preserving smooth upscaler (xBRZ-6x or MMPX on the *index* image, or the SR-upscaled mask thresholded) so their shape matches.
- Local palette quantization keeps the original's 56 colours (global nearest uses 110-200 of the 224) and avoids hue drift. With it, output colours are always ones the artist used in that spot.

### 4.3 Seams on lone tiles (E3)
Test: tiles (19,25), (137,26) and (136,1). The metric is the step across the wrap seam divided by the mean step across the other low-res pixel boundaries. 1.0 means "invisible".

| Padding of a lone 8x8 tile | NXbrz | Arzenal | PixelPerfect | UltraSharpV2 | RealESRGAN x4+ |
|---|---|---|---|---|---|
| zero (bare tile) | 4.4-6.5 | 8.1-18.0 | 4.7-10.1 | 3.1-9.1 | 3.0-5.2 |
| reflect 16 px (chaiNNer's suggestion for tiny images) | 3.9-5.3 | 8.6-10.5 | 4.4-9.5 | 3.7-9.9 | 1.7-6.8 |
| wrap 5x5 (40x40), centre crop | 1.9-2.5 | 1.2-2.1 | 0.9-3.4 | 0.8-2.3 | 1.1-1.8 |
| **circular conv padding**, bare tile | = wrap (MAE 0.001-0.005) | = wrap (MAE ≤ 0.17) | = wrap (MAE ≤ 0.17) | ≠ wrap (MAE 2-9; attention windows) | ≠ wrap (MAE 0.4-12; needs > 5x5 context) |
| *source art itself (nearest 6x)* | *1.4-1.8* | | | | |

Conclusions:
- Zero or reflect padding on a lone tile creates visible frames.
- Wrap context brings the seam down to the level that is **already in the source**: these frames do not tile with themselves in the original either. See `out/E3_*.png` (columns: zero | reflect | wrap5x5 | circular, each tiled 3x3).
- Circular padding is a zero-cost exact "infinite wrap" for pure-conv models.

### 4.4 Context sensitivity (E4)
Test: the same (shape, frame) cut from 40 random real map positions, each upscaled with 16 px of real context. The metric is the per-pixel std of the 48x48 result across instances, in 0-255 units.

| Tile | NXbrz | Arzenal | PixelPerfect | UltraSharpV2 | RealESRGAN x4+ |
|---|---|---|---|---|---|
| deep-water flats 65:8/10/16/19 (4 colours) | 0.04-0.52 | 0.09-0.96 | 0.09-1.25 | 0.18-0.80 | 0.75-1.54 |
| sand/dirt flats 10:7, 10:8 (6 colours) | **1.3-1.5** | 5.4-5.7 | 4.8-5.9 | 6.6-8.7 | 5.7-7.0 |

- In all models, the spread is concentrated in the 6 px border band.
- NXbrz is 4-6x more deterministic than the others. A per-tile canonical override built from it changes little with context.
- For the painterly models, use **consensus**: take the per-pixel median over all instances, or quantize the mean. Better still, use chunk-level overrides (section 5.3).
- Cost: about 9 minutes of CPU for 5 models × 6 tiles × 40 instances. On the 5070 Ti this is negligible.

---

## 5. Recommended pipeline design (SR part)

### 5.1 Scale to exactly 6x
| Route | How | Notes |
|---|---|---|
| **8x model, then box 0.75** | Arzenal, MS-Unpainter, Sphax-Alpha-NN, or a custom 8x | Supersampling gives clean anti-aliasing. **Preferred.** |
| 4x model, then Lanczos 1.5x | NXbrz, UltraSharpV2, ... | Slight softness. Fine before palette quantization. |
| 4x model twice (16x), then box 6/16 | chaiNNer "Custom Scale" does this automatically: `ceil(log_s(6))` passes, then BOX [V-src] | Doubles any invention. Use only with very faithful models. |
| 2x, then 3x | Real-CUGAN 2x + 3x, or DAT-2 x3 (photo) | No pixel-art 3x exists. Not recommended. |
| Algorithmic 2x (MMPX/xBRZ), then 3x net | Needs a 3x net trained on such inputs | Option for a custom model. |
| **Custom model: 8x, or 3x on a 2x-nearest prescale** | traiNNer-redux supports 1/2/3/4/8 [V-src] | Best long-term option. |

### 5.2 Context for tiny inputs (key design decision)
1. **Do not use self-wrap per tile** except for the few frames that really self-tile. Test that per frame with the source seam ratio above: ≤ 1.1 means self-tiling.
2. **Chunk-level processing.** U7 terrain is defined per chunk: 16x16 tiles, 2105 distinct chunks used on the map. Render each chunk with its **most frequent real neighbours** as 16-32 px padding (or a 3x3 block of real chunks from the map), upscale, and crop to 768x768.
   - Chunks do appear in different neighbourhoods. Either accept a small chunk-border mismatch, or blend a 6-12 px band at chunk borders at runtime, or run consensus over the chunk's instances.
3. **Per-tile canonical overrides** (if the engine design is tile-based): cut each (shape, frame) from all chunk results. Combine with per-pixel median or trimmed mean, then local-quantize. The border band carries most of the variance (section 4.4), so a deterministic model (NXbrz-like) or a custom model matters here.
4. **Pure-conv models plus circular padding** give exact periodicity for any *region* that should wrap, for example a whole repeating texture sheet assembled from a shape's 32 frames in their natural arrangement. It needs no extra compute.
5. **Map windows** (superchunk renders, 2048x2048) can also be processed whole with overlapping tiles (chaiNNer auto-tiling or Real-ESRGAN-lp style). On 16 GB, ESRGAN handles 512-1024 px tiles easily.

### 5.3 Post-SR conversion to indexed art
1. Optional colour correction: chaiNNer *Wavelet Color Fix* against a nearest-upscaled original, as AlbionHiRes did with ImageMagick "colorize".
2. **Local palette quantization.** Candidates are the original indices in the (2r+1)² low-res neighbourhood (r=1), with OKLab distance. Fall back to global nearest over 0x00-0xDF. chaiNNer's *Quantize to Reference* does the same in RGB [V-src]. Optionally use ordered dithering *only* inside a ramp.
3. **Cycling ranges:**
   - Build a mask of original 0xE0-0xFE pixels.
   - Upscale the **index image or mask** with a label-safe method (xBRZ 6x is the native maximum of xBRZ [V-fetch]; MMPX is 2x only and palette-preserving [S]).
   - Inside the mask, write the label-upscaled original index ("copy semantics"). Never let the quantizer emit ≥ 0xE0.
   - For flowing water, a gradient of cycling indices along the flow can be authored (see palette.md §7.4).
4. **Transparency (RLE sprites, later):** use an RGBA model (PocketMonsters-Alpha, Sphax-Alpha-NN, Gen5-Alpha), or chaiNNer's separate-alpha path. Before SR, "alpha-bleed" the RGB under transparent pixels to stop halos. Threshold alpha to binary afterwards (U7 uses a 0xFF key). Handle translucent-palette pixels as labels, like cycling.
5. **Round-trip QA gate per tile:** box-downscale the final 6x indexed tile and compare with the original. Flag tiles where index agreement falls below a threshold (for example 70 %) or where a single 8x8 cell's mean ΔE exceeds a bound. This is a cheap automatic "no meaning change" check.

### 5.4 Custom model (best route to consistent quality)
- **Framework:** traiNNer-redux (Windows: `install.bat`, Python 3.13). Start from `options/_templates/train/<ARCH>/<ARCH>_fidelity.yml`.
  - For speed and circular padding, use RealPLKSR, SPAN or ESRGAN-lite.
  - For quality, use DAT2 (window attention, so use wrap context rather than circular).
  - Pretrain from a permissive checkpoint: 4x-PBRify-UpscalerV4 (CC0), DAT-2 research weights (Apache-2.0), traiNNer pretrains.
- **Data:**
  - (a) **Synthetic pairs.** HR = painted or hand-made fantasy terrain textures with compatible licences. LR = area-downscale ÷6, then quantize to the **U7 palette 0** (with and without ordered dither). Optionally re-pixel with pyxelate-style downsampling (MIT) to mimic hand-pixel edges.
  - (b) **Bootstrapped targets.** A few hundred U7 tiles or chunks upscaled with a diffusion img2img pass (another agent's angle), hand-curated, then used as HR for the real U7 LR.
  - This distils the chosen look into a deterministic, context-stable network and keeps the whole tileset in one style.
- **Losses:** pixel/L1 or MS-SSIM-L1 plus a light perceptual loss, no or low GAN for fidelity, plus **`averageloss`** (AvgPool-downscale consistency) [V-src] to enforce the round-trip constraint.
- **Scale:** 8x (then box to 6x), or 3x with a 2x-nearest prescaled input. 6 is not a supported scale.
- **Compute:** a SPAN or RealPLKSR fidelity run of 50-150k iterations at lq_size 64 is a few hours to about one day on a 5070 Ti (estimate; traiNNer's sample log shows a 4x SPAN at about 4.7 it/s on an unspecified GPU [V-src]).

---

## 6. Install notes (Windows, E:\Dati\Ultima7_Upscale)

```bat
:: Python scripting route (recommended for the batch pipeline)
py -3.13 -m venv E:\Dati\Ultima7_Upscale\venv
E:\Dati\Ultima7_Upscale\venv\Scripts\activate
pip install torch torchvision --index-url https://download.pytorch.org/whl/cu130
pip install spandrel pillow numpy safetensors
:: optional: pip install spandrel_extra_arches   (non-commercial architectures)
```
```python
# minimal 6x runner with spandrel (works for .pth/.safetensors of ESRGAN, DAT, SPAN, ...)
from spandrel import ModelLoader; import torch
d = ModelLoader().load_from_file(r"E:\Dati\Ultima7_Upscale\models\8x-Arzenal-v1-1.pth").cuda().eval()
# for pure-conv archs, exact wrap-around:
# for m in d.model.modules():
#     if isinstance(m, torch.nn.Conv2d) and m.padding != (0, 0): m.padding_mode = "circular"
with torch.inference_mode(): y = d(x.cuda())   # x: 1x3xHxW float 0..1 (context window)
```
- **chaiNNer:** download v0.25.1 or a nightly from chainner.app or GitHub. In *Dependency Manager*, update PyTorch (RTX 50 note in the v0.25.0 release). For headless batch runs: `chainner.exe run chain.chn --override overrides.json`. The CLI is experimental and console output is hidden [V-src docs].
- **traiNNer-redux:** `git clone https://github.com/the-database/traiNNer-redux` → `install.bat` → `python train.py --auto_resume -opt options/train/<ARCH>/<cfg>.yml`.
- **VRAM (estimates):** ESRGAN RRDB fp32 on a 160x160 input is well under 2 GB. DAT2 on 160x160 needs a few GB. Superchunk windows (2048 px) need tiling, which chaiNNer does automatically. 16 GB is never the constraint here. The whole map is about 2105 chunks × < 1 s each on GPU. On CPU I measured 5-15 s per 128 px window.

---

## 7. Pitfalls found or confirmed

1. **Google Drive "pth" files are RAR archives** (4x-xbrz, 4x-scalenx, 4x-xbrz-dd). spandrel fails with "unpickling stack underflow". After extraction, SHA-256 matches OpenModelDB [V-run]. OneDrive and pCloud links (UltraFArt, BigFArt, PixelSharpen, the Alpha models) need a browser.
2. **Zero padding at tile edges** produces dark frames. **Reflect padding on 8x8 tiles** produces bright or mirrored frames [V-run]. Use real context.
3. **Self-wrap is not neutral:** U7 frames are not self-tiling (source seam ratio 1.4-1.8) [V-run].
4. **Circular padding only for pure-conv models.** Transformers (DAT, HAT, SwinIR, ATD) disagree with wrap context by 2-9/255 [V-run].
5. **General SR models keep staircases.** Pixel-art models remove them [V-run]. This is a choice of look, not a bug.
6. **Chaining small-scale models** (2x applied three times) accumulates colour shift [V-run].
7. **Round-trip metrics favour blur.** Always pair them with visual review. Bicubic "wins" fidelity.
8. **Global palette quantization** of SR output spreads 110-200 palette entries over a 56-colour region and drifts hue. Use local quantization [V-run].
9. **Licences:**
   - 45/53 pixel-art models are non-commercial (NC). SA variants ask derivatives of the *model* to keep the licence.
   - The status of *outputs* is generally not addressed by those licences.
   - The U7 art itself remains EA/Origin copyright, so packs are "requires original data" mods anyway.
   - Prefer WTFPL/CC0 (PixelPerfectV4, PBRify V4) or a self-trained model if distribution matters.
10. **traiNNer-redux:** Python 3.14 is not supported, and the cu128 index stops at torch 2.11 [V-src, V-fetch].
11. **WebFetch summaries sometimes misdate GitHub releases.** The chaiNNer v0.25.0 summary said 2024. The GitHub API says **2025-10-19** [V-fetch].

---

## 8. Prior art (closest analogues)

- **AlbionHiRes** (2022, Albion 1995, 256-colour 16x16 tiles) [V-fetch]:
  - ESRGAN 4x-deviantPixelHD at 4x, then waifu2x CUnet denoise.
  - Tiles were **extracted with context from real game maps** ("otherwise the neural network will not have enough information").
  - "Colorize" correction against the original, masks for transparency, centre crop.
  - Some edge seams remained. The authors suggest averaging along edges.
  - This is the same idea as section 5.2, minus consensus and palette return.
- **Doom ESRGAN presets** (Doomworld, 2023-2024): chaiNNer preset built around 4x-Struzan ("pencil style"), separate sprite and texture models, "Erodil-3-RGB" preset in Feb 2024 [S]. The forum page sits behind Cloudflare (403) and was not fetched.
- **Barony ESRGAN tiles** (Steam Workshop) [S, not fetched].
- **Exult** already ships runtime scalers (2xSaI, hq2x/3x/4x, xBR, bilinear, point) in `exult-1.12.1/imagewin/` [V-run, local tree]. Those are useful as label-safe or baseline scalers.

---

## 9. Pointers outside this angle (not evaluated here)
- One-step diffusion restorers popular in 2025-26 ComfyUI workflows: **SeedVR2** (ByteDance, Apache-2.0 [S], 3B/7B, FP8/INT8/NVFP4 variants, native ComfyUI node) [S]. Also **AuraSR** (GAN, now loadable by spandrel 0.4.2 [V-fetch]).
- Algorithmic pixel-art scalers: **xBRZ** 2-6x (GPL-3.0, `SCALE_FACTOR_MAX = 6`) [V-fetch]. **MMPX** (McGuire & Gagiu, JCGT 2021, 2x, palette-, transparency- and single-pixel-preserving) [S]. **ScaleFX**. **Kopf-Lischinski depixelizing** (SIGGRAPH 2011 vectorization) [S]. **Structure-aware block-size scaling** (Applied Sciences, Feb 2026) [S].

---

## 10. Sources

**Verified by fetching or cloning**
- OpenModelDB data repo (clone, last commit 2026-08-14): https://github.com/OpenModelDB/open-model-database (model JSON for every model named above)
- OpenModelDB model page: https://openmodeldb.info/models/4x-PixelPerfectV4
- chaiNNer releases (GitHub API): https://github.com/chaiNNer-org/chaiNNer/releases. Source clone (commit 2026-07-31): `backend/src/packages/chaiNNer_standard/image_filter/quantize/quantize_to_reference.py`, `.../image_dimension/resize/resize_pixel_art.py`, `backend/src/nodes/impl/upscale/basic_upscale.py`, `.../chaiNNer_pytorch/pytorch/processing/upscale_image.py`, `wavelet_color_fix.py`, `guided_upscale.py`, `docs/05--CLI.md`
- chaiNNer issue #2422 (xBRZ/ScaleNX request, open): https://github.com/chaiNNer-org/chaiNNer/issues/2422
- spandrel README and v0.4.2 release: https://github.com/chaiNNer-org/spandrel
- traiNNer-redux (clone, commit 2026-10-03): https://github.com/the-database/traiNNer-redux (`docs/source/getting_started.md`, `config_reference.md`, `loss_reference.md`, `traiNNer/losses/basic_loss.py`, `options/_templates/train/*`)
- Upscayl README and releases (GitHub API, v2.15.0 2024-12-25): https://github.com/upscayl/upscayl
- Real-ESRGAN model zoo: https://github.com/xinntao/Real-ESRGAN/blob/master/docs/model_zoo.md
- UltraSharpV2 model card: https://huggingface.co/Kim2091/UltraSharpV2
- PyTorch wheel indexes: https://download.pytorch.org/whl/cu128/torch/ , https://download.pytorch.org/whl/cu130/torch/ (and cu126, cu129, cu132)
- AlbionHiRes README: https://github.com/IS4Code/AlbionHiRes
- joeyballentine ESRGAN fork (seamless tile/mirror/replicate/alpha_pad modes): https://github.com/joeyballentine/esrgan
- Real-ESRGAN-lp: https://github.com/Alhasan-Abdellatif/Real-ESRGAN-lp
- xBRZ header (DOSBox-X doxygen): https://dosbox-x.com/doxygen/html/xbrz_8h_source.html
- HuggingFace model search API (no dedicated 2025-26 pixel-art SR models found): https://huggingface.co/api/models?search=pixel-art
- GitHub search API (2025-26 pixel-art SR repositories are tiny hobby projects)

**Search-snippet only**
- Doomworld "Doom ESRGAN AI-upscale models" (403 Cloudflare): https://www.doomworld.com/forum/topic/136060-doom-esrgan-ai-upscale-models/
- Doomworld sprite method: https://www.doomworld.com/forum/topic/106611-a-simple-method-of-scaling-up-2d-sprites-with-esrgan/
- Local Padding paper: https://arxiv.org/abs/2309.02340
- MMPX: https://jcgt.org/published/0010/02/04/paper.pdf , https://github.com/ITotalJustice/mmpx
- Kopf & Lischinski 2011: https://dl.acm.org/ft_gateway.cfm?id=1964994
- Structure-Aware Pixel Art Scaling (2026): https://www.mdpi.com/2076-3417/16/5/2314
- Real-CUGAN: https://github.com/bilibili/ailab/tree/main/Real-CUGAN
- nunif / waifu2x: https://github.com/nagadomi/nunif
- pyxelate: https://github.com/sedthh/pyxelate
- SeedVR2: https://docs.comfy.org/tutorials/utility/seedvr2
- PyTorch Blackwell (2.7 / cu128 first support): https://github.com/comfyanonymous/ComfyUI/discussions/6643
- Barony ESRGAN tiles: https://steamcommunity.com/sharedfiles/filedetails/?id=2194368690

**Local artefacts (this session)**
- Scripts: `/home/simonea/ultima7_exult/tmp/sr_bakeoff/bakeoff.py`, `/home/simonea/ultima7_exult/tmp/sr_bakeoff/seams.py`
- Results: `/home/simonea/ultima7_exult/tmp/sr_bakeoff/out/` (`E1_sheet.png`, `E1_zoom.png`, `E1_<model>_rgb.png` and `_qlocal.png` (576x576 = 6x of the 96x96 window), `E1_metrics.json`, `E1b_metrics.json`, `E3_*.png`, `E3_metrics.json`, `E4_metrics.json`)
- Models (1.5 GB, SHA-256-checked): `/home/simonea/ultima7_exult/tmp/sr_bakeoff/models/`

---

## Fact-check

*An adversarial fact-check was run on 2026-10-04. I tried to refute the 10 claims that matter most for a decision, using primary sources (git clones, raw files, PyPI, the PyTorch wheel indexes, the Hugging Face API) and runs on this machine's RTX 5070 Ti in WSL2. The GitHub REST API was rate-limited, so dates come from git tags and commits instead. Scratch files are in `/home/simonea/ultima7_exult/tmp/factcheck/`. OpenRouter and diffusion claims are not in this report and were not checked here.*

| # | Claim | Verdict | Evidence (source, date) |
|---|---|---|---|
| 1 | **4x-NXbrz**: ESRGAN 64nf/23nb, 4x, by archerpolation, 2021-06, CC-BY-NC-SA-4.0. HR side trained on xBRZ and ScaleNX output. Direct download from the oracle mirror. SHA-256 verified. | **Confirmed** | The OpenModelDB JSON (repo commit 782aac0, 2026-08-14) gives `date 2021-06-09`, the licence, `dataset: Xbrz + ScaleNX`, 150k iterations and sha256 `a822ae2d…`. The local `.pth` hashes to the same value. The oracle URL returns HTTP 200 today. |
| 2 | **8x-Arzenal-v1-1 / 8x-MS-Unpainter**: ESRGAN 8x, 2021-02 and 2021-03, CC-BY-NC-SA-4.0, direct oracle-mirror downloads. | **Confirmed** | JSON: Arzenal is by computerk, 2021-02-10, sha `2166b4c4…`. MS-Unpainter is by foolhardy, 2021-03-24, sha `f5780ebe…`, and is an interpolation of Arzenal, MS-Unpainter, Sphax and Yandere. Both local hashes match, and the Arzenal oracle URL returns 200. |
| 3 | **Permissive licences**: 4x-PixelPerfectV4 is WTFPL (by Mutin Choler, 300 anime images, Google Drive), and 4x-PBRify-UpscalerV4 is CC0, which makes it "the cleanest fine-tune base". | **Confirmed, with a caveat** | JSON: PixelPerfectV4 is WTFPL, 2020-11-16, `datasetSize 300` "Random anime images", Google Drive only. PBRify V4 is CC0-1.0, DAT2, 2025-05-19, and its GitHub release URL returns 200 (sha matches). **Caveat:** CC0 *pure-convolution* siblings also exist. They are **4x-PBRify-UpscalerSPANV4** (SPAN, 2024-04-11) and 4x-PBRify-RPLKSRd-V3 (RealPLKSR + DySample, 2024-09-23). SPAN accepts the circular-padding trick (§4.3) and is much faster, so it may be the better CC0 base for a seam-free custom model. Among pixel-art-tagged models, the permissive ones are PixelPerfectV4, 1x-ArtClarity (WTFPL), HDCube and Skyrim-Alpha (CC0), as the report says. |
| 4 | **4x-UltraSharpV2**: DAT2, released 2025-05-23, CC-BY-NC-SA-4.0, 11.2 M params, about 140 MB safetensors. The Lite variant is RealPLKSR. | **Confirmed** | The HF card says "5/23/25", CC BY-NC-SA 4.0, and "Lite … RealPLKSR". The HF API gives `cc-by-nc-sa-4.0` and a 139,792,588-byte safetensors file. spandrel counts **11.21 M parameters**. The rest of the file is about 90 MB of attention buffers. |
| 5 | **OpenModelDB catalogue**: 53 Pixel-Art models dated 2019-02-21 to 2022-05-10. 52 are ESRGAN, 45 are NC, and there is no 2025-26 pixel-art model. | **Confirmed** for OpenModelDB. Weakly supported beyond it. | Recount on a fresh clone gives 671 models in total and 53 tagged `pixel-art`. That is 52 esrgan + 1 spsr, and 25 CC-BY-NC-SA + 20 CC-BY-NC = 45 NC. The 2024-26 entries are all anime, cartoon, game-texture or video-restoration models. A search of the HF model API ("pixel", "pixel-art") found no pixel-art SR model, only LTX "pixel-space" video upscalers. A web search found nothing either. |
| 6 | **traiNNer-redux trains only scales 1/2/3/4/8, so there is no native 6x.** "6 is not a supported scale" (§5.4). | **Refuted (overstated)** | `ALL_SCALES = [1, 2, 3, 4, 8]` in `traiNNer/archs/arch_info.py` only drives template generation. The config schema says "*Most* architectures support 1, 2, 3, 4, or 8". `scale` is a free `int`, and `sr_model.py:58` passes it straight to the network constructor (`build_network({**opt.network_g, "scale": opt.scale})`). I found no global check on the scale value. Pixel-shuffle architectures build fine at 6x: spandrel `SPAN(upscale=6)` and `RealPLKSR(upscaling_factor=6)` both map 16x16 to 96x96 (run here). **A native-6x SPAN or RealPLKSR custom model is therefore plausible**, which avoids the 8x→box or 3x-on-2x detour. I did not run a full training job. ESRGAN (power-of-2 upsampling) and Real-CUGAN (2/3/4) remain restricted. The rest of the row holds: Apache-2.0 (`LICENSE.txt`), last commit 2026-10-03, `torch>=2.11.0`, the docs say "Python 3.13 recommended, 3.12 supported, 3.14 not supported", and `install.bat` uses the cu128 index. |
| 7 | **RTX 5070 Ti / PyTorch**: cu128 index frozen at 2.11.0. cu130 and cu132 have 2.14.1. cu126 has no sm_120. Use `--index-url …/cu130`. | **Confirmed**, and tested on this GPU | The wheel indexes (cp313 win_amd64, fetched today) list cu126 = 2.14.1, cu128 = **2.11.0**, cu129 = 2.9.0, cu130 = **2.14.1** and cu132 = 2.14.1. PyTorch `RELEASE.md` (main) shows that 2.11 was the last release with CUDA 12.8. Its 2.14 arch matrix gives CUDA 12.6.3 as Maxwell to Hopper (**no Blackwell**) and CUDA 13.0/13.2 as including Blackwell 12.0. On this machine (WSL2, driver 617.14), `torch 2.14.1+cu130` reports `RTX 5070 Ti (12, 0)`, the arch list includes `sm_120`, and CUDA matmul and conv run. |
| 8 | **Runners**: chaiNNer v0.25.0 (2025-10-19) updated Torch for the RTX 50 series. *Resize Pixel Art* has hqx/Eagle/AdvMAME/SaI but no xBRZ or MMPX (issue #2422 open). *Upscale Image* has Wrap/Reflect/Replicate padding. The CLI is experimental. xBRZ goes up to 6x. spandrel v0.4.2 dates from 2026-02-21. Upscayl's last release is v2.15.0 (2024-12-25). | **Confirmed** | The chaiNNer tag `v0.25.0` is dated 2025-10-19 and its release notes say "Update Torch & Torchvision to support RTX 50 series". The `ResizeAlgorithm` enum stops at HQ4x and the source has no xbrz or mmpx. Issue #2422 ("Add xBRZ and ScaleNX…", opened 2023-12-24) is open. `PaddingType` offers Wrap/Reflect/Replicate. `docs/05--CLI.md`: "CLI mode is **experimental**", `run … --override`. The xBRZ header has `SCALE_FACTOR_MAX = 6` and is GPL-3.0. On PyPI, spandrel 0.4.2 was uploaded 2026-02-21, and it includes FDAT and AuraSR. A WebFetch summary of the GitHub releases page wrongly said 2024, which repeats pitfall 11. On Upscayl's releases page, the latest release is v2.15.0 (2024-12-25). A `v2.15.1` git tag exists from the same day but has no release. |
| 9 | **VRAM**: "16 GB is never the constraint here" and "On 16 GB, ESRGAN handles 512-1024 px tiles easily". Also "about 2105 chunks × < 1 s each on GPU". | **Partly refuted** | Measured on the 5070 Ti (fp32, inference_mode, peak allocated). For a 160 px chunk window: NXbrz 469 MB / 0.06 s, **Arzenal 8x 1.7 GB / 0.07 s**, UltraSharpV2 765 MB / 0.29 s, PBRify V4 620 MB / 0.21 s. So the chunk-level claims hold. For a 512 px input: NXbrz 4.2 GB / 0.6 s, UltraSharpV2 4.35 GB / 2.9 s, but **8x Arzenal peaks at 16.6 GB**, which is above the 16,303 MiB of dedicated VRAM. It spilled to shared memory and took 6.5 s. **For 8x models, keep input tiles at 256-384 px or use fp16.** 1024 px tiles do not fit for any of these models in fp32. |
| 10 | **Bake-off numbers and the circular-padding trick**: NXbrz reaches 80.5 % index match after local quantization. NXbrz per-pixel std is 1.3-1.5 on sand against about 5-9 for the others. Circular conv padding equals infinite wrap for pure-conv models but not for DAT. | **Confirmed** (the numbers match the saved metrics; I rechecked circular padding independently) | `out/E1_metrics.json` gives NXbrz `rt_match_qlocal 80.5`, xbrz 82.1 and UltraSharpV2 `rt_dE_rgb 1.70 / 80.1`. `out/E4_metrics.json` gives NXbrz 10:7/10:8 = 1.33/1.47, Arzenal 5.38/5.69, PixelPerfect 4.75/5.85, UltraSharpV2 6.58/8.74 and RealESRGAN 5.65/7.03. I did not re-run the bake-off. Independent GPU re-test with a random 8x8 tile, comparing circular padding with an 11x11 wrap centre crop: MAE (0-255) is **0.010 for NXbrz, 0.023 for Arzenal and 15.3 for UltraSharpV2**, which confirms that the trick fails for DAT. |

**Minor corrections found along the way**
- Real-ESRGAN: the latest commit on `master` is **2024-04-03** (a4abfb2), not 2024-08. The verdict "dormant" stands. I could not check `pushed_at` because of the API rate limit.
- MS-Unpainter's OpenModelDB author is *foolhardy*, and Arzenal's is *computerk*. The report gives no author for either, so nothing is wrong.
- The PocketMonsters-Alpha "pCloud only, RGBA, nearest-downscaled LR" description is confirmed by its JSON (`inputChannels 4`, LR "downscaled with nearest neighbor", only a pCloud URL).

**What changes for the decision**
1. A **native-6x custom model** (SPAN or RealPLKSR in traiNNer-redux with `scale: 6`) is feasible at the architecture level. Try it before settling on 8x→box. Also consider **4x-PBRify-UpscalerSPANV4 (CC0, pure-conv)** as the fine-tune base, because it keeps circular padding available.
2. For 8x models on the 5070 Ti, keep input tiles at 384 px or smaller, or use fp16. Chunk-sized windows (about 160 px) are trivial: under 2 GB and under 0.1 s.
3. Everything else in the recommendation table survived refutation attempts: availability, licences, dates, hashes and Blackwell setup.
