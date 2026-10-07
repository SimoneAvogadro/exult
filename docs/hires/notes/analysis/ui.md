# UI, gumps, fonts, mouse and other buffer consumers: analysis of current master

Scope: everything other than the world renderer that draws into, or reads from, the main `Image_window8` buffer. That covers gumps, fonts, the mouse, conversation, text effects, drag, intro/menu/cheat "scenes", FLI, screenshots, Exult Studio and touch UI. For each one this report records where it draws today, which coordinate system it uses, and what happens if the game-view buffer becomes S times larger (goal S=6).

Repo: `/home/simonea/ultima7_exult/exult-hires` (git master `8b6ab6b43`). All paths below are relative to it.

---

## 0. Summary

1. **This master already has a GPU layer compositor. Nearly all UI is off the main buffer.** Every gump, the mouse pointer, the dragged item, the dragged gump, floating text, conversation faces and text, the display map, the perf overlay and every full-screen "scene" (intro, endgame, menus, cheat screen, shape browser, credits) paint into **their own 8-bit layer buffers**. Each layer is converted to ARGB and drawn on top of the world image by `Image_window::composite_layers()` at display resolution (`imagewin/imagewin.cc:2091-2168`, called from `UpdateRect` at `:2211`). The main buffer now holds essentially only the **world view**, plus a few overlays (section 2).
2. **Game-pixel coordinates are the hub.** Every UI hit test goes through the same chain: display coords, then `screen_to_game`, then `game_to_screen`, then `screen_to_layer`, then layer-local. Every layer placement uses `game_to_screen` plus a per-kind UI scale factor. Keep `screen_to_game`/`game_to_screen` returning and accepting **logical game pixels** (e.g. 320x200) and the entire UI stack keeps working without changes.
3. **World picking does not read the framebuffer.** `Game_window::find_object` and `Gump::has_point` test against the RLE shape data (`shapes/vgafile.cc:706-742`), and `get_pixel8` is never called by engine code. Hit testing therefore stays in game pixels for free.
4. **What breaks under an S-times main buffer** is a short list:
   - code that reads raw main-buffer pixels: `mini_screenshot`, `ImageBufferPaintable`, the paletted screenshot, the scalers' use of `ibuf->line_width`, and `scale_layer_color`;
   - code that treats the buffer extents (`get_full_width`/`get_start_x`/`get_end_x`) as game coordinates: HUD gumps, `Gump::isOffscreen`, fast-mouse clipping and the dirty rects;
   - the `Image_window` design in which `ibuf` means "current render target", not "the main buffer".
5. **Recommendation for milestone 1:** keep all UI at 1x on its own layers (it already is). Make the main world buffer a **scaled buffer with a logical (game-pixel) API**: storage is S times larger, while coordinates, clips and extents stay in game pixels. The scale becomes a property of the *target buffer* (`render_scale` on `Image_buffer8`), not a global setting. Layers and scene buffers keep scale 1, so `push_render_target` keeps working unchanged. **Milestone 2:** add an optional per-layer *content scale* (layer buffer k times larger with the same logical size), so the dragged item, container icons, faces, cursor and fonts can also use hi-res overrides.

---

## 1. Rendering architecture in current master

### 1.1 Main buffer
- `Image_window8` is created by `Game_window` (`gamewin.cc:326`) around a single `Image_buffer8` whose pixels live in `draw_surface` (8-bit paletted SDL surface, `imagewin/imagewin.cc:726`: `draw_width = inter_width/scale + 2*guard_band`).
- `create_surface` sets `ibuf->width/height` to the *full* draw area minus the guard band. It sets `offset_x/y` so that game coordinate (0,0) is the top-left of the *game area*, and points `bits` at that origin (`imagewin.cc:540-578`). The full buffer can be **larger than the game area** in Fit/AspectCorrect modes (`get_draw_dims`, `imagewin.cc:1315-1436`), giving `get_start_x() < 0` and band regions. `Game_window::paint` fills those bands with the border colour (`gamerend.cc:371-384`), and HUD gumps place themselves into them.
- Accessors: `get_game_width/height` is the game area, or the scene size in scene mode (`imagewin.h:640-646`). `get_full_width/height` and `get_start_x/y`/`get_end_x/y` are the **current target buffer's** extents (`imagewin.h:624-654`).
- Presentation: `show()` runs the scaler from `draw_surface` to `inter_surface`, then the fill scaler to `screen_texture`, then `UpdateRect` (`imagewin.cc:882-1117`). `UpdateRect` renders `screen_texture_a` to the window and then calls **`composite_layers()`** (`imagewin.cc:2170-2219`). The renderer uses SDL logical presentation sized to the window (`imagewin.cc:662,670`). "Display coords" (`display_width/height`, `imagewin.cc:672`) are those logical render coordinates.

### 1.2 Layers (the UI compositor)
- `Image_window::Layer` (`imagewin.h:175-251`) holds:
  - its own 8-bit SDL surface plus an `Image_buffer8` with a guard band (`create_layer`, `imagewin.cc:1575-1599`);
  - a logical size `logw/logh` (equal to the buffer size today);
  - a transparent index, `z`, `visible`/`dirty`, an optional explicit `dest` rect list in **display coords**;
  - a `UiLayerKind`, `alpha`, and an `index_argb` override table for translucency.
- Kinds (`imagewin.h:70-88`): Default, Conversations, MousePointer, Gumps, HudGumps, TextGumps, ModalGumps, DisplayMap, TextEffects, FullScreenScene, FullScreenBilinear and FullScreenPoint. Each kind has a `UiLayerConfig` (width/height, scaler, fill mode, fill scaler, fixed palette). The config comes from `config/video/ui/*` in `apply_ui_layer_config` (`exult.cc` ~2915-3100; default 420x263 Fit). Width/height 0x0 means "Auto = game area size" (`imagewin.cc:1927-1945`).
- Composite (`imagewin.cc:2091-2168`):
  1. Filter by `ui_layer_kind_mask` and sort by `z`.
  2. Lazily create an ARGB streaming texture of size `(logw+2gb+2)*render_scale x (logh+2gb)*render_scale`.
  3. When dirty, convert 8-bit to ARGB with the **live palette** (or a fixed UI palette) in `Image_window8::refresh_layer` (`iwin8.cc`), optionally pre-scaled by the UI scaler (`layer_render_scale`, `scale_layer_color`, `imagewin.cc:1713-1781`).
  4. `SDL_RenderTexture` to each `dest` rect, or to `compute_layer_fill_dest` by kind.
- Palette changes and rotations mark **all** layers dirty (`iwin8.cc:119`, `iwin8.cc:173`). Layers therefore follow day/night tints and cycling, and they never share pixels with the world buffer.
- `screen_to_layer` / `layer_to_screen` (`imagewin.cc:2050-2079`) map display coordinates through the layer's dest rect.

### 1.3 Render-target redirection
- `Game_window::push_render_target(buf)` / `pop_render_target(prev)` (`gamewin.cc:541-550`) switch **two** globals:
  - `Image_window8::ib8` **and** `Image_window::ibuf` (`iwin8.h:63-68`, `set_render_buffer`);
  - the static `Shape_frame::scrwin` (`shapes/vgafile.h:55,69-71`), used by every `Shape_frame::paint_*(x,y)` overload without an explicit buffer (`vgafile.h:98-120`).
- **Fonts ignore their `win` argument.** `Font::paint_text` and `paint_text_fixedwidth` call `ignore_unused_variable_warning(win)` (`shapes/font.cc:260,299,478,519`) and draw glyphs through `Shape_frame::paint_rle(x,y)`, which goes to `scrwin`. `paint_text_box` sets the clip on the passed `win` (`font.cc:97-99`) but the glyphs go to `scrwin`. Text therefore always goes to the current render target, and callers that pass `gwin->get_win()->get_ib8()` (menulist, txtscroll, playscene, exultmenu, `shapeid.cc:505-521`) work because that *is* the current target.
- **Design hazard.** `Image_window` has no separate pointer to "the main buffer": `ibuf` is the current target. `show()`, `create_surface`, `free_surface`, the guard-band helpers and the scalers all use `ibuf`:
  - `show()` uses `ibuf->clip`, `get_full_width` and `ibuf->line_width`, and the scalers read `ibuf->line_width` (e.g. `imagewin/scale_point.cc:42`, `scale_hq2x.cc:58`);
  - `scale_layer_color` temporarily overwrites `ibuf->line_width`/`height` (`imagewin.cc:1761-1777`);
  - `show()` detects "a scene is redirected" by comparing the target size to `draw_surface` (`imagewin.cc:967-968`).

  A hi-res buffer whose physical size differs from its logical size makes this coupling fragile. Add an explicit `main_ibuf` member, and give the scalers the physical pitch of `draw_surface`.

---

## 2. Main buffer vs. layers: inventory

### 2.1 Drawn on separate layers (independent of world resolution)

| Consumer | Where | Layer kind / z | Buffer size |
|---|---|---|---|
| Mouse cursor | `mouse.cc:198-277` (`ensure_mouse_layer`, `draw_cursor_to_layer`, `position_mouse_layer`) | MousePointer, z `1<<20` (`mouse.cc:196`) | max extent of pointers.shp frames (`mouse.cc:151-186`) |
| Dragged object | `drag.cc:317-404` | MousePointer kind, z `(1<<20)-1` (`drag.cc:298`) | frame size, 1x |
| Dragged gump | `drag.cc:448` calling `render_gump_to_layer(gump, 1<<19)` | same as gumps | gump bounds |
| All gumps (containers, paperdolls, stats, spellbook, modal menus, options, Yes/No, sliders, item menu buttons, ...) | `Gump_manager::render_gump_part_to_layer` `gumps/Gump_manager.cc:143-285`, called from `render_gumps_to_layer` `:321-337` | Gumps / HudGumps / TextGumps / ModalGumps, z `1<<18` + tier + index (`:83-86`) | `g->get_dirty()` + 4 px margin, 1x |
| HUD gumps (ShortcutBar, Face_stats, 1 or 2 parts) | same, edge-anchored (`Gump_manager.cc:208-255`) | HudGumps | 1x |
| Text gumps (Scroll/Sign/Book) | `gumps/Text_gump.cc:33-85` | TextGumps, z `(1<<18)+(1<<16)+1` (`Text_gump.h:56`) | gump rect, centred on display |
| Floating text (`Text_effect`) | `effects.cc:1163-1215` | TextEffects, z `(1<<18)+(1<<16)` (`effects.cc:71`) | text size |
| Conversation faces + text + choice bg | `usecode/conversation.cc:218-330`, `590-640`, `760-830` | Conversations, z 0 / -1 | **fixed 320x200** (`conversation.cc:52-53`), placed by `compute_ui_layer_dest` (`:267-271`) |
| Display map / cheat map | `usecode/intrinsics.cc:1478-1515`, `cheat.cc:1255-1300` | DisplayMap, z `(1<<19)+1` | map shape size |
| Full-screen scenes (`Scene_layer`/`Scene_view`, `scene_layer.h`) | Intro/endgame/credits/quotes/new game: `gamemgr/bggame.cc:406,1506,1554,1955,1962,1972`, `gamemgr/sigame.cc:278,886,1059,1362,1369,1379`. Main menu `game.cc:584`. Exult menu `exultmenu.cc:389,514,540`. Cheat screen `cheat_screen.cc:135`. Scripted scenes `playscene.cc:938`. Shape browser `browser.cc:144`. SI load screen `gamewin.cc:3172` | FullScreenScene, z `1<<19` | 320x200 or 360x225 |
| Perf overlay | `perf.cc:89,105` | FullScreenBilinear, z INT_MAX | 960x720 / 256x20 |
| Sound test | `audio/soundtest.cc` (draws into a Scroll gump's layer) | TextGumps | - |

Inside a `Scene_view`, `Game::topx/topy/centerx/centery` and `ibuf` are rewritten to scene-relative values, and scene mode makes `get_game_width/height` report the scene size (`scene_layer.h:203-237`, `imagewin.h:839-850`). Intro and menu code therefore uses `win->get/put`, `get_full_width`, `get_end_x`, etc. on the **scene buffer**, not the world buffer. That includes the bggame backups at `gamemgr/bggame.cc:514-1294`, `SDL_SurfaceOwner(src->get_bits())` at `bggame.cc:693-701`, FLI `win->put` at `flic/playfli.cc:338,363`, and TextScroller at `txtscroll.cc:236-311`. None of these are affected by enlarging the world buffer.

### 2.2 Drawn into the main buffer (world view) today
1. World terrain and objects: `Game_window::paint` calls `render->paint_map` (`gamerend.cc:328-362`), including the chunk flats cache `copy8(cflats->get_bits(),...)` (`gamerend.cc:529`).
2. World effects (sprites, projectiles, weather, lightning): `effects->paint()` (`gamerend.cc:367`, `effects.cc:286-290`).
3. Border bands outside the game area: `gamerend.cc:371-384`.
4. **Modal background snapshot:** `BackgroundPaintable`/`ImageBufferPaintable` painted by `render_gumps_to_layer(true)` (`Gump_manager.cc:321-326`). It is captured with `iwin->get()` of the full buffer (`shapeid.cc:643-654`) and used for the cheat-screen quit dialog (`cheat_screen.cc:781`).
5. **Dragged object, legacy path:** in map-edit, bbox-debug or while `dropping`, the object is painted straight into the main buffer (`drag.cc:418-440`).
6. **Item-menu outlines** of world objects: `Itemmenu_gump::Outline_painter` passed as the `Paintable` of the modal loop (`gumps/ItemMenu_gump.h:114-125`, `ItemMenu_gump.cc:177-185`).
7. Map-edit overlays: `Move_grid` green footprint (`exult.cc:3358-3369`), bbox (`gamerend.cc:609-617`), tile grid (`gamerend.cc:180,283`), egg debug lines (`objs/egg.cc:1069-1082`).
8. Crystal ball `UI_display_area`: world plus sprite 10 in the main buffer (`usecode/intrinsics.cc:1791-1840`).
9. Load plasma (`gamewin.cc:2982-3002`, from `setup_load_palette` `:3161`) and `clear_screen` (`gamewin.cc:489-497`).
10. **Stray gump paints:** gumps call `paint()` from event handlers. The current target at that moment is the main buffer, so they leave unscaled duplicates that the next full repaint overwrites. Master already works around this: `do_modal_gump` forces a full repaint after events (`Gump_manager.cc:1101-1111`), and `Notebook_gump` redirects to a scratch buffer (`gumps/Notebook_gump.cc:485-520`).
11. Fallback when layer creation fails: the cheat screen paints into the window (`cheat_screen.cc:141-150`).

No gump currently opts out of layers: `Gump::uses_render_layer()` defaults to `true` (`gumps/Gump.h:261-263`), and the only overrides also return `true` (`ShortcutBar_gump.h:84`, `Face_stats.h:69`).

---

## 3. Coordinate systems and input flow

| Space | Definition | Conversions |
|---|---|---|
| Window/OS | SDL event coords | `SDL_ConvertEventToRenderCoordinates(renderer,&event)` at every event site (`exult.cc:1793,1932,2025,...`; `Gump_manager.cc:885-986`; `menulist.cc:330`; `cheat_screen.cc:851`; `bggame.cc:2088`; `sigame.cc:1457`; `intrinsics.cc:2581`) |
| Display | renderer logical coords, `display_width x display_height` | - |
| Game | game pixels relative to game-area origin (0..game_w); the full buffer may extend to negative and beyond | `Image_window::screen_to_game` (`imagewin.cc:1277-1294`): scene mode goes via `screen_to_layer(active_scene_layer)`; fastmouse is 1:1 plus `Mouse::fast_offset`; otherwise `gx = sx*inter_width/(scale*display_width) + start_x`. Inverse is `game_to_screen` (`:1296-1313`) |
| Layer | layer-buffer pixels | `screen_to_layer` / `layer_to_screen` through the dest rect (`imagewin.cc:2050-2079`) |
| Gump-local | game coords of the gump as painted in its layer | `Gump_manager::map_game_to_gump` (`Gump_manager.cc:287-319`): game to display to layer, then `+layer_bounds` (handles 2-part HUD) |
| Scene | scene buffer pixels (320x200 / 360x225) | `Scene_view::screen_to_scene` (`scene_layer.h:276-284`), `CheatScreen::screen_to_cheat` (`cheat_screen.cc:87-93`) |
| Persisted gump position | **display** coords of the gump centre (`Container::setGumpXY`) | written in `Gump::~Gump` via `game_to_screen` (`gumps/Gump.cc:112-138`), read in `Gump::Gump` via `screen_to_game` (`Gump.cc:49-75`) |

Main-loop flow (`exult.cc`):
- **Button down** (`:1791-1810`): `screen_to_game`, then `gump_man->find_gump(x,y)`, then `map_game_to_gump` and `gump->on_button(gx,gy)`. If no gump is hit, the world gets the click.
- **Double-click** (`:1292-1295`): `find_object(x,y)` in game coords.
- **Motion** (`:2024-2039`): `screen_to_game`, then `Mouse::move(mx,my)`. `move` stores game coords, clamps for fastmouse using `get_end_x/y` (`mouse.cc:321-346`), and repositions the cursor layer through `game_to_screen` and `get_pointer_scale` (`mouse.cc:233-256`).
- **Modal gumps:** `handle_modal_gump_event` (`Gump_manager.cc:869-1000`) does the same, and drag motion uses raw game coords (`sync_drag_anchor`).
- **Conversation choices:** `conversation_choice(x,y)` goes game to display to conv layer, then `conv_choices[i].has_point` (`conversation.cc:868-886`).
- **World picking:** `Game_window::find_object` (`gamewin.cc`, around `Game_object* Game_window::find_object`) iterates chunks, applies `get_shape_rect(obj).has_world_point`, then `Shape_frame::has_point(x-ox, y-oy)` on **RLE data** (`shapes/vgafile.cc:706-742`). `Gump::has_point` uses the same routine plus widget rects (`Gump.cc`, `Gump_widget.cc:36-41`). `get_pixel8` exists (`ibuf8.h:99`, `iwin8.h:143`) but **no engine code calls it**.
- **Touch:** `TouchUI::setTextInputArea` uses `game_to_screen` then `SDL_RenderCoordinatesToWindow` (`touchui.cc:51-62`).
- **Exult Studio:** `Send_location` sends the scroll position, `get_width()/c_tilesize`, `get_height()/c_tilesize` and `get_scale_factor()` (`gamewin.cc:1028-1044`), all in tiles. Studio drag-and-drop goes through `SDL_EVENT_DROP_*`, `screen_to_game` and `Move_grid` (`exult.cc:2180-2320, 3315-3370`). The protocol (`server/server.cc`) is tile-based and never touches the framebuffer.

**How layers are placed:**
- Gump layers: dest centre = `game_to_screen(gump centre)`, size = `b.w * get_ui_scale_factor(kind)` (`Gump_manager.cc:203-209`). HUD gumps instead anchor to display edges, with the edge detected in **buffer extents** (`:213-255`).
- Text effects: the same pattern (`effects.cc:1203-1213`).
- Dragged item: scaled to the world's native display-per-game-pixel ratio, computed with `game_to_screen(0,0)` and `game_to_screen(gamew,gameh)`, and enlarged to the pointer scale (`drag.cc:362-403`).
- Conversation and text gumps: centred by UI config (`conversation.cc:267-271`, `Text_gump.cc:76-84`).

Consequence: once `game_to_screen` is correct, every UI layer lines up with whatever resolution the world is shown at.

---

## 4. Raw pixel access, save/restore and "buffer = game" assumptions

### 4.1 Raw memory consumers (`get_bits`, `bits`, `line_width`, `get`/`put`)
| Site | What it does | Main buffer? |
|---|---|---|
| `imagewin/iwin8.cc:181-224` `mini_screenshot` | reads `ibuf->get_bits()`/`get_line_width()` and averages 3x3 over the central 288x180 **game px**, using `get_game_width/height` offsets | **yes**, breaks at S>1 |
| `gamewin.cc:3126-3140` `create_mini_screenshot` | repaints the map into the main buffer, then calls the above | yes |
| `shapeid.cc:643-654` `ImageBufferPaintable` | `create_buffer(get_full_width(), get_full_height())` + `iwin->get/put` | yes (current target) |
| `imagewin.cc:1254-1256` `screenshot(paletted)` | saves `draw_surface` (world only; gumps are layers and are already missing) | yes |
| `exult.cc:2890-2905` `--buildmap` | `paint_map_at_tile` + paletted screenshot | yes |
| scalers `imagewin/scale_*.cc` | read `draw_surface->pixels` with `ibuf->line_width` | yes (physical) |
| `imagewin.cc:1738-1781` `scale_layer_color` | temporarily rewrites `ibuf->line_width/height` | mutates the current target |
| `gamerend.cc:529` | `copy8(cflats->get_bits(),128,128,...)` chunk flats cache to main | yes (world subsystem) |
| `gamemap.cc:1706` | builds the flats cache | separate buffer |
| `gamemgr/bggame.cc:514-1294`, `693-701`; `flic/playfli.cc:116,338,363` | backups / FLI frame put | scene buffer, unaffected |
| `mouse.cc:177` | allocates `backup` (legacy, **no longer used**: the cursor is a layer) | main-format buffer, harmless |

**Mouse backing store:** none in practice. The cursor layer is composited, `box`/`dirty` are still tracked in game coords, and `blit_dirty` calls `iwin->show(dirty...)` with a **game-coordinate rect** (`mouse.h:179-181`). **Gump drag:** no save/restore; the dragged gump is re-rendered into a layer.

### 4.2 Code that treats buffer extents as game coordinates
- `Game_window::get_full_rect`, `clip_to_win`, `set_all_dirty` (`gamewin.h:241-243, 257-260, 779`): dirty rects live in game coords but are clipped to buffer extents.
- HUD anchoring uses `iwin->get_start_x/y()` and `get_end_x/y()` against gump content in game coords (`Gump_manager.cc:219-222`).
- `ShortcutBar_gump::createButtons` (`ShortcutBar_gump.cc:127-131, 293`) and `Face_stats::create_buttons` (`Face_stats.cc:450-503`) use `get_full_width`/`get_start_x`/`get_end_y` to lay out in game coords.
- `Gump::isOffscreen` (`Gump.cc:583-598`) mixes gump rects in game px with full-buffer extents.
- `Mouse::move` fastmouse clamp (`mouse.cc:329-330`).
- `clear_screen` and plasma (`gamewin.cc:489-497, 2982-3002, 3161`).
- `ExultMenu::calc_win` uses `get_full_height` (`exultmenu.cc:210`); this is inside a scene, so it is unaffected.
- `Image_window::get_ui_width/height` Auto equals `game_width` (`imagewin.cc:1927-1945`). If `game_width` became physical, every Auto-sized UI kind would shrink S times.
- `Notebook_gump` scratch buffer is sized `gwin->get_width() x get_height()` (`Notebook_gump.cc:504-509`).

---

## 5. Impact assessment if the game-view buffer becomes S times larger

Two implementation variants are possible. The assessment differs sharply between them.

### Variant P: "physical" (buffer and all its accessors in S-scaled pixels; game code converts)
- Every item in section 4.2 breaks: HUD layout, `isOffscreen`, dirty rects, mouse clamp, Auto UI size, `clear_screen` extents.
- `screen_to_game` would return physical px, so `find_object`, `find_gump`, gump layer placement, the conversation hit test and cursor placement would all be off by S unless every call site divides.
- Hundreds of world-render call sites would need `*S`.
- **Not recommended.**

### Variant L: "logical API, scaled storage" (recommended)
`Image_buffer8` gains `render_scale` (default 1). Its public API (`fill8`, `copy8`, `put_pixel8`, `fill_translucent8`, `draw_line8`, `set_clip`, `is_visible`, `get_width/height`, offsets) stays in game pixels and multiplies internally. `Shape_frame::paint_*(win,...)` checks `win->render_scale` to choose a hi-res override or a nearest-neighbour expand. `Image_window` keeps `get_game_*`, `get_full_*` and `get_start/end_*` logical, and adds physical accessors for `show()`, the scalers and screenshots.

- **Unaffected (works as-is):**
  - all layers in 2.1 (they are separate 1x buffers, and `push_render_target` swaps `ib8`/`scrwin` to a scale-1 buffer);
  - all `Scene_view` intro, menu, cheat and browser code, including bggame `get/put` and FLI;
  - fonts (they follow `scrwin`);
  - mouse and cursor layer;
  - gump/conversation/map hit testing;
  - `find_object` / `has_point`;
  - Exult Studio protocol and touch UI;
  - HUD layout, `isOffscreen` and the dirty-rect clamps (they stay logical);
  - Auto UI size.
- **Must be adapted:**
  1. `Image_window::screen_to_game`/`game_to_screen` (`imagewin.cc:1277-1313`). The non-fast formula uses `inter_width/scale` as "logical full width". Once the buffer is S times larger and `scale`/`inter_width` change meaning (e.g. scaler scale 1, inter = S·W), it must be rewritten in logical terms. **This is the single most important touchpoint for UI correctness.**
  2. `show(x,y,w,h)` receives logical rects (`Mouse::blit_dirty`, `Game_window::show`) and must convert them to physical. The scene-detection size check (`imagewin.cc:968`) must compare physical sizes.
  3. Scalers must use the physical pitch of `draw_surface`, not `ibuf->line_width` (the current target).
  4. `scale_layer_color` must stop mutating `ibuf`; use a local descriptor or the main buffer explicitly.
  5. `mini_screenshot` must average (3S)x(3S) blocks or sample, or (cleaner) `create_mini_screenshot` should paint into a temporary **1x** target via `push_render_target`.
  6. `ImageBufferPaintable`, `Image_window::create_buffer`, `get` and `put`: `create_buffer` must create a buffer with the **same render_scale as the current target**, and `get`/`put` must copy physical rows. Otherwise the modal background snapshot is wrong or overruns.
  7. Paletted screenshot and `--buildmap` produce S-times images. That is acceptable, or optionally a feature; document it.
  8. `free_surface`/`create_surface`/`resized` must operate on an explicit main-buffer pointer, never on "current target".
- **Visual-only mismatches (acceptable in milestone 1):**
  - The dragged item (`drag.cc:317-404`), container/paperdoll/shortcut-bar icons, conversation faces, cursor, fonts and the display map stay at original art and are upscaled by the GPU, while the world shows hi-res overrides.
  - Item-menu outlines and map-edit grid/egg/bbox lines drawn in the S-times buffer become S-pixel-thick nearest-neighbour lines.
  - The legacy dragged-object path (map edit / drop) draws into the main buffer, so it *would* use hi-res art there.
- **Performance:**
  - In gump mode, modal loops (`Gump_manager.cc:1101-1111`) and many event handlers call `gwin->paint()`, a **full world repaint**, on every event or frame. At S=6 that is 36 times the fill work for UI-only changes. UI now lives in layers, so a "UI-only dirty" path that skips the world repaint would pay off.
  - Palette rotation re-converts all layers (`iwin8.cc:173`); that is cheap at 1x and unchanged.

### Should UI stay at 1x in its own layers for milestone 1?
**Yes.** It already is: layers are composited at display resolution independent of the world scaler, so UI sharpness does not depend on S. Moving UI into the S-times buffer would undo master's layer work and break the per-kind UI scale and fixed-palette features. Hi-res UI art comes later through a per-layer content scale (section 6, milestone 2), not by drawing UI into the world buffer.

---

## 6. Recommended design hooks

### Milestone 1 (world hi-res, UI untouched)
- Give `Image_buffer8` a `render_scale` field and logical-coordinate primitives. The main world buffer gets S; `create_layer` buffers and `Scene_layer` buffers keep 1.
- Override selection happens inside `Shape_frame::paint*` and `Shape_manager::paint_shape` (`shapeid.h:171-183`) based on `win->render_scale`. Note that `paint_shape` receives a bare `Shape_frame*` and does not know (file, shape, frame), so the override pointer should hang off the `Shape_frame` at load time. Font glyphs (`fonts.vga`) and pointers (`pointers.shp`) are ordinary `Shape_frame`s, so the same hook covers them later.
- `Image_window`:
  - add `main_ibuf`;
  - keep `get_game_*`/`get_full_*`/`get_start_*` logical;
  - add physical accessors for `show`, the scalers, guard band and screenshots;
  - rewrite `screen_to_game`/`game_to_screen` in logical units;
  - make the fast-mouse path keep 1 display px = 1 game px, or 1/S if desired.
- Fix the raw consumers listed in section 5 (mini screenshot, `ImageBufferPaintable`, `create_buffer`/`get`/`put`, `scale_layer_color`, scalers' pitch).
- Video options: add a "world render scale / hi-res art" setting (`VideoOptions_gump::save_settings`, `gumps/VideoOptions_gump.cc:543-605`, calling `gwin->resized`, `setup_video`, config `config/video/*`). Make sure `Game_window::resized` re-points `Shape_frame::set_to_render` at the new main buffer (`gamewin.cc:918-921`).

### Milestone 2 (hi-res UI content where useful)
- Add `content_scale k` to `Layer`. The buffer is `(logw·k) x (logh·k)` with `render_scale = k`, `logw/logh` stay logical, and the composite `src` rect uses physical size (`imagewin.cc:2149-2150`). `screen_to_layer` keeps returning logical coords. All gump, mouse and conversation hit testing stays unchanged.
- Candidates:
  - dragged-item layer with k=S, so it matches the world;
  - gump layers (container icons) with k = ceil of the UI scale factor;
  - conversation layer (faces.vga overrides);
  - mouse layer (pointer overrides);
  - text effects and fonts (font overrides).
- Note: `index_argb` translucency tables and `layer_render_scale` pre-scaling operate per buffer pixel and work unchanged.

---

## 7. Touchpoints (for the design doc)

| Where | What | Change needed |
|---|---|---|
| `imagewin/imagewin.cc:1277-1313` | `screen_to_game` / `game_to_screen` | Express in logical game px independent of the S-times storage; this is the hub for all UI hit tests and layer placement |
| `imagewin/imagewin.h:620-654`, `imagewin.cc:540-578` | `get_full_*`, `get_start_*`, `get_end_*`, `create_surface` | Keep logical; add physical accessors; set up a scaled main buffer |
| `imagewin/iwin8.h:55-72`, `gamewin.cc:541-550` | `set_render_buffer` / `push_render_target` (`ib8`, `ibuf`, `Shape_frame::scrwin`) | Keep; scale must come from the target buffer; add explicit `main_ibuf` |
| `imagewin/imagewin.cc:882-1117` | `show()` | Convert logical dirty rects to physical; fix the size check at `:968`; scalers use the physical pitch |
| `imagewin/scale_*.cc` | scalers use `ibuf->line_width` | Use the `draw_surface` pitch / main buffer |
| `imagewin/imagewin.cc:1738-1781` | `scale_layer_color` mutates `ibuf` | Stop mutating the current target |
| `imagewin/iwin8.cc:181-224`, `gamewin.cc:3126-3140` | `mini_screenshot` | Sample S-times buffer correctly or render a 1x pass |
| `shapeid.cc:643-654`, `imagewin.cc:848-852,943-950` | `ImageBufferPaintable`, `create_buffer`, `get`/`put` | Scale-aware snapshot (same render_scale as source) |
| `imagewin/imagewin.cc:1254-1256`, `exult.cc:2890-2905` | paletted screenshot / buildmap | Accept S-times output or downsample |
| `shapes/vgafile.h:55-120`, `shapeid.h:171-183` | `Shape_frame::paint*`, `Shape_manager::paint_shape` | Hook for override / NN upscale based on `win->render_scale` |
| `shapes/font.cc:255-330, 472-545` | `Font::paint_text*` ignore `win` and use `scrwin` | Fine for M1; for font overrides the same Shape_frame hook applies |
| `gumps/Gump_manager.cc:143-285, 287-319` | gump layer render, placement, `map_game_to_gump` | None for M1; M2 optional content scale |
| `gumps/Gump_manager.cc:321-326, 1055-1130` | modal background (main buffer) + full repaint per event | Scale-aware snapshot; consider a UI-only repaint path for performance |
| `drag.cc:317-404, 413-448` | dragged item layer (1x, native world scale) / legacy main-buffer path | M2: content scale S for overrides |
| `mouse.cc:198-256, 321-346` | cursor layer, game-coord mouse, fastmouse clamp via `get_end_x` | None if extents stay logical |
| `gumps/ShortcutBar_gump.cc:127-131,293`, `gumps/Face_stats.cc:450-503`, `gumps/Gump.cc:583-598` | layout and offscreen tests in buffer extents | None if extents stay logical |
| `usecode/conversation.cc:218-330, 868-886` | 320x200 conversation layer + choice hit test | None for M1; M2 content scale for face overrides |
| `effects.cc:1163-1215` | text effect layers | None |
| `scene_layer.h`, `cheat_screen.cc:87-150`, `game.cc:584`, `exultmenu.cc:389-575`, `gamemgr/*.cc`, `playscene.cc:938`, `flic/playfli.cc` | full-screen scene layers at 1x | None (keep scene buffers at scale 1) |
| `gumps/ItemMenu_gump.cc:177-185`, `exult.cc:3358-3369`, `objs/egg.cc:1069-1082`, `gamerend.cc:180,283,609-617` | world-overlay debug/outline drawing into main buffer | Works with logical primitives (S-px-thick lines); optionally thin physical lines |
| `gamewin.cc:2982-3002, 3161`; `gamewin.cc:489-497` | plasma / `clear_screen` on main buffer | Works with logical primitives |
| `gumps/VideoOptions_gump.cc:543-605`, `gamewin.cc:918-921`, `exult.cc:3128` | resize / video settings | Add render-scale option; re-point the render target after resize |
| `gamewin.cc:1028-1044` | Exult Studio `view_pos` sends `get_scale_factor()` | Decide what scale to report (Studio uses tiles; low risk) |

---

## 8. Risks and open questions

**Risks**
- `ibuf` doubles as "current render target" and "main buffer". Any resize, scaler or guard-band call while a layer is pushed corrupts the layer. A scaled main buffer increases the chance of physical/logical mix-ups.
- Event handlers that paint gumps directly hit the main buffer. Harmless today (overwritten), but with scaled primitives they also trigger override lookups for gump art.
- Full world repaints on every UI event become 36 times more expensive at S=6 (modal loops, gump mode, drag).
- Hi-res override silhouettes may differ from the 1x RLE used by `has_point` picking. This is acceptable (picking in game px) but may feel off at edges.
- Visual mismatch between hi-res world objects and their 1x icons in gumps, on the dragged-item layer and in the item-menu outlines.
- If true-colour overrides are chosen instead of paletted ones, the main buffer can no longer be 8-bit. UI layers are unaffected (separate 8-bit buffers following the palette), but the world/UI palette-fade interplay must be redone.

**Open questions**
- Should fast-mouse (fullscreen) keep 1 display px = 1 game px, or allow sub-game-pixel cursor precision?
- Should `get_scale_factor()`, which Studio reads, report the scaler scale or S?
- Should the paletted screenshot and `--buildmap` output S-times images (a nice feature) or stay 1x?
- For milestone 2, what content scale should gump layers use: S, `ceil(ui_scale_factor)`, or configurable?
- Should a UI-only repaint path (without re-rendering the world) be added in milestone 1 for performance?
