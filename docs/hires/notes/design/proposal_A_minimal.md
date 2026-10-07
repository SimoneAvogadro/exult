# Proposal A: minimal-risk, upstream-friendly hi-res render scale ("A_minimal")

Repo: `/home/simonea/ultima7_exult/exult-hires`, upstream master `8b6ab6b43`. Every `file:line` refers to that tree.
Input: `docs-hires/analysis/00_architecture_map.md` (read in full), plus `ibuf.md`, `present.md`, `world.md`, `ui.md`, `build.md`, `palette.md`, `gap_1`, `gap_2`, `gap_4`, `gap_6`, `gap_7` and `upscale-research/00_recommendation.md`.
I re-checked these claims in the code:
* the paint_tile quirks at `objs/chunkter.cc:104,119-126,259`;
* `create_surface` / `create_scale_surfaces` / `show()` / `UpdateRect` / guard band at `imagewin/imagewin.cc:540-770, 882-1114, 1142-1252, 2170-2220`;
* `ShouldPaintIntoGuardband` at `imagewin/imagewin.h:801-827`;
* the primitives at `imagewin/ibuf8.cc:43-600` and `imagewin/ibuf8.h:30-115`;
* `Import_png8` / `Export_png8` at `shapes/pngio.cc:45-260`;
* `set_palette` / `rotate_colors` / `mini_screenshot` at `imagewin/iwin8.cc:96-226`;
* `paint_chunk_flats` at `gamerend.cc:520-531` and `write_minimap` at `gamemap.cc:1690-1723`;
* `BuildGameMap` and the CLI at `exult.cc:289-312, 825, 986-989, 2857-2912`;
* the path tags at `gamemgr/modmgr.cc:56-85, 592-617`;
* `push/pop_render_target` at `gamewin.cc:541-550`;
* `Sprites_effect::paint` at `effects.cc:459`;
* `scale_layer_color` at `imagewin/imagewin.cc:1738-1781`;
* the BG intro scene views at `gamemgr/bggame.cc:398-406`.

---

## 0. The proposal in twelve lines

1. **Keep the map's core and nothing more.** S is a property of the render target (D1). It lives in a new `Image_buffer::pixel_scale` field. The API stays logical and only storage is physical (D2). The single main `ib8` is mutated in place and the world is never rendered twice (D3). Frames without an override use nearest-neighbour (NN) inside the primitives (D6).
2. **S=1 stays byte-identical to upstream**, apart from one mandatory determinism fix (P3, zero-fill of the flats cache). The bug fixes P1, P2 and P5 go upstream as separate PRs. They do **not** go into the fork's hi-res line, so an unmodified upstream build stays a free golden oracle.
3. **Present path:** one new private function, called from the top of `show()` when `world_scale > 1`. It does an ARGB8888 + LUT upload of the dirty rect into the *existing* `screen_texture` / `screen_texture_a` pair, which is recreated at `full·S`. The existing `UpdateRect`, layer compositing, screenshot and mouse mapping are reused unchanged. It uses SDL 3.2 API only.
4. **S selection:** `render_scale` from config (default 1, meaning off). It is lowered only by hard limits: the renderer's max texture size and a pixel budget. No automatic derivation in M1, which matches the user's "render high, downscale at the end". GPU scale mode: NEAREST for an integer ratio, LINEAR otherwise.
5. **Terrain:** the existing `Chunk_terrain` cache becomes a scaled `Image_buffer8`. The *unchanged* `copy8` call gives the NN fallback for free. A per-tile override, keyed by `(shape, frame&31)`, is a 48x48 physical block copy. There are no per-terrain content-hash overrides, no 64-byte source gate and no byte-budget LRU in M1.
6. **Override store:** loose raw-index PNGs, `SSSS_FF.png`, read with the existing `Import_png8(..., -1, ...)`. A subdirectory is a strict group (all files valid or the group is skipped). There are two search roots: `<PATCH>/hires/x<S>/flats`, then `<HIRES>/x<S>/flats`. Art exists per scale (`x6`, optionally `x3`), so the engine never resamples indexed art.
7. **Tests:** doctest unit tests, SDL-free. They cover a primitive fuzz oracle `phys(S) == NN(S=1)`, an S=1 digest golden recorded on upstream code, the S-selection table, the store loader, and the pinned legacy fill rules. A headless `--render-test` gives in-process A/B oracles for NN, identity and marker packs, plus a present read-back test. `--buildmap` hashes are compared against the upstream+P3 build.
8. **Order:** something visible early. M1a is "the world at S=6 with NN, pixel-identical to today's point x6". M1b adds the terrain overrides. M1c covers Windows, performance and the first complete 6x tile pack. Sprites (M2) and UI (M3) are outlined only.
9. **Engine diff for M1:** about 1,400 changed or added lines outside tests. Most of it is in `imagewin/ibuf8.cc` (scaled branches) and two new files (`shapes/hires_flats.*`, `render_test.cc`).
10. **Off by default.** No new library dependency. libpng is already used and stays behind `HAVE_PNG_H`. Rebases with upstream's layer/texture work conflict on a handful of one-line hooks.
11. **Art:** the deterministic xBRZ+snap+consensus route (WSL CPU) gives a complete, valid BG 6x flat pack within days. NXbrz (WSL CUDA) and diffusion (Windows GPU) then improve the curated families on the same QA gates.
12. **Estimate:** about 19 engineering days to M1c, plus art iterations.

---

## 1. Goals and non-goals

**Goals (M1)**
* G1. Render the world view at an integer pixel scale S (target 6) into the main 8-bit buffer. Game logic, hit testing, dirty rects, scrolling and draw order stay in game pixels.
* G2. Draw frames without an override with NN, so the result is pixel-identical to today's `point` xS.
* G3. Override terrain flats (shapes.vga flat frames) per tile, and per group of tiles, with 8S×8S indexed art. Missing tiles fall back to NN.
* G4. When the window is smaller than S × the view, downscale only at present time (the user's preference). When it is larger, upscale with NEAREST if the ratio is an integer and LINEAR otherwise.
* G5. S=1 is byte-identical to upstream, modulo P3. Everything new is off by default.
* G6. Automated tests run headless in WSL: unit, golden and oracle tests. The fork also builds on Windows (MSYS2 UCRT64).
* G7. Deliver a complete 6x BG flat pack that passes the QA gates, with a path to curated quality.

**Non-goals for M1** (explicitly deferred)
* RLE sprite overrides (M2), UI/gump/font/face overrides (M3).
* Per-terrain (128x128 chunk) overrides, CRC or source gates, a hi-res LRU, a companion VGA pack format, `--dump-art`.
* Automatic S derivation (`auto`), INDEX8 textures, a halving-chain downscale for ratios below 0.5, a VideoOptions UI for S.
* Thin (1 physical px) outlines or grids, and physical sub-pixel lerp. Debug lines simply become S px thick.
* Android/iOS overrides: no libpng there. The NN world at S still works.

---

## 2. Prerequisite fixes: what changes 1x and what does not

| # | Item | Decision | Why |
|---|---|---|---|
| P1 | `chunkter.cc:104` `tiley + y > 0` (row 0 never used as a fill neighbour) | **Not in the fork's hi-res line.** Upstream PR `chunkter: allow row-0 neighbours in fill search`, with a unit test. | It is a genuine off-by-one, but it changes the 1x picture under RLE objects in 779 of 2,105 used BG terrains. The hi-res design does not depend on it, because per-tile keys are `(shape, frame)` and the fill source is taken from the engine itself (§3.5). Keeping 1x identical lets the unmodified upstream binary serve as the S=1 oracle. If upstream merges the fix, the fork picks it up on rebase and the goldens get regenerated once. No art is invalidated, because no art is keyed on the 1x terrain render. |
| P2 | `chunkter.cc:119-126` full-chunk fallback may pick void 12/0 | Same as P1 (separate upstream PR). | Same reasoning. 112 BG positions. |
| P3 | `chunkter.cc:259` + `ibuf8.h:37-39` uninitialised cache | **In the fork, first commit**, also offered upstream. `rendered_flats->fill8(0)` before painting. | Determinism is mandatory. It changes only pixels whose content was undefined: tiles with a missing shape (BG flat 48/8, 149 tiles in 2 terrains) and stale content after `commit_edits`. The reference build for goldens is "upstream + P3". |
| P4 | `paint_tile` should report the substitute it painted | Done as a **pure refactor** (`find_flat_source`, §3.5) with pinning tests. | No output change. It is needed so the hi-res pass uses the same substitute. |
| P5 | `effects.cc:459` uses `get_scrolltx_lo()` for y | Upstream PR only. | It changes 1x visuals during lerp. The oracles run with lerp off, so the fork does not need it. |
| P6 / P7 / P8 | `encode_rle runs[200]`, `get_rle_shape len==0`, `reload_shapes` kind mapping | M2 (sprites). Each is offered upstream as a bug fix with a test. | Not on the terrain path. |
| P9 | `build-linux` is -O0 | Add `build-o2` (and `build-asan`); never measure on -O0. | Measured 6–23 ms per frame at -O0. |
| P10 | dead `toggle_fullscreen` | Not touched. | Out of scope. |

---

## 3. Architecture

### 3.1 Overview

```
                    game px everywhere (logic, clip, dirty rects, mouse)  ─────────────┐
u7chunks ─► Chunk_terrain ─► rendered_flats: Image_buffer8(128,128, pixel_scale=S)      │
             find_flat_source()   copy8()  → NN S×S (unchanged call)                    │
             Hires_flats::find()  put_phys() → 48x48 override block                     │
                     │ blit()                                                           │
                     ▼                                                                  ▼
 shapes ─► Shape_frame painters (unchanged) ─► Image_buffer8 main ib8 (logical API, pixel_scale=S)
                                               bits = draw_surface (INDEX8, full·S + 2·gb)
                                                     │ show(): world_scale>1 → show_world_scaled()
                                                     ▼   (logical rect ×S, LUT 8→ARGB, LockTexture(rect))
                                     screen_texture (ARGB8888, full·S, STREAMING)
                                                     ▼   UpdateRect (unchanged): dirty → screen_texture_a
                                     screen_texture_a (TARGET, NEAREST|LINEAR) → logical display rect
                                     composite_layers() (UI at 1x, unchanged) → SDL_RenderPresent
```

The map's invariants I1–I5 hold by construction:
* `inter_width/scale` is unchanged, so `screen_to_game` is unchanged (`imagewin.cc:1291-1292`).
* All geometry the API exposes stays logical.

### 3.2 Image buffer: `pixel_scale`

**Data** (`imagewin/imagebuf.h:57-66`): add `int pixel_scale = 1;` (protected), and `int get_pixel_scale() const`.
* `width`, `height`, `offset_x`, `offset_y` and the clip rect keep game-px semantics.
* `bits` is the physical address of logical (0,0). `line_width` is the physical pitch.
* The physical address of logical (x,y) is `bits + y·S·line_width + x·S`. Negative logical coordinates work as they do today.
* Also `= delete` the copy constructor and copy assignment of `Image_buffer`. This is defensive, and it is kept only if the tree still compiles, which shows nobody copies buffers.

**New constructors** (`imagewin/ibuf8.h:36-47`):
* `Image_buffer8(unsigned w, unsigned h, int scale)`: owned. It allocates `w·S × h·S`, value-initialised. Line width is `w·S`.
* `Image_buffer8(unsigned char* origin, int line_width, int w, int h, int offset_x, int offset_y, int scale)`: a non-owning view. Unit tests need it to get negative offsets and foreign pitches, and M2 reuses it as the "physical view".

**New members:**
* `void blit(const Image_buffer8& src, int destx, int desty)`. Same scale: a logical clip, then a physical row copy that honours `src.line_width`. At S=1 with `src.line_width == src.width` it calls `copy8` verbatim, so the output is identical. A different scale is a programming error (asserted); M1 never needs it.
* `void put_phys(const unsigned char* src, int pw, int ph, int px, int py)`. A physical copy clipped against `clip × S`. It is used only by the terrain override path.

**Scaled branches.** Each primitive keeps its existing body in `if (pixel_scale == 1) { ... }`, untouched. Below that is a scaled branch that reuses the existing logical clip helpers (`clip`, `clip_x`, `imagebuf.h:69-100`) and then writes physical memory. There are three internal helpers:
* `phys(x, y)`, the pointer;
* `rep_row(dst, src, w)`, which expands each byte ×S;
* `dup_rows(first, wbytes)`, which memcpys the first physical row into the other S−1 rows.

| Primitive (`ibuf8.cc` lines) | Scaled branch |
|---|---|
| `fill8(pix)` 140-146 | memset of `line_width · height·S` from the physical (−offset·S) origin |
| `fill8(rect)` 152-167, `fill_hline8` 173-181 | logical clip, then memset `S·w` on `S·h` (resp. S) rows |
| `copy8` 360-384, `copy_hline8` 390-402 | logical clip, `rep_row` + `dup_rows` per source row |
| `copy_hline_translucent8` 408-434 | per source pixel: opaque → write S×S; translucent → `xforms[c−first][*p]` **for every physical pixel** (the destination may already hold hi-res detail) |
| `fill_hline_translucent8` 440-456, `fill_translucent8` 462-481 | apply `xform[*p]` to every physical pixel of the S-scaled span |
| `copy_transparent8` 488-513 | non-zero source pixels become S×S blocks (dead code; implemented for completeness) |
| `get_pixel8` / `put_pixel8` (`ibuf8.h:99-107`) | read the top-left physical sample / logical clip, then an S×S block |
| `draw_line8` 185-354 | Bresenham in game px, plotting each logical pixel as `fill8(…,1,1,…)` or `fill_translucent8(…,1,1,…)`. Debug and cheat callers only; S px thick, as the NN oracle requires |
| `copy` 43-68 (scroll, earthquake) | all coordinates ×S, then memmove `S·h` rows of `S·w` bytes. It stays unclipped, like today; callers pass in-range rects |
| `get` 74-99 / `put` 105-110 | `get`: same clip as today, then a physical copy into `dest` using `dest->line_width` (same scale). `put`: a same-scale source becomes `blit`; a scale-1 source keeps `copy8`, which replicates |
| `fill_static` 115-134 | one `std::rand` per *logical* pixel, then an S×S block. The RNG consumption is identical to S=1 |
| `paint_rle` 516-669, `paint_rle_remapped` 672-825 | each RLE scan is one contiguous opaque span, so: decode the scan (with remap if needed) into a small row buffer, then call the scaled `copy_hline8(row, scanlen, scanx, scany)`. Correct by construction, because it reuses the clip, at the cost of one extra pass over source bytes |
| `create_another` (`ibuf8.h:58-60`) | returns an owned buffer of the **same** scale. `ImageBufferPaintable` (`shapeid.cc:643-654`) and save-unders then round-trip exactly |
| `draw_box`, `draw_beveled_box` | unchanged; they are built from the virtual primitives |

None of the `Shape_frame` painters (`shapes/vgafile.cc:483-699`), the `Image_window8` qualified wrappers (`iwin8.h:94-149`) or the fonts need a change in M1. They all reach the primitives above, and the scale is a field, not a subclass.

### 3.3 Window and present path

**State** (`imagewin/imagewin.h:320-409`, `Image_window`):
* `Image_buffer* main_ibuf`, set to `ib` in the constructor (`imagewin.h:569-578`);
* `int world_scale = 1`;
* `SDL_Color world_colors[256]`;
* `bool world_full_upload = true`;
* `int world_scale_requested()`, which reads the config.

The name `world_scale` avoids a clash with the existing `Layer::render_scale` (`imagewin.cc:2130`).

**Surface creation** (`create_scale_surfaces`, `imagewin.cc:585-770`). `display_width/height` are known from lines 672-673. Right after the initial clear (675-683) and just before the texture creation at line 690, add one call:

```cpp
if (create_world_scaled_surfaces(w, h)) return true;   // S>1 handled entirely in a new function
// ... existing code unchanged for S == 1
```

`create_world_scaled_surfaces`:
1. Read the request from `config/video/hires/render_scale`.
2. Query `SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER` from `SDL_GetRendererProperties(screen_renderer)`. A value of 0 means 16384.
3. Compute `world_scale = compute_world_scale(req, inter_width/scale, inter_height/scale, maxtex, budget)` (§3.4). If the result is 1, return false, which falls through to today's path.
4. Otherwise create:
   * `screen_texture`: `SDL_PIXELFORMAT_ARGB8888`, STREAMING, `pw = full_w·S`, `ph = full_h·S`;
   * `screen_texture_a`: ARGB8888, TARGET, same size. Its scale mode is NEAREST when `display_w/pw == display_h/ph` and that ratio is an integer ≥ 1, otherwise LINEAR;
   * `draw_surface`: INDEX8, `pw + 2·gb × ph + 2·gb`, plus its palette.
   * Then set `inter_surface = paletted_surface = draw_surface`, apply the same `set_ui_layer_config` calls as today, and return true.
5. On any failure, free, set `world_scale = 1` and return false. Exult then runs exactly as today. This is the fail-soft behaviour in place of the fatal throw at 555-561.

**Buffer wiring** (`create_surface`, `imagewin.cc:564-577`). The same statements, each multiplied by S:
```cpp
const int S = world_scale;  ibuf->pixel_scale = S;
ibuf->width  = (draw_surface->w - 2*guard_band) / S;  ibuf->height = (draw_surface->h - 2*guard_band) / S;
ibuf->line_width = draw_surface->pitch / ibuf->pixel_size;
ibuf->offset_x = (get_full_width() - get_game_width()) / 2;  ibuf->offset_y = ...;   // logical
ibuf->bits = pixels + (guard_band + ibuf->offset_y*S) * line_width + guard_band + ibuf->offset_x*S;
```
For S=1 this is algebraically the current expression.

**Guard band.**
* `ShouldPaintIntoGuardband()` (`imagewin.h:801`) gets `if (world_scale > 1) return false;` as its first line, before it dereferences `screen_texture`.
* `FillGuardband()` (`imagewin.cc:1226`) returns early when `world_scale > 1`. It is called from `menulist.cc:306` outside scenes and would otherwise memcpy logical widths into physical rows.
* `Begin/EndPaintIntoGuardBand` then never activate, so no physical value is ever written into a logical field. This was map risk #1.

**`show()`** (`imagewin.cc:882`). After the `ready()`/`scene_mode` preamble, add `if (world_scale > 1) { show_world_scaled(x, y, w, h); return; }`. The new function:
1. If `ibuf != main_ibuf || scene_mode`, a layer or scene is the target, so the world did not change. Set `world_full_upload = true` and call `UpdateRect(&empty, &world_full, false)`. This skips today's whole-texture lock, which would otherwise invalidate `screen_texture` (present.md risk 4).
2. Clip with `ibuf->clip(...)`, exactly as today (903), and convert to buffer coordinates (`x -= start_x`).
3. Palette check: `memcmp(SDL_GetSurfacePalette(draw_surface)->colors, world_colors, 1024)`. If the palette differs, or `world_full_upload` is set, take the whole buffer as the rect and copy the palette. This replaces the map's W9 hooks in `set_palette`/`rotate_colors`. Those already write `draw_surface`'s palette (`iwin8.cc:108-117, 160-169`), so no change in `iwin8.cc` is needed.
4. Build `uint32 lut[256] = 0xFF000000 | r<<16 | g<<8 | b` (256 entries, negligible).
5. `SDL_LockTexture(screen_texture, &prect, &pixels, &pitch)` with `prect = rect × S`. Convert the rows from `draw_surface` (offset by the guard band) through the LUT, then unlock.
6. `UpdateRect(&prect_f, &world_full, false)`. Unchanged code then copies the dirty rect into `screen_texture_a`, draws `screen_texture_a` onto the logical display rect, composites the layers and presents.

**`UpdateRect`** (`imagewin.cc:2170-2220`): one guard, `if (dirtyRect && dirtyRect->w > 0 && dirtyRect->h > 0)` around the first `SDL_RenderTexture`. S=1 never passes an empty rect, so S=1 behaviour does not change.

Screenshots need no code:
* The non-paletted path re-renders `screen_texture_a` with `last_fullrect`.
* The paletted path saves the S× INDEX8 `draw_surface` with the physical 4-px crop. That is an authoring feature: an S× indexed world image.

**`mini_screenshot`** (`iwin8.cc:190-201`): replace the raw `pixels[pitch*Y + X]` read with `ib8->get_pixel8(X, Y)`. That is identical at S=1. At S>1 it takes the top-left sample of each block, which is an exact S-invariant for NN renders (oracle O7).

**Not changed:**
* `screen_to_game` / `game_to_screen` (1277-1313);
* `scale_layer_color` (1738-1781). It patches and restores `line_width/height` of the current target and never reads `pixel_scale`. Consequently **no** `assert(scale==1)` goes into the scalers, unlike map W11, because it would fire here;
* all scalers, all layer code, `iwin8.cc` palette code.

### 3.4 Choosing S

New header-only, SDL-free `imagewin/world_scale.h`:

```cpp
inline int compute_world_scale(int requested, int full_w, int full_h, int max_tex, long long max_px) {
    int s = std::clamp(requested, 1, 8);
    if (max_tex <= 0) max_tex = 16384;
    while (s > 1 && (s*full_w > max_tex || s*full_h > max_tex || 1LL*s*s*full_w*full_h > max_px)) --s;
    return s;
}
```

Defaults: `max_px = config/video/hires/max_mpixels` (default 16) × 2^20.

| Configuration (gap_2) | full | requested | S_eff |
|---|---|---|---|
| windowed 1920x1200, 320x200, Fit | 320x200 | 6 | 6 (1:1, NEAREST) |
| fullscreen 3440x1440 Auto ACF, scale 4 | 860x300 | 6 | 6 (5160x1800, LINEAR 0.67/0.8) |
| recommended fullscreen preset: scale 6 Auto ACF | 573x200 | 6 | 6 (3438x1200, LINEAR 1.0/1.2) |
| Studio zoom x1 on 1920x1200 | 1920x1200 | 6 | 1 (budget) |
| `--buildmap` window 2048² | 2048x2048 | forced 1 | 1 |

If S is lowered, a one-line log entry says so. The flats cache re-renders lazily, because its scale no longer matches (§3.5). `Game_window::resized` (`gamewin.cc:916-937`) needs no change. Its toast can append `"(xS)"`, which is optional.

### 3.5 Terrain path

**Refactor without behaviour change** (`objs/chunkter.cc:86-133`). `paint_tile` is split into:
* `static int find_flat_source(int tx, int ty, TileKind (*kind)(int idx, void*), void* ctx)`. It returns the index (0..255) of the tile whose flat frame is painted at (tx,ty), or −1. `TileKind` ∈ {None, Flat, FlatVoid (12/0), Rle}. It is a byte-for-byte port of today's loops, **including** the `tiley + y > 0` bound and the missing 12/0 skip in the full-chunk scan. Unit tests pin both quirks, so a later upstream P1/P2 merge becomes a deliberate test update.
* `void Chunk_terrain::paint_flats(Image_buffer8& dst, bool overrides)`:

```cpp
dst.fill8(0);                                         // P3
const int S = dst.get_pixel_scale();
for (ty..) for (tx..) {
    const int src = find_flat_source(tx, ty, kind_of, this);  if (src < 0) continue;
    const ShapeID& sid = shapes[src];
    const unsigned char* hi = (S > 1 && overrides) ? Hires::flat(sid.get_shapenum(), sid.get_framenum() & 31, S) : nullptr;
    if (hi) dst.put_phys(hi, 8*S, 8*S, tx*8*S, ty*8*S);
    else    dst.copy8(sid.get_shape()->get_data(), c_tilesize, c_tilesize, tx*c_tilesize, ty*c_tilesize);  // NN at S
}
```

At S=1 the write sequence equals today's, plus the leading `fill8(0)`.

**Cache** (`chunkter.h:43, 95-101`; `chunkter.cc:248-268`):
* `get_rendered_flats(int scale = 1)` re-renders when `rendered_flats->get_pixel_scale() != scale` or `rendered_gen != Hires::generation()`. `render_flats(scale)` allocates `Image_buffer8(128,128,scale)` and calls `paint_flats(*rendered_flats, true)`.
* `commit_edits` re-renders at the cache's current scale.
* `Map_chunk::get_rendered_flats` (`objs/chunks.h:228-230`) forwards the scale.

No global flush is needed: a scale or generation mismatch is detected per terrain on access. Destructors and `clear_chunks` free caches as today.

**Queue size** (`chunkter.cc:234-242`): return `max(100, (cw+3)·(ch+3))` from the current game area. This is the formula the original author left in comments. It changes performance only, never pixels, and fixes the LRU thrash above 100 chunks. Memory at S=6 is at most about 59 MB, because the pixel budget keeps the views small. No byte-budget LRU.

**Consumers:**
* `Game_render::paint_chunk_flats` (`gamerend.cc:520-531`):

  ```cpp
  Image_buffer8* tgt = gwin->get_win()->get_ib8();
  if (Image_buffer8* c = olist->get_rendered_flats(tgt->get_pixel_scale())) tgt->blit(*c, xoff, yoff);
  ```

  Because the cache scale is taken from the **current target**, the in-process A/B harness (§8) works through `push_render_target`.
* `Game_map::write_minimap` (`gamemap.cc:1690-1723`) renders each terrain into a local `Image_buffer8 tmp(128,128)` with `paint_flats(tmp, false)` and averages that. It never builds 3,072 S× caches and never churns the shared cache.
* `render_all` (terrain-editor mode, `chunkter.cc:284-313`) and the direct flat paint in `Shape_frame::paint` (`vgafile.cc:525-534`) get NN through the scaled `copy8`. They get no overrides in M1, which is acceptable for editor-only paths.

### 3.6 Hi-res flat store

New `shapes/hires_flats.{h,cc}`. It is compiled only `#ifdef HAVE_PNG_H`; otherwise the stub returns nullptr.

```cpp
class Hires_flat_store {            // pure: no config, no path tags, no SDL -> unit-testable
public:
    struct Report { int loaded = 0, rejected = 0, groups_skipped = 0; std::vector<std::string> messages; };
    Report load(const std::vector<std::string>& roots, int scale);  // roots in precedence order
    const unsigned char* find(int shape, int frame) const;         // frame &= 31; nullptr if none
    int scale() const;  size_t size() const;  void clear();
private:
    int scale_ = 0;
    std::unordered_map<uint32_t, std::vector<unsigned char>> tiles_; // key = shape << 5 | frame
};
namespace Hires {                   // engine glue (same file, behind config/path tags)
    const unsigned char* flat(int shape, int frame, int scale);    // lazy (re)load per scale
    unsigned generation();  void invalidate();  void set_enabled(bool);  bool enabled();
}
```

Loading rules (deterministic, simple):
* Each root is scanned with `std::filesystem::directory_iterator` (upstream already uses `std::filesystem`: `gamedat.cc:50`, `cheat.cc:50`), with names sorted.
* A top-level `SSSS_FF.png` is a **single-tile override**.
* A subdirectory is a **strict group**. It is skipped if its name ends in `.off`. If any `.png` in it fails to parse or validate, the whole group is skipped and named in the log.
* Non-PNG files are ignored, so provenance JSON and READMEs can stay in the pack.
* Per file: `Import_png8(path, -1, ...)` (raw indices; no tRNS handling because `transp_index = -1`, `pngio.cc:131-136`). Accept only colour type PALETTE, exact size `8S × 8S`, and no index `0xFF` (flats must be opaque; it is the border colour). The palette is not checked in the engine; the offline validator does that.
* The name must match `^\d{4}_\d{2}\.png$`, and `frame < 32`.
* Precedence: the first root wins per key (`<PATCH>` before `<HIRES>`). Inside a root, the first in sorted order wins, and a duplicate is a warning.
* Glue: `Hires::flat` returns nullptr when `enabled()` is false. The first call for a scale S loads `{<PATCH>/hires/x<S>/flats, <HIRES>/x<S>/flats}` (only defined, existing directories) and bumps `generation`.
  * Loading all 3,885 BG tiles costs about 0.2–0.4 s once, on the first S>1 terrain render.
  * Memory is about 9 MB at S=6.
* Invalidation: `Hires::invalidate()` at the start of `Shape_manager::load` (`shapeid.cc:150`, game or mod switch) and in `reload_shapes` (`shapeid.cc:420`, ES). An optional dev key action calls `set_enabled(!enabled())` + `set_all_dirty()` for in-game A/B.

**No runtime source gate in M1.** gap_6 measured that none of the installed mods (Keyring, Ultima6v1.2, islefaq) replaces a flat. Packs are per game, because `<HIRES>` defaults to the game's own path. The offline validator checks a pack against the effective game data. The CRC gate arrives in M2, where mods do patch 70–263 shapes. A mod that changes flats ships its own `<PATCH>/hires`, or the user sets `overrides=no`.

### 3.7 Other main-buffer consumers (audit result)

| Consumer | At S>1 |
|---|---|
| `ImageBufferPaintable` (`shapeid.cc:643-654`) | `create_buffer` returns a same-scale buffer, and get/put copy physically. Exact |
| Scroll `view_*` (`gamewin.cc:1641-1735`), earthquake (`effects.cc:1829-1873`) | `copy` scaled branch |
| Border fill, blackness, clear_screen, plasma, saving dim (`gamerend.cc:364-384, 648-708`; `gamewin.cc:489-497, 2982-3002`) | scaled `fill8` / `fill_translucent8` |
| Legacy drag path, `display_area`, egg/barge/bbox debug lines (`drag.cc:413-448`, `usecode/intrinsics.cc:1791-1840`, `shapes/shapeinf.cc:546-607`) | scaled primitives; lines are S px thick |
| BG intro zoom `SDL_SurfaceOwner(get_bits)` (`gamemgr/bggame.cc:689-710`), FLI, menus | run inside `Scene_view` (`bggame.cc:406` etc.) on scale-1 scene buffers. Unaffected |
| Scalers, `scale_layer_color`, layers | never touch an S>1 world (§3.3) |
| `write_minimap`, `mini_screenshot`, `paint_chunk_flats` | changed (§3.3, §3.5) |
| `BuildGameMap` (`exult.cc:2857-2912`) | `config->set("config/video/hires/render_scale", "1", false)` before the window is created |

### 3.8 Change list (M1)

| File | Change | Lines (approx.) |
|---|---|---|
| `imagewin/imagebuf.h` | `pixel_scale`, accessor, deleted copy ops | +15 |
| `imagewin/ibuf8.h`, `ibuf8.cc` | 2 ctors, `blit`, `put_phys`, helpers, scaled branches of 17 primitives | +450 |
| `imagewin/world_scale.h` (new) | `compute_world_scale` | +25 |
| `imagewin/imagewin.h`, `imagewin.cc` | members; `create_world_scaled_surfaces`; wiring ×S; guard-band early-outs; `show_world_scaled`; UpdateRect guard | +170 |
| `imagewin/iwin8.cc` | `mini_screenshot` via `get_pixel8` | ±3 |
| `objs/chunkter.h/.cc`, `objs/chunks.h` | `find_flat_source`, `paint_flats`, scale/generation cache, queue size | +90 / −40 |
| `gamerend.cc`, `gamemap.cc` | `paint_chunk_flats`, `write_minimap` | ±20 |
| `shapes/hires_flats.h/.cc` (new) | store + glue | +260 |
| `gamemgr/modmgr.cc` | `<{GAME}_HIRES>` (config `.../hires`, default `$game_path/hires`) and the `<HIRES>` clone in `setup_game_paths` | +8 |
| `shapeid.cc` | 2× `Hires::invalidate()` | +2 |
| `exult.cc`, `render_test.cc` (new) | `--render-test` declare/guard/dispatch; BuildGameMap force S=1; harness | +12 / +350 |
| build files | `Makefile.am` EXULTSOURCES, `shapes/Makefile.am`, `Makefile.common` (`SHAPES_OBJS` += `pngio.o hires_flats.o`; `render_test.o`), `msvcstuff/vs2019/Exult.vcxproj(+filters)`, `configure.ac` + `tests/Makefile.am`, `.gitignore` (`data/shortcutbar*`) | +60 |

To minimise rebase conflicts with upstream's ongoing layer/texture rework, every S>1 behaviour sits in **new functions** (`create_world_scaled_surfaces`, `show_world_scaled`, the scaled branches). The existing functions gain only one-line hooks.

---

## 4. Configuration and CLI

| Key | Values | Default | Meaning |
|---|---|---|---|
| `config/video/hires/render_scale` | 1..8 | 1 | requested S for the world; 1 = feature off |
| `config/video/hires/max_mpixels` | integer | 16 | pixel budget for `full·S` (megapixels) |
| `config/video/hires/overrides` | yes/no | yes | load override packs (no = pure NN, used by oracles) |
| `config/disk/game/<name>/hires` | path | `$game_path/hires` | base pack root, tag `<HIRES>`; survives mod activation |

The keys are independent of `scale`/`scale_method`. Those keep their meaning: Auto game-area divisor, mouse mapping, and UI-layer scalers. When `render_scale > 1` the world scaler is simply not used.

**Recommended user configurations:**
* windowed 1920x1200, game 320x200, Fit, `render_scale=6`;
* fullscreen 3440x1440, `scale=6`, Auto, Aspect Correct Fit, `render_scale=6`. This is the 573x200 view; see gap_2 §0.6 for the 860x300 trade-off.

**CLI:**
* `--render-test "<k=v,...>"` (M1, §8.2). It skips `setup_video` like `--buildmap` (`exult.cc:825`) and is dispatched next to it (`exult.cc:986-989`).

No other new switches. A VideoOptions entry and `render_scale=auto` are post-M1 (WP-16).

---

## 5. Override files and directory layout

```
<HIRES>/                                  default <game_path>/hires   (BG: .../ultima7/hires)
  x6/
    flats/
      0019/                               strict group (here: all frames of shape 19)
        0019_00.png ... 0019_31.png
      water/                              any name = custom group
        0010_00.png  0010_01.png ...
      0123_04.png                         single-tile override
      0042.off/                           disabled group (skipped)
      provenance/*.json, README.txt       ignored by the engine
  x3/flats/...                            optional art for S=3 (never derived by the engine)
<PATCH>/hires/x6/flats/...                mod- or patch-specific; wins over <HIRES>
```

**Tile PNG:**
* 8-bit palette (colour type 3), exactly 48x48 at x6 (`8S×8S`);
* **raw engine indices** (no ipack +1 rotation), and no tRNS;
* PLTE should be palette 0 with `c8 = v·255/63`. It is ignored by the engine and checked by the validator;
* no `0xFF`;
* cycling indices (0xE0–0xFE) only where the 1x source pixel is in the same cycle range (validator rule; map §4.2);
* naming `SSSS_FF.png`, where FF is the flat frame (`&31`).

The engine does not need `oFFs` for flats.

Groups are the unit for "selective per-group" activation:
* By default the pack tools group by shape. Frames of a flat shape form a macro-texture, so a half-converted shape would look patchy.
* Modders can make material groups such as `water/` or `roads/`.
* A single tile at the top level is the unit for "selective per-tile".

---

## 6. Present and downscale path

* Let `r = display / (full·S)` per axis.
  * `r` equal on both axes and an integer ≥ 1 (1:1 at the user's windowed profile): NEAREST, exact.
  * Otherwise: LINEAR. For `0.5 ≤ r < 1` a single bilinear pass is a correct area-like downscale; at exactly 2:1 it is the 2x2 box. The aspect-correct 1.2 vertical stretch also uses LINEAR.
* `r < 0.5` cannot happen with the recommended configurations. If a user forces it, LINEAR aliases. It is logged once.
  * The post-M1 fix (WP-16) is a halving chain: TARGET textures at ½, ¼, … with LINEAR, then the final pass. That is about 30 lines in `UpdateRect`.
* Cost at 1920x1200: LUT conversion about 0.9 ms for a full frame, 9.2 MB upload (present.md §7.5). That is about today's `point` fill-scaler cost. At 5160x1800 (the 860x300 view) it is about 3.6 ms plus 37 MB per full frame. That is why the recommended fullscreen preset is the 573x200 view.
* An INDEX8 texture plus `SDL_SetTexturePalette` (SDL ≥ 3.4) would quarter the upload and remove the LUT pass. It is a post-M1 optimisation under `#if SDL_VERSION_ATLEAST(3,4,0)` with a runtime fallback (WP-16). The ARGB path stays the baseline, so upstream's SDL 3.2.14 CI keeps building.

---

## 7. Sprite path (M2, outline) and UI (M3, outline)

The goal of the outline is the smallest hook that can carry real sprite art without touching `Vga_file::get_shape`.

1. **Store.** `<HIRES>/x6/shapes/SSSS_FF.png` (raw index, tRNS only on 255, `oFFs = (−xright_h, −ybelow_h)` per map G3), plus groups exactly as for flats. At load, for each key, call `sman->get_shapes().get_shape(shape, frame)` to obtain the authoritative `Shape_frame*`. This loads only the 1x frames that have art. Validate the canonical geometry (map §4.3 G1–G4) against it, encode to RLE (needs P6), and record it in a **side table** `unordered_map<const Shape_frame*, Hires_frame>`.
   * This uses no identity stamp, no slot in `Shape_frame` and no I/O on the paint path.
   * `Shape_frame` pointers are stable until `reset`/`reload`, which already call `Hires::invalidate()`.
2. **Paint.** In `Shape_frame::paint_rle / _remapped / _translucent / _transformed / _outline` (`vgafile.cc:483-699`), add: `if (win->get_pixel_scale() > 1) if (auto* h = Hires::frame(this, S)) { paint h through a physical view of win at (x·S, y·S); return; }`. The physical view is the view constructor from §3.2, with the clip ×S. Culling with `is_visible` stays logical.
3. **Reflections.** At load, for actor shapes, also register `get_shape(shape, f|32)` when it is synthesised. Its payload is the transposed hi-res base (map R1).
4. **Groups.** The same strict directory rule. The map's derived groups (animation cycles, actor frames 0..31, missiles, barges) become **validator** rules that propose directory groupings. The engine does not derive them.
5. **Validity gate.** A CRC32 of the canonical 1x frame is stored in the PNG name suffix or in a pack index, decided in M2. On mismatch the engine falls back to NN. This is needed because mods patch shapes.vga.
6. **Memory.** Eager load is fine while packs are partial. If complete packs exceed about 200 MB, switch to the map's D8 (lazy payload plus a frame-boundary LRU). The side table makes that a local change.

**M3 UI:** per-layer content scale k (map D9). The dragged item, cursor, gumps, fonts and faces then use the same `Hires::frame` hook through `push_render_target`. Not designed further here.

---

## 8. Test strategy

### 8.1 Unit tests (SDL-free, `make check`, CI-safe, synthetic data only)

Framework: **doctest** (single header, MIT, C++11; works with g++ 9, mingw and MSVC), vendored as `tests/doctest.h`.
* Autotools: `tests/Makefile.am` with `check_PROGRAMS = hires_unit` and `TESTS = hires_unit`, linking `imagewin`/`shapes`/`files` convenience libs plus `$(PNG_LIBS) $(ZLIB_LIBS) $(SDL_LIBS)`. Tests never initialise SDL.
* `Makefile.common` gets a `hires_unit$(EXEEXT)` target so Windows can run the same binary.

| Test file | What it asserts |
|---|---|
| `test_ibuf_golden.cc` | A fixed-seed stream of 20k primitive ops (all primitives, random clips, negative offsets via the view ctor, real-shaped xform tables, RLE frames built with `Shape_frame(pixels, …)`) on S=1 buffers. The FNV-1a-64 digest after every 1k ops must equal the digests **recorded on unmodified upstream code** in WP-02. Proves S=1 byte identity of `ibuf8.cc` |
| `test_ibuf_scaled.cc` (O1) | The same op stream applied to an S=1 and an S=k buffer (k ∈ {2,3,6}, several seeds). After every op `phys(S) == NN(ref)` over the whole physical buffer, including bands. Also covers `get/put` round trips, `copy`, `blit`, `create_another` scale, `put_phys` clipping, `get_pixel8` sampling and `fill_static` RNG parity |
| `test_world_scale.cc` | the §3.4 table, texture and budget limits, clamps |
| `test_flat_source.cc` | pins the legacy fill rules: row-0 neighbour ignored; 12/0 skipped in the 3x3 pass but not in the full scan; row-major first hit; all-RLE gives −1; null shapes |
| `test_hires_flats.cc` | fixtures written with `Export_png8(name, -1, …, false)`: valid tile; wrong size, non-palette, `0xFF` rejected; group strictness (one bad file skips the group); `.off` skipped; root precedence; frame `&31`; duplicate handling; bad names; empty and missing roots |

### 8.2 Headless engine harness `--render-test` (real BG data; local and nightly)

`render_test.cc`, modelled on `BuildGameMap`:
1. Force `render_scale=1` for the window, set gamma 1, create `Game_window(w,h,…,1,point,Fit,point)`.
2. Run `create_game`, `init_files(false)`, `srand(seed)` (re-seeds after `gamewin.cc:587-588`), map init, `set_map`, palette 0. Gamedat is empty (static mode). Audio is off in the test config.
3. Spec: `tx,ty,w,h,lift,scales=2:3:6,mode=nn|identity|marker|plain,repaint=N,present=0|1,bench=N,out=DIR`.

For each check, the region is rendered with `paint_map_at_tile` into a pushed `Image_buffer8(w,h)` (reference) and into pushed `Image_buffer8(w,h,S)` targets. That works because S is carried by the target and the flats cache follows the target's scale.

| Oracle | Mode | Assertion |
|---|---|---|
| O2 | `nn` (overrides off) | `phys(S) == NN(ref)` exactly |
| O4a | `identity` (pack = NN×S of every original flat) | equal to O2 output exactly. Proves the store, lookup, `put_phys`, substitute handling and cache path |
| O4b | `marker` (pack = NN×S with the top-left physical pixel of each block set to a marker index) | every differing physical pixel is a marker sub-pixel holding the marker value, and the count is > 0. Proves overrides are really used |
| O6 | `repaint=N` | after a full paint at S, N random logical sub-rect repaints leave the buffer unchanged |
| O7 (reduced) | always | `mini_screenshot`-style 3×3 sampling via `get_pixel8` is identical for ref and S (NN modes) |
| Present | `present=1` | creates the window at display `w·S × h·S` with `render_scale=S`; paints into the main buffer; `show()`; RGB read-back (`SDL_RenderReadPixels`, factored out of `screenshot()`) must equal `LUT(NN(ref))` exactly (software renderer, NEAREST 1:1). It also asserts `screen_to_game` on a 16×16 grid matches S=1 maths |
| Bench | `bench=N` | median/p95 of `paint_map_at_tile` at S (no present) |

Outputs: `ref_1x.png`, `hi_S.png` (indexed, via `Export_png8`), `diff.png` on failure, and a one-line summary. Exit code 0/1.

**Regions** (picked in WP-07 from the superchunk renders; BG static mode):
* open grass;
* coast with water sparkles (cycling indices);
* dense forest (large RLE plus translucency);
* Britain at lift 16/10/5;
* mountains;
* superchunk border;
* world wrap (tx near 0 and 3071).

Dynamic mode (pinned saves, NPCs, effects) is M2.

### 8.3 Golden and integration scripts (`tests/game/`, skipped with exit 77 when `U7_BG_STATIC` is unset)

* `buildmap_golden.sh`:
  * runs `--buildmap 0|1|2` (432 PNGs, about 1 min) with `render_scale=6` in the config, which proves the S=1 forcing;
  * compares SHA-256 against the manifest produced by the **reference build** (upstream `8b6ab6b43` + P3, `build-upstream/`);
  * any difference fails.
* `render_regions.sh`: the region table × S ∈ {2,3,6} × {nn, identity, marker}, plus one `present=1` run with `SDL_RENDER_DRIVER=software`.
* `tools/hires/mkpack_identity.py --marker`: generates the identity and marker packs from `art_original/shapes/flat` into a scratch `<HIRES>`.
* All runs use `SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy`, with `DISPLAY`/`WAYLAND_DISPLAY` unset, and a `test.cfg` derived from `run/exult.cfg`. That config must also set `patch`, `savegame_path`, `gamedat_path` and `hires` to scratch dirs: the current `run/exult.cfg` sets no `patch`, so `game.cc:521` would create `<game_path>/patch` in the user's install (build.md §6.3).
* Game data and goldens never enter the repo (Origin copyright). Expected hashes live in `/home/simonea/ultima7_exult/tests-data/`.

### 8.4 Sanitisers and manual acceptance

* `build-asan` (`-O1 -g -fsanitize=address,undefined`): `make check` plus `render_regions.sh`, then one interactive session at S=6.
* **Manual checklist** (WSLg and Windows):
  * walk and scroll with lerp on and off;
  * open, drag and close gumps; cheat screen (ImageBufferPaintable);
  * rain, clouds, earthquake; a combat round;
  * save (thumbnail correct), load;
  * change scale in VideoOptions and toggle fullscreen;
  * terrain editor in map-edit mode;
  * the night palette and fades.

---

## 9. Build and CI

**Linux (here, user space):**
* `source /home/simonea/ultima7_exult/deps/env.sh`. The autotools route already works and is configured in `build-linux`.
* Add three more out-of-tree dirs next to it, all with `../exult-hires/configure --prefix=… --disable-exult-studio --disable-gimp-plugin --disable-aseprite-plugin --disable-shp-thumbnailer`, plus:
  * `build-o2`: `--with-optimization=normal --with-debug=symbols`. Used for performance and the game tests;
  * `build-asan`: `CXXFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined"`;
  * `build-upstream`: a `git worktree` of `8b6ab6b43` plus the P3 commit only. Reference binary for goldens.
* After editing `Makefile.am` or `configure.ac`, rerun `autoreconf -vi` in the repo; the generated files stay untracked. Then `make -j16 && make check`.
* `tools/hires/ci.sh`:
  1. build `build-o2` and `build-asan`;
  2. `make check` in both;
  3. if BG data is present: `buildmap_golden.sh`, `render_regions.sh`, the `bench` regions with a regression threshold;
  4. Python tool tests (`pytest tools/hires/tests`, via the existing `tools-venv`).

  It writes `tmp/ci-<date>.log`. Run it before every commit series.

**Fork CI on GitHub** (optional, if the fork is pushed): append `make check` after the build step of `ci-linux.yml`. That job builds SDL 3.2.14, which proves the no-3.4-API rule. `ci-msvc.yml` does not run in forks, so the vcxproj changes are verified by the Windows MSYS2 build only.

**Windows** (build.md §5 W1, no admin):
1. Install MSYS2 to `E:\Dati\Ultima7_Upscale\msys64`. Run `pacman -S` for `mingw-w64-ucrt-x86_64-{toolchain,sdl3,libpng,zlib,libogg,libvorbis,fluidsynth,libtimidity,munt-mt32emu,ntldd}` plus `base-devel git`.
2. Clone the WSL repo to `E:\Dati\Ultima7_Upscale\src\exult-hires`, because MSYS2 cannot build from UNC paths.
3. Run `make -f Makefile.mingw -j16 Exult.exe hires_unit.exe`, then `make -f Makefile.mingw install U7PATH=E:/Dati/Ultima7_Upscale/ExultHires`. **Never** install over the 1.12.1 folder: it is SDL2/msvcrt and its DLLs clash.
4. Run with `-c E:\Dati\Ultima7_Upscale\exult-hires.cfg`, pointing at the BG install and the pack under `E:\Dati\Ultima7_Upscale\packs\bg`.
5. Renderer matrix through `SDL_RENDER_DRIVER=direct3d11|direct3d12|vulkan|opengl`. For each renderer:
   * the windowed profile (NEAREST 1:1);
   * the fullscreen ACF preset (LINEAR);
   * a forced small window (r ≈ 0.6);
   * read back the perf overlay numbers.
6. Registering new sources in `Makefile.common` (and `pngio.o` in `SHAPES_OBJS`, `Makefile.common:231-258`) is part of WP-12. The iOS project is updated for upstream hygiene; the code compiles out there through `HAVE_PNG_H`.

---

## 10. Art production plan: 6x BG terrain tiles

This follows `upscale-research/00_recommendation.md`. The engine side is deliberately agnostic: any route that emits valid `SSSS_FF.png` 48x48 raw-index tiles plugs in.

| Stage | Where | Output | Gate |
|---|---|---|---|
| A0. Fix inputs | WSL | `u7art.py`: `render_chunk` emulates `paint_tile` **as the engine does today** (P1/P2 quirks included, via the same rules as `find_flat_source`; a Python test runs the same pinned cases); fix `V2_CHUNK_HDR` (10-byte `FF FF FF FF "exlt" 00 00`); world tile grid; context windows (chunk + 16–32 px apron from the most frequent real neighbours); macro-texture sheets for the 77 8x4 shapes; per-frame cycling-mask sidecar | context renders have **no** black holes under RLE (gap_1 §6) |
| A1. **Route 3 v0** (deterministic) | WSL CPU (`tmp/algo`: xBRZ 1.9, `u7algo.local_snap`) | all **3,885** BG flats: xBRZ 6x on a uniquified palette, local snap to indices, per-(shape,frame) mode consensus, cycling indices restored from the source mask with a label-safe scaler, no 0xFF | A1–A3 hard; B1 ≥ 97 %; C1 ≤ 1.2× region; D1 flags reviewed |
| A2. Pack and review | WSL + engine | `tools/hires/mkpack.py` (group by shape) → `packs/bg-r3/x6/flats/`; `hirescheck.py` (format, palette, index classes vs source, completeness warnings, source match against the game's effective `shapes.vga`); in-engine review with the `overrides` toggle and `--render-test mode=plain` contact sheets of the §8.2 regions | the M1b acceptance pack |
| B. **Route 2 v1** | WSL CUDA (torch cu130 sees the 5070 Ti; `tmp/sr_bakeoff` models) | 4x-NXbrz on context windows, Lanczos 1.5x, the same snap and consensus; chosen per material family against v0 | same gates; side-by-side sheets |
| C. **Route 1 v2** (curated) | Windows ComfyUI (SDXL + xinsir Tile on an xBRZ base, 12x, box 2:1, back-projection, quantise) in `E:\Dati\Ultima7_Upscale` | highest-usage families first, from `manifest.json` `map_uses`: grass (19, 147–149), water and shore (incl. 10), dirt/sand, roads, town floors. Then transitions | human review at 6x in-engine plus F1 contact sheets; seam C2 on map pairs |

Notes:
* **Edge contract.** The map's "outer band = D(own 1x border)" rule is an *art-side* post-process option (B = 3, D = NN). It is A/B-tested on grass, water and roads in stage A2. The engine does not care.
* **Per-terrain art** (768² chunk renders) is not produced for M1. If seam QA (C1/C2) shows unacceptable joints for some families after route 2/1, per-terrain overrides become an M1+ engine task (precedence slot already designed in `paint_flats`). Their key would be the FNV-1a-64 of the zero-filled 1x render. Because 1x stays identical to upstream, the Python emulation has a fixed target.
* **Licensing.** Packs derive from Origin art, so they stay local (outside the repo). NXbrz is CC-BY-NC-SA, which is acceptable for personal packs; do not redistribute the model. xBRZ (GPLv3) runs offline only and is never linked into Exult.
* **SI** later: the same pipeline, its own palette, `<SI game_path>/hires`.

---

## 11. Milestones and work packages

**M0: baseline.** Upstream goldens recorded; test infrastructure green; no behaviour change apart from P3.

**M1a: scaled world (NN), visible early.** Acceptance:
1. `render_scale=6` on the windowed profile looks pixel-identical to today's `point` x6. The present test is exact; play is checked manually.
2. `make check` passes; O1 holds for S ∈ {2,3,6}; the S=1 digest is unchanged.
3. `buildmap_golden.sh` is byte-identical to upstream+P3.
4. O2, O6 and O7 pass on all regions.
5. The ASan session is clean.
6. At -O2, full world paint at 320x200 S=6 is ≤ 2 ms p95 (bench).

**M1b: terrain overrides.** Acceptance:
1. O4a and O4b pass on all regions for S ∈ {2,3,6}, using x2/x3/x6 identity packs.
2. Single-tile, group, disabled-group and `<PATCH>` precedence work (unit and manual).
3. The route-3 BG pack loads in under 0.5 s, and play at S=6 shows it.
4. `overrides=no` returns exactly the M1a output.

**M1c: Windows, performance, first art.** Acceptance:
1. The MSYS2 build runs on D3D11, D3D12 and Vulkan with both user profiles.
2. Present is ≤ 2 ms at 1920x1200 per the perf overlay. The frame with lerp stays within 6.7 ms on the 9700X/5070 Ti.
3. The route-3 pack passes the QA gates. A curated route-2/1 pack exists for at least the top 5 families.

**M2 (sprites) and M3 (UI):** §7. Estimates are indicative.

| ID | Name | Scope | Tests / acceptance | Depends | Days |
|---|---|---|---|---|---|
| WP-01 | Baselines and build matrix | `build-o2`, `build-asan`, `build-upstream` (+P3 worktree); record `--buildmap 0/1/2` SHA-256 from the reference; region list draft | reference hashes reproducible over 2 runs | – | 0.5 |
| WP-02 | Test infrastructure | vendor doctest; `tests/Makefile.am`, `configure.ac`, `SUBDIRS`; `make check`; `test_ibuf_golden` digests recorded on **unmodified** ibuf8.cc; `tools/hires/ci.sh` skeleton | `make check` green on upstream code | WP-01 | 1 |
| WP-03 | P3 + terrain refactor | `fill8(0)`; `find_flat_source` + `paint_flats` + pinning tests; `write_minimap` local 1x buffer; queue-size formula | `test_flat_source`; buildmap golden == upstream+P3 | WP-02 | 1 |
| WP-04 | Scaled `Image_buffer8` | `pixel_scale`; ctors; `blit`, `put_phys`; 17 scaled branches; `create_another` | `test_ibuf_scaled` (O1, 3 scales × seeds); golden digest unchanged; ASan clean | WP-02 | 3.5 |
| WP-05 | Window and present | `world_scale.h` + tests; `create_world_scaled_surfaces`; `create_surface` wiring; guard-band early-outs; `show_world_scaled`; UpdateRect guard; `mini_screenshot`; config key; fail-soft | `test_world_scale`; manual run at S=6 under WSLg | WP-04 | 2.5 |
| WP-06 | World integration | `get_rendered_flats(scale)`; `paint_chunk_flats` → `blit`; BuildGameMap forces S=1 | play at S=6 (NN) | WP-03, WP-05 | 0.5 |
| WP-07 | `--render-test` harness | CLI; static A/B (O2, O6, O7); present read-back; bench; region table; `buildmap_golden.sh`, `render_regions.sh` | **M1a gate** | WP-06 | 2 |
| WP-08 | Hi-res flat store | `hires_flats.{h,cc}`; `<HIRES>` tag (modmgr); config; invalidation hooks; build registration (autotools) | `test_hires_flats` | WP-02 | 2 |
| WP-09 | Override composition | `Hires::flat` in `paint_flats`; generation checks; `mkpack_identity.py` (identity, marker, any S); O4a/O4b in the harness; dev toggle key (optional) | **M1b gate** (with WP-11 pack) | WP-07, WP-08 | 1.5 |
| WP-10 | Pack tooling | `hirescheck.py`; `mkpack.py`; `u7art.py` fixes (fill emulation sharing the pinned cases, v2 header, context windows) + pytest | validator rejects crafted bad packs; Python fill emulation matches the engine on all used BG terrains (cross-check via `--render-test mode=plain` at S=1, flats-only region) | WP-03 | 1.5 |
| WP-11 | Art v0 (route 3) | full BG flat pack + QA report + contact sheets | QA gates A–D; loads in engine | WP-10 | 2.5 |
| WP-12 | Windows build | MSYS2 setup; `Makefile.common`/vcxproj/filters/xcode registration; `hires_unit.exe`; renderer matrix | M1c items 1–2 | WP-09 | 1.5 |
| WP-13 | Performance pass | -O2 bench; fast paths (run-length `memset` in the scaled RLE path; row-wise translucent); perf scopes per pass (`gamerend.cc:192-291`) | bench thresholds; O1 still green | WP-07 | 1.5 |
| WP-14 | Art v1/v2 (routes 2 and 1) | NXbrz on WSL CUDA; ComfyUI curated families; QA; review loop in engine | M1c item 3 | WP-11 | 8 |
| WP-15 | Upstream PR series | split into: tests infra; P3; `pixel_scale` (no-op at 1, with O1); present path (off by default); chunk cache at scale + render-test; flat store. Separately P1/P2 (updating pinned tests), P5 | each PR builds and passes `make check` alone | WP-12 | 1 |
| WP-16 | Post-M1 options | halving-chain downscale; INDEX8 under `SDL_VERSION_ATLEAST(3,4,0)`; `render_scale=auto`; VideoOptions entry | present tests at r < 0.5; INDEX8 vs ARGB read-back equality | WP-12 | 2 |
| WP-20 | M2 sprites (outline) | P6/P7/P8; side-table store; physical-view painting; reflections; CRC gate; dynamic-mode render-test | O4a/O4b for RLE, translucent and reflected frames | WP-09 | 10 |

M1a ≈ 11 days (WP-01…07). M1b ≈ +7.5 (WP-08…11). M1c ≈ +4 engine days (WP-12, 13, 15) plus art. Total M1 engine work is about 19–20 days.

---

## 12. Risks

| # | Risk | Sev. | Mitigation |
|---|---|---|---|
| 1 | Logical/physical unit mix-ups give out-of-bounds writes (guard band, `copy`, raw readers) | Critical | Guard-band functions are disabled at S>1. Raw readers were audited (§3.7). Scaled branches reuse the logical clip helpers. O1 fuzz, ASan builds, O6 |
| 2 | A texture over the renderer limit, or excessive memory, at large views or Studio zoom | High | `compute_world_scale` caps by max texture size and pixel budget; fail-soft to S=1 instead of a throw |
| 3 | A missed main-buffer reader shows wrong pixels at S>1 | Med | grep audit (`get_bits`, `get_line_width`, `create_buffer`, `->get(`/`->put(`), O7, the manual checklist; UI is untouched by construction |
| 4 | Present cost for big views (5160x1800: about 3.6 ms LUT + 37 MB upload per full frame with lerp) | Med | Recommended presets (573x200 view); bench; INDEX8 option (WP-16) |
| 5 | Per-tile art seams | High (visual) | Context consensus + QA C1/C2; edge-band option; per-terrain overrides kept as a designed escalation, not built |
| 6 | AI art breaks index semantics (cycling, 0xFF, ramps) | High | Engine rejects 0xFF; validator enforces the cycle-range and ramp rules; quantiser per research §2.6 |
| 7 | Stale art when a mod patches flats (no runtime source gate in M1) | Low today (0 mods affect flats, gap_6) | Per-game packs; `<PATCH>` precedence; validator source check; `overrides=no`; CRC gate in M2 |
| 8 | Fork drifts from upstream, or rebase conflicts with upstream's rendering rework | Med | All S>1 logic in new functions with one-line hooks; 1x golden == upstream; small PR series (WP-15) |
| 9 | Translucency or xform semantics at S | Med | Per-physical-pixel application; O1 uses real-shaped xform tables |
| 10 | The P3 delta hides an upstream-visible change | Low | Reference build is upstream+P3; P3 diff is confined to null-shape tiles (verified once in WP-03) |
| 11 | Visual mismatch until M3: UI, cursor and dragged item at 1x; debug lines S px thick | Low | Accepted (non-goal) |
| 12 | First S>1 frame hitch while loading about 4k PNGs (slower from `/mnt/e` in WSL) | Low | One-off about 0.3 s; optional preload at the end of `Shape_manager::load`; companion VGA in M2 if needed |
| 13 | LINEAR palette-texture behaviour differs per backend | Low | ARGB baseline (no palette textures in M1); renderer matrix in WP-12 |
| 14 | Python fill emulation diverges from the engine (bad AI context) | Med | Shared pinned test cases; engine cross-check in WP-10 |

---

## 13. Deviations from the map (D1–D10 and the inventory)

* **D3/D4.** No new `world_texture` member. The existing `screen_texture`/`screen_texture_a` pair is recreated as ARGB8888 at `full·S`, and `UpdateRect`, screenshot and compositing are reused unchanged. Palette changes are detected by comparing `draw_surface`'s palette on each `show()`, instead of hooking `set_palette`/`rotate_colors` (W9 dropped). INDEX8/`SDL_SetTexturePalette` is deferred (Q2 answered "ARGB only in M1"). Reason: a smaller, self-contained diff with fewer GPU resources to manage across resizes, and no SDL 3.4 dependency.
* **D5.** S is not derived from display/full and not snapped to divisors of the art scale. `render_scale` is an explicit request, lowered only by the texture and pixel budget. Reason: the user explicitly prefers rendering high and downscaling at the end, and both user profiles land on S=6 with r ≥ 0.67 anyway. `auto` is an optional later key.
* **D5/W7.** The scale mode is NEAREST or LINEAR only; PIXELART and the halving chain are deferred.
* **D6.** Same principle (NN in the primitives). `paint_rle` is implemented as "decode scan, then scaled `copy_hline8`" for correctness by construction. No physical view is needed in M1; the view constructor exists for tests and M2.
* **D7.**
  * Per-tile overrides only.
  * No per-terrain FNV-1a content-hash overrides and no 64-byte source check in M1.
  * The cache is checked by `(pixel_scale, generation)` per terrain.
  * Queue size is `max(100, working set)`; no byte budget.
  * Reason: per-terrain art costs about 290 MiB for BG, needs P1/P2 and exact Python emulation, and seam quality can first be attacked art-side (consensus, edge band) at a fraction of the cost.
* **D8 (M2).** A side table keyed by `Shape_frame*`, filled at pack load. There is no identity stamp in `Vga_file::get_shape`, no LRU and no slot in `Shape_frame` in the first sprite iteration. Groups are directories, not engine-derived rules; derivation lives in the validator. Reason: avoids the frame-0-sweep problem and keeps the diff local. D8 remains the upgrade path for complete packs.
* **D10.** Same framework and oracles. The harness is static-mode only in M1; dynamic mode moves to M2. O7 is reduced to the `mini_screenshot` invariant, because logic invariance is guaranteed by the logical API and caught by O2.
* **Prerequisites.**
  * P1 and P2 are **not** applied in the fork (1x byte-identical to upstream); they go upstream as separate PRs.
  * P3 is applied.
  * P4 is a no-change refactor.
  * P5 is upstream-only.
  * P6–P8 move to M2. P10 is dropped.
* **Inventory.**
  * H1/H10/H14: a two-root search chain (`<PATCH>/hires`, `<HIRES>`); no `<DATA>/hires/<game>` root.
  * H12: reuse `Import_png8(…, −1, …)` instead of a new PNG wrapper.
  * H11/H13/H15/H16: CRC, rules library, `--dump-art` deferred to M2; `u7art.py` fixes kept (WP-10).
  * W10/W11: no scaler asserts (they would fire inside `scale_layer_color`).
  * W14: `mini_screenshot` samples via `get_pixel8` instead of 3S×3S averaging.
  * G13/G14/G17: no flat overrides in the terrain editor or direct flat paints (NN only).
  * C1: three config keys plus one path key instead of five.
  * C2: VideoOptions deferred.
  * C5 `srand` hook: only inside `--render-test`.
