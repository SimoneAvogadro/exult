# Gap #2: effective render scale vs. the real video configs, Auto game area, ExultStudio zoom and GPU texture limits

Exult master @ 8b6ab6b43 (`/home/simonea/ultima7_exult/exult-hires`), SDL 3.4.18 from `deps/`.
Related analyses: `present.md` (presentation pipeline, §7.3 "Presenting S x full into D") and `world.md` (§6 and §10.3, terrain cache).
This document does not repeat those. It answers one question: **which render scale S should the engine actually use, given the configurations it can really be in?**

---

## 0. TL;DR

1. "320x200 → 1920x1200" holds only for the user's **windowed** profile. The **fullscreen** profile (the one marked active, `<fullscreen>yes`) runs Auto game area + Aspect Correct Fit at 3440x1440. Through `get_draw_dims` that gives a logical view of **860x300** game px, an intermediate surface of 3440x1200 and a presentation ratio of **4.0 x 4.8** display px per game px. A fixed S=6 there means a 5160x1800 world buffer (9.29 Mpx, 4x the 2.3 Mpx planned for 1920x1200) and a non-uniform 0.667 x 0.8 downscale. Confirmed (§2, §3).
2. Auto game area divides the display by `scale` (`imagewin.cc:1320-1321, 1347-1348`). Any path that lowers `scale` enlarges the world view in game px. ExultStudio zoom (`exult.cc:2768-2785`) and the VideoOptions x1..x8 selector (`VideoOptions_gump.cc:329-342`) both do this. With a fixed S=6, the buffer would reach 11520x7200 (Studio x1, 83 Mpx). On a 3440x1440 Auto/Fill x1 setup it would reach 20640x8640, which **exceeds the 16384 D3D11/D3D12/Vulkan texture limit**. That makes `create_scale_surfaces` fail and throws a fatal `exult_exception` (`imagewin.cc:555-561, 695-697`, caught at `exult.cc:471`).
3. Nothing queries `SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER`. There is also an **ordering problem**. `free_surface()` destroys the SDL renderer on every resize (`imagewin.cc:831-834`), and `get_draw_dims` runs before the new renderer exists (`imagewin.cc:552` vs `626`). The limit can only be known inside `create_scale_surfaces`, after `SDL_CreateRenderer`, or from a cached value.
4. The chunk-flat cache is capped at a hard-coded **100** entries (`chunkter.cc:234-242`). A full repaint touches up to 30 chunks at 320x200, 50 at 860x300, 128 at 1720x600, 216 at 1920x1200 and 420 at 3440x1440 (simulated with the exact `paint_map` formulas, `gamerend.cc:206-219`). Above 100 the MRU queue is accessed cyclically, so **every chunk misses on every full repaint** (LRU worst case). There is no "flush all" API. `free_rendered_flats()` is called only by the eviction path (`chunkter.cc:253`), and the destructor deletes directly (`chunkter.cc:184`). So any runtime change of S (VideoOptions, fullscreen toggle, Studio zoom, all funnelled through `Game_window::resized`, `gamewin.cc:916-937`) would blit stale 128x128 buffers. Shape reloads from ExultStudio (`shapeid.cc:420-468`) do not invalidate the flats either. That staleness already exists today.
5. **Proposed rule.** Derive S from the *presentation ratio* instead of fixing it: `p = max(display_w / full_w, display_h / full_h)`, `S_need = ceil(p)`, `S_eff = clamp(snap(S_need), 1, S_tex)`. Here `snap()` rounds up to a divisor of the art scale (1, 2, 3 or 6 for 6x art) so indexed override art only ever needs integer box/mode downsampling. With this rule the world buffer always stays close to the display size (at most 1.5x per axis), and S=6 is reached exactly where it pays off. §6 gives the worked numbers for every config.
6. For the user's fullscreen, the cleanest setups are `scale=6` + Auto + ACF (a 573x200 view, 3438x1200 buffer, only the 1.2 vertical stretch, 4.13 Mpx) or `scale=5` + Auto + ACF (a 688x240 view, 4128x1440 buffer, horizontal 0.833 downscale only). Keeping the current 860x300 view costs 9.29 Mpx per full repaint at S=6.

---

## 1. How the current code derives every size

### 1.1 Inputs: config → `setup_video` → `Game_window` → `Image_window`

* `setup_video()` (`exult.cc:3128-3304`) reads `scale`, `scale_method`, `display/{width,height}`, `game/{width,height}` (0 means Auto), `fill_mode` and `fill_scaler` from `config/video` or from `config/video/window`. The choice depends on `fullscreen || share_video_settings` (`exult.cc:3149-3151`). The user has `share_video_settings=no`, so windowed mode reads `<window>`.
* The scale is **forced** by the scaler: Hq3x/3xBR give 3, Hq4x/4xBR give 4, and every scaler other than point, SDLScaler, interlaced and bilinear gives 2 (`exult.cc:3203-3211`). The VideoOptions gump mirrors this (`VideoOptions_gump.cc:329-349`). Only point, interlaced, bilinear and SDLScaler offer x1..x8.
* On startup, `VIDEO_INIT` constructs `Game_window` (`exult.cc:869`, `3267`), which builds `Image_window8` (`gamewin.cc:326`) and then calls `create_surface()`. Every later change goes through `Game_window::resized()` (`gamewin.cc:916-937`) and then `Image_window::resized()` (`imagewin.cc:858-876`). The "nothing changed" early-out there is commented out (`:862-865`), so **every call frees and rebuilds all surfaces, both screen textures and the SDL renderer** (`free_surface`, `imagewin.cc:811-842`, renderer destroyed at `:831-834`).
* The `highdpi` key in the user's cfg is a leftover from 1.12.1: 1.12.1 reads it (`exult-1.12.1/imagewin/imagewin.cc:642-644`), master does not. Master always passes `SDL_WINDOW_HIGH_PIXEL_DENSITY` (`imagewin.cc:587`). The VideoOptions enum still has an unused `id_high_dpi` (`VideoOptions_gump.h:69`). On Windows, SDL3 declares per-monitor-v2 DPI awareness by default (`SDL_windowsvideo.c:578-591`), so window sizes are physical pixels. `Image_window::nativescale` is computed (`imagewin.cc:659`) and never read.

### 1.2 `get_draw_dims` (`imagewin.cc:1315-1436`)

Inputs: `sw, sh` (configured display), `scale`, `fill_mode`, `gw, gh` (0 means Auto). Outputs: the logical game area `gw x gh` and the intermediate ("inter") size `iw x ih`.

* **Fill** (`:1318-1326`): Auto gives `gw=sw/scale, gh=sh/scale`. Then `iw=gw*scale, ih=gh*scale`.
* **Fit** (`:1327-1344`): Auto as for Fill. Then the height or the width decides and the other axis gets bands (`iw=sw*ih/sh`, or the reverse).
* **AspectCorrectFit** (`:1345-1370`): Auto gives `gw=sw/scale, gh=sh*5/(scale*6)`. When the height decides and `gh` is the Auto value, `ih=sh*5/6` and `iw=sw*ih*6/(sh*5)`.
* **Centre and its variants** (`:1371-1398`) and **explicit WxH** (`:1399-1425`).
* Rounding fix (`:1427-1433`): if `iw == (sw/scale)*scale`, then `iw=sw` (the same applies to `ih`).

`create_surface()` (`imagewin.cc:540-578`) calls it **with the configured display size**, before any window or renderer exists (`:552`). It then sizes the 8-bit `draw_surface` as `inter/scale + 2*guard_band` (`:726-728`). So `ibuf->width = inter_w/scale` is the "full" width in game px, which includes any bands. The game area sits centred in it (`offset_x/y`, `:573-575`).

### 1.3 Window, renderer, display size, textures (`create_scale_surfaces`, `imagewin.cc:585-770`)

* In fullscreen, `SDL_SetWindowFullscreen(win, true)` without a display mode gives **borderless desktop fullscreen**. The `SDL_SetWindowFullscreenMode` code is under `#if 0`, with the comment "This does not appear to have any effect" (`:595-621`; SDL semantics in `SDL_video.h:1020-1041`). Then `w,h` are **overwritten with the render output size** (`:637-655`). As a result `display_width/height` (`:672-673`) is the desktop size, which can differ from the `sw,sh` that `get_draw_dims` used.
* In windowed mode, `display_width/height` is the requested window size in window coordinates. No output-size query is made (`:663-671`).
* `SDL_SetRenderLogicalPresentation(w,h,LETTERBOX)` (`:662`, `:670`) defines the coordinate space of the final draw and of the mouse events (`SDL_ConvertEventToRenderCoordinates` at `exult.cc:1793` and elsewhere).
* `screen_texture` (STREAMING) and `screen_texture_a` (TARGET) are both sized `max(inter_w, w) + 2*gb*scale` by `max(inter_h, h) + 2*gb*scale` (`:690-705`).
* Which intermediate surface is used: if `scaler == fill_scaler`, or `scale == 1`, or (SDLScaler with point or bilinear), then `inter_surface = draw_surface` and the GPU does all the scaling. Else if `inter != display`, a separate 32-bit `inter_surface` is created. Else `inter_surface = nullptr` and the scaler writes straight into the locked texture (`:740-760`).

### 1.4 Per-frame path (`show`, `imagewin.cc:882-1114`; `UpdateRect`, `:2170-2220`)

* `SDL_LockTextureToSurface(screen_texture, nullptr, …)` locks the **whole** texture (`:957`). On D3D11 this creates a staging texture of the locked size on every lock and copies all of it on unlock (`SDL_render_d3d11.c:1754-1806, 1859`). So each `show()` costs a full-texture upload, whatever the size of the dirty rect.
* Phase 1 (`:968-1015`) runs the scaler, `draw_surface` → `inter_surface`, on the dirty rect only.
* Phase 2 (`:1018-1049`) runs the fill scaler on the **entire** inter → display surface. The destination is `texture - 2*gb*scale` (`:1034-1035`). When inter is larger than the display, the texture is inter-sized, so phase 2 is a 1:1 copy and the **GPU does the downscale** in `UpdateRect` (`:2199`: `screen_texture_a` with `fullRect` → whole logical display). The scale mode is LINEAR by default. The pipeline therefore already supports "render bigger, downscale at present". `get_draw_dims` simply never produces that case for Auto game areas.
* Mouse mapping, `screen_to_game` (`:1291-1292`): `gx = sx*inter_w/(scale*display_w) + start_x`, which equals `sx*full_w/display_w + start_x`. The result depends only on `full_w = inter_w/scale` and the display size, **not on how many pixels the world buffer has**. `game_to_screen` is the inverse (`:1310-1311`). Any S design that keeps `ibuf` in game px and leaves `inter_w/scale` unchanged keeps hit-testing correct for free.

---

## 2. The user's real configurations, run through the code

Source: `C:/Users/Simone/AppData/Local/Exult/exult.cfg` `<video>` (via `/mnt/c/...`). The installed Windows binary is **Exult 1.12.1 (SDL2)**, per `stdout.txt` ("Exult version 1.12.1"). Master reads the same keys except `highdpi`. The fullscreen mode list in `stdout.txt` tops out at 3440x1440, which is the desktop.

| | Windowed profile (`<window>`) | Fullscreen profile (top level, **active**: `<fullscreen>yes`) |
|---|---|---|
| display | 1920x1200 | 3440x1440 (= desktop, so the render output size is the same) |
| game | 320x200 (explicit) | 0x0 (**Auto**) |
| scale / scaler | 4 / Interlaced | 4 / 4xBR (scale forced to 4, `exult.cc:3205-3206`) |
| fill mode / fill scaler | Fit / Point | Aspect Correct Fit / Bilinear |
| `get_draw_dims` → game | 320x200 | **860x300** (`3440/4`, `1440*5/24`) |
| → inter | 1280x800 (Fit: `ih=200*4`, `iw=1920*800/1200`) | **3440x1200** (`ih=1440*5/6`, `iw=3440`) |
| full (`draw_surface` w/o guard) | 320x200 | 860x300 |
| presentation ratio px, py | **6.0, 6.0** | **4.0, 4.8** |
| surfaces | separate `inter_surface` (Interlaced ≠ Point, inter ≠ display) | separate `inter_surface` (4xBR ≠ Bilinear, 1200 ≠ 1440) |
| phase 1 (CPU) | Interlaced 4x → 1280x800 (1.02 Mpx) | 4xBR → 3440x1200 (4.13 Mpx) |
| phase 2 (CPU) | Point arb 1.5x → 1920x1200 (2.30 Mpx) | Bilinear `X1Y12` fast path (`BilinearScaler.cpp:75-76`) → 3440x1440 (4.95 Mpx) |
| `screen_texture` (ARGB) | 1952x1232 = 9.6 MB, uploaded on every `show()` | 3472x1472 = 20.4 MB, uploaded on every `show()` |
| full-repaint chunk window | 6x5 = 30 chunks | 10x5 = 50 chunks |

Notes:
* The windowed profile uses `Interlaced`, so ExultStudio zoom (`set_scaleval`) **does nothing** there. It requires `scaler == point` (`exult.cc:2771`).
* Compared with the original 320x200, the 860x300 view is 2.69x wider, 1.5x taller and 4.03x larger in area. That is a gameplay choice the user has made, and a render-scale policy must not silently change it.

---

## 3. What a fixed S=6 would mean in each reachable configuration

Simulated with a Python re-implementation of `get_draw_dims` and the `paint_map` chunk-range formulas (scratch script, not in the repo). "full" is `inter/scale`, i.e. what the world buffer must cover in game px.

| Config (display, scale, mode, game) | full (game px) | px, py | buffer @ fixed S=6 | Mpx | fits 16384? |
|---|---|---|---|---|---|
| Windowed user (1920x1200, 4, Fit, 320x200) | 320x200 | 6.00, 6.00 | 1920x1200 | 2.30 | yes |
| Fullscreen user (3440x1440, 4, ACF, Auto) | 860x300 | 4.00, 4.80 | **5160x1800** | **9.29** | yes |
| Fullscreen, scale 6, ACF, Auto | 573x200 | 6.00, 7.20 | 3438x1200 | 4.13 | yes |
| Fullscreen, scale 5, ACF, Auto | 688x240 | 5.00, 6.00 | 4128x1440 | 5.94 | yes |
| Fullscreen, scale 2, ACF, Auto (selectable in VideoOptions with point) | 1720x600 | 2.00, 2.40 | 10320x3600 | 37.2 | yes, but 297 MB for 2 ARGB textures |
| Fullscreen, scale 1, Fill, Auto | 3440x1440 | 1, 1 | **20640x8640** | 178 | **no, fatal** |
| Windowed 320x200 with scale 8 (inter > display) | 320x200 | 6, 6 | 1920x1200 | 2.30 | yes |
| Studio zoom k=1 (1920x1200, Fill, Auto) | 1920x1200 | 1, 1 | **11520x7200** | **82.9** | yes, but 332 MB per ARGB texture |
| Studio zoom k=2 | 960x600 | 2, 2 | 5760x3600 | 20.7 | yes |
| Studio zoom k=3 | 640x400 | 3, 3 | 3840x2400 | 9.2 | yes |
| Studio zoom k=4 / 5 / 6 | 480x300 / 384x240 / 320x200 | 4 / 5 / 6 | 2880x1800 / 2304x1440 / 1920x1200 | 5.2 / 3.3 / 2.3 | yes |
| Studio zoom k=7 / 8 | 274x171 / 240x150 | 7 / 8 | 1644x1026 / 1440x900 (then upscaled) | 1.7 / 1.3 | yes |

ExultStudio zoom details (`exult.cc:2744-2785`, keys in `keyactions.cc:228-238`):
* It is active only with cheats on, in map-editor mode, windowed, and with the point scaler.
* It cycles `current_scaleval` 1..8. The value starts at 1 (`exult.cc:163`) and is never initialised from the configured scale.
* It calls `gwin->resized(resx, resy, fullscreen, 0, 0, k, scaler, Fill, point)` with the **configured** display size of the active profile (`:2775-2783`).
* So the world view in game px is display/k. At k=1 that is the whole 1920x1200 window in game px, 216 chunks.
* The scale is sent to ExultStudio in `Send_location` (`gamewin.cc:1038`), but Studio ignores it (`mapedit/locator.cc:320`, "++++Scale? Later."). The protocol needs no change.

Conclusions:
* With a fixed S, buffer size grows as `S² x (display/scale)²`, so **zooming out explodes quadratically**.
* With S tied to the presentation ratio, the buffer is always about display-sized (§6).
* A fixed S=6 on the user's fullscreen gives a 9.29 Mpx render, 37 MB of ARGB upload per `show()` with the current lock-whole-texture path (or 9.3 MB with INDEX8), and a LINEAR downscale of 0.667 x 0.8. That ratio is ≥ 0.5, so a single LINEAR pass is acceptable per `present.md` §7.3, but it is wasted work: the display only resolves 4 x 4.8.

---

## 4. GPU texture limits

* No call to `SDL_GetRendererProperties` or `SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER` exists in the tree (grep over `*.cc/*.h/*.cpp`).
* SDL 3.4.18 values:
  * D3D11 16384 (`SDL_render_d3d11.c:718`). This is the **default on Windows**: the driver order is D3D11, D3D12, D3D9, … (`SDL_render.c:110-118`).
  * D3D12 16384 (`SDL_render_d3d12.c:3560`).
  * Vulkan 16384 (`SDL_render_vulkan.c:4618`).
  * SDL_GPU 16384 (`SDL_render_gpu.c:1832`).
  * OpenGL: `GL_MAX_TEXTURE_SIZE` (`SDL_render_gl.c:1910-1919`).
  * D3D9: `min(MaxTextureWidth, MaxTextureHeight)` (`SDL_render_d3d.c:1956`).
  * Software renderer: the property is not set (0), so there is no check (`SDL_render.c:1522-1524`).
* Failure path today: `SDL_CreateTexture` fails → `create_scale_surfaces` returns false (`imagewin.cc:695-697`, `707-709`) → `try_scaler` false → retry with point → false → `throw exult_exception("Failed to creat Display Sorfaces")` (`imagewin.cc:555-561`). That ends the game via `exult.cc:471` (emergency save, exit). When it happens during a VideoOptions apply or a Studio zoom it is just as fatal. Layer textures fail softly: the layer is skipped (`imagewin.cc:2139-2144`).
* **Ordering problem.** S_eff has to be known before `draw_surface`, the hi-res buffer and `screen_texture` are sized (`:690-737`). The renderer, which is the only source of the limit, is created at `:625-626`, and `free_surface()` has just destroyed the previous one. Options:
  * (a) Compute S_eff inside `create_scale_surfaces`, right after `SDL_CreateRenderer` and the fullscreen output-size query (`:626-673`), and before the texture and surface creation. This is also the only place where the real `display_width/height` is known.
  * (b) Keep a static `last_max_texture_size`, defaulting to 16384, refreshed whenever a renderer is created.
  * (c) Stop destroying the renderer on resize (bigger change; layer textures would survive).

  (a) combined with (b) is the minimum.
* Even below the limit, texture size matters for frame time. With the current full-texture lock on every `show()` (`imagewin.cc:957`), the per-show upload is proportional to the texture area. That is another reason to keep the uploaded world texture close to display size (or to use `SDL_UpdateTexture` on dirty rects, `present.md` §7.2).

---

## 5. Chunk-flat cache vs. view size, and the missing invalidation

### 5.1 Capacity vs. working set

* `paint_map` (`gamerend.cc:206-219`) visits chunks from one tile left/above the view to `2 + (scroll + (w+6)/8 + 8)/16`, i.e. 2 chunks plus 8 tiles past the right and bottom edge (the comment mentions smooth scrolling). `paint_chunk_flats` calls `get_rendered_flats()` for **every** chunk in that window (`gamerend.cc:227-233`, `520-531`).
* Maximum chunks per full repaint, over all 16 scroll residues:

| Game area (game px) | chunks | flats bytes @S=1 | @S=6 (768x768 = 576 KiB each) |
|---|---|---|---|
| 320x200 | 6x5 = 30 | 0.5 MB | 16.9 MiB |
| 573x200 | 8x5 = 40 | 0.6 MB | 22.5 MiB |
| 860x300 (user fullscreen) | 10x5 = 50 | 0.8 MB | 28.1 MiB |
| 640x400 (Studio k3) | 8x6 = 48 | 0.8 MB | 27.0 MiB |
| 960x600 (Studio k2) | 11x8 = 88 | 1.4 MB | 49.5 MiB |
| 1720x600 (FS ACF x2) | 16x8 = **128** | 2.0 MB | 72.0 MiB |
| 1920x1200 (Studio k1) | 18x12 = **216** | 3.4 MB | 121.5 MiB |
| 3440x1440 (FS Fill x1) | 30x14 = **420** | 6.6 MB | 236 MiB |

* `Figure_queue_size()` returns a constant **100** (`chunkter.cc:234-242`). The commented-out formula `(ceil(w/128)+3)*(ceil(h/128)+3)` is a correct upper bound for every row above: 30, 40, 60, 56, 88, 136, 234, 450.
* Eviction (`chunkter.cc:248-259`) removes one tail entry per new allocation while `queue_size > 100`. `get_rendered_flats()` first moves the chunk to the head (`chunkter.h:95-101`). Rows marked bold exceed 100. A full repaint walks the chunks in the same row-major order every frame, which is the cyclic-access worst case for LRU: **every access misses**, and all 128/216/420 chunk flats are re-rendered on every full repaint. Full repaints happen every frame while lerped smooth scrolling is active (`world.md` §5, `present.md` §7.5).
  * At S=1 that is about 3 to 7 MB of memcpy per frame: tolerable, and probably unnoticed today.
  * At S=6 it would be 121 to 236 MiB per frame.
  * With S tied to the presentation ratio (§6), large game areas automatically get small S (Studio k1 → S=1, FS x2 → S=3 or 2). The cost stays bounded, but the thrash itself remains a bug. Fix: compute the queue size from the current game area, plus a byte budget (`entries <= budget / (16384*S²)`, never below the working set).

### 5.2 No flush on S change or on art change

* `free_rendered_flats()` (`chunkter.cc:274-277`) has exactly one caller, the eviction path (`:253`). `~Chunk_terrain` deletes the buffer directly (`:184`). Nothing walks `render_queue` (static circular list, `chunkter.cc:34-35`, `chunkter.h:46-47`) or `Game_map::chunk_terrains` (static vector, `gamemap.cc:79`) to drop the caches.
* `rendered_flats` is allocated as `Image_buffer8(c_chunksize, c_chunksize)` (`chunkter.cc:259`) and blitted with a hard-coded `copy8(bits, 128, 128, …)` (`gamerend.cc:529`). If the S-aware version allocates `128*S` buffers, a runtime S change leaves wrong-size buffers in up to `queue_size` terrains. They would be used until evicted, which means wrong scale or an out-of-bounds read if the blit trusts the new S.
* Events that can change S at runtime. All go through `Game_window::resized` (`gamewin.cc:916-937`):
  * VideoOptions apply and revert (`VideoOptions_gump.cc:565-569`, `585-591`);
  * the fullscreen toggle (`keyactions.cc:256`, `266`, then `setup_video(TOGGLE_FULLSCREEN)`, then `exult.cc:3281`);
  * the VideoOptions fullscreen revert (`VideoOptions_gump.cc:585`);
  * Studio zoom (`exult.cc:2783`).

  The only resize path that bypasses it is `Image_window::toggle_fullscreen()` (`imagewin.cc:1119-1140`). It has no callers and also contains a bug: `w = display_height; h = display_height;` (`:1123-1124`).
* Art-change events that also must invalidate: ExultStudio `reload_shapes` (`shapeid.cc:420-468`, which clears only the `shape_cache` pointer map, `:423`), a future override-pack reload, and Studio terrain edits (already handled by `commit_edits` → `render_flats`, `chunkter.cc:208-216`).
* `Game_map::write_minimap()` (`gamemap.cc:1690-1723`) calls `get_rendered_flats()` for **every** terrain (about 3,000) and averages `get_width()*get_height()` pixels, assuming `line_width == width`. At S=6 that is 36x more work and churns the whole queue. It should use 1x data.

### 5.3 Recommended invalidation mechanism

A global **render generation** counter would be cheap and robust. It is bumped whenever S_eff changes (in `Game_window::resized` after `win->resized`, `gamewin.cc:919`, before `paint()` at `:926`) and whenever override art or shapes are reloaded. Each `Chunk_terrain` stores the generation, and S, its `rendered_flats` was built for. `get_rendered_flats()` (`chunkter.h:95-101`) re-renders or reallocates on mismatch. This self-heals lazily, needs no list walk, and also fixes the existing Studio `reload_shapes` staleness.

An explicit `static void Chunk_terrain::flush_all()` that walks `render_queue` and frees all entries (`queue_size = 0`) is the alternative. It is needed anyway if memory must be returned at once, for example when going from S=6 to S=1.

---

## 6. Proposed rule for the effective render scale

### 6.1 Definitions (all available at the end of `create_scale_surfaces`)

* `full_w, full_h` = `inter_w/scale, inter_h/scale` = `ibuf->width/height`: what the world buffer must cover in game px, including the bands.
* `D_w, D_h` = `display_width/height`. Better: the physical render output size (`SDL_GetCurrentRenderOutputSize`), which today is queried only in fullscreen (`imagewin.cc:641`). Using it makes HiDPI windowed setups (macOS) resolve correctly.
* `px = D_w/full_w`, `py = D_h/full_h`: display pixels per game pixel. Fit can make these differ per axis through bands; ACF makes `py ≈ 1.2*px`.
* `S_art`: the scale the override art is authored at (6).
* `S_cfg`: a user cap (`config/video/hires/render_scale`, "auto" or 1..S_art).
* `S_tex = floor(maxtex / (max(full_w, full_h) + 2*gb))`. With S applied to the guard band too: `(full + 2*gb)*S ≤ maxtex`.
* `S_mem`: an optional byte budget for world buffer + chunk caches.

### 6.2 Rule

```
p       = max(px, py)                 // or max(px, py/1.2) for AspectCorrect* modes, see 6.4
S_need  = max(1, ceil(p - 1e-6))
S_snap  = smallest d in divisors(S_art) with d >= S_need, else S_art      // {1,2,3,6} for 6x art
S_eff   = min(S_snap, S_cfg, S_tex, S_mem)  (if a cap breaks divisibility, step down to the next divisor)
S_eff == 1  =>  hi-res path off: today's pipeline, scalers as configured.
```

Why snap to divisors of S_art: overrides are meant to stay palette-indexed (cycling 0xE0-0xFF, Xform translucency). Indexed art can only be downsampled by integer factors without inventing colours (box mode/majority, or "pick the top-left subpixel"). 6 to 5 or 6 to 4 needs fractional resampling, which requires RGB filtering and re-quantisation and breaks the palette guarantees.

With snapping, the downscale ratio is always within [0.5, 1] when `S_need ≤ S_art`, so a single LINEAR pass suffices (`present.md` §7.3). Upscaling (> 1) happens only when the display out-resolves the art (`S_need > S_art`), for example Studio zoom k=7/8 or the ACF vertical axis.

### 6.3 Worked results

| Config | px, py | S_need | S_eff (snap) | world buffer | Mpx | final present |
|---|---|---|---|---|---|---|
| Windowed user (320x200 Fit, any scale) | 6, 6 | 6 | **6** | 1920x1200 | 2.30 | 1:1 |
| Fullscreen user (860x300) | 4, 4.8 | 5 | 6 (or 5 without snap) | 5160x1800 (4300x1500) | 9.29 (6.45) | 0.667 x 0.8 (0.8 x 0.96) |
| FS, scale 6, ACF, Auto (573x200) | 6, 7.2 | 8 | 6 (cap) | 3438x1200 | 4.13 | 1.0 x 1.2 (Bilinear `X1Y12` or GPU LINEAR) |
| FS, scale 5, ACF, Auto (688x240) | 5, 6 | 6 | 6 | 4128x1440 | 5.94 | 0.833 x 1.0 |
| FS 320x200 explicit, ACF (scale 4 or 6) | 6, 7.2 | 8 | 6 | 3438x1200 (game 1920x1200 + bands) | 4.13 | 1.0 x 1.2 |
| FS, scale 2, ACF, Auto (1720x600) | 2, 2.4 | 3 | 3 | 5160x1800 | 9.29 | 0.667 x 0.8 |
| FS, scale 1, Fill, Auto (3440x1440) | 1, 1 | 1 | **1 (off)** | 3440x1440 | 4.95 | as today |
| Studio k=1 / 2 / 3 | k | k | 1 / 2 / 3 | 1920x1200 | 2.30 | 1:1 |
| Studio k=4 / 5 | 4 / 5 | 4 / 5 | 6 | 2880x1800 / 2304x1440 | 5.18 / 3.32 | 0.667 / 0.833 |
| Studio k=6 | 6 | 6 | 6 | 1920x1200 | 2.30 | 1:1 |
| Studio k=7 / 8 | 7 / 8 | 8 | 6 (cap) | 1644x1026 / 1440x900 | 1.69 / 1.30 | up 1.17 / 1.33 |

Properties:
* The world buffer is at most 1.5x the display per axis (2.25x the pixels). The worst case is S_need=4 snapped to 6. For a 3440x1440 desktop that is at most 5160x2160, well under 16384, so `S_tex` is a safety net only.
* The chunk-cache working set per repaint (§5.1) stays about constant in bytes: about display area plus margins.
* No user-visible game-area change. `scale` keeps its meaning (Auto divisor and game zoom). Mouse mapping is unchanged (§1.4).

### 6.4 Policy knobs to decide

1. **Snap or not.** Without snapping (S_eff = min(S_need, S_art)), the fullscreen user gets S=5 and 6.45 Mpx, but needs fractional 6→5 resampling of the art at load time. That is easy for RGBA overrides and lossy for indexed ones.
2. **Reference axis for AspectCorrect modes.** Using `py` makes ACF always render 1.2x taller and then downscale (better vertical quality, about 20% more pixels). Using `py/1.2` (the "pixel-aspect-free" axis) renders at the horizontal ratio and leaves the 1.2 stretch to the presenter, like today. For the user's 860x300, both give 6 with snapping.
3. **Suggested user presets.**
   * Windowed: 1920x1200, 320x200, Fit. Any `scale` gives S=6 1:1; `scale=6` also removes today's inter stage.
   * Fullscreen: `scale=6`, Auto, ACF (573x200 view, 4.13 Mpx), or `scale=5` (688x240 view, 5.94 Mpx).

   Both are reachable today by editing the cfg, or from VideoOptions with the point, interlaced or bilinear scalers. With 4xBR the scale is forced to 4 (`exult.cc:3205-3206`, `VideoOptions_gump.cc:345-346`).
4. **Where the hi-res world bypasses `scaler`.** With S_eff > 1 the world scaler (4xBR, Interlaced) does not run on the world (`present.md` §7.4). If the hi-res mode is exposed as a new pseudo-scaler instead of a separate key, then:
   * `setup_video` must not force its scale to 2 (`exult.cc:3207-3211`);
   * VideoOptions must offer x1..x8 for it (`VideoOptions_gump.cc:330-333`);
   * `set_scaleval` must accept it (`exult.cc:2771`);
   * `try_scaler` needs a full `size_mask` (`imagewin.cc:786`).

   A separate key avoids all of these.

### 6.5 Where to compute it, and how to test it

* Pure function, unit-testable without SDL: `static int Image_window::compute_render_scale(int disp_w, int disp_h, int full_w, int full_h, FillMode, int s_art, int s_cfg, int max_tex, size_t mem_budget)`, next to the static `get_draw_dims` (`imagewin.h:315`). The repo has no test framework (no gtest/catch hits), so this is an ideal first target together with `get_draw_dims` itself. The tables in §2, §3 and §6.3 are ready-made expected values.
* Call site: inside `create_scale_surfaces` after the renderer and display size are known (`imagewin.cc:672-673`), before `screen_texture` and `draw_surface` creation (`:690`, `:726`). Store it in a member that `scale_layer_color` does **not** swap (that function temporarily rewrites `scale`, `draw_surface`, `inter_surface`, `paletted_surface` and `ibuf` fields, `imagewin.cc:1758-1779`).
* Expose `get_render_scale()`. `Game_window::resized` compares it with the previous value and bumps the render generation (§5.3) before `paint()`. Optionally include S in the resize toast (`gamewin.cc:930`).

---

## 7. Touchpoints (summary)

| Where | What | Change |
|---|---|---|
| `imagewin/imagewin.cc:1315-1436`, `imagewin.h:315` | `get_draw_dims` (static) | Keep as is. Add a sibling static `compute_render_scale()` (§6.5). Unit-test both. |
| `imagewin/imagewin.cc:540-578` | `create_surface` uses config dims before the renderer exists | Do not compute S here. Size the hi-res buffer after S is known. |
| `imagewin/imagewin.cc:585-770` | `create_scale_surfaces`: renderer, real display size, textures | Query `SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER` after `:626` (cache it statically). Compute S_eff after `:673`. Size textures and hi-res buffer from S_eff. Fail soft (lower S) instead of throwing. |
| `imagewin/imagewin.cc:811-842` | `free_surface` destroys the renderer on every resize | GPU-side override caches must be rebuilt after every resize (or stop destroying the renderer). |
| `imagewin/imagewin.cc:858-876` | `resized` always rebuilds (no-op check commented out) | Fine. S_eff is recomputed on every call. |
| `imagewin/imagewin.cc:957`, `2170-2220` | full-texture lock per `show()`; final GPU stretch | The hi-res texture should be about display-sized (rule §6) and updated by dirty rect. |
| `imagewin/imagewin.cc:1277-1313` | `screen_to_game`/`game_to_screen` | Must keep using `inter/scale` (game px). Never use S. |
| `imagewin/imagewin.cc:1758-1779` | `scale_layer_color` swaps `scale` and surfaces | The new S member and hi-res buffer must not be touched by the swap. |
| `imagewin/imagewin.cc:1119-1140` | dead `toggle_fullscreen` with a w/h bug | Remove, or fix and route through `Game_window::resized`. |
| `gamewin.cc:916-937` | single funnel for runtime size and S changes | Bump the render generation / flush flats when S_eff changes, before `paint()` (`:926`). |
| `exult.cc:2744-2785` | Studio zoom: Auto+Fill, k=1..8, point only | With rule §6, S_eff follows k automatically. Allow the hi-res mode in the `scaler == point` guard (`:2771`) if it is a pseudo-scaler. |
| `exult.cc:3128-3304` | `setup_video` config parsing, scale forcing | New key `config/video/hires/render_scale` (auto or 1..6), plus `art_scale` if needed. Do not force the scale for the hi-res mode (`:3203-3211`). |
| `gumps/VideoOptions_gump.cc:329-349, 543-621` | scale choices, apply/revert | Show the effective S. The revert path calls `resized` twice (fine with generation counters). |
| `objs/chunkter.cc:234-242` | `Figure_queue_size()` = 100 | Use `(ceil(gw/128)+3)*(ceil(gh/128)+3)`, plus a byte budget at S. |
| `objs/chunkter.cc:248-277`, `chunkter.h:95-101` | flats allocation / MRU / free | Allocate at S. Add a generation/S check in `get_rendered_flats` and/or a static `flush_all()`. |
| `gamerend.cc:529` | `copy8(…, c_chunksize, c_chunksize, …)` hard-coded 128 | Must use the buffer's physical size/stride at S. |
| `gamerend.cc:206-233` | paint range builds caches for off-screen chunks | Skip chunks that do not intersect the clip (`world.md` §10.3). Matters more at S. |
| `gamemap.cc:1690-1723` | `write_minimap` iterates all terrains' flats | Use 1x flats (or a dedicated 1x path). |
| `shapeid.cc:420-468` | `reload_shapes` does not invalidate flats | Bump the render generation (fixes the existing staleness too). |

---

## 8. Risks and latent issues found

1. **Fatal texture failure.** Any S policy that can exceed the renderer's max texture size crashes the game on startup, on VideoOptions apply or on Studio zoom (`imagewin.cc:555-561`). The limit is never queried today.
2. **LRU cyclic thrash** whenever the paint window exceeds 100 chunks (1720x600 and up; Studio k ≤ 2 at 1920x1200). It exists today at S=1. It would be catastrophic at a fixed S.
3. **Stale-size flats** after a runtime S change. There is no flush API, and reload_shapes does not invalidate either (existing bug for Studio shape edits on flats).
4. **Config vs. real display size in fullscreen.** `get_draw_dims` uses the configured `display/*`, while the actual surface is the desktop (`imagewin.cc:637-655`). If they differ, the presentation ratio differs from what Auto assumed. S must be derived from the real display size, after window creation.
5. **HiDPI windowed.** `display_width` is in window points (no output-size query, `imagewin.cc:663-671`). S_need computed from it underestimates on macOS Retina. On Windows SDL3 is per-monitor-v2 DPI aware, so this is not an issue for the user.
6. **Per-show full-texture upload** on D3D11 (`imagewin.cc:957` with `SDL_render_d3d11.c:1754-1806`). The upload cost grows with any oversized world texture. Partial `show()`s (mouse moves, text) pay for the full texture.
7. **Indexed overrides vs. fractional S.** Non-divisor S (4 or 5 for 6x art) forces RGB resampling and breaks palette-index guarantees. Hence the snap rule, or keeping RGBA override paths separate.
8. **The `highdpi` key is ignored** in master (always HIGH_PIXEL_DENSITY). The user's cfg was written by 1.12.1 (SDL2), and the Windows binary is still 1.12.1, so the master behaviour on Windows has not yet been observed with these exact cfgs.
9. `current_scaleval` starts at 1 regardless of the configured scale (`exult.cc:163`), so the first Studio zoom step jumps from the configured scale to 2. This is cosmetic, but surprising when S follows the zoom.
10. The resize toast (`gamewin.cc:930`) and the "scaled size < 320x200" warning (`VideoOptions_gump.cc:553-563`) know nothing about S. That is harmless.

---

## 9. Open questions

* Snap S_eff to divisors of S_art (6 for the user's fullscreen, 9.29 Mpx), or allow 5 with resampled art (6.45 Mpx)? Or recommend that the user change the fullscreen view to 573x200 or 688x240?
* For AspectCorrect modes, should S follow `py` (supersampled vertical, then downscale) or `py/1.2` (presenter stretches 1.2, as today)?
* Should "auto" be the default for `render_scale`, with an explicit value meaning "cap" (never force a buffer larger than the display needs), or should an explicit value force S (for screenshots and authoring, e.g. 6x at any window size)?
* Is a memory budget for the S-scaled chunk cache needed (for example 128 MiB), or is `(cw+3)*(ch+3)` entries enough given that S shrinks as the view grows?
* Should the renderer stop being destroyed on every `resized()`, so that GPU-side override atlases (if used) survive resolution changes?
