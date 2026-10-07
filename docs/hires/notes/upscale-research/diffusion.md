# Local diffusion upscaling of Ultima VII terrain to 6x (RTX 5070 Ti 16 GB): research and recommended ComfyUI pipeline

Date: 2026-10-04. Angle: running local diffusion and SR models on the user's Windows PC (RTX 5070 Ti 16 GB, Blackwell sm_120). Work folder: `E:\Dati\Ultima7_Upscale`.
Companion docs: `docs-hires/analysis/palette.md` (index rules) and `docs-hires/analysis/world.md` (chunk/terrain rendering).

Evidence tags used below:

* **[V]**: I fetched and read the page (date of fetch: 2026-10-04).
* **[S]**: search-result snippet only, not opened. Treat it as a lead.
* **[M]**: measured locally on this machine from the real BG game data (`/mnt/e/Games/RolePlayingGames/ultima7/STATIC`) or from `nvidia-smi`.
* **[A]**: my own analysis or estimate.

---

## 0. TL;DR

1. **Per-tile upscaling with circular (wrap-around) padding is wrong for U7.** [M] Most terrain shapes are **8x4-frame macro-textures**: frame *f*'s right neighbour is *f+1* and its lower neighbour is *f+8*, so 32 frames form one 64x32 px periodic texture. Only 33 of the 3,121 tiles used on the BG map ever appear surrounded by copies of themselves. The correct unit is the **per-shape macro-texture, wrapped toroidally**. Tiles at transitions need real-map context.
2. **The diffusion model must not see 8x8 tiles at native size.** SDXL, FLUX, Z-Image and Qwen all use f=8 VAEs, so one 8x8 tile is a single latent cell. Work at **12x** (1 source pixel = 12 working px), with 1-1.5 MP canvases. Then **box-downscale 2:1 to 6x**. This is the "render high, downscale at the end" idea applied to asset generation.
3. **Primary recommendation [A]: SDXL base 1.0 + xinsir ControlNet-Tile-SDXL (both commercially usable) in ComfyUI.**
   * Input: an **xBRZ-6x (then 2x lanczos) smoothed** image, not nearest-neighbour.
   * Sampling: denoise **0.30-0.50**, tile CN strength **0.6-0.9**, fixed seeds and a prompt per material class.
   * Seamless output: run on a toroidally padded macro-texture. SDXL is a conv U-Net, so the circular-padding nodes also work as a cross-check.
   * Post-process (Python, outside ComfyUI): box down to 6x, then **back-projection** so every 6x6 block averages exactly to the source pixel (DDNM range/null-space idea). Then restore the palette-cycling mask and quantize in OKLab to a **per-shape ramp subset of indices 0x01-0xDF**, with **no error-diffusion dithering**.
4. **Alternatives worth A/B testing:**
   * **SeedVR2 3B/7B** (Apache-2.0, one-step, "minimal hallucination" restorer, fits 16 GB with FP8 + BlockSwap).
   * **Z-Image-Turbo + Fun-ControlNet Tile 2.1-2601** (Apache-2.0, 8 steps, released Jan 2026).

   Both are DiT/transformer models, so wrap-around needs pad-and-crop (circular conv patching does nothing on them).
5. **Not recommended for production:**
   * FLUX.1-dev / Kontext-dev / FLUX.2-dev (non-commercial licence and heavy; DiT, so no circular padding).
   * Qwen-Image-Edit-2509/2511 and its upscale LoRAs (20B, colour/geometry drift, LoRAs trained on photos).
   * SUPIR / HYPIR (non-commercial, photo priors). StableSR, DiffBIR, CCSR and LucidFlux (photo restoration).
   * Pixel-art LoRAs, which go the wrong way (they *make* low-res pixel art).

   Instruction-edit models are still useful for **style exemplars** (a few hand-picked macro-textures) that then feed IP-Adapter or a style LoRA.
6. **Blackwell stack:**
   * Windows: the current official **ComfyUI Windows portable "nvidia" build (Python 3.13, PyTorch cu130)** [V]. sm_120 has been in stable PyTorch since **2.7 (cu128)** [V].
   * Driver on this PC: 615.78, CUDA UMD 13.4 [M]. The GPU is also visible inside WSL2 [M], so a Linux-native ComfyUI in WSL is a valid alternative.
   * Leave xformers and SageAttention off at first. PyTorch SDPA is enough for SDXL.
7. **The workload is small.** [M]/[A] There are 3,885 raw 8x8 flat frames, which is about 36 MP at 12x. One full SDXL pass over all macro-textures takes minutes, not hours. The bottleneck is **human review and seam QA**, not GPU time. Generate several seeds per item and curate.

---

## 1. What the terrain data actually looks like (measured)

All numbers are **[M]**, computed from BG `u7chunks`, `u7map` and `shapes.vga` with numpy (script in Appendix A).

| Fact | Value | Implication |
|---|---|---|
| Chunk templates in `u7chunks` | 3,072 (1,572,864 B / 512) | |
| Templates actually referenced by `u7map` | **2,105** | Size of a possible per-chunk-template override |
| World tile instances that are raw 8x8 flats (shape < 150) | **98.2 %** of 9.44 M | RLE flats and objects are a later phase |
| Raw 8x8 frames in shapes 0-149 | **3,885** | Full asset count at 6x: 3,885 x 48x48 ≈ 8.9 MP (≈ 8.6 MB as 8-bit) |
| Distinct (shape, frame) flats used on the BG map | **3,121** in **123** shapes | |
| Shapes whose frames form an **8-wide x 4-tall periodic macro-texture** | **77** shapes (2,313 used frames) | Upscale the 64x32 px macro-texture with toroidal wrap |
| Flat-flat edges: identical tile on both sides | 20.1 % | Seamless if the tile is self-consistent |
| Flat-flat edges: consistent with the 8x4 macro layout | **46.5 %** | Seamless by construction with macro-texture wrap |
| Flat-flat edges: same shape, other arrangement | 16.1 % | Needs context or seam lock |
| Flat-flat edges: cross-shape (transitions) | 17.3 % (10.2 % between two macro shapes, 4.7 % macro/transition, 2.4 % transition/transition) | Needs context or seam lock |
| Unique cross-shape tile pairs / shape pairs | 60,737 / 3,050 | Too many to hand-author. Must be automated |
| Unique 3x3 neighbourhoods around flats | 363,341 (median 16 per tile id, p90 244) | A tile has no single "true" context |
| Tiles that ever appear fully surrounded by themselves | **33 of 3,121** | Per-tile circular padding is the wrong prior |
| Palette indices used by all flats | 147 distinct. Per shape: median 17 (2-52). Per frame: median 6 | Quantize to a *per-shape ramp subset*, not the full 256 |
| Cycling indices in flats | only `0xE0-0xE7` and `0xFE`. 369 frames in 48 shapes. **0.47 % of pixels** | Water sparkles and "void" stars. Restore them through a mask, not through diffusion |

Visual check [M]: I rendered shape 19 (the most common, ~2.99 M instances) as an 8x4 frame grid. It is the deep-water macro-texture: blue, with white `E0-E7` sparkles, and it tiles visibly as a 64x32 texture. Shapes 147/148 are grass macro-textures and 149 is dirt. Shape 10 is sand, with shoreline transition frames mixed in. Shape 48 is black void with cycling stars.

---

## 2. Why the naive pipelines fail (design constraints)

1. **Latent granularity [A].** The SD1.5/SDXL/FLUX.1/Z-Image VAEs downsample by 8. A 128x128 chunk at 1x is a 16x16 latent; one 8x8 tile is a single latent cell. The model has to work on a pre-enlarged image. At 12x a source pixel covers a 1.5x1.5 latent cell, which leaves the model room to draw sub-pixel texture.
2. **Nearest-neighbour input + low denoise = blurry staircases [A].** With a blocky nearest-neighbour image as init and hint, low denoise (≤0.3) keeps the 12x12 blocks. High denoise (≥0.6) reinvents content. If the image is pre-smoothed with a pixel-art scaler (xBRZ supports native **2x-6x** [S]) or a pixel-art ESRGAN model, a **lower** denoise is enough. That gives less hallucination for the same perceived sharpness.
3. **Seams [M]/[A].** Wrapping each tile onto itself is right for only ~20 % of edges and wrong for the 8x4 macro-textures (46.5 %). The macro-texture is the periodic unit.
4. **Circular padding only works on conv U-Nets** (SD1.5/SDXL) [S]. DiT/flow models (FLUX.1/2, Z-Image, Qwen-Image, SeedVR2) patchify with linear or non-overlapping layers and use global or windowed attention, so "make all Conv2d circular" has no effect on them. For DiTs, use pad-and-crop (toroidal pad, process, crop the centre period) or "latent rolling" [S].
5. **Error-diffusion dithering breaks seams [A].** Floyd-Steinberg is order-dependent and non-local, so the same border pixels quantize differently in different tiles. Use nearest-colour, world-aligned ordered dithering (Bayer 8/16 divides 48), or diffusion confined to each 6x6 block (see §8).
6. **Global palette effects [V, repo doc].** Day/dusk/night, lighting and cycling are palette operations on indices (`docs-hires/analysis/palette.md`). Output must be **8-bit indices against palette 0 (DAY)**, and colour choices should stay inside the ramps the original tile uses, so night palettes darken the art the same way.
7. **Instruction-edit models re-render everything [A].** Qwen-Image-Edit, Kontext and FLUX.2 encode, regenerate and decode the whole canvas at their own resolution. Colour drift, geometry drift and invented details are expected. Qwen itself lists "image drift" as something 2511 *mitigates* [V]. That is acceptable for making a style reference, not for 3,885 frames that must match each other.

---

## 3. Software stack on RTX 5070 Ti (Blackwell, sm_120)

### 3.1 Facts

* **PyTorch 2.7** (blog updated 2025-05-15) added Blackwell support and the first CUDA 12.8 wheels: `pip install torch==2.7.0 --index-url https://download.pytorch.org/whl/cu128` [V]. Older wheels fail with "sm_120 is not compatible" or "no kernel image" [S, ComfyUI #7127 and #8276, Forge #3087].
* **ComfyUI README (current)** [V]:
  * The NVIDIA portable is `ComfyUI_windows_portable_nvidia.7z` with **Python 3.13 + PyTorch CUDA 13.0**, for "20 series and above".
  * `..._nvidia_cu126.7z` is only for 10-series cards; do **not** use it.
  * Manual install: `pip install torch torchvision torchaudio --extra-index-url https://download.pytorch.org/whl/cu130`.
* The original "50-series support thread" (Comfy discussion #6643, active Jan-Jul 2025) recommended cu128 builds. It warned that **updating the old portable could downgrade torch**, and that early SageAttention/Triton builds failed on sm_120 [V]. That advice is from 2025 and has been replaced by the cu130 portable.
* **This PC** [M]: `nvidia-smi` in WSL2 shows *RTX 5070 Ti, 16,303 MiB, driver 615.78, CUDA UMD 13.4*. That covers cu128, cu129 and cu130 wheels. WSL currently sees about 31 GB of RAM.
* **SageAttention for Windows** (woct0rdho fork) [V]:
  * Wheels for torch ≥ 2.7 (2.8/2.9/2.10+, the stable-ABI wheels cover 2.10+), CUDA 12 and 13. **sm_120 kernels only in cu128+ wheels.**
  * Needs `triton-windows` first. Enable with `--use-sage-attention`.
  * SageAttention 3 (FP4) is Blackwell-only. SeedVR2 can use it, with a fallback chain [V].
* **NVFP4** (Blackwell FP4 tensor cores) [V, third-party blog, 2026-06-08]:
  * Needs a cu130 torch (e.g. 2.9.1+cu130), driver ≥ 580 and a 2026 ComfyUI.
  * NVFP4 checkpoints exist for FLUX.1-dev, FLUX.2-dev, Kontext-dev, Z-Image and Qwen-Image. **None for SDXL.**
  * Claimed: FLUX.1-dev at 14 GB instead of 26 GB, about 2x the speed of FP8 on a 5090.
  * Some softening of fine detail, which matters for texture work. Treat NVFP4 as optional.

### 3.2 Recommended install (Windows-native, matches the user's request)

```bat
:: 1) Get the current portable build (Python 3.13, torch cu130)
::    https://github.com/Comfy-Org/ComfyUI/releases  ->  ComfyUI_windows_portable_nvidia.7z
::    extract to E:\Dati\Ultima7_Upscale\ComfyUI_windows_portable
:: 2) Folders used by the pipeline
mkdir E:\Dati\Ultima7_Upscale\work\in E:\Dati\Ultima7_Upscale\work\out E:\Dati\Ultima7_Upscale\work\workflows E:\Dati\Ultima7_Upscale\work\qa
:: 3) Start ComfyUI listening for the WSL driver script; use the work folders for I/O
cd /d E:\Dati\Ultima7_Upscale\ComfyUI_windows_portable
.\python_embeded\python.exe -s ComfyUI\main.py --windows-standalone-build --listen 0.0.0.0 --port 8188 ^
    --input-directory E:\Dati\Ultima7_Upscale\work\in --output-directory E:\Dati\Ultima7_Upscale\work\out
:: 4) Verify the GPU arch is supported
.\python_embeded\python.exe -c "import torch;print(torch.__version__, torch.cuda.get_device_name(0), torch.cuda.get_arch_list())"
::    expect a +cu130 (or cu128) build and 'sm_120' in the arch list
```

Custom nodes go in `ComfyUI\custom_nodes`, installed with `git clone` and then `..\python_embeded\python.exe -m pip install -r requirements.txt`, or through ComfyUI-Manager:

| Node pack | Purpose | Licence / status |
|---|---|---|
| `spinagon/ComfyUI-seamless-tiling` | `Seamless Tile` (circular conv for SDXL), `Make Circular VAE`, `Circular VAE Decode`, `Offset Image` (seam check) [V] | Built for SD-style conv models |
| `numz/ComfyUI-SeedVR2_VideoUpscaler` | SeedVR2 3B/7B, FP16/FP8/GGUF, BlockSwap, tiled VAE, single images, RGBA; v2.5.24 dated 2025-12-24 [V] | Code Apache-2.0/MIT; weights Apache-2.0 [V] |
| `cubiq/ComfyUI_IPAdapter_plus` (optional) | Style reference for consistency | **Maintenance-only since 2025-04-14** [S] |
| `Fannovel16/comfyui_controlnet_aux` (optional) | Tile preprocessor, lineart/canny hints for floors | — |
| `ssitu/ComfyUI_UltimateSDUpscale` (optional) | Only for the chunk/world-scale variant (§6.4) | GPL-3.0 [V] |
| `shiimizu/ComfyUI-TiledDiffusion` (optional) | MultiDiffusion / Mixture of Diffusers for very large canvases | **MultiDiffusion and Tiled VAE parts are CC BY-NC-SA 4.0**, the rest GPL-3 [S] |
| `OliverCrosby/ComfyUI-Universal-Seamless-Tiles` (optional) | Latent rolling + circular VAE decode for DiTs (FLUX, Wan) [V] | MIT. **Initial commit 2026-07-14, unproven** [V] |

**Networking WSL → Windows:** with the default NAT mode, start ComfyUI with `--listen 0.0.0.0` and call the Windows host IP (`ip route | awk '/default/{print $3}'`). You may need a firewall rule. Alternatively set `networkingMode=mirrored` in `%UserProfile%\.wslconfig` so `localhost:8188` works. Automation uses ComfyUI's HTTP API: `POST /prompt`, `GET /history`, `GET /view`, `POST /upload/image`, `ws /ws` [V].

**Alternative: ComfyUI inside WSL2.** The GPU is visible [M], so you can `git clone https://github.com/Comfy-Org/ComfyUI`, create a venv, run `pip install torch torchvision --index-url https://download.pytorch.org/whl/cu130`, then `pip install -r requirements.txt`.

* Pros: no triton-windows; Linux wheels for SageAttention and flash-attn.
* Cons: keep the models on the WSL ext4 disk, because loading 7-20 GB checkpoints over `/mnt/e` (9P) is slow; WSL RAM is capped (31 GB now), which matters for BlockSwap/offload.

### 3.3 Blackwell pitfalls

* Any custom node that ships its **own CUDA extension** compiled before 2025 lacks sm_120 and fails with "no kernel image". Examples: old xformers, some SUPIR, flash-attn or nunchaku builds. Check each node's wheel tags.
* Do not mix cu126 and cu13x wheels in `python_embeded`. Pin the torch version after a working setup. Portable updates have replaced torch in the past [V].
* Do not enable SageAttention, torch.compile or NVFP4 until the plain SDPA pipeline produces correct images. Speed is not the bottleneck here.

---

## 4. Candidate models and tools

| Candidate | Kind | Licence | VRAM on 16 GB | Fit for U7 6x tiles [A] |
|---|---|---|---|---|
| **SDXL base 1.0** + **xinsir/controlnet-tile-sdxl-1.0** | img2img + tile ControlNet (conv U-Net) | SDXL: CreativeML Open RAIL++-M [V]. CN: Apache-2.0 [V] | ~8-10 GB fp16 incl. CN | **Best primary.** Mature, controllable. Native circular padding. Cheap enough for many seeds. xinsir recommends ~1024² buckets, conditioning scale 1.0, ~30 steps. Its SR mode is flagged "may be unstable" [V], so we use it as a *refiner*, not as the SR step |
| **SeedVR2** 3B / 7B (ByteDance Seed) | One-step diffusion restoration (DiT, adaptive window attention) | Weights Apache-2.0 [V] | 3B FP8 easy. 7B FP8-mixed / FP16 with BlockSwap [V] | **Strong A/B candidate** for the SR step. Designed to restore without inventing much. Colour-correction modes (LAB, wavelet, HSV, AdaIN). Tiled VAE. Batch must be 4n+1 (use 1). The model card warns about **oversharpening on lightly degraded inputs** [V]. Trained on photo/video, so pixel-art behaviour is untested. No circular padding: pad and crop |
| **Z-Image-Turbo** (6B, released 2025-11-27) + **Z-Image-Turbo-Fun-ControlNet Tile 2.1-2601-8steps** (6.7 GB; lite ~1.9-2.0 GB; released 2026-01-12) | Distilled DiT (8 NFE, guidance 0) + tile CN trained up to 2048² for SR | Apache-2.0 [V] | "Fits 16 GB" [V] | **Modern A/B candidate.** Official ComfyUI template `utility_z_image_turbo_2k_upscaler` [S]. Control scale 0.65-1.0 [V]. Photoreal bias. DiT, so pad-and-crop |
| FLUX.1-dev + jasperai/Flux.1-dev-Controlnet-Upscaler | DiT + upscaler CN (trained with Real-ESRGAN-style degradations; CN scale 0.6, 28 steps, guidance 3.5) [V] | **FLUX.1-dev non-commercial** [V] | fp8/NVFP4 ~12-14 GB | Works, but heavier and non-commercial with no advantage for stylised 2D art. Not first choice |
| FLUX.1-Kontext-dev (+ "pixel style" LoRAs) | Instruction editing | Non-commercial (BFL self-serve commercial licence exists) [S] | fp8 ~12 GB+ | Pixel LoRAs convert *to* pixel art, the wrong direction. Exemplar generation at most |
| FLUX.2-dev (32B, 2025-11-25) | Generation + multi-reference editing | Non-commercial; VAE Apache-2.0 [S] | Needs 4-bit or offload plus lots of RAM [S] | Too heavy for batch production on 16 GB |
| FLUX.2-klein-4B (2026-01-15) | Small generation + editing model | **Apache-2.0** [V] (9B variant non-commercial [S]) | ~13 GB [V] | Possible exemplar or style tool. DiT, so no circular padding |
| Qwen-Image-Edit-2509 / **2511** (20B) | Instruction editing, multi-image | Apache-2.0 [V] | fp8 ~20 GB file, so offload; or nunchaku SVDQuant [S] | Exemplars only. Upscale LoRAs exist (vafipas663 2509-Upscale is **GPL-3.0**, trained to undo "pixelation up to 16x", **"not trained on 2D/illustrations/CGI"** [V]; prithivMLmods 2511-Unblur-Upscale [S]). Photo bias plus drift |
| SUPIR (SDXL) / **HYPIR** (SD2.1 LoRA, SIGGRAPH 2025) | Photo restoration | **Non-commercial only** [V/S] | SUPIR 10 GB at ≤1024² [S] | Photo priors (skin, foliage, film grain). Not suited |
| StableSR / DiffBIR / CCSR / LucidFlux (ICLR'26, FLUX-based) | Real-world SR/restoration (2023-2026) | Mixed (CCSR Apache-2.0 [S]) | 8-28 GB | Trained on real-world photo degradations. Wrong domain |
| nerijs/pixel-art-xl and other pixel LoRAs | SDXL LoRA that generates pixel art, meant to be downscaled 8x nearest [S] | OpenRAIL-M [S] | small | Wrong direction (low-res output). Skip |
| Retro Diffusion "RD Tile" | Commercial pixel-art tileset model; wraps in X/Y; palette control [S] | Commercial API; Aseprite extension runs smaller local models [S] | — | Interesting for *new* 1x tiles, not for faithful 6x upscaling |
| **xBRZ 6x** | Deterministic pixel-art scaler (native 2-6x) [S]. Exult already ships an xBR scaler, `imagewin/scale_xbr.cc` (Hyllian xBR "based on HqMAME version by Zenju", 2x-oriented) [M]. A standalone xBRZ library or port is still needed for a native 6x | Open source (GPL) | CPU | **Stage-1 pre-smoother** before diffusion. Also the deterministic seam-lock baseline |
| OpenModelDB pixel-art ESRGANs (4x-PixelPerfectV4, 4x xbrz, 4x NXbrz, 8x Arzenal) [S] | Feed-forward SR (ComfyUI `UpscaleModelLoader`) | Per model (check each) | <2 GB | Alternative Stage-1. Deterministic, so no seed variance |
| **traiNNer-redux** (custom SR training) [S] | Train a small SR model (SPAN/ESRGAN-class) on synthetic pairs | Apache-2.0-style (BasicSR fork) | 16 GB is enough | **Later option:** train a U7-specific 6x model on pairs made by "box-down 6x + quantize to U7 palette" from painted textures. Deterministic, consistent, no hallucination knobs |
| OneTrainer / kohya_ss | Style-LoRA training (SDXL) [S] | Open source | 16 GB is enough for SDXL LoRA [S] | Lock the style after ~30-60 curated outputs |

Speed data points (low confidence):

* UL Procyon SDXL FP16 on a 5070 Ti: 11.2 s/image [S]. That benchmark runs many steps; it is not our use case.
* My estimate [A] for SDXL img2img at ~1.2 MP, about 13 effective steps (28 steps × 0.45 denoise) plus tile CN: **~3-6 s per canvas**.
* One FLUX.2-on-5070 Ti blog [V] gives figures I consider unreliable. It names a non-existent "Flux 2 Schnell" and lists VRAM that does not fit a 32B model. Ignored.

---

## 5. Fidelity vs hallucination with 8x8 sources

* **6x invents 35 of every 36 pixels [A].** "Faithful" has to be defined operationally:
  1. **Round-trip consistency.** `quantize(box_down_6x(out)) == original` for ≥ 97 % of pixels per tile. Back-projection plus constrained quantization gets close to 100 % by construction.
  2. **Structure.** Edges and lines in the xBRZ baseline are kept. Compare edge maps, or SSIM on blurred luminance.
  3. **No new semantics.** Human review on contact sheets and in-world previews (§9.4).
* **Back-projection (range/null-space projection, as in DDNM, arXiv 2212.00490 [S]).**
  * Let `y` be the original tile (linear RGB), `x` the generated 6x tile, `D` the 6x6 box average and `U` nearest-neighbour upsampling.
  * Then `x' = x + U(y − D(x))` satisfies `D(x') = y` exactly. The low-frequency content is the original art; only the 6x6 sub-pixel texture comes from the model.
  * If the correction looks blocky, iterate with a bicubic `U` (iterative back-projection) for a smoother correction.
  * Benefit 1: low-frequency seams between tiles become exactly the original 1x seams.
  * Benefit 2: the result stays correct when the engine downsamples a 6x frame to a lower target resolution.
* **Denoise regimes** (SDXL + tile CN, xBRZ-smoothed 12x input) [A]:
  * **0.20-0.30**: sharpening and cleanup only. Little new texture.
  * **0.35-0.50**: new micro-texture (grass blades, ripples, grain) with the layout intact. **Production range.**
  * **≥0.55**: reinterpretation. Shoreline shapes, plank directions and road edges start to move. Use only for organic materials, and always with back-projection.
* **Per-class settings** (hand-label the 123 used shapes, about an hour) [A]:

| Class | Denoise | CN strength | Notes |
|---|---|---|---|
| Water, void | 0.30-0.40, or diffusion skipped for void | 0.8-0.9 | Cycling pixels restored by mask (§8) |
| Grass, dirt, sand, swamp, rock | 0.40-0.55 | 0.6-0.8 | Hallucination is mostly harmless here |
| Floors, planks, flagstones, carpets, roads, any tile with straight lines or symbols | 0.25-0.35 | 0.85-1.0 | Optionally add a lineart/canny CN from the xBRZ baseline |
| Transition tiles (shorelines, path edges) | 0.30-0.45 | — | Inpainted in context (§6.2) |

* **Failure modes to watch:**
  * Photographic textures (pebbles and leaves that look like photos): the base model's bias. Counter with negative prompts and an IP-Adapter or LoRA style lock.
  * Colour drift: fixed by back-projection plus per-shape palette restriction.
  * Glints smeared into the water colour: fixed by the cycling mask.
  * Different seeds giving different "grain" between adjacent shapes: fixed by shared seeds per class and by batching all frames of a shape into one canvas.

---

## 6. Seamlessness strategy (the core design)

### 6.1 Tier 1: macro-textures (covers ~66.6 % of edges [M])

1. For each of the 77 macro shapes, assemble frames 0-31 into an 8x4 grid: **64x32 px at 1x**. Check the layout against the map statistics; non-32-frame shapes need their own layout.
2. **Toroidal pad:** take the 3x3 periodic tiling and crop to 2 periods × 2 periods centred on one period (128x64 at 1x). Stage-1 xBRZ runs on this padded image, so its 5x5 kernels see the true periodic neighbours.
3. Upscale to 12x (xBRZ 6x, then lanczos 2x) → 1536x768. That is about 1.2 MP, a good SDXL canvas.
4. Diffusion refine (§9.2). Then crop the **centre period** (768x384 at 12x) and box-down to 6x (384x192).
5. **Seam healing:** the left and right borders of the centre crop came from different noise. Check with `Offset Image` (roll by half a period). If a seam shows:
   * Roll by half a period.
   * Inpaint a ~1-2 source-pixel band across the now-central seam cross at low denoise.
   * Roll back.

   The classic make-seamless trick, model-agnostic.
6. **SDXL-only shortcut:** run the single period (768x384, or 1024x512 at 16x) with `Seamless Tile` + `Circular VAE Decode`.
   * Pitfall [A]: the ControlNet module is *not* patched to circular padding by that node, so check edges with `Offset Image`. Pad-and-crop has no such issue.
7. Cut the 6x period back into 32 frames of 48x48.

### 6.2 Tier 2: transition and variant tiles, outside-in inpainting (~33 % of edges)

1. **Order:** finish all macro shapes first (Tier 1), keeping their 12x intermediates.
2. For each non-macro frame, pick its **most frequent 3x3 map context** (from the adjacency statistics; median 16 contexts per tile). Build a 3x3 mosaic at 12x:
   * the 8 neighbours from **already-finished hi-res tiles**;
   * the centre from the xBRZ baseline.
3. **Masked inpaint** of the centre tile plus a ~0.5-1 source-pixel feather into the neighbours. Use core `SetLatentNoiseMask`/`InpaintModelConditioning` plus `DifferentialDiffusion` for the soft mask, and tile CN on the whole mosaic. The new tile is drawn to blend into real hi-res neighbours.
4. Crop, box-down, back-project, quantize.
5. Process in dependency order: transition tiles whose neighbours are all finished go first. For chains such as shoreline next to shoreline, iterate.

### 6.3 Tier 3: seam lock for everything else [A]

For edges whose real contexts differ from the one used in Tier 2, the remaining mismatch is high-frequency only, because back-projection already matches the low frequencies.

* Optional **border band blend:** in the outer 2-4 px of each 48x48 tile, blend towards the xBRZ-6x baseline computed with the actual neighbour (feather 3→9 px).
* Measure first. Seam energy (mean |gradient| across tile borders ÷ mean |gradient| inside tiles, on world previews) tells you whether the band is needed. It costs some detail at the rim.

### 6.4 Engine-level alternatives (for the engine design discussion) [A]

* **Per-chunk-template override:**
  * 2,105 used templates at 6x = 768² each ≈ **1.24 GB raw 8-bit**, a few hundred MB as PNG [A].
  * Every transition inside a chunk is generated in true context. Only chunk borders (12.5 % of tile edges) can seam.
  * Matches Exult's per-terrain `Chunk_terrain::rendered_flats` cache (`world.md` §6).
  * GPU: about 2,105 SDXL canvases at 1536², roughly 3-5 h [A].
* **World megatexture:**
  * 24,576² px at 1x → 6x ≈ 21.7 GP. About 87 GP at 12x, so days of GPU time and tens of GB of storage.
  * Perfectly seamless and non-repetitive, but needs a streaming renderer and loses the tile-edit semantics. Probably not worth it.
* **Per-tile override (default):** ~3,885 × 2.3 KB ≈ 9 MB. Seams are handled by Tiers 1-3.

---

## 7. Style consistency across the tileset [A]

* One checkpoint, one VAE, and one sampler/scheduler/steps for the whole run. A **fixed seed per material class**. Prompt templates per class, for example:
  * positive: `top-down orthographic {material} ground texture, hand-painted 1990s PC RPG game art, rich detail, even lighting, seamless`
  * negative: `perspective, objects, people, text, watermark, photo, blurry, noise, jpeg artifacts, border`
* Put all frames of a shape (and related shapes such as grass 147/148) **on one canvas**, so they share noise statistics.
* Pick 3-5 approved macro-textures as **style references** through IP-Adapter (SDXL, "style transfer" weight type, 0.3-0.5). Note the IPAdapter_plus node pack is maintenance-only [S].
* After curating ~30-60 outputs, train a small **SDXL style LoRA** (kohya/OneTrainer; 16 GB is enough [S]) and re-run everything with it at 0.4-0.7. This is the most reliable way to get a uniform look across 123 shapes.
* Keep every 12x intermediate and a JSON sidecar (model hashes, seed, denoise, CN strength, prompt) per output, so a re-run is reproducible.

---

## 8. Palette quantization and cycling indices [A]

1. **Target palette:** palette 0 (DAY) of `palettes.flx`, 6-bit VGA → 8-bit. Never emit `0xFF`. Avoid `0x00` unless the source frame uses it. Cycling indices `0xE0-0xFE` only through the mask (step 3). This follows `palette.md`.
2. **Allowed set per shape:** the union of indices used by the shape's frames (median 17 [M]) plus the members of those indices' **brightness ramps** (`Palette::get_ramps`, ~17 ramps). This gives enough shades for 48x48 detail. Night and dusk palettes then remap the art exactly as they remap the original, and stray hues are impossible (no blue in grass unless the original had it).
3. **Cycling mask:** `M6 = nearest_up6(src_idx in E0..FE)`. Inside `M6` write the source index unchanged. Optionally shape the 6x6 glint (e.g. keep a 4x4 core plus a cross) to look less blocky. Exclude `M6` from back-projection and from quantization of the neighbours. Only 0.47 % of pixels [M], but they carry the water animation.
4. **Colour distance:** OKLab (or CIEDE2000) nearest colour after back-projection, done in linear RGB.
5. **Dithering:**
   * Default **none**.
   * If banding shows, use **block-constrained error diffusion**: serpentine Floyd-Steinberg that resets at each 6x6 block, so each block's mean stays close to the source and the result is local and seam-safe.
   * Or use world-aligned Bayer 8x8/16x16 (48 is a multiple of both).
   * Never use global FS or Atkinson (non-local, breaks seams).
6. ComfyUI palette nodes (PixelArt-Detector "Palette Converter" with custom palette PNGs, MIT [V]; PixelGridHelpers "Enforce Palette" with HEX lists [V]) are fine for previews. Production quantization should be our own numpy script, because we need the ramps, the mask, block-constrained dithering and index output.

---

## 9. Concrete ComfyUI workflow

### 9.1 Stage 0: data prep (Python in WSL, tools-venv)

* Extract the 3,885 flats as indexed PNGs into `art_original/flats/shape_SSS/frame_FF.png`, plus a palette PNG and a cycling mask per frame.
* Build:
  * `macro/shape_SSS_pad.png`: 2x2-period toroidal pad at 1x.
  * `ctx/shape_SSS_FF_ctx.json`: top-k 3x3 contexts from `u7chunks`/`u7map`.
  * `class_map.json`: shape → material class and settings.
* Stage 1 (CPU): xBRZ 6x on the padded images, then lanczos 2x → 12x. Write to `/mnt/e/Dati/Ultima7_Upscale/work/in/macro12/...`. Keep a nearest-12x copy for A/B tests.

### 9.2 Stage 2: refine graph "U7_macro_SDXL_tile" (save in API format to `work\workflows\`)

```
LoadImage                 image = macro12/shape_019_pad.png            (1536x768)
CheckpointLoaderSimple    sd_xl_base_1.0.safetensors
VAELoader (optional)      sdxl_vae_fp16_fix.safetensors (madebyollin) - fp16-safe decode
[optional] IPAdapterUnifiedLoader + IPAdapterAdvanced  ref=style_refs/*.png, weight 0.35, weight_type "style transfer"
CLIPTextEncode (+)        "{class prompt}"
CLIPTextEncode (-)        "{negative}"
ControlNetLoader          xinsir controlnet-tile-sdxl-1.0 (diffusion_pytorch_model.safetensors)
ControlNetApplyAdvanced   image = LoadImage, strength = class.cn (0.6-0.9), start 0.0, end 0.9
VAEEncode                 pixels = LoadImage
KSampler                  seed = class.seed (+k for candidates), steps 28, cfg 5.0,
                          sampler dpmpp_2m, scheduler karras, denoise = class.denoise (0.30-0.50)
VAEDecode                 (VAEDecodeTiled only for > 2 MP canvases)
SaveImage                 filename_prefix = out12/shape_019_s{seed}_d{denoise}
```

Variant for the single-period SDXL shortcut: put `Seamless Tile` (x+y) between the checkpoint and the KSampler, and use `Make Circular VAE` / `Circular VAE Decode`.

Variant "U7_macro_SeedVR2":

* LoadImage (xBRZ-6x of the padded period, 768x384) → SeedVR2 loaders (3B FP8, or 7B FP8-mixed with BlockSwap ~16-24 blocks; tiled VAE 1024/128) → SeedVR2 upscaler with target short side = 2× input (→12x), batch 1, colour correction `lab` or `wavelet` → SaveImage.
* Node names change between versions; use the node pack's example workflow as the base.

Variant "U7_macro_ZImage":

* Start from the built-in template `utility_z_image_turbo_2k_upscaler`.
* Load `Z-Image-Turbo-Fun-Controlnet-Tile-2.1-2601-8steps.safetensors` (or the lite version).
* 8 steps, guidance/cfg 1 (the "0 guidance" Turbo setting), control scale 0.65-1.0. Sweep denoise.

Transition graph "U7_ctx_inpaint_SDXL":

* LoadImage (3x3 mosaic at 12x: finished neighbours plus xBRZ centre) and LoadImageMask (centre tile plus feather).
* `DifferentialDiffusion` → `InpaintModelConditioning` (or `VAEEncode` + `SetLatentNoiseMask`) → same tile CN on the whole mosaic → KSampler denoise 0.35-0.5 → VAEDecode → SaveImage.

### 9.3 Driver loop (Python in WSL; ComfyUI HTTP API)

```python
# pseudo-code
wf = json.load(open('U7_macro_SDXL_tile.api.json'))
for shape in macro_shapes:
    for k in range(N_CANDIDATES):              # e.g. 4 seeds
        set_inputs(wf, image=f'macro12/shape_{shape:03d}_pad.png', seed=cls_seed(shape)+k,
                   denoise=cls(shape).denoise, cn=cls(shape).cn, prompt=cls(shape).prompt)
        pid = post(f'http://{WINHOST}:8188/prompt', {'prompt': wf, 'client_id': CID})['prompt_id']
        wait_ws(pid)                           # or poll GET /history/{pid}
# outputs land in E:\Dati\Ultima7_Upscale\work\out\out12\  ->  /mnt/e/Dati/Ultima7_Upscale/work/out/out12/
```

### 9.4 Stage 3: post and QA (Python)

* Crop the centre period → box 2:1 → 6x → back-projection (§5) → optional seam band (§6.3) → cycling mask (§8) → per-shape ramp quantization → split into 48x48 indexed PNGs (`art_6x/flats/shape_SSS/frame_FF.png`) plus a JSON sidecar.
* **QA artefacts:**
  1. `Offset Image`-style half-period rolls of each macro-texture.
  2. Round-trip error map per frame.
  3. **World previews:** render ~50 representative real chunks (towns, coasts, roads, swamps) at 6x from the new tiles. Compare side by side with an xBRZ-6x render of the same area.
  4. Seam-energy metric per tile pair.
  5. Contact sheets of all candidates for human picking.

### 9.5 Throughput estimate [A]

| Job | Canvases | Estimate |
|---|---|---|
| Macro-textures (77 shapes, 1.2 MP each) | 77 per seed | ~5-8 min per seed |
| Transition and variant frames (~800-1,600 contexts) | ~0.2-0.4 MP each | ~1-2 h for 4 candidates each |
| Per-chunk-template variant (§6.4) | 2,105 at 1536² | ~3-5 h per seed |

Only the chunk-template variant would need overnight runs.

---

## 10. Risks and pitfalls

1. **Seams on transitions** (~33 % of edges are not covered by macro wrap) [M]. Tiers 2-3 reduce them; world previews will show whether the remaining high-frequency mismatches are visible at 1920x1200.
2. **Hallucination on structured tiles** (floors, roads, symbols). Use the per-class low denoise, a lineart CN and back-projection, and review by hand.
3. **Style drift** between shapes processed separately. Use shared seeds and canvases, IP-Adapter or a style LoRA, and fixed sampler settings.
4. **Palette limits:** per-shape subsets (median 17 indices) may posterize 6x detail. Use ramp extension or block-constrained dithering. True colour would break palette lighting and cycling (`palette.md`).
5. **Blackwell stack fragility:** custom CUDA extensions without sm_120, portable updates swapping torch, triton-windows and SageAttention mismatches. Pin versions and keep attention on SDPA by default.
6. **DiT models cannot use circular-conv tiling**, so pad-and-crop is required. The "Universal Seamless Tiles" node is new (initial commit July 2026) and unproven.
7. **Licences:**
   * FLUX.1/Kontext/FLUX.2-dev and FLUX.2-klein-9B: non-commercial.
   * SUPIR and HYPIR: non-commercial.
   * TiledDiffusion's MultiDiffusion part: CC BY-NC-SA.
   * Qwen upscale LoRA: GPL-3.
   * The source art is EA/Origin copyright. Recommendation: **distribute the pipeline, not the generated art pack.** Users generate it from their own game files. Prefer the Apache/OpenRAIL stack (SDXL + xinsir tile, SeedVR2, Z-Image) so the tooling stays redistributable.
8. **Data-handling pitfalls:**
   * Don't let ComfyUI or PIL convert indexed PNGs through sRGB/gamma inconsistently. Do back-projection in linear RGB and quantization in OKLab.
   * Keep 12x intermediates (PNG, RGB).
   * WSL to `/mnt/e` I/O is slow for multi-GB model files. Fine for small PNGs.
9. **Snippet-only claims:** speed figures, some licences and the DiT/circular-padding statement are marked [S]. Verify them during setup.

---

## 11. Suggested experiment plan (first session on the GPU)

1. Install the portable cu130 build, check `torch.cuda.get_arch_list()` contains sm_120, and run the default SDXL workflow.
2. Shapes 19 (water), 147 (grass), 149 (dirt), one floor shape, and shape 10 (sand + shore transitions). For each, A/B test:
   * Stage 1: nearest-12x vs xBRZ-6x+2x.
   * Stage 2: SDXL+tile vs SeedVR2-3B vs Z-Image+Tile.
   * Denoise: {0.3, 0.4, 0.5}.
   * Seeds: 2.
3. Apply the same post (back-projection, quantization). Build a 6x world preview of a coast and town chunk. Pick the stack.
4. Run the whole macro set, curate, then the Tier-2 transitions. Optionally train the style LoRA and re-run.

---

## 12. Sources

Verified by fetching [V] (2026-10-04):

* ComfyUI README (portable builds; cu130; manual pip): https://github.com/Comfy-Org/ComfyUI
* ComfyUI 50-series thread (2025-01 to 2025-07): https://github.com/Comfy-Org/ComfyUI/discussions/6643
* PyTorch 2.7 release (Blackwell, cu128; updated 2025-05-15): https://pytorch.org/blog/pytorch-2-7/
* ComfyUI server routes: https://docs.comfy.org/development/comfyui-server/comms_routes
* SageAttention Windows wheels: https://github.com/woct0rdho/SageAttention
* NVFP4 guide (2026-06-08, third-party): https://runaihome.com/blog/comfyui-nvfp4-rtx-speed-guide-2026/
* xinsir ControlNet Tile SDXL (Apache-2.0): https://huggingface.co/xinsir/controlnet-tile-sdxl-1.0
* SDXL base 1.0 (OpenRAIL++-M): https://huggingface.co/stabilityai/stable-diffusion-xl-base-1.0
* SeedVR2 ComfyUI node v2.5.24 (2025-12-24): https://github.com/numz/ComfyUI-SeedVR2_VideoUpscaler
* SeedVR2 ComfyUI weights (Apache-2.0): https://huggingface.co/numz/SeedVR2_comfyUI
* SeedVR2-3B model card (Apache-2.0, limitations): https://huggingface.co/ByteDance-Seed/SeedVR2-3B
* Z-Image-Turbo (2025-11-27, Apache-2.0): https://huggingface.co/Tongyi-MAI/Z-Image-Turbo
* Z-Image-Turbo Fun ControlNet Union/Tile 2.1 (2601/2602): https://huggingface.co/alibaba-pai/Z-Image-Turbo-Fun-Controlnet-Union-2.1
* Jasper FLUX.1-dev ControlNet Upscaler (non-commercial): https://huggingface.co/jasperai/Flux.1-dev-Controlnet-Upscaler
* FLUX.2-klein-4B (Apache-2.0): https://huggingface.co/black-forest-labs/FLUX.2-klein-4B
* Qwen-Image-Edit-2511 (Apache-2.0, 20B): https://huggingface.co/Qwen/Qwen-Image-Edit-2511
* Qwen-Edit-2509 Upscale LoRA (GPL-3.0, photo-only): https://huggingface.co/vafipas663/Qwen-Edit-2509-Upscale-LoRA
* HYPIR (non-commercial; SD2.1; 2025-07): https://github.com/XPixelGroup/HYPIR
* Seamless tiling nodes: https://github.com/spinagon/ComfyUI-seamless-tiling
* Universal Seamless Tiles (initial commit 2026-07-14): https://github.com/OliverCrosby/ComfyUI-Universal-Seamless-Tiles
* Ultimate SD Upscale (GPL-3.0): https://github.com/ssitu/ComfyUI_UltimateSDUpscale
* PixelArt-Detector (MIT, palette converter): https://github.com/dimtoneff/ComfyUI-PixelArt-Detector
* PixelGridHelpers (Enforce Palette): https://github.com/molbal/ComfyUI-PixelGridHelpers
* Z-Image Tile vs SeedVR2 planned benchmark (no data yet, 2026-10-01): https://github.com/chrbayer/AI/issues/30
* FLUX.2 on 5070 Ti blog (judged unreliable): https://apatero.com/blog/flux-2-rtx-5070-ti-16gb-performance-guide-2025

Snippet-only [S]:

* sm_120 errors: https://github.com/Comfy-Org/ComfyUI/issues/7127 · https://github.com/Comfy-Org/ComfyUI/discussions/8276 · https://github.com/lllyasviel/stable-diffusion-webui-forge/issues/3087
* Community Blackwell Windows bundle: https://github.com/hiroki-abe-58/ComfyUI-Win-Blackwell
* Circular padding vs DiT; Universal Seamless Tiles description: https://comfy.icu/extension/OliverCrosby__ComfyUI-Universal-Seamless-Tiles · https://www.runcomfy.com/comfyui-nodes/ComfyUI-seamless-tiling
* FLUX.2 (2025-11-25, 32B, non-commercial dev): https://en.wikipedia.org/wiki/Flux_(text-to-image_model) · https://kiadev.net/news/2025-11-26-flux2-32b-flow-transformer-4mp
* FLUX.2-klein-9B (non-commercial): https://huggingface.co/black-forest-labs/FLUX.2-klein-9B
* Kontext-dev licence: https://huggingface.co/black-forest-labs/FLUX.1-Kontext-dev · https://bfl.ai/blog/flux-1-kontext-dev
* Kontext pixel LoRAs: https://huggingface.co/Shakker-Labs/FLUX.1-Kontext-dev-LoRA-Pixel-Style · https://civitai.com/models/1731051/kontext-pixel-art
* Qwen 2511 fp8 VRAM: https://localaimaster.com/blog/qwen-image-edit-local-guide
* Qwen 2511 nunchaku: https://huggingface.co/QuantFunc/Nunchaku-Qwen-Image-EDIT-2511
* Qwen 2511 unblur LoRA: https://huggingface.co/prithivMLmods/Qwen-Image-Edit-2511-Unblur-Upscale
* Z-Image upscaler template: https://comfy.org/workflows/utility_z_image_turbo_2k_upscaler.app-b3bbbf217b89/
* SUPIR licence/VRAM: https://github.com/kijai/ComfyUI-SUPIR
* LucidFlux: https://github.com/W2GenAI-Lab/LucidFlux
* StableSR: https://github.com/IceClear/StableSR
* CCSR: https://replicate.com/csslc/ccsr/readme
* Pixel Art XL: https://huggingface.co/nerijs/pixel-art-xl
* Retro Diffusion RD Tile: https://replicate.com/retro-diffusion/rd-tile · https://astropulse.itch.io/retrodiffusionai
* OpenModelDB pixel-art models: https://openmodeldb.info/models/4x-PixelPerfectV4 · https://openmodeldb.info/models/4x-xbrz · https://openmodeldb.info/models/8x-Arzenal-v1-1
* xBRZ 2-6x: https://en.wikipedia.org/wiki/Pixel-art_scaling_algorithms · https://github.com/bell345/xbrz-rs
* DDNM: https://arxiv.org/abs/2212.00490
* traiNNer-redux: https://github.com/the-database/traiNNer-redux
* IPAdapter_plus maintenance status: https://github.com/cubiq/ComfyUI_IPAdapter_plus
* TiledDiffusion licence: https://github.com/shiimizu/ComfyUI-TiledDiffusion
* Procyon SDXL 5070 Ti: https://www.storagereview.com/review/asus-prime-geforce-rtx-5070-ti-review
* LoRA trainers 2026: https://sanj.dev/post/onetrainer-vs-kohya-ss-vs-ai-toolkit/
* Krita AI seamless request: https://github.com/Acly/krita-ai-diffusion/issues/956

---

## Appendix A: reproduce the terrain statistics [M]

```python
# python u7_terrain_stats.py <U7 STATIC dir>   (numpy; tested with tools-venv numpy 2.4.6)
import sys, struct, numpy as np
S = sys.argv[1] if len(sys.argv) > 1 else '/mnt/e/Games/RolePlayingGames/ultima7/STATIC/'
c = np.fromfile(S + '/u7chunks', np.uint8).reshape(-1, 256, 2).astype(np.int32)
tid = ((c[..., 0] + 256 * (c[..., 1] & 3)) * 32 + ((c[..., 1] >> 2) & 31)).reshape(-1, 16, 16)  # shape*32+frame
m = np.fromfile(S + '/u7map', '<u2').reshape(12, 12, 16, 16)                                  # superchunk -> chunk refs
world = m.transpose(0, 2, 1, 3).reshape(192, 192)
W = tid[world].transpose(0, 2, 1, 3).reshape(3072, 3072)                                       # world tile grid
print('templates', len(c), 'used', len(np.unique(world)), '| flat share', ((W // 32) < 150).mean())
def edges(a, b, horiz):
    ok = ((a // 32) < 150) & ((b // 32) < 150); a, b = a[ok], b[ok]
    same = a // 32 == b // 32; fa, df = a % 32, b % 32 - a % 32
    macro = same & (((df == 1) & (fa % 8 != 7)) | ((df == -7) & (fa % 8 == 7))) if horiz else \
            same & (((df == 8) & (fa < 24)) | ((df == -24) & (fa >= 24)))
    return a == b, macro & (a != b), same & ~macro & (a != b), ~same
H = edges(W[:, :-1].ravel(), W[:, 1:].ravel(), True); V = edges(W[:-1].ravel(), W[1:].ravel(), False)
n = H[0].size + V[0].size
for name, h, v in zip(['identical', 'macro8x4', 'same-shape-other', 'cross-shape'], H, V): print(name, (h.sum() + v.sum()) / n)
d = open(S + '/shapes.vga', 'rb').read(); hist = np.zeros(256, np.int64); frames = 0
for s in range(150):
    off, size = struct.unpack_from('<II', d, 0x80 + 8 * s)
    a = np.frombuffer(d, np.uint8, size, off); frames += size // 64; hist += np.bincount(a, minlength=256)
print('raw 8x8 frames', frames, '| distinct indices', (hist > 0).sum(), '| cycling-pixel share', hist[0xE0:0xFF].sum() / hist.sum())
```

Output on BG:

```
templates 3072 used 2105 | flat share 0.982
identical 0.2015
macro8x4 0.4651
same-shape-other 0.1609
cross-shape 0.1726
raw 8x8 frames 3885 | distinct indices 147 | cycling-pixel share 0.0047
```

The per-shape macro classification (77 macro shapes), the 3x3-context counts and the "33 self-surrounded tiles" figure came from extended versions of the same script:

* A shape is "macro8x4" when more than 60 % of its same-shape edges follow the +1/−7 (H) and +8/−24 (V) frame pattern.
* Contexts are the unique 3x3 tile-id neighbourhoods around flats on the 3072x3072 world grid.

Shape 19 rendered as an 8x4 grid confirmed the periodic water texture visually.

---

## Fact-check

Adversarial check run on 2026-10-04. For each claim I fetched the primary source and tried to refute it. For the measured data, I re-ran Appendix A and also wrote a separate implementation (`tmp/factcheck/indep.py`). Verdicts: **CONFIRMED**, **REFUTED** (wrong as stated, even if the conclusion survives), **UNVERIFIABLE** (no primary evidence found).

| # | Claim | Verdict | Evidence (primary source, date) |
|---|---|---|---|
| 1 | SDXL base 1.0 is CreativeML Open RAIL++-M. xinsir controlnet-tile-sdxl-1.0 is Apache-2.0. xinsir says its SR mode "may be unstable" | **CONFIRMED** | HF model cards. xinsir's exact words: "performance may unstable and next version is optimizing!" It recommends 1024², conditioning scale 1.0 and 30 steps. The card gives no date. |
| 2 | SeedVR2 numz node v2.5.24 is dated 2025-12-24 and is Apache-2.0. Batch must be 4n+1. Tiled VAE defaults are 1024/128. SageAttention 3 is optional. The model card warns about oversharpening | **CONFIRMED** | numz GitHub README; ByteDance-Seed/SeedVR2-3B card (Apache-2.0, arXiv 2506.05301, 2025-06): "tend to overly generate details on inputs with very light degradations … oversharpened results occasionally". The README gives VRAM only as tiers ("12-16GB" with FP8 + BlockSwap/VAE tiling). There is no per-model figure for 3B FP8 or 7B. |
| 3 | Z-Image-Turbo is 6B, Apache-2.0 and from 2025-11-27, fits 16 GB, uses guidance 0. The Tile 2.1-2601-8steps CN is 6.7 GB (lite about 2 GB), dated 2026-01-12, trained up to 2048², control scale 0.65-1.0 | **CONFIRMED** | Tongyi-MAI/Z-Image-Turbo card ("fits comfortably within 16G VRAM", "Guidance should be 0"). alibaba-pai Union-2.1 repo: Tile-2.1-2601-8steps is 6.71 GB and Tile lite-2601 is 2.02 GB. 2601 is dated Jan 12, 2026. Nuance: the first Tile 8-step file came on 2025-12-22. The newer 2602 release (2026-02-26) is Union-only, with **no Tile 2602**. |
| 4 | "Official ComfyUI template: `utility_z_image_turbo_2k_upscaler`" (listed as the Tile-CN path) | **REFUTED** (as implied) | The template exists in `Comfy-Org/workflow_templates` (`templates/utility_z_image_turbo_2k_upscaler.app.json`, first commit 2026-03-10, last 2026-09-28). It **does not use the Tile ControlNet**. The graph is RealESRGAN_x4plus, then lanczos 0.5x, then Z-Image-Turbo img2img (KSampler 5 steps, cfg 1, denoise 0.33, AuraFlow shift 3). The Tile-CN graph has to be built by hand. |
| 5 | xBRZ scales natively 2-6x. xbrz-rs is a GPL port | **CONFIRMED** | bell345/xbrz-rs README: "2x up to 6x", a Rust port of Zenju's C++ xBRZ, GPL-3.0. Last push 2024-07-28: an old but stable library. |
| 6 | Exult's `imagewin/scale_xbr.cc` is "2x-oriented", so a separate xBRZ is needed for 6x | **REFUTED** (the conclusion still holds) | Local source `exult-1.12.1/imagewin/imagewin.cc` (lines ~231-265) and `scale_xbr.h` register **2xBR, 3xBR and 4xBR**. None is 5x or 6x, so a native 6x scaler still needs external xBRZ. |
| 7 | jasperai Flux.1-dev-Controlnet-Upscaler is non-commercial. Settings are CN 0.6, 28 steps, guidance 3.5 | **CONFIRMED** | HF card: "Flux.1-dev model licence". The example uses `controlnet_conditioning_scale=0.6`, 28 steps and `guidance_scale=3.5`. It was trained with synthetic photo degradations (noise, blur, JPEG). |
| 8 | FLUX.2-klein-4B is Apache-2.0, released 2026-01-15, ~13 GB, does editing and multi-reference. The 9B is non-commercial | **CONFIRMED** | HF card: Apache-2.0, "fits in ~13GB VRAM", "multi-reference editing", 9B "under a non-commercial license". The card does not state the release date. The date comes from BFL's X post and Jan-2026 press, and is consistent with OpenRouter `created` = 2026-01-14 22:20 UTC. |
| 9 | Qwen-Image-Edit-2511 is Apache-2.0 and 20B. vafipas663's upscale LoRA is GPL-3.0 and "not trained on 2D/illustrations" | **CONFIRMED** | Qwen card: Apache-2.0, "20B params", and it says 2511 *mitigates* image drift. The vafipas663 card says GPL-3.0 and "Not trained on: 2d, illustrations, artworks, text, graphics, CGI". It recovers from "Pixelation up to 16x" on photos. |
| 10 | Qwen-Image-Edit is "also reachable through the OpenRouter-style APIs" | **REFUTED for OpenRouter** | The live `GET openrouter.ai/api/v1/models?output_modalities=image` (2026-10-04) lists 59 models and has **no qwen-image-edit id**. Qwen is offered only as `qwen/qwen-image-3` and `qwen/qwen-image-3-pro` (both $0.003/image). The local snapshot `openrouter_image_models_2026-10-04.json` agrees. `black-forest-labs/flux.2-klein-4b` *is* on OpenRouter. |
| 11 | HYPIR is non-commercial only (SD2.1, SIGGRAPH 2025) | **CONFIRMED** | XPixelGroup/HYPIR README: "strictly for non-commercial purposes". It is built on SD 2.1 and was presented at SIGGRAPH 2025. |
| 12 | Blackwell sm_120 has been in stable PyTorch since 2.7 (cu128). The ComfyUI nvidia portable ships Python 3.13 + CUDA 13.0 | **CONFIRMED** | PyTorch 2.7 blog (updated 2025-05-15): Blackwell support plus CUDA 12.8 wheels. It is labelled **Prototype** in 2.7. The ComfyUI README says `ComfyUI_windows_portable_nvidia.7z` is Python 3.13 / CUDA 13.0, and the manual install uses `whl/cu130`. |
| 13 | "This PC has driver 615.78 / CUDA UMD 13.4" | **REFUTED** (minor) | `nvidia-smi` in WSL reports `NVIDIA-SMI 615.78.02  KMD Version: 617.14  CUDA UMD Version: 13.4`. So 615.78 is the WSL nvidia-smi/userspace version and the Windows kernel driver is **617.14**. It still covers cu128 and cu130, so no conclusion changes. |
| 14 | spinagon circular padding works on SD-style conv models. Its ControlNet is not patched | **CONFIRMED** | `SeamlessTile.py` (HEAD 2026-02-12) patches every `Conv2d` in `model.model` and in the VAE `first_stage_model` only. The ControlNet model is never touched, so its borders stay zero-padded. The repo licence is **GPL-3.0** (the report says only "open source"). |
| 15 | Universal Seamless Tiles is new (July 2026) and unproven | **CONFIRMED** | GitHub API shows a single commit date, 2026-07-14. |
| 16 | OpenModelDB 4x-PixelPerfectV4 exists and is a pixel-art ESRGAN | **CONFIRMED** | OpenModelDB: ESRGAN 4x, "Sprite Upscaler", **WTFPL**, dated 2020-11-16, 63.9 MB. Caveat: it was trained on 300 *anime* images, and it is a 2020 model. |
| 17 | traiNNer-redux is open source with BasicSR lineage | **CONFIRMED** | README: Apache-2.0, a fork of joeyballentine/traiNNer-redux, itself a fork of BasicSR. It supports SPAN, ESRGAN and others. 6x support was not verified. |
| 18 | FLUX.1-Kontext-dev is non-commercial, and a BFL self-serve commercial licence exists | **CONFIRMED** | HF card: FLUX.1 [dev] Non-Commercial License. bfl.ai/blog/flux-1-kontext-dev and help.bfl.ai describe the self-serve licensing portal. |
| 19 | Retro Diffusion RD Tile generates new tiles with X/Y wrap and palette control. It is commercial and not a 6x upscaler | **UNVERIFIABLE** (partly) | Replicate page: it generates "tilemap elements", tilesets with "smart transitions" and "limited color pixel art". It is not an upscaler. X/Y wrap, palette control and pricing are **not stated** there. |
| 20 | SDXL + tile CN uses ~8-10 GB and takes "seconds per canvas" on a 5070 Ti | **UNVERIFIABLE** | There is no primary benchmark for this exact setup. It is the author's estimate [A]. Measure it in experiment step 1. |
| 21 | MEASURED terrain facts: 3,072 templates / 2,105 used; flats 98.2 %; edge split 20.1 / 46.5 / 16.1 / 17.3 %; 3,885 raw frames; 147 indices; 0.47 % cycling pixels; 3,121 used tiles in 123 shapes; 77 macro shapes (2,313 frames); 33 self-surrounded; cycling only E0-E7 + FE in 369 frames / 48 shapes | **CONFIRMED** | Appendix A re-run reproduces all of its outputs exactly. A separate re-implementation gives 3,121 / 123, 33, 77 / 2,313, and 369 / 48 with values {E0-E7, FE}. Its frame decode `(b1>>2)&0x1f` matches Exult's `Chunk_terrain` (`objs/chunkter.cc:117-118`); without the mask, 96,368 entries (12 %) decode wrongly. Caveats: (a) "77" depends on the >60 % threshold (80 at >50 %); (b) only **74 of the 77** have 32 frames, so the "32 frames = 64x32 texture" layout does not hold for 3 of them; (c) "33" means all 8 neighbours are identical (51 if only the 4 orthogonal ones are counted); (d) one used tile id points to a frame that does not exist in `shapes.vga`. |

**Net effect on the recommendation.**

* The primary stack holds: it is licence-clean, the quote is real and seamless SDXL tooling exists.
* Corrections:
  * The Z-Image "official template" is plain img2img after RealESRGAN, with no Tile CN.
  * Qwen-Image-Edit is **not** on OpenRouter; only Qwen-Image-3 / 3-pro are.
  * Exult already has a 3xBR and a 4xBR, but still no native 6x.
  * The Windows driver is 617.14.
  * Treat 3 of the 77 macro shapes as partial macro-textures (fewer than 32 frames).
