# Proposal B: performance and GPU-first ("CPU-indexed world, GPU-finished frame")

Architect: B_perf_gpu. Date: 2026-10-04.
Repo: `/home/simonea/ultima7_exult/exult-hires`, master `8b6ab6b43`. Every `file:line` refers to that tree.
Input: `docs-hires/analysis/00_architecture_map.md` (the "map", decisions D1-D10, touchpoints P/B/W/G/H/R/C/U) plus the detail reports, and `upscale-research/00_recommendation.md`.
New evidence for this proposal comes from benchmarks and probes in `/home/simonea/ultima7_exult/tmp/b_perf_gpu/` (Appendix A).

---

## 0. Verdict in brief

1. **The CPU in-place S-x 8-bit buffer (map D2-D3) is the right baseline. Keep it as the only authoritative world raster.**
   - Every per-pixel effect in the world is an operation on the *destination index*: xform translucency, shadows, blood, invisibility, rect shading, remaps and outlines. A GPU compositor could only reproduce them with per-object ping-pong targets or framebuffer fetch.
   - What a GPU world could save is small. At S=6 for a 320x200 view, the measured CPU world paint is 0.32-1.57 ms at -O2 (gap_4). Terrain flats are 0.04 ms of that; the rest is object traversal, which stays on the CPU in any design.
2. **Everything after the index buffer moves to the GPU.**
   - **Palette:** an `SDL_PIXELFORMAT_INDEX8` world texture shares `draw_surface`'s `SDL_Palette`. Palette cycling, day/night transitions and fades then cost a 1 KB palette upload, with no pixel conversion.
   - **Resampling:** a filter ladder picks NEAREST for integer ratios, PIXELART for fractional upscale and palette-aware LINEAR for ratios in [0.5, 1). An exact **area (box) filter in linear light** runs as a custom fragment shader through SDL 3.4's `SDL_GPURenderState`, with a GPU halving chain as the fallback.
   - **Present-time effects:** the earthquake shake becomes a texture offset (optional).
3. **Measured on this machine (§3):**
   - A full INDEX8 upload of a 1920x1200 world costs 0.11-0.37 ms of CPU, a dirty rect 0.02-0.11 ms, and a palette-only change 0.001-0.002 ms. The ARGB alternative costs 1.0-1.9 ms.
   - INDEX8 with `SDL_SetTexturePalette` at NEAREST 1:1 is **bit-exact** against a CPU LUT on four renderers.
   - The area shader runs through SDL's `gpu` renderer and matches a CPU reference within 0.63-1.0 LSB. The same filter on the CPU costs **5.6 ms per frame**.
4. **Downscale quality** (real 6x terrain art, against an exact linear-light area filter):
   - SDL's palette LINEAR scores 43-46 dB at r = 0.67-0.8, with up to 1.29x the reference shimmer on dithered art. It is an acceptable default; the area shader is the quality tier.
   - NEAREST scores 33-38 dB with up to 3.9x shimmer, so it is **never used for downscale**.
5. **Render-scale policy (changes D5): render at the art scale, downscale at the end.**
   - S_eff is the *largest* divisor of S_art (6, 3, 2) that fits the texture limit, a pixel budget (default 10 Mpx) and a supersampling cap (default 3:1 per axis).
   - This is the user's stated preference. It avoids index-space downsampling of 6x art whenever the budget allows. It matches D5 for both of the user's real profiles.
6. **1x behaviour.** Fixes P1 (off-by-one), P2 (void-tile fallback) and P3 (zero-fill) land as **three separate, upstreamable commits at the base of the fork**, before any art is hashed. The S=1 reference oracle is a binary built from that base commit (§5).
7. **M1 cost:** about 41 person-days including art tooling and the baseline art set, plus 3 days of optional extras (fine scrolling, frame pacing). M2 and M3 are outlined.

---

## 1. Goals and non-goals

**Goals**
- G1: The world renders at S_eff (6 for the 320x200 windowed profile) into a palette-indexed buffer. Game logic, hit-testing, dirty rects and the UI stay in game px (map I1-I5).
- G2: Selective 6x overrides for terrain flats in M1, at per-tile (shape, frame) and per-terrain (content hash) granularity, with NN fallback. Sprites follow in M2 and UI in M3.
- G3: Correct palette semantics at every scale: cycling, day/night/fog/light palettes, fades, the border-255 swap, xform translucency, invisibility, ramp remaps.
- G4: Frame budget on the target (Windows, RTX 5070 Ti, 1920x1200 to 3440x1440): world paint ≤ 1.6 ms (320x200 at S=6) and present ≤ 0.5 ms of CPU time per frame. No hitch above 4 ms when crossing into new terrain.
- G5: A high-quality present: an integer ratio is exact; a downscale is area-filtered (alias-free) when the GPU tier is available; a fractional upscale is sharp.
- G6: Deterministic tests that run headless in WSL. The GPU tiers are validated by readback against CPU references.

**Non-goals**
- A GPU world compositor (terrain tiles, sprite quads, GPU translucency). It is rejected in §2.
- RGBA override art or a 32-bit world buffer (palette.md §6.2).
- Per-pixel lighting, HDR, or shader post-effects beyond resampling (the CRT and scanline style). The shader hook makes them possible later, but they are out of scope.
- ExultStudio rendering changes. Studio zoom only needs to keep working through the S policy.

---

## 2. What runs where: alternatives evaluated

| Option | Description | World cost at S=6 (320x200) | Palette semantics | Effort / risk | Verdict |
|---|---|---|---|---|---|
| **A. Map baseline** | CPU S-x INDEX8 raster; ARGB+LUT world texture; INDEX8 only optional | 0.32-1.57 ms paint, plus 1.0-1.9 ms LUT and ARGB upload per full frame, plus a full re-convert on every palette tick | exact | medium | good, but leaves 1-2 ms per frame and palette re-conversion on the CPU |
| **B. This proposal** | Option A's raster; INDEX8 texture sharing the SDL palette; GPU filter ladder plus area shader; write-tracked uploads | the same paint, plus 0.02-0.37 ms of upload; a palette tick costs about 0 | exact (indices never leave the CPU raster; the GPU does index→RGB only) | medium (+2.5 days for the shader tier) | **chosen** |
| C. GPU world compositor | Terrain chunks as GPU textures, sprites as GPU quads, xform translucency emulated | saves at most about 1 ms of pixel work; object traversal (80-100 % of the cost) stays | translucency needs a destination-index read: R8 targets plus ping-pong per translucent object, or approximations through `translucency_argb` (not exact) | very high: rewrites the render loop, outlines, invisibility, remaps, blackness | rejected |
| D. 1x raster, GPU NN upscale, hi-res sprites as overlay quads | Keep 1x; draw override frames as GPU quads over the NN image | low CPU cost | breaks painter's order (an override under a later sprite), translucency over overrides, cycling inside overrides | high | rejected |
| E. 32-bit world buffer, RGBA art | — | 4x memory, a full re-render on every palette tick (10 Hz) | requires emulating every palette effect | very high | rejected (palette.md §6) |

The CPU raster is not the bottleneck:
- At S=6 the terrain pass is a 0.04 ms `memcpy` of cached chunks (gap_4 §3.3).
- Objects cost 70-175 ns each to traverse at any S.
- A full S=6 frame uses about a third of the 6.7 ms budget per iteration.

What does burn CPU time in option A is converting 2.3-9.3 Mpx to RGB on every show and every palette tick, and potentially filtering them. A GPU does both almost for free. Option B removes exactly that cost.

---

## 3. New measurements (this proposal)

Machine: Ryzen 7 9700X, WSL2, SDL 3.4.18 (local build), g++ -O2.

In WSL, SDL renders through **software** backends: lavapipe (Vulkan), llvmpipe (GLES2) and the software renderer. CPU-side call costs and correctness are therefore meaningful here. GPU execution time and present time are not.

The WSLg present path costs **6-7 ms per frame at 1920x1200 regardless of the technique** (X11 software presentation). **WSL is not a target for present-performance work.**

### 3.1 Present-path CPU costs (`present_bench.c`, `bench_win.c` built for Linux; ms per frame)

| Operation (world texture 1920x1200) | opengles2 (llvmpipe) | vulkan (lavapipe) | gpu (SDL_GPU on Vulkan) |
|---|---|---|---|
| INDEX8 full `SDL_UpdateTexture` (2.3 MB) | 0.11-0.31 | 0.26-0.37 | 0.13-0.21 |
| INDEX8 dirty rect 288x384 (an NPC at S=6) | 0.03-0.11 | 0.04 | 0.02 |
| Palette change only (`SDL_SetPaletteColors`) | 0.001 | 0.002 | 0.001 |
| ARGB: CPU LUT 8→32 + upload (9.2 MB) | 1.13-1.21 | 1.33-1.40 | 1.02-1.87 |
| Draw call issue (any filter) | ≤ 0.09 | ≤ 0.03 | ≤ 0.016 |

CPU-only reference (`cpu_area.cc`):
- LUT 8→32 of 1920x1200: **0.47 ms**.
- Fused LUT and fractional area downscale 1920x1200 → 1280x800, scalar: **5.6 ms**, in sRGB or linear light.
- NN x6 of a 320x200 frame by `memset`/`memcpy`: **0.04 ms**.

### 3.2 Correctness probes

| Probe | Result |
|---|---|
| INDEX8 + `SDL_SetTexturePalette`, NEAREST 1:1, readback vs CPU LUT | **0 / 2,304,000 mismatches** on `gpu`, `vulkan`, `opengles2`, `software` |
| Area shader (`area.hlsl` → SPIR-V via DXC) through `SDL_CreateGPURenderState`, readback vs CPU area reference | k=1: max 1.0, mean 0.001 LSB; k=1.5 (1920→1280): max 0.63 LSB; k=3 (5160x1800 → 1720x600): max 0.63 LSB |
| DXIL compile of the same shader (`dxc.exe` via WSL interop) | OK (5.9 KB); not yet executed on D3D12 (needs WP2) |
| `bench_win.exe` (llvm-mingw + SDL3-devel-3.4.18-mingw) | builds. A run from the `\\wsl.localhost` path stalled behind a Windows security prompt and was killed. **It must be run from a local Windows folder (WP2).** |

SDL 3.4.18 facts that the design relies on (verified in `deps/src/SDL3-3.4.18`):
- **Native INDEX8 everywhere.** All main renderers support it: d3d11, d3d12, vulkan, gpu, metal, gl, gles2, d3d9 and software.
- **LINEAR is palette-correct.** It does 4 palette lookups, then blends (`D3D11_PixelShader_Common.hlsli`, `SamplePaletteLinear`).
- **Palette changes reach the GPU without new code.** A texture's palette syncs to the public `SDL_Palette`'s version at draw time (`SDL_render.c:715-767`), so sharing `draw_surface`'s palette is enough.
- **A custom shader must carry its own palette.** `SDL_SetGPURenderState` binds only the INDEX8 texture at slot 0 and leaves SDL's palette texture unbound (`gpu/SDL_render_gpu.c:1006-1060`). Our shader passes the palette as 1 KB of uniforms.

### 3.3 Downscale quality (`downscale_quality.py`, real 6x terrain crops from the upscale research)

The reference is an exact area filter in linear light. "Shimmer" is the frame-to-frame change caused by a 1-hi-res-px scroll, divided by the reference's change; 1.00 is ideal.

| Content | r | SDL LINEAR (sRGB 4-tap) PSNR / p99 / shimmer | NEAREST PSNR / p99 / shimmer |
|---|---|---|---|
| xBRZ 6x + snap | 0.667 | 46.0 dB / 3.6 / 1.04x | 38.3 / 12.0 / 1.18x |
| xBRZ 6x + snap | 0.800 | 43.5 / 4.9 / 1.00x | 35.1 / 17.2 / 1.01x |
| same + 1-px ordered dither | 0.667 | 45.4 / 3.9 / 1.29x | 34.1 / 14.2 / 3.12x |
| same + 1-px ordered dither | 0.800 | 43.1 / 5.4 / 1.22x | 32.9 / 18.4 / 2.88x |
| NN 6x (no overrides) | 0.800 | 41.8 / 7.0 / 1.00x | 33.8 / 21.0 / 1.03x |
| art (both) | 0.500 | 47.6-47.8 / 2.1-2.3 / 1.00x (an sRGB-space 2x2 box; the residual is only the gamma error) | 32.3-34.8 / 17.6-19.5 / up to 3.91x |

So:
- LINEAR is an acceptable default for r ∈ [0.5, 1).
- The area shader removes the residual shimmer on dithered art and is gamma-correct.
- NEAREST is unacceptable for downscale.
- Below r = 0.5, LINEAR aliases, because SDL renderers have no mipmaps. That range needs the halving chain or the area shader.

---

## 4. Architecture

### 4.1 Data flow

```
 game px everywhere (logic, clip, dirty rects, hit tests) ─────────────────────────────┐
 shapes.vga / u7chunks ─► Shape_frame (1x) ──┬─► scaled primitives (NN, memcpy/memset)   │
                     hires store (M1 flats) ─┘   into Image_buffer8 main (scale=S,       │
                                                   bits = draw_surface, full·S + gb)      │
                                                   + WriteTracker (logical bbox)           │
                                                          │ show(): rect ∩ tracker, ×S    │
                                                          ▼                               │
           world_texture  INDEX8 STREAMING  (full·S)  ◄── SDL_UpdateTexture(rect)        │
           palette = draw_surface's SDL_Palette (shared; set_palette/rotate_colors)      │
                                                          │ UpdateRect(): filter ladder   │
              Tier 2: SDL_GPURenderState area shader (linear light, k per axis) ─┐       │
              Tier 1: NEAREST | PIXELART | LINEAR | halving chain ──────────────┼─► logical rect (0,0,dw,dh)
              Tier 0 (SDL<3.4): ARGB texture, CPU LUT on rect / full on palette tick ┘    │
                                                          ▼                               ▼
                                         composite_layers() (unchanged) ─► SDL_RenderPresent
```

### 4.2 Decisions

| # | Decision | Relation to the map |
|---|---|---|
| B1 | S belongs to the render target (`Image_buffer8::scale`). Logical API, physical storage, `ib8` mutated in place, no dual write | adopts D1-D3 |
| B2 | Frames without an override use on-the-fly NN inside the primitives, with `memset`/`memcpy` row kernels. Frames with an override use the existing painters through a physical view | adopts D6 |
| B3 | **The world texture is INDEX8 and shares `draw_surface`'s `SDL_Palette`.** The fork requires SDL ≥ 3.4. ARGB+LUT is kept only under `#if !SDL_VERSION_ATLEAST(3,4,0)` (Tier 0) | **modifies D4** (reversed priority) |
| B4 | **Write-tracked uploads.** The scaled primitives of the main buffer union their clipped logical rect into a `WriteTracker`. `show()` uploads `requested ∩ tracked` × S. An empty tracker means no upload: redraw and present only | **new** (D4 uploaded the requested rect, but `Game_window::show()` always requests the full window, gamewin.h:747-755) |
| B5 | **Filter ladder** chosen per axis from r = output px / texture px. Optional **area shader tier** via `SDL_GPURenderState` when the renderer is `gpu` | **extends D4/W7** |
| B6 | **S policy "render high, downscale late":** the largest divisor of S_art within the caps (§4.5) | **modifies D5** |
| B7 | Terrain: the exact 1x `src1x`, then the precedence per-terrain hash → per-tile → NN. The cache is sized from the view and a byte budget. **Override decoding never blocks paint**: per-tile flats are preloaded on a worker, per-terrain images are decoded asynchronously, and NN or per-tile serves as a placeholder until a generation bump | adopts D7, **adds async** |
| B8 | Sprites in M2 as map D8, with the payload painted by the `memcpy` fast-path RLE painter | adopts D8 |
| B9 | UI in M3 as map D9, plus **INDEX8 layer textures** with per-layer palettes, so palette ticks stop forcing CPU re-conversion of every layer (iwin8.cc:119, 173) | extends D9 |
| B10 | Tests as map D10, plus GPU readback probes against CPU references (software renderer always; `gpu` renderer on lavapipe when present) | extends D10 |
| B11 | Optional: earthquake as a present offset; fine (physical-px) scrolling; frame pacing while lerping | modifies G9, Q8 |
| B12 | Prerequisite fixes P1, P2, P3 and P5 as separate upstreamable commits at the fork base. P4 is a refactor. P6-P8 come in M2 | adopts §3.1, with an explicit oracle policy (§5) |

### 4.3 Image buffer core (`imagewin/`)

Adopt map B1-B9 as written:
- B1: `scale` in `Image_buffer`, logical clip; delete the copy constructor and assignment.
- B2: a scaled branch in every virtual primitive; `draw_line8` runs its DDA in game px and plots S×S blocks.
- B3: an NN RLE kernel that clips once per scanline and writes S rows.
- B4: scaled, clipped `copy`. B5: physical `get`/`put`. B6: same-scale `create_buffer`. B7: `phys_view()` and a scale-aware `blit`.

Additions in this proposal:

1. **`WriteTracker`** (imagebuf.h):
   ```cpp
   struct WriteTracker { int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
     void add(int x, int y, int w, int h);  bool empty() const;  TileRect take(); };
   WriteTracker* tracker = nullptr;   // non-null only on the main buffer when S>1
   ```
   - Only the scaled branch calls `tracker->add(...)` after clipping, so the scale==1 path stays byte-identical.
   - Raw `get_bits()` writers were checked with grep; none target the main buffer (`gamemap.cc:1706` and `iwin8.cc:190` read; `bggame.cc:700` writes a scene buffer).
   - The scalers never run at S>1 (map W11).
   - `EXULT_HIRES_FULL_UPLOAD=1` disables the tracker for bisecting.
2. **1x fast paths.** `paint_rle` and `paint_rle_remapped` (ibuf8.cc:516-825) get `memcpy`/`memset` per run, with output verified identical. This matters at -O0 and for override payloads (gap_4 §3.5).
3. **Optional `fine_offset`** (§4.9): a physical nudge in −(S−1)..0, applied before physical clipping. It is 0 unless fine scrolling is on.

### 4.4 Presentation (`imagewin/imagewin.{h,cc}`, `iwin8.{h,cc}`)

**New `Image_window` state (W1).** None of it is touched by `scale_layer_color` (imagewin.cc:1738-1781):

```cpp
int            render_scale = 1;         // S_eff
Image_buffer8* main_ibuf    = nullptr;   // persistent ib8 (iwin8.cc:64)
SDL_Texture*   world_texture = nullptr;  // INDEX8 STREAMING full·S (Tier 0: ARGB8888)
WriteTracker   world_writes;             // attached to main_ibuf when S>1
enum class PresentTier { None, Argb, Index8, Area } tier;
struct HalvingChain { SDL_Texture* rgb; std::vector<SDL_Texture*> levels; } halving;  // lazily built
SDL_GPURenderState* area_state = nullptr; AreaUniforms area_u;   // Tier 2
bool           world_palette_dirty = true;  // Tier 0 only
static int     cached_max_tex = 16384;
```

**Creation** (`create_scale_surfaces`, imagewin.cc:585-770; `create_surface` 540-578; W3/W4):
1. Before `SDL_CreateRenderer` (626), apply `config/video/renderer` via `SDL_HINT_RENDER_DRIVER` (and `SDL_HINT_GPU_DRIVER` for `gpu`).
2. After it, cache `SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER` (0 = unlimited) and read `SDL_GetCurrentRenderOutputSize`, which is HiDPI-correct.
3. Call `compute_render_scale(...)` (§4.5). If S>1:
   - skip `screen_texture`/`screen_texture_a` (saves 2 × 9.6 MB);
   - create `draw_surface` as INDEX8 `(full·S + 2gb)²`, with gb = 4 physical px;
   - create `world_texture` as INDEX8 STREAMING `full·S`, with `SDL_SetTexturePalette(world_texture, SDL_GetSurfacePalette(draw_surface))`.
4. `create_surface` sets the logical `width = full`, `scale = S`, `line_width = pitch` and the physical `bits`. It attaches `world_writes` and marks it full.
5. **Fail soft.** On allocation failure, retry with the next lower divisor, then S=1, instead of throwing (imagewin.cc:555-561).
6. **Tier 2.** If `SDL_GetGPURendererDevice()` is non-null and `present_filter` is `auto` or `area`, create the shader from the committed SPIR-V/DXIL/MSL header and then `area_state`. On any failure, log once and use Tier 1.

**Resize and device loss (W5).** `free_surface` (811-842) also destroys `world_texture`, the halving chain and `area_state`. The renderer is still destroyed on every resize (map Q16 unchanged), so everything is rebuilt in step 6. On `SDL_EVENT_RENDER_DEVICE_RESET` or `SDL_EVENT_RENDER_TARGETS_RESET`, mark `world_writes` full and drop the halving chain.

**`show()` (W6).** A new branch goes before today's lock at imagewin.cc:957:

```cpp
if (render_scale > 1 && ibuf == main_ibuf && !scene_mode) {
    TileRect r = clip_to_full(x, y, w, h).intersect(world_writes.take());  // logical
    if (!r.empty())   upload_world(r);   // ×S; INDEX8: SDL_UpdateTexture(world_texture, &pr,
                                         //   phys_pixels + pr.y*pitch + pr.x, pitch)
    if (tier == PresentTier::Argb && world_palette_dirty) upload_world_argb_full();
    UpdateRectWorld(false);
    return;
}
```
- The upload source is `draw_surface->pixels + gb·(pitch+1)` plus the physical offset.
- `Mouse::blit_dirty` and partial shows come through here too. The tracker makes them almost free.
- Replace the size heuristic at 968 with `ibuf == main_ibuf` (map W6).

**`UpdateRectWorld` (W7)** replaces `UpdateRect`'s first block when S>1:
1. Set the render target to `nullptr`, clear, and draw `world_texture` into the whole logical rect with the filter chosen below.
2. `composite_layers()` (unchanged).
3. Present, unless this is the screenshot path.

The **filter ladder** works per axis. Let `L = SDL_GetRenderLogicalPresentationRect()` (in output pixels) and `rx = L.w / tex_w`, `ry = L.h / tex_h`.

| Condition | Tier 2 (area available) | Tier 1 |
|---|---|---|
| rx and ry integer ≥ 1 | NEAREST | NEAREST |
| min(r) ≥ 1, fractional (ACF 1.2, HiDPI 1.25) | area shader (sharp area upscale) | PIXELART (`SDL_SCALEMODE_PIXELART`, palette-aware) |
| 0.5 ≤ min(r) < 1 | area shader, linear light | LINEAR (palette-aware 4-tap) |
| min(r) < 0.5 | area shader (supports k ≤ 8 per axis) | halving chain: INDEX8 → ARGB TARGET 1:1 NEAREST, then exact 2:1 LINEAR passes until r ≥ 0.5, then LINEAR |

`present_filter=nearest|linear|pixelart|area` forces a filter, for screenshots and A/B comparison.

**Area shader (Tier 2).** Source `imagewin/shaders/world_area.frag.hlsl` (prototype: `tmp/b_perf_gpu/area.hlsl`, Appendix B).
- It reads the INDEX8 texture at slot 0 with `Load`, plus a 1,056-byte uniform block `{src_size, k_linear, uint4 pal[64]}`.
- Per output pixel it integrates the k_x × k_y texel footprint with exact coverage weights (at most 8x8 taps), averaging in linear light. At k=1 it reproduces NEAREST.
- It costs about 9 loads per output pixel at r = 0.67: microseconds on a discrete GPU.
- **Palette hook.** `set_palette` and `rotate_colors` (iwin8.cc:96-174) already build `colors2[]`. They call `on_palette_changed(colors2)`:
  - Tier 2 copies it into `area_u.pal` and calls `SDL_SetGPURenderStateFragmentUniforms`;
  - Tier 0 sets `world_palette_dirty`;
  - Tier 1 needs nothing.

**Palette ticks must present (W9).** `Game_window::rotatecolours` (gamewin.cc:1054-1079) sets `painted` only for non-palettized windows. When `win->get_render_scale() > 1` it must also call `set_painted()`. This costs one redraw and present without an upload every 100 ms. Today cycling is visible only because full shows happen to be frequent.

**Guard band (W8).** `ShouldPaintIntoGuardband()` (imagewin.h:802-827) returns `false` first when `render_scale > 1`, before it dereferences `screen_texture`. `Begin/End/FillGuardband` become no-ops.

**Screenshots (W13).**
- Paletted: an S-x indexed PNG of `draw_surface` with the physical 4-px crop (an authoring feature).
- Non-paletted: `UpdateRectWorld(true)` plus `SDL_RenderReadPixels` (fix the latent crop at 1266).
- `mini_screenshot` (iwin8.cc:181-226) averages 3S×3S physical blocks. That is exact for NN renders (oracle O7).

**Tier 0 (SDL < 3.4 only):** an ARGB8888 STREAMING world texture. `upload_world` LUT-converts the rect from `colors[]`; a palette tick converts everything (0.47-0.9 ms, 9.2 MB). This exists so that upstream CI on 3.2.14 compiles. The fork never runs it on the target.

### 4.5 Render-scale policy (changes D5)

The policy is a pure static function next to `get_draw_dims` (imagewin.h:315) and is unit-tested:

```
int compute_render_scale(out_w, out_h, full_w, full_h, aspect_y /*1.2 for ACF modes, else 1*/,
                         S_art=6, S_cfg /*auto=S_art, or cap*/, forced /*0 or N*/,
                         max_tex /*0=unlimited*/, px_budget=10e6, ss_max /*3 with area or halving, else 2*/)
  if forced: return forced                         // authoring/screenshots; still checked against max_tex
  p = max(out_w / full_w, out_h / (full_h * aspect_y))
  if p <= 1.0 or S_cfg == 1: return 1             // the display cannot show more than 1 px per game px
  for d in divisors(S_art) descending:             // 6, 3, 2
     if d > S_cfg: continue
     if max_tex and (full·d + 2gb) > max_tex on either axis: continue
     if full_w·full_h·d² > px_budget: continue
     if d > ss_max · p: continue                   // at most ss_max:1 supersampling per axis
     return d
  return 1
```

| Configuration | full (game px) | p | map D5 | **B6** | world (Mpx) | final r |
|---|---|---|---|---|---|---|
| Windowed user (1920x1200, 320x200 Fit) | 320x200 | 6 | 6 | **6** | 2.30 | 1:1 |
| Window 1280x800, 320x200 | 320x200 | 4 | 6 | **6** | 2.30 | 0.667 |
| Window 640x400, 320x200 | 320x200 | 2 | 2 | **6** (Tier 1/2 with halving or area) | 2.30 | 0.333 |
| Fullscreen user (3440x1440, ACF Auto, 860x300) | 860x300 | 4 | 6 | **6** | 9.29 | 0.667 × 0.8 |
| FS scale 6, ACF Auto (573x200) | 573x200 | 6 | 6 | **6** | 4.13 | 1.0 × 1.2 |
| FS scale 2, ACF Auto (1720x600) | 1720x600 | 2 | 3 | **3** (6 would be 37 Mpx) | 9.29 | 0.667 × 0.8 |
| FS scale 1, Fill (3440x1440) | 3440x1440 | 1 | 1 | **1** | — | as today |
| Studio zoom k=2 / 3 | 960x600 / 640x400 | 2 / 3 | 2 / 3 | **3 / 6** | 5.18 / 9.22 | 0.667 / 0.5 |
| Windowed at 125 % DPI (2400x1500 output px) | 320x200 | 7.5 | 6 | **6** | 2.30 | 1.25 up (area or PIXELART) |

Notes on the policy:
- **Why it differs from D5.** For p ≤ 3, D5 renders at S = 2 or 3, so 6x art must be downsampled in index space, which cannot average colours. B6 renders the art as authored and lets the GPU average it in linear light. The extra cost is about 1 ms at 320x200.
- **Index-space downsampling remains for budget-forced S = 3 or 2.** Per block, it picks the in-block index whose palette-0 colour is nearest the block's linear-light mean. Cycling and translucent masks follow the block majority. Results are cached per (frame, S_eff).
- **`hires/max_mpx` defaults to 10.** The 9.29 Mpx fullscreen case is estimated at 1.3-5.3 ms of paint plus up to 9.3 MB of upload per full frame. WP11 must measure it. The recommended presets stay those of gap_2 (`scale=6` or `5`, Auto, ACF).
- **S changes go through `Game_window::resized`** (gamewin.cc:916-937): bump the render generation and flush the flats when S changed (map G2).
- **`--buildmap` forces S=1** (map G18).

### 4.6 Terrain path (M1)

This adopts map G11-G16 and gap_1 §5, with the following performance specifics.

**1. `render_flats`** (chunkter.cc:248-268):
- Always build `src1x`: 128², zero-filled (P3), painted by `paint_tile`. After P1/P2 and with P4, `paint_tile` returns the effective `ShapeID` per tile.
- If S>1, compose `rendered_flats` = `(128S)²` in this order:
  1. **Per-terrain override**, keyed by `fnv1a64(src1x)`. If its decoded image is resident, `memcpy` it (576 KiB, about 0.03 ms). If it exists but is not decoded yet, enqueue an asynchronous decode and fall through.
  2. **Per-tile overrides**, keyed by `(shape, frame&31)` and resident (preloaded). Each is accepted only if its 64-byte source equals the tile's 1x pixels.
  3. **NN expansion** of the 1x tile, using the `memset`/`memcpy` kernel.

  Store `{S, generation, used_terrain_override}` with the cache.
- When an asynchronous decode completes, the main thread:
  - polls a lock-free completion queue at the start of `Game_window::paint`;
  - bumps that terrain's generation;
  - calls `add_dirty` for the screen rects of the chunks that use it.

  The chunk then repaints with the per-terrain art. Paint never waits.

**2. Cache sizing** (chunkter.cc:234-242). The capacity is `(ceil(gw/128)+3)·(ceil(gh/128)+3)` entries, capped by `hires/cache_mb` (default 128) at `16384·S²` bytes per entry and never below the working set.
- 320x200 at S=6: 30 entries, 17.7 MB.
- 860x300 at S=6: 60 entries, 35 MB.
- This also fixes today's thrash above 100 chunks (gap_2 §5.1).

**3. Blit** (gamerend.cc:520-531). `copy8(get_bits(), 128, 128, …)` becomes `win->blit(*cflats, xoff, yoff)`: physical row copies, clipped. Chunks whose 128x128 game-px rect misses the clip are skipped before `get_rendered_flats()`.

**4. Store loading (H1, M1 subset).**
- Per-tile flats (BG: about 9 MB) are decoded on a worker right after `Shape_manager::load` (libpng, independent `png_struct`s). NN is used until the worker completes (expected < 1 s); completion bumps the generation.
- Per-terrain PNGs (about 140 KiB each; the top 200 take about 28 MB) are kept compressed in RAM and decoded on demand off the main thread (about 1.2 ms each).
- `hires/async_decode=no` makes both synchronous, for deterministic tests.

**5. Invalidation.**
- The generation is bumped by S changes, pack reloads, `reload_shapes` (P8) and `clear_chunks` (G16).
- `set_flat` frees the cache (G13), and `write_minimap` uses `src1x` (G15).
- The terrain editor's `render_all` gets the per-tile lookup (G14).

**6. Hitch budget.** A cold chunk costs at most about 0.25 ms. A new chunk row (up to 7 chunks at 860x300) therefore costs at most about 1.5 ms. The `paint_map` range already includes 1-2 off-screen chunks of margin (gamerend.cc:206-219), which also hides async decode latency.

### 4.7 Sprite path (M2, outline)

This adopts map D8 and H2-H13 as written:
- a hi-res slot on `Shape_frame`, identity stamped in `get_shape` (no I/O);
- the payload resolved on the paint path;
- a byte-budget LRU (256 MB desktop) that evicts at the frame boundary (`Game_window::show`);
- reflections derived by transpose;
- strict groups;
- a CRC gate;
- a sparse companion VGA as the shipping format, opened as a path.

Performance-specific points:
1. **Payloads paint through the fast RLE painter** on a physical view. gap_4 mode 2 models all-override scenes at 0.37-1.57 ms per frame (320x200, S=6). At -O0 the byte loops would cost 6-23 ms, which is why the fast paths ship in M1.
2. **Cold misses never block.** Companion-VGA frames need only a read. Dev-mode PNGs decode on the worker and use an NN placeholder plus a generation-driven `add_dirty` of the object's rect, the same pattern as terrain.
3. **Translucent painters batch per row** at S (gap_4: +0.17 ms in `brit_xlu`).
4. **No GPU involvement.** Sprites stay in the index raster, so xform translucency over terrain and over other sprites stays exact.

### 4.8 UI (M3, outline)

This adopts map D9 and W15/U1-U4: per-layer content scale k, the same `Shape_frame` hook, and a logical layer size that does not change. The GPU-first extension:

- **INDEX8 layer textures.** Each layer gets an `SDL_Palette`: the live `colors[]` or the fixed UI palette (palette.cc:234-282), with alpha 0 on index 255 and `index_argb` alpha on translucent indices.
- `refresh_layer` (iwin8.cc:281-322) then becomes a dirty-rect upload of raw indices. A palette tick (iwin8.cc:119, 173) becomes a palette version bump instead of a re-conversion of every layer.
- This applies to layers using NoScaler, point or bilinear; hqNx/xBR layers keep today's RGB path. It pays off once layers are k = 6 (a 1920x1200 layer is 2.3 Mpx).

### 4.9 Optional extras (M1.5)

1. **Earthquake as a present offset** (effects.cc:1829-1873). Instead of copying ±4 game px, showing and copying back, offset the world draw's destination rect by `(dx·S, dy·S)`. This saves two 2.3 MB copies and an extra present per step, and leaves the index buffer unshaken.
2. **Fine scrolling** (`hires/fine_scroll`, off by default, oracle-exempt).
   - `paint_lerped` (gamerend.cc:434-514) computes the scroll position in physical units. It splits it into a game-px part (used exactly as today) and a residual `rx ∈ [0,S)`.
   - `paint_map` sets `main_ibuf->fine_offset = (−rx, −ry)` for world objects.
   - Objects that receive `avposx_ld` in `get_shape_location` (gamewin.cc:1275-1313: camera actor, party, grouped barge) paint with offset 0 through an RAII guard, so they stay fixed while the world moves in 1/S-px steps.
   - The touch list is 28 `get_shape_location` sites and 19 `scrolltx_lo` users.
   - A present-time offset was rejected because the camera actor would jitter by up to 1 game px.
3. **Frame pacing while lerping.** `Delay()` (gumps/gump_utils.h:41-55) always sleeps up to 10 ms, which judders against 120/144 Hz vsync. While lerping with vsync on, skip it or use `SDL_DelayPrecise` up to the next refresh.

---

## 5. 1x behaviour and the engine fixes (decision)

**Decision: change 1x behaviour, through separate upstreamable commits, made first.**

1. `fix: paint_tile neighbour row 0 (chunkter.cc:104, '> 0' → '>= 0')` (P1). This is a genuine off-by-one. It changes the fill under RLE tiles in 779 of 2,105 used BG terrains (gap_1 §2.1). Those pixels are mostly hidden under the RLE objects.
2. `fix: skip void tile 12/0 in the full-chunk fallback (chunkter.cc:119-126)` (P2). It makes the fallback consistent with the 3x3 search's explicit intent.
3. `fix: zero-fill rendered_flats before painting (chunkter.cc:259)` (P3). **Mandatory.** It only removes undefined behaviour.
4. `fix: Sprites_effect y lerp uses scrollty_lo (effects.cc:459)` (P5). A visible jitter bug.
5. `refactor: paint_tile returns the effective ShapeID` (P4). No output change.

Why apply them instead of keeping strict upstream identity:
- Per-terrain override keys are FNV-1a hashes of the 1x render.
- Adopting P1/P2 *after* art has been hashed would orphan the per-terrain overrides of 37 % of used BG terrains.
- Hashes and goldens must therefore be computed exactly once, on the final 1x semantics.

Upstream: each commit goes upstream as its own PR. If upstream rejects P1/P2, the fork keeps them. The only cost is a 1x difference under RLE objects.

**Oracle policy.** The "S=1 identity" reference is a binary built from the commit right after these fixes (tag `hires-base`). Every later fork commit must produce `--buildmap 2` and `--render-test` output at S=1 that is byte-identical to `hires-base`. A second check, run once, compares `hires-base` against stock upstream with a mask of the RLE-fill positions. It proves the fixes changed only what they claim to.

---

## 6. Configuration and CLI

New keys under `config/video/hires/` (read in `setup_video`, exult.cc:3128-3304). They are not tied to scaler forcing (exult.cc:3203-3211).

| Key | Values (default) | Meaning |
|---|---|---|
| `enabled` | yes/no (yes) | Master switch. `no` gives today's pipeline exactly |
| `render_scale` | `auto` \| 1 \| 2 \| 3 \| 6 \| `force:N` (auto) | Cap, or force (authoring/screenshots; still clamped by max_tex) |
| `art_scale` | 6 | S_art; must match the pack |
| `max_mpx` | 10 (4 on 32-bit/Android) | Pixel budget for `full·S²` |
| `supersample_max` | 3 | Per-axis cap on render/display (forced to 2 when neither area nor halving is available) |
| `present_filter` | `auto` \| nearest \| linear \| pixelart \| area (auto) | §4.4 ladder |
| `linear_light` | yes | The area shader averages in linear light |
| `overrides` | on \| off \| identity \| marker (on) | identity/marker are test packs generated in memory (O4a/O4b) |
| `cache_mb` | 128 (64 on 32-bit/Android) | Flats cache byte budget (M2: the LRU has its own key) |
| `async_decode` | yes | Tests set it to no |
| `fine_scroll` | no | §4.9 |
| `partial` | strict \| lenient (strict) | M2 group activation |

Other keys:
- `config/video/renderer`: `auto` (SDL default order, which is D3D11 on Windows) \| direct3d11 \| direct3d12 \| vulkan \| gpu \| opengl \| opengles2 \| software.
- `config/video/gpu_driver`: auto \| vulkan \| direct3d12 (only for `renderer=gpu`).
- After WP11, the fork's Windows default becomes `gpu` if the probe shows it is stable and not slower than D3D11.
- `config/disk/game/<name>/hires_path` gives `<PREFIX_HIRES>` (map H14).
- `VideoOptions_gump` shows S_eff, the tier and the filter (C2, optional).

CLI, declared next to `--buildmap` (exult.cc:289-312, 393-430, 986-989; the `setup_video` guard at 825):
- `--render-test "<spec>"`: map C4 / gap_7 §5, region A/B through `push_render_target`. Added fields: `filter=` and `present=1`. With `present=1` it also renders through the present path with the given filter and writes the RGB readback, for O8.
- `--present-probe <out.json>`: the in-engine version of `bench_win`. It runs N frames for each tier and filter on the live renderer and records upload, draw and present times plus the readback checks. It is used on Windows in WP2/WP11.
- `--hires-scale N` and `--renderer NAME`: per-run overrides (also used by CI).

---

## 7. Override formats and directory layout

The search chain is `<PATCH>/hires/x6` → `<PREFIX_HIRES>/x6` → `<DATA>/hires/<game>/x6` (map H14). `<PREFIX_HIRES>` defaults to `$game_path/hires`, so it survives mod activation.

```
x6/
  pack.json                 {"game":"BG","art_scale":6,"palette0_sha1":"…","format":1,"generator":"tools/hires/pack.py …"}
  flats/SSSS_FF.png         48x48, colour type 3, PLTE = effective palette 0 (c8 = v*255/63), raw engine indices,
                            no tRNS, no index ≥ 0xE0 except where copied from the source cycling mask, never 0xFF
  flats/SSSS_FF.json        {"shape":S,"frame":F,"src64":"<128 hex>","crc32":"…","route":"xbrz6-snap|nxbrz|sdxl-tile",
                             "model_sha256":"…","seed":…,"qa":{"B1":…,"B3p99":…,"C1":…}}
  terrain/<16hex>.png       768x768, same PNG rules; the flat layer including the fill under RLE tiles; key = FNV-1a-64
                            of the zero-filled 16,384-byte src1x after P1-P3
  terrain/<16hex>.json      {"fnv64":"…","src1x_crc32":"…","terrains_seen":[…], "route":…, "qa":{…}}
```

Rules enforced by the loader. A violation means the override is rejected with one log line and NN is used:
- exact size;
- PLTE exactly equal to the effective palette 0 (8-bit, `v·255/63`);
- no tRNS;
- the index-class rules of map §4.2;
- `src64` (per-tile) and the FNV/CRC (per-terrain) match the engine's 1x data.

**Edge contract** (map §4.7): the outer band of B = 3 px of every override equals D(1x border) with D = NN index replication in v1. It is checked by the pack validator, not at runtime.

M2 adds the sparse companion VGA (`x6/shapes.vga`, …) as the shipping format for everything, flats included, decoded to raw 48x48 at load. Loose PNG remains the authoring and dev format.

---

## 8. Test strategy

**Framework.** `doctest` (single header, MIT) in `tests/`, run as automake `check_PROGRAMS` with `AM_TESTS_ENVIRONMENT = SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy` (map C6). The unit tests link `imagewin/ibuf8.cc`, `imagebuf.cc`, the scale-policy function and the terrain composer without SDL.

**Unit tests (CI, no game data):**

| Test | Asserts |
|---|---|
| `scale_policy` | `compute_render_scale` against the table in §4.5, plus gap_2 §2-§6 rows, the texture-limit and budget edge cases, and `forced` |
| `draw_dims` | `get_draw_dims` pinned to gap_2's tables (guards against refactor drift) |
| O1 `prim_fuzz` | 10^5 random op sequences (all primitives incl. xform, RLE, remap, outline, copy, get/put) on S=1 and S=k ∈ {2,3,6}: `phys == NN(ref)` |
| `tracker_complete` | in the same fuzz, every changed physical pixel lies inside `tracker.take()·S` |
| `fast_paths` | 1x `paint_rle` with memcpy runs is byte-identical to the legacy loops (kept in the test as the reference) |
| `fnv_crossref` | the C++ FNV-1a-64 equals the Python implementation on fixed vectors and on 3 synthetic 1x renders |
| `flats_compose` | precedence terrain > tile > NN; `src64` mismatch rejects; missing shape gives a zero cell; generation invalidation; async completion triggers a dirty repaint (simulated queue) |
| `png_rules` | the loader rejects tRNS, wrong PLTE, 0xFF in flats, wrong size, cycling index outside the source mask |
| `area_ref` | the CPU area reference (shared with the GPU probe) has unit weights and reproduces NEAREST at k=1 |

**Present tests (SDL, headless, CI):**
- `software` renderer under `SDL_VIDEO_DRIVER=offscreen`, with readback:
  - r=1 equals `LUT(index)` exactly (already measured at 0 / 2.3 M mismatches);
  - a palette tick without writes changes only the cycled indices and uploads 0 bytes;
  - a partial write uploads only `tracked × S`;
  - Tier 0 equals Tier 1 at r=1.
- `gpu` renderer when a Vulkan ICD exists (lavapipe in WSL; skipped otherwise): the area-shader readback stays within 1 LSB of `area_ref` for k ∈ {1, 1.25, 1.5, 3}. The prototype measured 0.63-1.0.

**Golden and oracle tests** (real BG data, local or nightly; `--render-test` static mode, pinned config, empty gamedat, audio off, `async_decode=no`):
- O2 `render_S == NN(render_1)` for S ∈ {2, 3, 6} on 12 regions (shore, water/lava, forest, town at lifts 16/10/5, mountains, dungeon, world-wrap edges).
- O4a identity flats pack (generated by `overrides=identity`) gives output identical to O2. O4b marker pack gives exactly the predicted diff.
- O6 repaint idempotence; O7 S-invariant scalars including `mini_screenshot`; O3 dynamic mode with a pinned save.
- S=1 identity against `hires-base` (§5), and a nightly in-memory full-map sweep at S=6 (gap_7 §5).

**Performance gates** (`--render-test ... perf=N`, -O2, medians; fail on > 20 % regression against a stored baseline):
- world paint at S=6, 320x200 ≤ 1.6 ms on the 8 gap_4 scenes;
- cold-chunk composition ≤ 0.25 ms;
- upload bytes per full frame = `full·S²`, and 0 on a palette tick.

**Windows validation (manual, WP2/WP11):** `bench_win.exe` and the in-engine `--present-probe` on direct3d11, direct3d12, vulkan and gpu (1920x1200 → 1920x1200 / 1280x800; 5160x1800 → 3440x1440), then the user's two real profiles.

---

## 9. Build and CI

**Linux (this machine, user-space).**
- `source /home/simonea/ultima7_exult/deps/env.sh`.
- Create `build-linux-o2` with `CXXFLAGS="-O2 -g"` (`--with-optimization=normal`). `build-linux` stays -O0 for debugging only (P9).
- Create `build-linux-asan` with `-O1 -g -fsanitize=address,undefined`.
- `make check` runs the units and the headless present tests.
- Golden and oracle scripts live in `tools/hires/ci/` and take the BG path from `run/exult.cfg`.

**SDL version.**
- The fork's `configure.ac` (479-485) gets `PKG_CHECK_MODULES(SDL, sdl3 >= 3.4.0)` behind `--enable-hires` (default yes in the fork).
- All 3.4 calls sit behind `#if SDL_VERSION_ATLEAST(3,4,0)`, with Tier 0 otherwise, so a build against upstream CI's 3.2.14 still compiles (map C7).

**Shaders.**
- `imagewin/shaders/world_area.frag.hlsl` is the source.
- `world_area.frag.{spv,dxil,msl}.h` are generated and **committed**, the same approach SDL itself takes. Normal builds need no shader compiler.
- `tools/hires/build_shaders.sh` regenerates them:
  - DXC (`dxc.exe` through WSL interop works and was verified here; the Linux DXC release needs glibc ≥ 2.34, which this Ubuntu 20.04 lacks);
  - `-T ps_6_0` for DXIL;
  - `-spirv -fspv-target-env=vulkan1.0` for SPIR-V;
  - optionally `spirv-cross --msl` for macOS. If MSL is missing, macOS falls back to Tier 1.
- The new sources are registered in all four build descriptions (map C6).

**Windows.** Follow build.md §5 W1: MSYS2 UCRT64 under `E:\Dati\Ultima7_Upscale\msys64`, driven from WSL, with a working copy on `E:`. Build with `make -f Makefile.mingw` and install into `E:\Dati\Ultima7_Upscale\ExultHires`, never over 1.12.1.
- No shader step is needed, because the headers are committed.
- Probes run from a local folder (`E:\Dati\Ultima7_Upscale\probe\`); running from `\\wsl.localhost` stalls behind a security prompt.
- The llvm-mingw cross-compile from WSL (used for `bench_win.exe`) is for probes only; Exult's Windows dependencies come from MSYS2.

**CI (GitHub, fork).** A job on ubuntu-24.04 with SDL ≥ 3.4 runs `make check` and the software-renderer present tests, with `mesa-vulkan-drivers` (lavapipe) for the area-shader readback test. Upstream's jobs stay and compile Tier 0 against 3.2.14.

---

## 10. Art production plan (6x BG terrain)

This aligns with `upscale-research/00_recommendation.md`. The engine side only fixes the contract: the formats in §7 and the QA gates.

1. **Tooling (WP9; `tools/hires/`).**
   - Fix `u7art.py`: emulate `paint_tile` after P1-P3 (fill under RLE), fix the v2 header (u7art.py:33), read `<PATCH>`, emit FNV-1a-64 keys, `src64` and CRCs, and refuse to run with a mod active.
   - `pack.py` writes the §7 layout. `validate.py` shares its rules with the loader tests. `qa.py` implements gates A1-F1 (recommendation §2.8).
   - `preview.py` runs `exult --render-test overrides=on scale=6 present=1 filter=area` on 50 regions. It writes 6x PNGs plus versions downscaled through the area reference at 1280x800 and at the 3440x1440 geometry, so reviewers see what the player sees.
2. **Baseline set (route 3, WSL CPU, now).** xBRZ 6x on real-map context windows with the fill emulated, local snap, per-(shape, frame) mode consensus, label-safe cycling pixels and an NN edge band. It covers all 3,885 BG flats in minutes, can be shared (offline GPL tool), and sets the QA thresholds.
3. **Default automatic set (route 2, WSL CUDA on the 5070 Ti, `tmp/factcheck/venv_cu130`).** 4x-NXbrz on chunk windows, Lanczos 1.5 to 6x, local snap, consensus, back-projection colour lock and edge band; minutes in total. The NXbrz licence (CC-BY-NC-SA) makes this pack **local-only**.
4. **Per-terrain overrides.** The top 200 BG terrains (81 % of map chunks) plus transition-rich ones (coast, roads, town floors), each upscaled as a whole 768² chunk with real neighbours, edge band applied, keyed by FNV. About 28 MB.
5. **Curated families (route 1, Windows ComfyUI, SDXL plus xinsir Tile over the route-3 base).** Water and shore, grass (the shape 19 torus), roads, floors. Human review; same gates.
6. **Delivery.** Packs per route under `E:\Dati\Ultima7_Upscale\packs\<route>\`, with the active one in `<BG>/hires/x6`, each with its QA report and preview sheets. Identity and marker packs are generated by the engine test mode, not stored.

---

## 11. Milestones and work packages

| WP | Name | Scope | Acceptance | Days | Depends |
|---|---|---|---|---|---|
| WP0 | Foundation | Fork branch; `build-linux-o2` and `-asan`; doctest plus `make check`; perf scopes per `paint_map` pass (R9); `run/` test configs | `make check` green; the -O2 build reproduces gap_4's 1x numbers within 10 % | 1.5 | — |
| WP1 | 1x fixes and reference | P1, P2, P3, P5 commits, P4 refactor, with unit tests; tag `hires-base`; S=1 golden set (`--buildmap 2` hashes plus 12 region PNGs); masked comparison against upstream | goldens are stable across 2 runs; the upstream diff is confined to the RLE-fill mask | 1.5 | WP0 |
| WP2 | Windows GPU probe | Run `bench_win.exe` on D3D11, D3D12, Vulkan and gpu on the 5070 Ti; record a JSON report; choose the default renderer for Tier 2 | INDEX8 readback exact on every backend; area within 1 LSB on gpu (D3D12 and Vulkan); a timing table | 1 | — (artifacts exist) |
| WP3 | Buffer scale core | Map B1, B2, B4-B7; `WriteTracker`; `phys_view`/`blit` | O1 plus `tracker_complete` pass for S ∈ {1,2,3,6}; the S=1 goldens are unchanged | 4 | WP0, WP1 |
| WP4 | RLE kernels | B3 NN kernels for paint, remapped, translucent (row-batched), transformed and outline; 1x `memcpy` fast paths | O1 extended to RLE; the `fast_paths` test passes; -O0 S=6 paint is ≤ 2x -O2 | 3 | WP3 |
| WP5 | Present Tier 1 and S policy | W1-W9, W11-W14, W16; `compute_render_scale`; INDEX8 world texture with the shared palette; `show()`/`UpdateRectWorld`; filter ladder; halving chain; Tier 0; device reset; `rotatecolours` present | `scale_policy` passes; headless present tests pass; the windowed profile runs at S=6 1:1 in WSL; a palette tick uploads 0 bytes | 5 | WP4 |
| WP6 | Present Tier 2 (area) | HLSL source, generated headers and script; GPURenderState plumbing; palette uniform hook; renderer and gpu_driver config; fallback | lavapipe readback ≤ 1 LSB; falls back cleanly on d3d11, software and missing shaders | 2.5 | WP5 |
| WP7 | Render-test harness and oracles | `--render-test` (C4, C5), static and dynamic modes, O2/O4a/O4b/O6/O7, perf gates, `--present-probe` | O2 passes at S ∈ {2,3,6} on 12 regions; perf gates are recorded | 3 | WP5 |
| WP8 | Terrain hi-res | G5, G11-G16; `src1x` plus FNV; terrain store with async preload and decode; PNG wrapper (H12); path tags (H14); config keys | O4a/O4b for flats; a cold chunk ≤ 0.25 ms; no paint blocks on decode; the 860x300 view keeps a flats hit rate of 100 % while static | 5 | WP1, WP5, WP7 |
| WP9 | Art tooling and baseline pack | u7art fixes, `pack.py`, `validate.py`, `qa.py`, `preview.py`; route-3 pack for all BG flats | `validate.py` passes on the pack; the engine loads every override (0 rejects); O5-style masked check | 5 | WP8 (validation), WP1 |
| WP10 | Art v2 | Route 2 for all flats plus the top-200 per-terrain set; route-1 pilot for 3 families; reviews | QA gates met; signed-off preview sheets | 8 | WP9 |
| WP11 | Windows release and validation | MSYS2 build; install; `--present-probe` and gameplay checks on both user profiles; default renderer and filter decided | paint ≤ 1.6 ms and present ≤ 0.5 ms CPU at 320x200 S=6; the 860x300 profile ≤ 6.7 ms per frame | 2 | WP6, WP8 |
| WP12 | Extras (opt.) | Earthquake present offset; fine scrolling; frame pacing | fine-scroll visual check plus O2 with the flag off; frame-time standard deviation < 1 ms while lerping | 3 | WP5 |
| WP13 | M2 sprite slot | H2-H5, H7-H8, H13; LRU; derived reflections; strict groups; worker decode | O4a for RLE, translucent and reflected frames; LRU never evicts in use | 8 | WP4, WP7 |
| WP14 | M2 packaging | Companion VGA (H6, P6, P7), `--dump-art` (H15), `exult_hirescheck` (H11) in CI | packs round-trip; the checker runs in CI | 8 | WP13 |
| WP15 | M3 UI | Layer content scale (W15, U1-U4); INDEX8 layer textures | hit-testing unchanged; a palette tick causes no layer re-conversion | 12 | WP13 |

Milestones:
- **M0 = WP0-WP2** (4 days): foundation, the reference oracle, real GPU facts.
- **M1a = WP3-WP7** (17.5 days): an NN hi-res world at S_eff with all oracles. The user can play at S=6.
- **M1b = WP8-WP9** (10 days): terrain overrides with the baseline art.
- **M1c = WP10-WP11** (10 days): production art and the Windows release.
- **M1 total: about 41.5 days.** M1.5 adds WP12 (3 days). M2 is about 16 days and M3 about 12 days on top.

---

## 12. Risks

| Rank | Risk | Mitigation |
|---|---|---|
| 1 | Tier 2 depends on SDL's `gpu` renderer, which is less battle-tested on Windows than D3D11 (vsync, resize, device loss, layer textures) | Tier 1 on D3D11 is the default until WP2/WP11 prove `gpu` stable; automatic fallback; `config/video/renderer` override |
| 2 | INDEX8 palette sampling on D3D11/12 has not yet been executed on the 5070 Ti (map risk 12) | Exactness verified on 4 backends in WSL; the WP2 probe is ready; Tier 0 exists at compile time; a runtime switch `present_filter=nearest` gives a debug path |
| 3 | Large views at S=6 (9.29 Mpx for the user's fullscreen) could exceed the frame budget during lerp (estimated paint 1.3-5.3 ms plus 9.3 MB of upload) | `max_mpx` budget; presets; the WP11 measurement; write-tracked uploads; later an optional UI-only repaint (map R5) |
| 4 | An incomplete `WriteTracker` leaves stale texture regions | Grep audit (§4.3); the fuzz invariant `tracker_complete`; the full-upload debug variable; O8 readbacks |
| 5 | Shader toolchain drift (DXC versions, SPIR-V binding layout) | Committed headers; a CI readback test on lavapipe; prototype bindings already verified against SDL 3.4.18's layout |
| 6 | Asynchronous decode introduces nondeterminism and visual pop-in | Sync mode in tests; the paint range's off-screen margin hides pop-in; the generation-bump repaint is local |
| 7 | 1x divergence from upstream (P1/P2) | Separate commits, upstream PRs, the `hires-base` oracle, the masked upstream comparison |
| 8 | Fine scrolling touches 47 sites | Off by default; oracle-exempt; RAII guard; its own WP |
| 9 | Art risks: index semantics, seams, staleness (map risks 3, 4, 6) | §7 loader rules, edge contract, source checks, QA gates |
| 10 | WSL present numbers are misleading (6-7 ms of WSLg presentation) | All present performance gates run on Windows; WSL gates cover only CPU-side upload bytes and times |

---

## 13. Deviations from the map

1. **D4: INDEX8 with a shared palette is primary; the fork requires SDL ≥ 3.4.** ARGB+LUT is only a compile-time fallback, the reverse of the map's priority. CPU cost is 4-10x lower, palette ticks are free, and the output is bit-exact on every renderer tested.
2. **D4/W6: write-tracked uploads** instead of the requested show rect, because `Game_window::show()` always requests the full window.
3. **D4/W7: the filter ladder** adds the area shader tier, the halving chain and per-axis selection. LINEAR only in [0.5, 1); NEAREST never for downscale.
4. **D5: the largest admissible divisor of S_art** instead of the smallest divisor ≥ S_need. This is the same for both user profiles; for p ≤ 3 it avoids index-space downsampling of 6x art.
5. **W3:** no `screen_texture`/`screen_texture_a` at S>1.
6. **W9:** no palette code in Tier 1; a uniform hook in Tier 2; and `rotatecolours` must request a present at S>1.
7. **G9:** the earthquake becomes an optional present-time offset.
8. **G6/Q8:** fine scrolling is an opt-in physical nudge with camera-actor exemption. A present-time offset is rejected.
9. **D7/H1:** override decoding is asynchronous and non-blocking, with a sync mode for tests.
10. **D9/W15 (M3):** INDEX8 layer textures.
11. **P1/P2:** adopted, with the `hires-base` oracle and a masked upstream comparison.

---

## Appendix A. Reproducing the measurements

Everything is in `/home/simonea/ultima7_exult/tmp/b_perf_gpu/`:

```bash
source /home/simonea/ultima7_exult/deps/env.sh
cd /home/simonea/ultima7_exult/tmp/b_perf_gpu
export LD_LIBRARY_PATH=$PWD/sysroot/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH   # libGLESv2 for opengles2
gcc -O2 -o present_bench present_bench.c $(pkg-config --cflags --libs sdl3)
./present_bench opengles2 1920 1200 1920 1200 300                # §3.1 (also: vulkan, auto)
gcc -O2 -o bench_lin bench_win.c $(pkg-config --cflags --libs sdl3) -lm
./bench_lin gpu 1920 1200 1280 800 30                             # area shader + readback checks
for r in gpu vulkan opengles2 software; do ./bench_lin $r 1920 1200 1920 1200 3; done
g++ -O2 -std=c++17 -o cpu_area cpu_area.cc && ./cpu_area         # CPU LUT / area / NN costs
/home/simonea/ultima7_exult/tmp/algo/venv/bin/python downscale_quality.py   # §3.3 (linear-light and sRGB variants)
/home/simonea/ultima7_exult/tmp/algo/venv/bin/python shimmer_srgb.py         # §3.3 table (SDL-like sRGB blend)
# shaders (DXC via WSL interop):
./dxcwin/bin/x64/dxc.exe -T ps_6_0 -E main -Fo area.dxil area.hlsl
./dxcwin/bin/x64/dxc.exe -spirv -T ps_6_0 -E main -fspv-target-env=vulkan1.0 -Fo area.spv area.hlsl
# Windows probe (copy bench_win.exe + SDL3.dll to a local Windows folder first):
#   bench_win.exe direct3d11|direct3d12|vulkan|gpu 1920 1200 1280 800 200
```

## Appendix B. Area shader core (prototype, `area.hlsl`)

```hlsl
float3 fetch(int2 t, float lin) {                      // INDEX8 texel -> palette RGB (optionally linear)
    uint i = (uint)(u_texture.Load(int3(t, 0)).r * 255.0 + 0.5);
    uint p = pal[i >> 2][i & 3];
    float3 c = float3(p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF) / 255.0;
    return lin > 0.5 ? to_lin(c) : c;
}
float4 main(PSInput input) : SV_Target {
    float2 c = input.v_uv * src_size.xy, h = 0.5 * kparams.xy, a = c - h, b = c + h;
    int2 i0 = (int2)floor(a), i1 = (int2)ceil(b) - 1, mx = (int2)src_size.xy - 1;
    float3 acc = 0; float wsum = 0;
    for (int y = i0.y; y <= i1.y && y <= i0.y + 7; y++) {
        float wy = min(b.y, y + 1) - max(a.y, y);
        for (int x = i0.x; x <= i1.x && x <= i0.x + 7; x++) {
            float w = (min(b.x, x + 1) - max(a.x, x)) * wy;
            acc += w * fetch(clamp(int2(x, y), 0, mx), kparams.z); wsum += w;
        }
    }
    float3 o = acc / max(wsum, 1e-6);
    return float4(kparams.z > 0.5 ? to_srgb(o) : o, 1.0);
}
```

Bindings for SDL 3.4's GPU renderer:
- the texture and sampler are at `(t0/s0, space2)`, which is SPIR-V set 2 binding 0 as a combined image sampler;
- the uniforms are at `(b0, space3)`, which is set 3;
- the inputs are `TEXCOORD0` (colour) and `TEXCOORD1` (uv), matching SDL's `tri_texture.vert`.
