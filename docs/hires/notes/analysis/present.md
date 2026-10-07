# Presentation pipeline and layers (Image_window): analysis for a hi-res render scale

Scope: `imagewin/imagewin.{h,cc}`, `imagewin/iwin8.{h,cc}`, `scene_layer.h`, the scalers (`scale_*.h/.cc`, `PointScaler`, `BilinearScaler*`, `ArbScaler.h`, `manip.h`) and `imagewin/save_screenshot.cc`, plus the callers that configure or depend on them (`exult.cc`, `gamewin.{h,cc}`, `gamerend.cc`, `mouse.cc`, `drag.cc`, `effects.cc`, `gumps/Gump_manager.cc`, `gumps/VideoOptions_gump.cc`, `palette.cc`).
Repo: `/home/simonea/ultima7_exult/exult-hires` (git master). Every file:line reference below points at that tree. Local SDL is 3.4.18 (`/home/simonea/ultima7_exult/deps/src/SDL3-3.4.18`).

---

## 0. TL;DR

* The game world is drawn into **one 8-bit buffer at game resolution** (`Image_window::ibuf`, whose bits live inside the SDL surface `draw_surface`, with a 4-pixel guard band). `show()` turns it into pixels on screen in up to three phases: (1) a software "scaler" at integer `scale`, (2) a "fill scaler" to the display size, then (3) GPU: `screen_texture` (streaming) → `screen_texture_a` (render target that accumulates partial updates) → backbuffer. After that all **UI layers** are composited, and the frame is presented (`imagewin.cc:882-1114`, `2170-2220`).
* **Invariant that matters most:** the *full* game buffer (`get_full_width() x get_full_height()` game pixels, including any border bands) is always stretched onto the whole **logical display rect** `(0,0,display_width,display_height)`. `screen_to_game()` / `game_to_screen()` are just that linear map (`imagewin.cc:1277-1313`). Mouse events are first converted to logical display coordinates with `SDL_ConvertEventToRenderCoordinates` (for example `exult.cc:1793`).
* Since the recent layer work, **the main buffer holds almost only the world**: terrain, objects, sprite effects and the border fill. Gumps, HUD, text effects, conversation, mouse cursor, dragged item, map and the full-screen scenes are each drawn into their own 8-bit **Layer** (its own `SDL_Surface`, ARGB texture and logical size). Layers are placed in display coordinates and composited on top in z order (`imagewin.cc:2091-2168`). So a hi-res world buffer is decoupled from the UI by construction.
* **Where S x plugs in (recommended):** add a second, hi-res 8-bit world surface (`full_w*S x full_h*S`) next to `draw_surface`, keep `ibuf`'s geometry in game pixels, and add a new present path similar to today's `SDLScaler` path ("convert at native buffer resolution, let the GPU scale"). Upload the dirty rect x S, preferably as an **SDL 3.4 INDEX8 texture with an `SDL_Palette`** (`SDL_SetTexturePalette`, `SDL_render.h:1016`), so palette rotations and fades cost no pixel re-conversion. Then draw it to the same logical rect as today. Coordinates, layers and hit testing stay unchanged.
* **Downscale (window smaller than S x game):** SDL `LINEAR` is correct only up to 2:1. Use a halving chain of render-target textures (an exact 2:1 bilinear pass equals a 2x2 box filter) and finish with a `LINEAR` pass, or use a CPU area filter fused with the palette lookup (measured at 1.7 ms/frame for 1920x1200 to 640x400). **Upscale or 1:1:** `NEAREST` or `PIXELART`.
* **Existing scalers:** they must be bypassed for the world when S > 1. They keep working for UI layers (`layer_render_scale` / `scale_layer_color`). Only point and Scale2x have 8 to 8 (palette-preserving) variants. All the others output RGB and cannot produce palette-indexed upscaled art.
* **Performance** (Ryzen 7 9700X, single thread, -O2, scratch benchmark): LUT 8 to 32 conversion of 1920x1200 takes **0.90 ms**. A point 6x upscale with LUT from 320x200 takes 0.76 ms, which is about what the current `point` fill scaler already costs per frame at 1920x1200. A 2x2 box downscale takes 0.76 ms. Presentation is not the bottleneck. The 36x drawing cost inside the world renderer, and the 9.2 MB/frame ARGB upload (2.3 MB as INDEX8), are the costs to watch.

---

## 1. Classes, buffers and ownership

| Object | Declared | Created / owned by | Size / format | Role |
|---|---|---|---|---|
| `Image_window8` (derives from `Image_window`) | `iwin8.h:42` | `Game_window` ctor `gamewin.cc:326` (`win`) | n/a | The window. Holds the 768-byte gamma-corrected palette `colors` (`iwin8.h:43`) and `ib8` |
| `ibuf` / `ib8` (`Image_buffer8`) | `imagewin.h:321`, `iwin8.h:44` | `new Image_buffer8(0,0,nullptr)` in `iwin8.cc:64`. Bits are **not owned** and point into `draw_surface->pixels` | game pixels | The drawing API used by all game code. Can be **redirected** to a layer buffer with `set_render_buffer` (`iwin8.h:63-68`) |
| `draw_surface` | `imagewin.h:404` | `create_scale_surfaces` `imagewin.cc:726-737` | `(inter_width/scale + 2*gb) x (inter_height/scale + 2*gb)`, 8-bit paletted | Storage for `ibuf`. It is also `paletted_surface` (`imagewin.cc:762`) |
| `inter_surface` | `imagewin.h:403` | `imagewin.cc:739-760` | `inter_w + 2*scale*gb`, display pixel format (alpha blending disabled) | Output of the phase-1 scaler before the fill stretch. Can alias `draw_surface`, or be null (then it is the temporarily locked `screen_texture`) |
| `screen_texture` | `imagewin.h:393-394` | `imagewin.cc:690-700` | `max(inter_w, w) + 2*gb*scale` (same for h), desktop format, **STREAMING**, NEAREST | Locked as a surface on every `show()` (`imagewin.cc:957`). Locking invalidates its previous content |
| `screen_texture_a` | `imagewin.h:395-396` | `imagewin.cc:702-711` | same size, **TARGET** | Accumulates the dirty rects of `screen_texture`. Its `fullRect` is drawn to the window every frame. Its scale mode decides SDLScaler NEAREST vs LINEAR (`imagewin.cc:742-743`) |
| `layers` (`vector<unique_ptr<Layer>>`) | `imagewin.h:407` | `create_layer` `imagewin.cc:1575-1599` | per layer: INDEX8 `SDL_Surface` with guard band plus an `Image_buffer8` over it (`imagewin.cc:2273-2277`, `ibuf8.h` guard-band ctor), and an ARGB8888 STREAMING texture | UI overlays |
| `layer_dst32_surfaces[3]` | `imagewin.h:409` | `get_layer_dst32_surface` `imagewin.cc:1783-1806` | ARGB8888, grow-only | Scratch space for the matting passes in `refresh_layer_scaled` |

`guard_band` is a constant 4 (`imagewin.h:357`). `ibuf->bits` is set up so that game coordinate (0,0) is the top-left pixel of the *game area*, centred inside the full buffer: `offset_x = (full_w - game_w)/2` and `bits = pixels + gb*(pitch+1) - start_x - start_y*pitch` (`imagewin.cc:564-577`). `get_start_x() = -offset_x` (`imagewin.h:624`). Coordinates left of or above the game area are therefore negative, and `Game_window::paint` fills those border bands with the border colour (`gamerend.cc:369-384`).

`Shape_frame::set_to_render()` is a second, static render-target pointer that must always follow `ib8`. `Game_window::push_render_target` / `pop_render_target` switch both together (`gamewin.cc:541-550`), and `Game_window::resized` resets it (`gamewin.cc:921`).

---

## 2. Configuration and resize flow

### 2.1 Config keys (`exult.cc:3128-3287`, `setup_video`)
The base path `vidStr` is `config/video` or `config/video/window`, depending on `share_video_settings` and fullscreen (`exult.cc:3150-3151`):
* `display/width`, `display/height`: window or fullscreen size. Defaults are the larger of 1024x768 and 320*scale x 240*scale on desktop, or the desktop size on mobile (`exult.cc:3213-3227`).
* `game/width`, `game/height`: logical game area, default 320x200. **0x0 = Auto** (derived from display/scale).
* `scale` (default 2), `scale_method` (scaler name, default `point`, `SDLScaler` is rejected here, `exult.cc:3198-3201`). `scale` is forced to 3 or 4 for Hq3x/3xBR and Hq4x/4xBR, and to 2 for the other fixed-size scalers (`exult.cc:3202-3212`).
* `fill_mode` (default `Fit` on desktop, `Fill` on mobile, parsed by `string_to_fillmode` `imagewin.cc:1438-1523`) and `fill_scaler` (default point, fallback bilinear).
* Other keys: `config/video/fullscreen`, `config/video/vsync` (read in `imagewin.cc:631-633`), `config/video/gamma/*`, `config/video/game/border/*` (border colour that replaces palette index 255 via `Palette::apply`, `palette.cc:169-185`), `config/video/disable_fades`, `config/video/fps`.
* The layer ("UI") config lives under `config/video/ui/...`: `universal`, `width/height` (default 420x263, 0x0 = Auto), `scale_method`, `fill_mode`, `fill_scaler`, `palette`. Per-kind sub-keys are `conversations`, `mouse_pointer`, `gumps`, `hud_gumps`, `text_gumps`, `modal_gumps`, `display_map` and `text_effect` (`exult.cc:2915-3122`, applied after every `VIDEO_INIT` / `TOGGLE_FULLSCREEN`, `exult.cc:3283-3286`). The UI gump is `gumps/UIOptions_gump.cc:632-702`.

The video gump (`gumps/VideoOptions_gump.cc:546-620`) applies changes through `gwin->resized(...)` and persists them through `setup_video(SET_CONFIG)`. Its fill-scaler UI only offers point, bilinear and SDLScaler (`VideoOptions_gump.cc:566-569`). Scale choices go up to x8 for point, interlaced, bilinear and SDLScaler (`VideoOptions_gump.cc:329-333`). **"point x6" already exists today** and gives exactly a 1920x1200 picture for 320x200.

ExultStudio zoom (`set_scaleval`, `exult.cc:2767-2783`) calls `resized(..., gw=0, gh=0, scale, point, Fill, point)`, so the game area becomes Auto-sized from display/scale. `--buildmap` creates a 2048x2048, scale-1 window (`exult.cc:2873-2887`) and saves paletted screenshots (`exult.cc:2905`).

### 2.2 Construction and resize
`Game_window(...)` creates `Image_window8(w,h,gw,gh,scale,fs,scaler,fill,fillsclr)` (`gamewin.cc:326`). `Image_window::Image_window` calls `create_surface(w,h)` (`imagewin.h:569-578`), which:
1. forces `fill_scaler` to an arb scaler or SDLScaler (`imagewin.cc:544-550`);
2. calls `get_draw_dims(w,h,scale,fill_mode, game_width, game_height, inter_width, inter_height)` (`imagewin.cc:552`). This computes the game area if it is Auto, plus the size of the *intermediate* image `iw x ih` that will later be stretched over the display;
3. calls `try_scaler` → `create_scale_surfaces` (`imagewin.cc:776-805`, `585-770`). These create or resize the window, the renderer, vsync, **logical presentation `w x h` LETTERBOX** (`imagewin.cc:662`, `670`), `display_width/height = w,h` (`672-673`), the textures and the surfaces. In fullscreen, `w,h` are replaced by the renderer output size in pixels and `nativescale = pixel_w / window_w` (`imagewin.cc:637-662`). Windowed mode uses `SDL_WINDOW_HIGH_PIXEL_DENSITY` with logical size equal to the window size in points.
4. points `ibuf` at `draw_surface` (`imagewin.cc:564-577`).

`get_draw_dims` semantics (`imagewin.cc:1315-1436`):
* `Fill`: `iw = gw*scale`, `ih = gh*scale`, stretched non-uniformly to the display.
* `Fit`: pads `iw` or `ih` so that `iw:ih = sw:sh`. The extra area becomes border bands in the full buffer.
* `AspectCorrectFit`: the same with 1:1.2 pixels.
* `Centre*` and `Centre xN`: `iw = 2*sw/factor` etc. For an explicit game size it ensures `iw >= gw*scale` (`imagewin.cc:1393-1397`).
* `WxH`: arbitrary target size.
* Finally, if `iw` (resp. `ih`) equals `(sw/scale)*scale`, it is snapped to `sw` (resp. `sh`) "to avoid scaling twice" (`imagewin.cc:1427-1433`).

`ibuf->width = inter_width/scale` (integer division), so **full buffer = floor(iw/scale) x floor(ih/scale)** game pixels.

`Image_window::resized(...)` (`imagewin.cc:858-876`) frees everything, including the renderer and every layer texture (`free_surface`, `imagewin.cc:811-842`), sets the new values, and recreates everything. `Game_window::resized` (`gamewin.cc:916-937`) then re-applies the palette, the shape render target, re-centres and repaints. There is **no live window-resize event handling**: the window is not resizable, and size changes go only through config or the gump.

`scale` has three meanings today: (a) the phase-1 software scale factor; (b) the divisor that turns display size into game size in Auto mode; (c) a multiplier on the texture guard band. `nativescale` is computed but never read anywhere else (grep shows only `imagewin.cc:109,659`).

---

## 3. The path from Image_buffer8 bits to the window: `Image_window::show(x,y,w,h)`

`imagewin.cc:882-1114`. The callers are `Game_window::show()` (`gamewin.h:747-755`, full-window `win->show()` when `painted`), `Mouse::blit_dirty` (partial, `mouse.h:179-181`), palette apply and fades (`palette.cc:110,193,418-480`), the FLI player, menus (partial rects, `menulist.cc:72-223`), the earthquake effect, and others.

1. If scene mode is on, mark all layers dirty (`imagewin.cc:894-896`). Then `EndPaintIntoGuardBand()`.
2. Clip `(x,y,w,h)` against **the current `ibuf` clip** (`imagewin.cc:903`). Convert to buffer coordinates (`x -= start_x`). Grow by 4 px, align to 4, and clip to `full + gb` (`imagewin.cc:906-944`).
3. `SDL_LockTextureToSurface(screen_texture)` gives `display_surface`. If there is no separate inter surface, `inter_surface` temporarily becomes `display_surface` (`imagewin.cc:957-965`).
4. **Only if `ibuf` is the main buffer** (its size matches `draw_surface`; this guard was added for issue #1011, `imagewin.cc:967-968`). When a layer or scene buffer is the render target, the main image is **not** re-scaled at all. Only `UpdateRect` runs, which re-composites the layers.
   * **Phase 1** (`imagewin.cc:972-1015`), when `draw_surface != inter_surface`: run `Scalers[scaler]` on the rect, either the arb path (`ArbScaler::Scale`) or the member function chosen by destination bpp (`fun8to565/555/16/32/8`). The output is written at `scale*(x+gb)` in `inter_surface`. All `show_scaled8to*` functions read `draw_surface`, `paletted_surface`, `ibuf->line_width`, `ibuf->height`, `scale` and `guard_band` (example: `scale_point.cc:80-92`, `scale_hq2x.cc:85-97`).
   * **Phase 2** (`imagewin.cc:1017-1049`), when `inter != display` and `fill_scaler != SDLScaler`: the arb fill scaler (Point or Bilinear) stretches the **whole** inter (or draw) surface to `display_surface` at size `max(iw,w) x max(ih,h)`. This happens on every `show()`, even for a 4x4 dirty rect.
   * **SDLScaler fill** (`imagewin.cc:1051-1093`): copy the whole inter surface (memcpy if the formats match, otherwise `SDL_BlitSurface`; for an 8-bit `draw_surface` source this is a full palette conversion) into `display_surface` *without scaling*. The GPU does the stretch later.
5. Unlock `screen_texture` (`imagewin.cc:1107`) and call `UpdateRect(&dirtyrect,&fullrect,false)` (`imagewin.cc:2170-2220`):
   * render target = `screen_texture_a`; `SDL_RenderTexture(screen_texture, dirty, dirty)`. Only the touched region is copied, which preserves the rest because locking invalidated `screen_texture`;
   * render target = window; clear; `SDL_RenderTexture(screen_texture_a, fullRect, nullptr)`. This is where the GPU stretch happens (`NEAREST` or `LINEAR` comes from `screen_texture_a`'s scale mode);
   * `composite_layers()`, then `SDL_RenderPresent`.

Which case applies is decided in `create_scale_surfaces` (`imagewin.cc:739-760`):

| Case | Condition | inter_surface | Phase 1 | Phase 2 | GPU stretch |
|---|---|---|---|---|---|
| A | `scaler == fill_scaler` or `scale == 1` or (`fill_scaler == SDLScaler` and `scaler` is point/bilinear) | `= draw_surface` | none | arb fill scaler 8 to 32, full buffer to `max(iw,w)` (or SDL blit 8 to 32 at 1x if SDLScaler) | `fullRect` to window (1:1, or a stretch for SDLScaler) |
| B | otherwise, `iw != w` or `ih != h` | own 32-bit surface | software scaler xS on the dirty rect | arb fill scaler or memcpy (SDLScaler) | same as A |
| C | otherwise (`iw == w` and `ih == h`) | null, so the locked `screen_texture` | software scaler straight into the texture surface | none | 1:1 |

The default desktop config (`point` / `point` / `Fit`, scale 2) is **case A**: phase 2 stretches 320x200 to the whole display with the arb `PointScaler` every frame. `PointScaler` has a fast path for integer scales (`PointScaler.cpp`, "Integer scaling, x and y"). Pixel format conversion uses `Manip8to32::copy`, which rebuilds the destination pixel from `SDL_Color` with shifts for each *source* pixel (`manip.h:306-312`, `ManipBaseDest::rgb`). It does not use a ready `uint32` LUT. `ManipBase::fmt` and `colors` are **static** members (`imagewin.cc:111-112`, `manip.h:62-75`), so scalers are not re-entrant.

**Palette:** `Image_window8::set_palette` and `rotate_colors(…, upd=1)` only update the `SDL_Palette` of `paletted_surface` / `draw_surface` and mark all layers dirty (`iwin8.cc:96-120`, `135-174`). Nothing on screen changes until a later `show()` re-converts pixels. Even then only the dirty rect is re-converted in cases B and C; case A and the SDLScaler path re-convert the whole buffer. `Game_window::rotatecolours` does not force a repaint for palettized windows (`gamewin.cc:1072-1075`, and `is_palettized()` is always true because `create_surface` sets `uses_palette = true`, `imagewin.cc:541`). Water cycling is visible only because the main loop shows full frames whenever anything is painted (`exult.cc:1518-1524`).

---

## 4. The layer system (what it offers)

**Data** (`imagewin.h:175-251`): an INDEX8 `SDL_Surface` with guard band; an `Image_buffer8` over it; the logical size `logw x logh` in UI pixels; `transparent` index (255 by default) or `opaque`; `visible`; `dirty`; `z`; a vector of explicit `dest` rects in display coordinates (several rects mean the layer is drawn several times, `e29aa959b`); `ui_kind`; `render_scale`; whole-layer `alpha`; an optional per-index ARGB table `index_argb` (translucency, used for the shortcut bar and the perf overlay); a name. `fixed_scale` is stored but **never used**.

**API** (`imagewin.h:692-775`, `imagewin.cc:1575-2079`): `create_layer` / `destroy_layer` (handles are reused slots), `get_layer_ibuf`, `layer_set_dirty / visible / opaque / z / alpha / index_argb / ui_kind / dest / clear_dest`, `screen_to_layer` / `layer_to_screen`, `mark_all_layers_dirty`. Game code draws into a layer by `push_render_target(lbuf)`, then the normal shape and font routines, then `pop_render_target` (examples: `Gump_manager.cc:199-201`, `drag.cc:353-361`, `effects.cc:1195-1201`, `scene_layer.h:112`).

**UI kinds and config** (`imagewin.h:70-88`, `145-156`, `366-388`): every `UiLayerKind` has a `UiLayerConfig {width,height,scaler,fill_mode,fill_scaler,protect,ui_palette,ui_palette_colors}`. `set_ui_config` writes all unprotected kinds and `set_ui_layer_config` writes one (`imagewin.cc:1820-1855`). The protected kinds `UiLayerFullScreenBilinear` and `UiLayerFullScreenPoint` are set to the display size with `NoScaler` in `create_scale_surfaces` (`imagewin.cc:766-767`). `UiLayerFullScreenScene` copies the *world* scaler and fill scaler with fill mode forced to `Fill` (`scene_layer.h:50-52`). `eff_ui_scale()` always returns 1 (`imagewin.h:378-380`). `ui_layer_kind_mask` hides whole kinds (used by `Scene_view` to show only the scene and the mouse, `scene_layer.h:212-222`).

**Placement:** with no explicit dest, `compute_layer_fill_dest(logw,logh,kind)` → `compute_fill_dest` fits the layer into the display by fill mode, centred (`imagewin.cc:1947-1993`). Most UI code instead computes dests itself. Gumps, text effects and the dragged item use `game_to_screen()` on the game-coordinate centre plus `get_ui_scale_factor(kind)` (`Gump_manager.cc:203-273`, `effects.cc:1203-1213`, `drag.cc:362-404`). The cursor uses `compute_ui_layer_dest(320,200,…)` for its scale and `game_to_screen` for the hotspot (`mouse.cc:233-262`). Layers are therefore positioned *relative to the world mapping*, not to the world buffer's pixel size.

**Refresh** (`composite_layers`, `imagewin.cc:2091-2168`, and `Image_window8::refresh_layer*`, `iwin8.cc:281-617`):
* `render_scale = layer_render_scale(layer)` (`imagewin.cc:1713-1736`): 1 for arb and SDL scalers. For fixed-factor software scalers it is that factor (2/3/4, from the single-bit `size_mask`).
* The texture size is `(logw + 2*gb + 2)*rs x (logh + 2*gb)*rs`, ARGB8888 STREAMING, BLEND, or NONE when opaque (`imagewin.cc:2138-2148`). Scale mode is LINEAR if the kind's scaler or fill scaler is bilinear or SDLScaler, otherwise NEAREST.
* `rs == 1`: palette LUT of 256 ARGB values (honouring transparency, `index_argb` and the fixed UI palette) and a straight copy loop (`iwin8.cc:281-322`).
* `rs > 1`: `refresh_layer_scaled` runs the *world* scaler code on the layer surface through `scale_layer_color`. That function temporarily repoints `draw_surface`, `inter_surface`, `paletted_surface`, `ibuf->line_width/height` and `scale` (`imagewin.cc:1738-1781`). The scaler runs 2 or 3 times with red, green and blue under the transparent index ("consensus difference matting") to recover alpha edges (`iwin8.cc:424-603`). Opaque layers take a single pass (`iwin8.cc:394-422`).
* Composite order: the main image first (in `UpdateRect`), then visible, unmasked layers sorted by `z` with a stable sort. Each one gets `SDL_RenderTexture(src = content without guard band, dst = each dest rect)`.
* Any palette change or rotation marks **every** layer dirty (`iwin8.cc:119`, `173`), so all visible layers are re-converted (and re-scaled 2-3 times with hqNx) on every palette tick.

**Can a layer have a different resolution than the UI or the world?** Yes. A layer's logical size is arbitrary, and its dest rect is any rect in display coordinates. `render_scale` already supports texture texels at `rs` x the logical size, with `src` computed accordingly (`imagewin.cc:2155-2156`). The world itself is *not* a layer. It is the base image drawn from `screen_texture_a` before the layers.

**Scenes** (`scene_layer.h`): `Scene_layer` is an opaque 320x200 (or custom-size) layer at `z = 1<<19`. `Scene_view` RAII pushes the scene buffer as render target for its whole lifetime, enables `scene_mode` (so `get_game_width/height` report the scene size, `imagewin.h:640-646`, and `screen_to_game` maps through the scene layer, `imagewin.cc:1280-1283`), masks the other kinds, and rewrites the caller's anchors. During a scene, `show()` skips phases 1 and 2 (ibuf ≠ draw_surface) and only composites.

**Layer users today:** mouse (`mouse.cc:205`), dragged item (`drag.cc:336`, kind MousePointer), gumps, HUD, text, modal (`Gump_manager.cc:181`), text effects (`effects.cc:1179`), conversation plus background (`usecode/conversation.cc:220,254`), map (`cheat.cc:1259`, `usecode/intrinsics.cc:1485`), scenes and load screen (`gamewin.cc:3172`), perf overlay (`perf.cc:89,105`), shape browser and cheat screen (`browser.cc:144`, `cheat_screen.cc:135`), sound test.

What is still drawn into the **main** buffer: the world (`render->paint_map`), sprite effects (`effects->paint()`), border bands, plasma (`gamewin.cc:2982-3002`), and pre-game menus and text scrollers that draw straight into `get_ib8()` (`menulist.cc`, `txtscroll.cc:209`, `exultmenu.cc:196`, `game.cc:98`) when they are not inside a `Scene_view`.

---

## 5. Coordinate systems and mapping

1. SDL window coordinates (points) → **logical display coordinates** (`display_width x display_height`) through `SDL_ConvertEventToRenderCoordinates` (called in every event loop: `exult.cc:1793,1932,2025,…`, `Gump_manager.cc:885-986`, `menulist.cc:330`, `cheat_screen.cc:851`, `bggame.cc:2088`, `sigame.cc:1457`, `intrinsics.cc:2581`).
2. Logical display → game pixels: `screen_to_game` (`imagewin.cc:1277-1294`): `gx = sx*inter_width/(scale*display_width) + start_x`. `game_to_screen` is the inverse (`imagewin.cc:1296-1313`). The `fast` mode (fast mouse) treats screen coordinates as game coordinates plus a stored offset (`mouse.cc:302-320`). In scene mode both functions go through `screen_to_layer` / `layer_to_screen` (`imagewin.cc:2050-2079`).
3. Game pixels → tiles and objects: world logic (`Game_window::find_object`, …) in game pixels and `c_tilesize`. Not affected by presentation.
4. Logical display → layer-local coordinates: `screen_to_layer` (dest rect and `logw/logh`). Gumps use `map_game_to_gump` (game → screen → layer, `Gump_manager.cc:287-315`).

**Invariant:** in every case (A, B, C, SDLScaler) the full buffer `floor(iw/scale) x floor(ih/scale)` maps onto `(0,0,display_w,display_h)`. The formula uses the exact rational `iw/scale`, so with odd `iw` there is up to one game pixel of rounding drift. When `max(iw,w) > w` (Centre with an explicit large game), `screen_texture` is larger than the display and is *downscaled* by the GPU to the logical rect. With HiDPI, SDL3 applies logical presentation as a scale on the render target ("scaling to the actual resolution as necessary", `SDL_render.h` doc of `SDL_SetRenderLogicalPresentation`). A texture with more texels than the logical rect therefore keeps its detail on a 2x backbuffer. This needs verifying on Windows D3D11/12 (see open questions).

**Consequence for hi-res:** if the hi-res world image is drawn into exactly the same logical rect that the current main image covers, then `screen_to_game`, `game_to_screen`, every gump, HUD, text, drag and cursor placement, and all hit testing keep working **unchanged**, whatever S is.

---

## 6. Screenshots and other read-backs

* `Image_window::screenshot(dst, paletted)` (`imagewin.cc:1254-1271`). With `paletted`, it saves `draw_surface` (game resolution, guard band stripped; used by `--buildmap`). Otherwise it calls `UpdateRect(nullptr,nullptr,true)`, which re-renders `screen_texture_a` with the static `last_fullrect` plus the layers, then `SDL_RenderReadPixels`, then `SaveIMG_RW(surf,…,guard_band)`. **Latent bug:** the read-back surface has no guard band, but 4 px are cropped on every side anyway (`imagewin.cc:1266`). PNG through libpng or PCX fallback (`save_screenshot.cc:88-156`, `314-377`).
* `Image_window8::mini_screenshot()` (savegame thumbnail, 96x60 from a 288x180 crop, 3x3 average then nearest non-cycling palette index) reads `ibuf->get_bits()` at game resolution (`iwin8.cc:181-226`, called from `Game_window::create_mini_screenshot`, `gamewin.cc:3126-3139`).
* `ImageBufferPaintable` snapshots and restores the whole main buffer through `get/put` (`shapeid.cc:643-654`, used by the cheat screen at `cheat_screen.cc:781`). `win->copy` is used for scrolling by one tile (`gamewin.cc:1654,1676,1700,1722`) and for the earthquake shake (`effects.cc:1862-1866`). The chunk-flats cache is blitted with `copy8` (`gamerend.cc:529`).

All of these act on the game-resolution main buffer. A hi-res path must either keep that buffer up to date (dual write) or provide hi-res equivalents.

---

## 7. Assessment: plugging in a render scale S

### 7.1 What must not change
`game_width/game_height`, `get_full_width/height`, `get_start_x/y`, the `ibuf` clip and offset, `scale` as the Auto divisor, and the `screen_to_game` map all stay in **game pixels**. If `ibuf` simply became S x larger, the following would break: `Game_window::get_width/height` and `get_game_rect`, `get_win_tile_rect` (`gamewin.h:212-249`), so chunk ranges, the paint clip, scroll bounds (`gamewin.cc:1085-1100`) and `find_object`; gump positions; HUD anchors (`Gump_manager.cc:221-258`); `UiLayerConfig` Auto (`imagewin.cc:1927-1945` uses `game_width`); the "scaled size < 320x200" check in the video gump; the `BeginPaintIntoGuardBand` game-width hack (`imagewin.cc:1142-1224`); `mini_screenshot`'s 288x180 crop; scene centring; and the smooth-scroll pixel offset (`gamerend.cc:434-516`, `scrolltx_lo` in game pixels). **So S must live in a separate hi-res buffer, or in an Image_buffer variant whose API stays in game coordinates.**

### 7.2 Recommended plug-in point (P1: hi-res world surface in Image_window)
* Add `int render_scale_S` (1 means off) and `SDL_Surface* hires_surface` (INDEX8, `full_w*S x full_h*S`, plus an optional guard band) to `Image_window`. Create, resize and free it with the other surfaces (`create_scale_surfaces`, `free_surface`, `create_surface`). Share the palette with `paletted_surface`: `set_palette` and `rotate_colors` must also update the hi-res palette (`iwin8.cc:108-117`, `160-169`).
* **Drawing side** (other analysts' scope): the main render target becomes a pair (game-res `ibuf` for logic and compatibility readers, plus the hi-res storage), or an `Image_buffer8` variant with a pixel scale. In either case `push_render_target(layer)` keeps going to 1x layers automatically, so the UI is unaffected. The touchpoints that write the main buffer are `copy8` for chunk flats (`gamerend.cc:529`), `win->copy` for scrolling, `fill8` for borders and blackness, `paint_rle`, translucency fills, and `put_pixel8`.
* **Present side:** in `show()`, when `render_scale_S > 1` and `ibuf` is the main buffer, skip phases 1 and 2 entirely. Instead upload `hires_surface`'s dirty rect x S to a `hires_texture`:
  * preferred: `SDL_PIXELFORMAT_INDEX8` STREAMING texture with `SDL_SetTexturePalette` (SDL ≥ 3.4. Supported in GL, GLES2, D3D9/11/12, Vulkan, Metal and GPU renderers, with palette shaders for NEAREST, LINEAR and PIXELART, e.g. `src/render/opengl/SDL_shaders_gl.h:34-36`, `direct3d11/SDL_render_d3d11.c:2966`). Upload is 2.3 MB for a full 1920x1200 frame. Palette rotation and fades become a 1 KB palette update, with no pixel work and no need to show a full frame. Note that the current configure only requires `sdl3` without a version (`configure.ac:480`);
  * fallback: ARGB with a 256-entry LUT conversion, as `refresh_layer` does (`iwin8.cc:305-320`): 0.9 ms/frame full-screen on a 9700X, 9.2 MB upload.
  * `SDL_UpdateTexture(rect)` keeps the texture content outside the rect, so the `screen_texture` → `screen_texture_a` accumulation is not needed on this path.
* In `UpdateRect`, draw `hires_texture` (full content rect) onto `(0,0,display_w,display_h)` where today `screen_texture_a` is drawn, then `composite_layers()`. Keep a "last rect" state for the screenshot path, as `last_fullrect` does (`imagewin.cc:2174-2207`).
* `ShouldPaintIntoGuardband()` must return false for the hi-res path, because no scaler reads the guard band (`imagewin.h:802-827`).

Alternative P2: a hi-res opaque world **layer** (new protected kind with NoScaler, z very low, dest = whole display). It reuses the texture, composite and screenshot code, but layer refresh converts the **whole** layer on any dirty flag (no dirty rect), always as ARGB, and the main path would still run on the 1x buffer. Acceptable for a first prototype, but worse in steady state. Better: **generalize Layer with a buffer pixel scale** (`buf_scale`, texture = `logw*buf_scale`, `src` like `render_scale`). That is the same mechanism needed later for hi-res gumps, faces, fonts and the **dragged item**, which is today a 1x layer of a *world* shape (`drag.cc:336-361`).

### 7.3 Presenting S x full into display size D
Let `r = D / (S*full)`, computed per axis; Fit and AspectCorrect may differ by axis.
* `r = 1`: 1:1, NEAREST.
* `r` an integer ≥ 2: NEAREST, or `SDL_SCALEMODE_PIXELART` (SDL 3.4, `SDL_surface.h:92`) for fractional or aspect cases.
* `0.5 ≤ r < 1` (for example 1920 to 1280): a single LINEAR pass is acceptable.
* `r < 0.5`: LINEAR alone aliases, because SDL renderers have no mipmaps. Options: (a) a GPU halving chain into TARGET textures (an exact 2:1 LINEAR sample is a 2x2 box), then a final LINEAR pass; (b) a CPU area filter fused with the palette LUT when `S*full/D` is an integer (3x3 area, 1920x1200 to 640x400: 1.7 ms); (c) choose S ≤ the display factor (but overrides authored at 6x would need pre-filtering in RGB, which loses palette indexing).
* The aspect-correct modes (1:1.2) always need non-integer vertical scaling. Use LINEAR or PIXELART.
* Interaction with `scale`: keep `scale` as the "game zoom" that sets Auto game size and `iw/ih`. S is independent. The natural preset is `scale = S = 6`, display 1920x1200, Fit, which gives r = 1.

### 7.4 Interplay with the existing scalers
* With S > 1, the world scaler (`scaler`) and the fill scaler must not run on the world. The existing `point x6` path (case A, `PointScaler` integer path) is a perfect **test oracle**: hi-res with S = 6 and no overrides must be pixel-identical to `point`, scale 6, Fit at 1920x1200.
* UI layers keep their own per-kind scalers (`layer_render_scale`, `scale_layer_color`). The scene layer copies `iwin->get_scaler()` (`scene_layer.h:50-52`), so it still works.
* `scale_layer_color` mutates `scale`, `draw_surface`, `paletted_surface` and `ibuf` fields (`imagewin.cc:1758-1779`). Any new member that the hi-res path reads during `show()` must not be affected by these temporary swaps.
* As **pre-scalers for frames without overrides** (load-time, asset side): only `point` (any factor) and `Scale2x` have 8 to 8 palette-preserving variants (`scale_2x.cc:37-47`, `scale_point.cc:36-44`, the `fun8to8` entries in `imagewin.cc:120-201`). hq2x/3x/4x, xBR, 2xSaI and bilinear produce RGB and would need re-quantization to the palette (dangerous for the cycling range 0xE0-0xFF and the xform indices).

### 7.5 Performance at 1920x1200
Measured with a scratch micro-benchmark (`g++ -O2`, one core, Ryzen 7 9700X):

| Operation | ms/frame |
|---|---|
| LUT 8 to 32, 1920x1200 | 0.90 |
| point 6x upscale + LUT, 320x200 to 1920x1200 (≈ what the `point` fill scaler already does today) | 0.76 |
| 2x2 box, 32-bit, 1920x1200 to 960x600 | 0.76 |
| fused LUT + 3x3 area, 8-bit 1920x1200 to 640x400 | 1.69 |

Conclusions:
* Presentation at 1920x1200 costs about the same as today's default `point` fill at that resolution.
* Upload: ARGB is 9.2 MB per full frame and INDEX8 is 2.3 MB.
* Full-frame updates are frequent: lerped smooth scrolling repaints everything each frame (`gamerend.cc:434-516`), and so do palette fades. So the dominant extra cost is **drawing 36x more pixels** (terrain caches, RLE sprites) and keeping both buffers when dual-writing. The present step is not the problem.
* Today the SDLScaler path converts the whole `draw_surface` every `show()` (`imagewin.cc:1057-1088`), and phase 2 always stretches the whole surface. The hi-res path should upload only dirty rects. With INDEX8 it also never needs re-conversion on palette ticks.
* UI layers are all re-converted on every palette rotation (`iwin8.cc:173`). Not a problem today, but it grows if UI layers become hi-res. INDEX8 layer textures would fix it too.

---

## 8. Touchpoints (summary)
See the structured list in the return value. Main sites: `imagewin.h` members and `show()` / `UpdateRect` / `create_scale_surfaces` / `free_surface` / `create_surface` / `resized`; `iwin8.cc` `set_palette` / `rotate_colors` / `mini_screenshot` / `refresh_layer*`; `screenshot`; `ShouldPaintIntoGuardband`; `screen_to_game` / `game_to_screen` (keep the invariant); `setup_video` / `apply_ui_layer_config` / `VideoOptions_gump` (new config); `Game_window::resized` / `push_render_target`; `gamerend.cc:529` and `win->copy` scrolling; `drag.cc` item layer; `ImageBufferPaintable`; `Scene_view` (must stay 1x).

## 9. Risks and latent issues found
1. `refresh_layer_scaled`'s opaque path allocates an unused `texpix` buffer on every refresh (`iwin8.cc:412`).
2. Layer texture width uses `+2` (`imagewin.cc:2141`), but `ssw` rounds `logw` up to a multiple of 4 (up to +3, `iwin8.cc:343`). When `logw % 4 == 1`, the opaque path's size check fails and falls back with an error message (`iwin8.cc:403-408`).
3. The screenshot read-back is cropped by `guard_band` on a surface that has no guard band (`imagewin.cc:1266`).
4. `show()` locks `screen_texture` (which invalidates its content) even when it then writes nothing (scene mode or a layer render target), and still copies `dirtyrect` into `screen_texture_a`. This is harmless only because an opaque scene covers it (`imagewin.cc:957`, `2183`).
5. Palette rotation becomes visible only on the next full `show()`, and partial shows mix old and new palettes (cases B and C). INDEX8 textures would remove this class of problem.
6. `ManipBase` static state makes scalers non-reentrant. Do not run the hi-res conversion on worker threads through the Manip classes; use a plain LUT.
7. INDEX8 textures need SDL ≥ 3.4. Behaviour with LINEAR palette filtering and `SDL_LockTexture` on INDEX8 should be checked on every backend Exult ships (Windows D3D11 and D3D12 especially).
8. The dragged world item, the cheat-screen and gump snapshot (`ImageBufferPaintable`), savegame thumbnails and `--buildmap` all read or draw the world at 1x. Each needs a hi-res-aware variant, or it silently shows low-res content.
9. The fixed window logical size equals the window size in points. On HiDPI windowed setups, verify that the hi-res texture is rasterized at backbuffer resolution and is not reduced to the point size first.

## 10. Open questions
* Should the hi-res path coexist with the game-res buffer (dual write, cheap fallback and compatible read-backs) or replace it?
* Should the window and display size default to S x game size (1920x1200) when hi-res is enabled, so that r = 1, or follow the current `display/*` keys?
* Should the downscale default be the GPU halving chain or the CPU area filter, and is the CPU path allowed to use threads?
* Should hi-res screenshots (raw S x 8-bit PNG with palette) be added? They would be useful for authoring and QA of override tiles.
* What minimum SDL version should be required: 3.4 for INDEX8 textures, or keep an ARGB fallback?
