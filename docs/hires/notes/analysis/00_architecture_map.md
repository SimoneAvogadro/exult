# 00: Consolidated architecture map for the hi-res (S×) render scale

Repo: `/home/simonea/ultima7_exult/exult-hires`, upstream master `8b6ab6b43` (C++17, SDL3). Local SDL is 3.4.18 (`deps/prefix`); upstream CI pins 3.2.14.
Every `file:line` below refers to that tree unless another path is given.

Inputs: `ibuf.md`, `present.md`, `shapes.md`, `world.md`, `ui.md`, `build.md`, `palette.md`, `gap_1.md` … `gap_7.md` (same directory). I read all of them in full.
The synthesizer re-checked every core path cited here in code: show(), create_surface/create_scale_surfaces, the guard-band functions, screen_to_game, Shape_manager/ShapeID dispatch, cache_shape, Shape::read, encode_rle, get_rle_shape, the Chunk_terrain cache, paint_chunk_flats, Game_window::paint/paint_map, the main loop, the render-target redirection, mini_screenshot, ImageBufferPaintable, the xform loader, and rotatecolours.
Section 6 lists the contradictions between the reports and how each one was settled.

Terminology:
* **Game px**: logical game pixels, 320x200 for the classic view.
* **S**: render scale; the goal is S = 6.
* **Physical px**: game px × S.
* **S_art**: the scale the override art is authored at (6).
* **S_eff**: the scale the engine actually uses (§0 D5).

---

## 0. Decisions at a glance (reconciled target architecture)

| # | Decision | Why |
|---|---|---|
| D1 | **S belongs to the render target.** `Image_buffer8` gets an `int scale`. The main world buffer gets `S_eff`. UI layers, scene layers and scratch buffers keep 1. | `push_render_target` swaps both `ib8/ibuf` and the static `Shape_frame::scrwin` (gamewin.cc:541-550, iwin8.h:63-68). A global S would break the 1x layers. It would also rule out the in-process A/B golden test (gap_7). |
| D2 | **Logical API, physical storage (variant a′ / "L").** All `Image_buffer8` coordinates, clip, `width/height/offset_x/offset_y` stay in game px. Only `bits` and `line_width` describe physical memory. The physical dims are logical × S. Each primitive branches `if (scale == 1) {today's code} else {scaled code}`. | About 600 call sites, all hit-testing, dirty rects, HUD layout, Auto UI size and `get_win_tile_rect` assume game px (world.md §9, ui.md §4.2). The scale==1 path stays byte-identical for ExultStudio and the tools that link `ibuf8.cc`. |
| D3 | **One main buffer and no dual write.** The persistent window `ib8` (created once, iwin8.cc:64) is **mutated in place**. When S>1 its memory is `draw_surface`, allocated at `full×S` (+4 px guard). The software scalers, phase 1, phase 2 and guard-band painting **never run on the world when S>1**. | Pointers to `ib8` are cached in `Shape_frame::scrwin`, `Game::ibuf`, `Cheat_screen::ibuf` and `ExultMenu::ibuf`. Painting has game-logic side effects (`Npc_actor::paint`, actors.cc:5133-5148), so the world must not be rendered twice. Because the scalers are bypassed, their 122 `ibuf->` reads (scale_*.cc) need no change in M1. |
| D4 | **New present path when S>1.** In `show()`, if `render_scale>1` and `ibuf == main_ibuf`, the logical dirty rect is multiplied by S and uploaded with `SDL_UpdateTexture` into a `world_texture`. `UpdateRect` then draws `world_texture` into the **same logical display rect** that `screen_texture_a` covers today. Texture format: baseline is ARGB8888 plus a 256-entry LUT, which works on SDL 3.2. When the build has SDL ≥ 3.4 (`SDL_VERSION_ATLEAST(3,4,0)`) and the renderer accepts it, INDEX8 plus `SDL_SetTexturePalette` is used instead. | `screen_to_game`/`game_to_screen` use `inter_width/scale` (imagewin.cc:1291-1292, 1310-1311), which equals the logical full width and does not depend on how many pixels the world has. So every layer, gump, cursor and hit test keeps working unchanged. |
| D5 | **S_eff is derived, not fixed.** `compute_render_scale()` is a pure static next to `get_draw_dims`. It takes `p = max(display/full)` per axis, computes `S_need = ceil(p)`, snaps up to a divisor of S_art (1, 2, 3, 6), and caps the result by `config/video/hires/render_scale`, by `SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER` and by a memory budget. S_eff = 1 means today's pipeline. | The user's windowed profile (1920x1200, 320x200, Fit) gives **S=6 at 1:1**. A fixed S=6 would give 5160x1800 on the active 3440x1440 ACF profile. With Studio zoom k=1 it would give 11520x7200, and with Fill x1 it would exceed 16384 and crash fatally (gap_2 §3). |
| D6 | **Frames without an override use on-the-fly nearest-neighbour inside the primitives.** Frames with an override are drawn with the **existing** painters through a *physical view* of the same buffer at `(x·S, y·S)`. | Measured at -O2 for a 320x200 view at S=6: 0.32–1.33 ms with NN on the fly, 0.37–1.57 ms with S²-sized data. The NN path costs no memory and causes no hitch (gap_4). The RLE format allows any size, so the painters can be reused verbatim. |
| D7 | **Terrain, milestone 1.** `Chunk_terrain::render_flats` always builds the exact, zero-filled 1x render (after the engine fixes). When S>1 it then composes a `(128·S)²` cache in this order: (1) a per-terrain override keyed by the **FNV-1a-64 content hash** of the 1x render; (2) per-tile overrides keyed by `(shape, frame&31)` and checked against the 64-byte source; (3) NN expansion. The cache is checked against S and a generation counter, can be flushed, and is sized from the view plus a byte budget. | Per-tile art alone leaves about 80% of tile edges as seams, and per-terrain art alone leaves a 768-px grid (gap_1). Terrain numbers and the `modified` flag are not stable identities. The fixed 100-entry LRU costs 59 MB and thrashes above 100 chunks (gap_2, gap_4). |
| D8 | **Sprites, milestone 2.** `Shape_frame` gets a hi-res slot. `Vga_file::get_shape` only *stamps the identity* (store, logical shape, frame; `frame&31` for flats) and does no I/O. The payload is resolved **on the paint path only**, then kept in a byte-budget LRU that evicts only at a frame boundary. Hi-res reflections are derived by transpose. Groups are activated strictly (all or nothing). A CRC gate rejects stale art. Shipping format is a sparse companion S× VGA; authoring format is raw-index PNG. | The startup frame-0 sweep (shapeid.cc:123-131 → vgafile.h:400-408) would load about 25 MB if attaching in get_shape did I/O (gap_3). There are 95 raw `sman->paint_shape(…, Shape_frame*)` call sites, so a hook keyed on `ShapeID` or the cache would miss them (verified). |
| D9 | **UI, milestone 3.** Layers get a per-layer *content scale* k: the buffer is `logw·k × logh·k` with `Image_buffer8::scale = k`, the logical size is unchanged, and the composite source rect is physical. The same `Shape_frame` hook then covers fonts, gumps, faces, cursors and the dragged item. | The UI already lives in 1x layers composited at display resolution (ui.md §0). |
| D10 | **Tests.** SDL-free doctest unit tests, including a primitive fuzz oracle `phys(S=k) == NN(S=1)`. A `--render-test` region renderer does an in-process A/B through `push_render_target`. Identity and marker overrides give exact oracles. | `--buildmap` is already byte-deterministic headless (build.md §6.3). The in-process A/B is valid only because of D1 (gap_7 §4). |

```
             logical game px everywhere ───────────────────────────────┐
shapes.vga ─► Vga_file ─► Shape_frame (1x, authoritative) ─┐            │
                         └─ hires slot (M2) ─► payload S× │            │
u7chunks ─► Chunk_terrain ─► src1x (16 KiB) ─► flats S× cache (M1)    │
                                                          ▼            ▼
                       Image_buffer8 main (scale=S, logical API) ── bits = draw_surface (full·S, INDEX8)
                                                          │  show(): rect×S, SDL_UpdateTexture
                                                          ▼
                       world_texture (INDEX8+palette | ARGB via LUT) ─► same logical display rect
                       layers (scale 1, later k) ─► composite_layers() ─► SDL_RenderPresent
```

---

## 1. End-to-end data flow of a rendered frame (current master)

### 1.1 Load time: from bytes to frames

1. **Path tags.** `BaseGameInfo::setup_game_paths` sets `<PATCH>` to the active mod's patch dir only, replacing rather than layering over the base patch dir (gamemgr/modmgr.cc:56-85). The per-game tags are registered at modmgr.cc:592-617. `<SERPENT_STATIC>` falls back to `"."` (modmgr.cc:1238-1243).
2. **Palette.** `<STATIC>/palettes.flx`, overridable by `<PATCH>` (palette.cc:141, 366-369). Values are 6-bit, `max_val=63` (palette.cc:57-62).
3. **Shape files.** `Shape_manager::load` (shapeid.cc:150-382) loads, in order:
   * gumps (177), paperdolls (179), BG imports (191-233);
   * sprites (235), faces (257-265), exult.flx (267), game flx (269-271), shortcut bar (273-279);
   * `read_shape_info()`, which calls `shapes.init()` (281, 116-120) and **forces frame 0 of every shape** (123-131 → vgafile.h:400-408);
   * fonts (283-304);
   * xforms (306-360: 17 blends, translucent range 0xEE-0xFE, verified) and `translucency_argb` (362-381).
4. **Vga_file.** `Vga_file::load`/`U7load` (vgafile.cc:1163-1219) keeps one `IDataSource` open per source. A plain path streams through `IFileDataSource`. A flex entry is read whole into RAM through `IExultDataSource`.
5. **Lazy frame fetch.** `Vga_file::get_shape` (vgafile.h:357-377) checks the import table first. Then `Shape::get` (vgafile.h:224-228) calls `Shape::read` (vgafile.cc:855-920), which:
   * walks the sources back to front with whole-shape granularity (866-885);
   * calls `Shape_frame::read` (398-450), which decides RLE vs flat from the data (411-418, 443-449) and reads RLE frames with `get_rle_shape` (456-477);
   * masks flat frame numbers `&31` (912-914);
   * for `frame|32` beyond nframes, builds a transposed reflection (915-918 → 802-824 → `Shape_frame::reflect` 73-126).

   Frames are **never evicted**. They are freed only by reset, load or new_shape.
6. **Frame cache.** `Shape_manager::cache_shape` (shapeid.cc:552-576) maps `(file,shape,frame)` to a raw `Shape_frame*` plus `has_trans`. `has_trans` is set from the TFA bit for shapes.vga and forced true for sprites.vga.
7. **Terrain.**
   * `u7chunks` (`<PATCH>` first) gives 3,072 `Chunk_terrain`s of 256 `ShapeID`s each (gamemap.cc:149-191, chunkter.cc:139-162).
   * `u7map` gives `terrain_map[192][192]`, read per superchunk (gamemap.cc:223-240).
   * `Map_chunk::set_terrain` turns RLE tiles into `Terrain_game_object`/`Animated_object` (chunks.cc:726-740).
   * Flat tiles are baked lazily into a 128x128 `rendered_flats` cache (chunkter.cc:248-268 via `paint_tile` 86-133). The cache is an MRU list hard-coded to **100** entries (234-242).

### 1.2 One main-loop iteration (exult.cc:1348-1524)

1. `Delay()` waits up to 10 ms (gumps/gump_utils.h:41-55).
2. Events are polled. Mouse coordinates go through `SDL_ConvertEventToRenderCoordinates` (exult.cc:1793, 1932, 2025, …).
3. `tqueue->activate(ticks)` (1391) runs animators, NPC steps and effect `handle_event`s. Some of these write the buffer immediately:
   * `scroll_if_needed` → `view_*`: an unclipped `win->copy` by 8 px plus a strip repaint (gamewin.cc:1641-1735);
   * `Earthquake`: copy ±4 px, show, copy back (effects.cc:1829-1873).
4. Either `paint_lerped(factor)`, which is a full repaint with a sub-tile offset (exult.cc:1445-1503 → gamerend.cc:434-514), or `paint_dirty()` (exult.cc:1504-1507 → gamerend.cc:624-632) over one bounding dirty rect (gamewin.h:782-784).
5. `Mouse::show()` (layer), `rotatecolours()` (gamewin.cc:1054-1079: ranges 0xFC+3, 0xF8+4, 0xF4+4, 0xF0+4, 0xE8+8, 0xE0+8, every 100 ms), then `gwin->show()` → `win->show()` (gamewin.h:747-755).

### 1.3 World paint, in game px

`Game_window::paint(x,y,w,h)` (gamerend.cc:328-414):

1. `BeginPaintIntoGuardBand` (imagewin.cc:1142-1211) may enlarge the rect and temporarily set `ibuf->width = draw_surface->w - 6` and `game_width += 2`.
2. Clamp to the game rect and `set_clip` (339-357).
3. `render->paint_map` (192-291):
   * Chunk range: from 1 tile before the view to 2 chunks + 8 tiles after it (206-219).
   * **Pass 1, flats:** `paint_chunk_flats` → `win->copy8(cflats->get_bits(), 128, 128, xoff, yoff)` (227-233, 520-531).
   * **Pass 2, flat RLE** terrain objects (235-241, 537-549).
   * Editor chunk outline (243-251).
   * **Pass 3, non-flat objects**, chunks taken diagonally, with dependency recursion (254-265 → 557-586 → 592-618).
   * Dungeon blackness (267-269, 648-708), selection outline (272-279), editor grid (281-289).
   * Returns the light-source count.
4. `effects->paint()` (367 → effects.cc:286-290): sprites, projectiles, rain, clouds.
5. Border fill with index 255 (369-384).
6. Gumps, drag and text effects go to **layers** (386-391).
7. On a complete repaint, the light pass picks a palette (394-410).
8. `EndPaintIntoGuardBand`.

Object path:
* `Game_object::paint` (objs/objs.cc:959-964) calls `get_shape_location`, which computes `x = (tx+1−scrolltx)·8 − 1 − 4·tz − scrolltx_lo (+avposx_ld)` (gamewin.cc:1259-1313).
* It then calls `ShapeID::paint_shape` (shapeid.h:348-356). That function applies an optional `palette_transform` LUT (shapeid.cc:603-641) and calls `Shape_manager::paint_shape` (shapeid.h:171-183), which dispatches to flat (`copy8`), remapped, opaque or translucent painting.
* `paint_invisible`/`paint_outline` (shapeid.h:358-366) use `get_shape()` and go to `Shape_manager::paint_invisible/paint_outline` (185-196).
* Actors force translucency (actors.cc:2090) and paint the weapon at `wihh` offsets (2118-2147, 2163-2234).

### 1.4 Pixel writes

* The `Shape_frame` painters (vgafile.cc:483-699) work as follows:
  * `paint_rle` culls with `is_visible`, then calls `Image_buffer8::paint_rle` (ibuf8.cc:516-670), which uses **byte-by-byte** loops.
  * The translucent painter maps indices `0xff−xfcnt … 0xfe` through `xforms[pix−xfstart][dest]` using `copy_hline_translucent8` and `fill_hline_translucent8` (ibuf8.cc:408-456).
  * The other painters are `paint_rle_transformed` (invisible), `paint_rle_outline` (1 px), and the flat `copy8(data,8,8,x−8,y−8)` (525-534).
* The target is the static `Shape_frame::scrwin` (vgafile.h:55). It is set at gamewin.cc:330 and 921 and swapped by push/pop_render_target (gamewin.cc:541-550).
* The `Image_window8` wrappers call `ib8->Image_buffer8::X` **qualified**, so they are bound statically (iwin8.h:94-149). `draw_line8` is the only exception.
* The main buffer's bits **are** the INDEX8 `draw_surface` pixels. `bits` is pre-offset so that (0,0) is the top-left of the game area, and there is a 4-px guard band (imagewin.cc:564-577, imagewin.h:357).

### 1.5 Layers

`push_render_target(layer_buf)` redirects drawing into a layer `Image_buffer8` that sits over an INDEX8 surface with a guard band (imagewin.cc:1575-1599, ibuf8.h:43-47). Users:
* gumps (Gump_manager.cc:143-285, 321-337);
* the mouse (mouse.cc:198-262);
* the dragged item (drag.cc:317-404);
* text effects (effects.cc:1163-1215);
* conversation, a fixed 320x200 layer (conversation.cc:52-53, 218-330);
* scenes (scene_layer.h).

Main-buffer writers other than the world are listed in §6 C6.

### 1.6 Presentation: `Image_window::show(x,y,w,h)` (imagewin.cc:882-1114)

1. Clip against the **current target's** clip and convert to buffer coordinates (`x −= start_x`). Enlarge by 4 and align to 4 (903-944).
2. Lock the **whole** `screen_texture` (957).
3. Gate: `ibuf->width + 2·gb == draw_surface->w` (968). If false, the target is a scene or layer, and only `UpdateRect` runs.
4. **Phase 1** runs only when `draw_surface != inter_surface`: the software scaler converts 8-bit to RGB on the dirty rect (972-1015).
5. **Phase 2** runs the arb fill scaler on the **whole** surface (1017-1049). The SDLScaler path instead does a full-surface copy or `SDL_BlitSurface` with the GPU doing the stretch (1051-1093).
6. `UpdateRect` (2170-2220):
   * render `screen_texture` → `screen_texture_a` for the dirty rect;
   * render `screen_texture_a` fullRect onto the **logical presentation** (`SDL_SetRenderLogicalPresentation(w,h,LETTERBOX)`, 662/670);
   * `composite_layers()` (2091-2168), which converts layers with the live `colors[]` (iwin8.cc:281-617);
   * `SDL_RenderPresent`.

| Case (create_scale_surfaces 739-760) | Condition | Phase 1 | Phase 2 | Where 8-bit becomes RGB |
|---|---|---|---|---|
| A (default point/point/Fit) | `scaler==fill_scaler`, or `scale==1`, or SDLScaler with point/bilinear | skipped (`inter==draw`) | whole surface on every show | phase 2 (arb fill scaler, or SDL blit for SDLScaler) |
| B | `inter != display` | scaler, dirty rect | whole inter surface | phase 1 |
| C | `inter == display` | scaler straight into the texture | none | phase 1 |

### 1.7 Palette path

1. `Palette::apply` (palette.cc:169-194) swaps the border colour into index 255 for palettes 0-12 except 9.
2. `Image_window8::set_palette` (iwin8.cc:96-120) computes `colors[]` with brightness and gamma, sets the SDL palette of `draw_surface`/`paletted_surface`, and marks every layer dirty.
3. `rotate_colors` (iwin8.cc:135-174) rotates the gamma-corrected `colors[]`.
4. Global effects are all palette swaps: day/night, fog, light, spell, invisibility, red flash, lightning (gameclk.cc:42-160, effects.cc:1497) and fades (palette.cc:391-483).
5. **No index changes.** The change becomes visible only when `show()` re-converts. In case A every `show()` converts the whole surface.

---

## 2. Coordinate systems and conversions

| # | Space | Unit and origin | Defined / converted at | Effect of S |
|---|---|---|---|---|
| 1 | World tile | `tx,ty` 0..3071, lift `tz` 0..15; `c_tilesize=8`, `c_tiles_per_chunk=16`, `c_chunksize=128`, `c_num_chunks=192` | exult_constants.h:28-38 | none |
| 2 | Chunk / superchunk | `cx,cy` 0..191 = tile/16; superchunk 0..11 = chunk/16. Chunk → game px with wrap: `Figure_screen_offset` | gamerend.cc:74-85; paint range 206-219 | none (the range stays in game px) |
| 3 | Scroll and lerp | `scrolltx/ty` is the top-left tile (gamewin.h:137, `set_scrolls` gamewin.cc:1085). `scrolltx_lo/ty_lo` is 0..7 game px, `avposx/y_ld` is the camera-actor delta (gamewin.h:979-981) | gamerend.cc:434-514 | none by default. Physical sub-pixel lerp is optional and exempt from the oracle. |
| 4 | **Game px** ("screen" in engine code) | origin = top-left of the game area, `[0,game_w)×[0,game_h)`. The full buffer extends to `[start_x,end_x)` with `start_x = −offset_x ≤ 0` (Fit bands) | tile → px `Get_shape_location` gamewin.cc:1259-1272 plus lerp 1278-1313; dirty rects gamewin.h:782-798; clip; `get_width()` = `game_width` (gamewin.h:212-218) | **stays logical** (D2) |
| 5 | Shape-local | origin is the hotspot; extents `[−xleft,xright]×[−yabove,ybelow]` (vgafile.h:124-146); RLE scan x/y are relative to the origin. Flats: `xleft=yabove=8`, `xright=ybelow=−1` | `has_point` vgafile.cc:706-742 (1-px slack) | hi-res frame: canonical mapping (§4.3) |
| 6 | Buffer px (== game px today) | address `bits + y·line_width + x`, where `bits` is pre-offset (imagewin.cc:564-577). Guard band of 4 px (imagewin.h:357). `show()` buffer coords = `x − start_x` (906-907) | `Image_buffer::clip*` imagebuf.h:69-100 | **becomes physical** = game px × S. Converted only inside `Image_buffer8` (scaled branch, physical view) and in the S>1 present path |
| 7 | Inter px | `inter = full × scale` (scaler output). `draw_surface = inter/scale + 2gb` | `get_draw_dims` imagewin.cc:1315-1436; 726-728 | `inter` and `scale` keep their meaning: Auto divisor and coordinate map |
| 8 | Display logical | `display_width × display_height` (imagewin.cc:672-673), LETTERBOX logical presentation (662/670). **The full buffer always maps onto `(0,0,dw,dh)`** | `UpdateRect` 2199 | world_texture is drawn into the same rect (D4) |
| 9 | Window/OS points, backbuffer pixels | events → `SDL_ConvertEventToRenderCoordinates`. Fullscreen replaces `w,h` with the render output size (imagewin.cc:637-655). Windowed uses points (HiDPI caveat) | exult.cc:1793 … | S_eff should use the real output size (gap_2 §6.1) |
| 10 | Mouse | display → game: `gx = sx·inter_w/(scale·display_w) + start_x` (imagewin.cc:1277-1294); inverse `game_to_screen` (1296-1313); fast mode 1:1 plus offset (mouse.cc:302-320); stored in game px (mouse.cc:321-346) | — | **no change.** Picking stays on 1x frames (`find_object` gamewin.cc:2096-2115) |
| 11 | Layer / gump / scene px | layer `logw×logh`, dest rects in display coords; `screen_to_layer`/`layer_to_screen` (imagewin.cc:2050-2079); `map_game_to_gump` (Gump_manager.cc:287-319); scene coords (scene_layer.h:276-284, cheat_screen.cc:87-93); persisted gump centre in display coords (Gump.cc:49-75, 112-138) | — | M3: content scale k; logical size unchanged |
| 12 | **Physical px (new)** | `phys = game px × S` from the same origin (the top-left sub-pixel of the game pixel). Hi-res frame extents follow §4.3 | `Image_buffer8` scaled branch, `phys_view()`, `show()` rect×S, chunk-flat cache `(128S)²` | — |

Invariants the design must keep:
* **I1.** `inter_w/scale = full_w` in game px. The world image covers the same logical display rect, so the mouse, layers and gumps are unaffected.
* **I2.** `get_game_*`, `get_full_*`, `get_start/end_*`, clip and dirty rects stay logical.
* **I3.** Tile and chunk maths (`c_tilesize=8`, used 264 times) stay unchanged.
* **I4.** Hit testing, emptiness, draw order and dirty extents always use the **1x** frame.
* **I5.** The painted game-px area does not depend on S, because paint has side effects (actors.cc:5133-5148).

---

## 3. Inventory of touchpoints

Difficulty: **S** < 1 day, **M** 1–3 days, **L** > 3 days.
Milestones:
* **M1** = hi-res world view at S with NN for everything, plus terrain overrides. This includes all prerequisites, presentation, config and tests.
* **M2** = overrides for all world sprites.
* **M3** = UI, gumps, fonts and faces.

"none" means no code change is needed, only verification.

### 3.1 Prerequisite fixes (land first, as separate commits, before any art is hashed)

| # | file:line | What | Required change | Diff | MS |
|---|---|---|---|---|---|
| P1 | objs/chunkter.cc:104 | Neighbour bound `tiley + y > 0` excludes row 0 (verified) | `>= 0`. Changes the 1x fill in 779 of 2,105 used BG terrains (SI: 1,085), so the golden images must be updated | S | M1 |
| P2 | objs/chunkter.cc:119-126 | Full-chunk fallback can pick void tile 12/0 | Skip 12/0 here as well (BG 112 positions, SI 194) | S | M1 |
| P3 | objs/chunkter.cc:259; imagewin/ibuf8.h:37-39 | `new unsigned char[w*h]` is uninitialised; missing shapes leave heap garbage (BG flat 48/8) | Value-initialise / `fill8(0)` before painting; new hi-res caches must be value-initialised too | S | M1 |
| P4 | objs/chunkter.cc:86-133 | `paint_tile` does not report the substitute flat it actually painted | Return the effective `ShapeID` so the hi-res pass can use the same substitute | S | M1 |
| P5 | effects.cc:459 | `Sprites_effect::paint` subtracts `get_scrolltx_lo()` from **y** (verified) | Use `get_scrollty_lo()` | S | M1 |
| P6 | shapes/vgafile.cc:295 | `encode_rle` uses `unsigned short runs[200]` on the stack (verified); measured up to 561 runs per hi-res row | `std::vector<unsigned short>(w+1)`. The same code is used by `reflect`, ipack and ES | S | M1 |
| P7 | shapes/vgafile.cc:467-470 | `get_rle_shape` with `len==0` writes `data[0..1]` through a null `unique_ptr` (verified) | Allocate 2 bytes first; pack writers must never emit empty frames | S | M2 |
| P8 | shapeid.cc:420-468 | `reload_shapes` indexes `shape_cache[]` with a u7drag kind (FONTS=2→PAPERDOL, FACES=3→SPRITES) and does not invalidate the flats | Map the U7_SHAPE kind to ShapeFile; bump the render generation | S | M2 |
| P9 | build-linux/Makefile:457,540 | The local build is -O0 (`CXXFLAGS = -pthread`; verified). At S=6, S² data costs 6–23 ms per frame | Reconfigure with `--with-optimization=normal` (or use the Makefile.common wrapper with -O2) for every performance evaluation | S | M1 |
| P10 | imagewin/imagewin.cc:1119-1140 | Dead `toggle_fullscreen` with `w = display_height` | Remove it, or route it through `Game_window::resized` | S | M1 (opt) |

### 3.2 Image buffer core (`imagewin/`)

| # | file:line | What | Required change | Diff | MS |
|---|---|---|---|---|---|
| B1 | imagewin/imagebuf.h:57-66, 142-210 | Fields `width/height/offset/bits/line_width`; clip helpers; accessors | Add `scale` (default 1) and derived physical accessors (`get_scale`, `get_phys_width = width·S`, …). Keep clip, `is_visible` and `ClipRectSave` logical. **Delete** the copy ctor and assignment (risk of double `delete[]`) | S | M1 |
| B2 | imagewin/ibuf8.h:30-115; ibuf8.cc:115-513 | Virtual primitives `fill8` ×2, `fill_hline8`, `draw_line8`, `copy8`, `copy_hline8`, `copy_hline_translucent8`, `fill_hline_translucent8`, `fill_translucent8`, `copy_transparent8`, `get/put_pixel8`, `fill_static` | Keep the scale==1 code byte-identical. Add a scale>1 branch that clips logically and writes S×S blocks. Translucent and xform paths apply the table **per physical pixel**. `draw_line8` runs the DDA in game px and plots S×S blocks, which also makes lines and grids S px thick | L | M1 |
| B3 | imagewin/ibuf8.cc:516-825 | `paint_rle` / `paint_rle_remapped` (non-virtual, byte loops) | Scaled NN kernel: clip once per scanline, expand each run ×S into a row buffer, `memcpy`/`memset` it into S rows. Also give the 1x loops `memcpy`/`memset` fast paths | M | M1 |
| B4 | imagewin/ibuf8.cc:43-68 | `copy` (scroll, earthquake) is an **unclipped** memmove | Multiply coordinates by S; add clipping against the buffer | S | M1 |
| B5 | imagewin/ibuf8.cc:74-110 | `get`/`put` assume `src->line_width == width`; `put` calls `copy8` qualified | Copy physical rows. Same scale: physical copy. Scale-1 source into a scaled destination: replicate | M | M1 |
| B6 | imagewin/ibuf8.h:58-60; imagewin.cc:848-852 | `create_another`/`create_buffer` | Return a buffer with the **source scale**, so `ImageBufferPaintable` round-trips. Add an explicit scale-1 variant for FLI and scenes | S | M1 |
| B7 | new in ibuf8.h/.cc | Physical view and scale-aware blit | Non-owning ctor with explicit offsets and clip ×S (the ctor at ibuf8.h:43-47 shows the pattern). `blit(const Image_buffer8& src, x, y)` copies physically when the scales match and replicates otherwise | M | M1 |
| B8 | imagewin/iwin8.h:94-149 | Qualified `ib8->Image_buffer8::X` wrappers | **none.** With an in-class scale field they dispatch correctly. Never introduce a subclass | none | M1 |
| B9 | imagewin/imagebuf.cc:58-89; ibuf8.cc:827-883 | `draw_box`, `draw_beveled_box` | none. They are built from the scaled primitives, so lines become S px thick | none | M1 |

### 3.3 Window and presentation (`imagewin/`)

| # | file:line | What | Required change | Diff | MS |
|---|---|---|---|---|---|
| W1 | imagewin/imagewin.h:320-409 | `Image_window` state | Add `render_scale` (S_eff), `main_ibuf` (the persistent ib8), `world_texture`, `world_palette_dirty` and the last world rect. `scale_layer_color` must never touch these | S | M1 |
| W2 | imagewin/imagewin.cc:1315-1436; imagewin.h:315 | static `get_draw_dims` | Keep it. Add a static `compute_render_scale(disp, full, fill, S_art, S_cfg, maxtex, budget)`. Unit-test both against the tables in gap_2 §2-§6 | S | M1 |
| W3 | imagewin/imagewin.cc:585-770 | `create_scale_surfaces`: renderer (626), real display size (637-673), textures (690-711), `draw_surface` (726-737), case selection (739-760) | After 673: query `SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER` (cache it statically) and compute S_eff. If S>1: `draw_surface` = `(full·S + 2gb)` INDEX8; create `world_texture` = `full·S`; skip `inter_surface`/`screen_texture(_a)` or keep them minimal. Fail soft by lowering S instead of throwing (555-561) | L | M1 |
| W4 | imagewin/imagewin.cc:540-578 | `create_surface` wires `ibuf` | If S>1: `width/height = (draw_surface->w/h − 2gb)/S`, logical offsets, `bits = pixels + gb·(pitch+1) + offset·S·(1+pitch)`, `line_width = pitch`, `scale = S`. Mutate `ib8` in place | M | M1 |
| W5 | imagewin/imagewin.cc:811-842, 858-876 | `free_surface` (destroys the renderer on every resize), `resized` | Free `world_texture`. Every GPU resource is rebuilt after every resize | S | M1 |
| W6 | imagewin/imagewin.cc:882-1114 | `show()`, gate at 968 | New branch **before** the gate, taken when `render_scale>1 && ibuf == main_ibuf && !scene_mode`: clip logical → buffer coords → ×S → `SDL_UpdateTexture(world_texture, rect)` (INDEX8: raw indices; ARGB: LUT conversion of the rect, or of the whole buffer when `world_palette_dirty`). Replace the size heuristic at 968 with `ibuf == main_ibuf` | M | M1 |
| W7 | imagewin/imagewin.cc:2170-2220 | `UpdateRect` draws `screen_texture_a` fullRect, then layers | If S>1: draw `world_texture` (content rect, no guard) to `nullptr` (the logical rect). Choose the scale mode from `r = display/(full·S)`: NEAREST when r is an integer ≥ 1, LINEAR when 0.5 ≤ r < 1, PIXELART for fractional upscale on SDL ≥ 3.4. Keep the "last rect" state for screenshots | M | M1 |
| W8 | imagewin/imagewin.h:802-827; imagewin.cc:1142-1252 | `ShouldPaintIntoGuardband`, `Begin/EndPaintIntoGuardBand`, `FillGuardband` | Return false **first** when S>1, before `screen_texture->w` is dereferenced. Begin/End/Fill become no-ops. Otherwise `ibuf->width = draw_surface->w − 6` puts a physical value into a logical field and corrupts the heap | S | M1 |
| W9 | imagewin/iwin8.cc:96-120, 135-174 | `set_palette`, `rotate_colors` | Also update `world_texture`'s `SDL_Palette` (INDEX8), or set `world_palette_dirty` (ARGB) | S | M1 |
| W10 | imagewin/imagewin.cc:1738-1781 | `scale_layer_color` patches `ibuf->line_width/height` of the **current** target and then restores them | Safe today because it restores. Decouple anyway: give the scalers an explicit source pitch and height (122 `ibuf->` reads in scale_*.cc) or use a local descriptor | M | M1 (assert) / M3 (refactor) |
| W11 | imagewin/scale_*.cc (122 reads, verified) | Scalers read `ibuf->line_width/height/width` | No change in M1, because they never run on an S>1 world. Add `assert(ibuf->get_scale()==1)` at the `show_scaled*` entry | S | M1 |
| W12 | imagewin/imagewin.cc:1277-1313 | `screen_to_game` / `game_to_screen` | **none.** Add a regression test that a click at display px p maps to the same game px for S=1 and S=6 | S | M1 |
| W13 | imagewin/imagewin.cc:1254-1271; save_screenshot.cc:95-99, 314-377 | Paletted screenshot saves `draw_surface` (crops 4 px); non-paletted re-renders the last rect | Paletted gives an S× indexed image with the physical 4-px crop. That is a feature and should be documented. Non-paletted must draw `world_texture`. Fix the latent guard-band crop on the read-back (1266) | S | M1 |
| W14 | imagewin/iwin8.cc:181-226 | `mini_screenshot` reads `bits` with game-px maths | Average 3S×3S physical blocks with the physical pitch. That is exact for NN renders, so it becomes an S-invariant (oracle O7) | S | M1 |
| W15 | imagewin/imagewin.h:175-251; imagewin.cc:1575-1599, 2091-2168; iwin8.cc:281-617 | Layer, `create_layer`, `composite_layers`, `refresh_layer*` | M3: per-layer `content_scale k` (buffer `logw·k`, `Image_buffer8::scale=k`, physical source rect, logical `screen_to_layer` unchanged). Fix the `+2` vs round-up-to-4 width mismatch (2141 vs iwin8.cc:343,403) and the unused `texpix` allocation (iwin8.cc:412) | M | M3 |
| W16 | imagewin/iwin8.cc:60-66; gamewin.cc:326-330 | Main ib8 created once | Set `main_ibuf` there; `Shape_frame::set_to_render(main)` stays | S | M1 |

### 3.4 Game window, world renderer, terrain

| # | file:line | What | Required change | Diff | MS |
|---|---|---|---|---|---|
| G1 | gamewin.cc:541-550; iwin8.h:63-68 | `push/pop_render_target` | No logic change: S travels with the buffer. Add a `get_main_render_target()` accessor | S | M1 |
| G2 | gamewin.cc:916-937 | `Game_window::resized`, the single funnel for size and S changes | After `win->resized` (919) and before `paint()` (926): if S_eff changed, bump the render generation or call `Chunk_terrain::flush_all`. Show S in the toast (930) | S | M1 |
| G3 | gamerend.cc:328-414 | `Game_window::paint` | Unchanged in game px. The guard band is off at S>1. The border fill goes through the scaled `fill8`. The light-pass test (394) stays logical | none | M1 |
| G4 | gamerend.cc:192-291 | `paint_map` traversal | Unchanged. Add perf scopes per pass (flats, flat RLE, objects, blackness) | S | M1 |
| G5 | gamerend.cc:520-531 | `copy8(cflats->get_bits(),128,128,…)` treats 128 as the source stride | Use the scale-aware `blit(*cflats, xoff, yoff)`. Skip `get_rendered_flats` for chunks whose 128×128 game-px rect does not intersect the clip | S | M1 |
| G6 | gamerend.cc:434-514 | `paint_lerped` | none (S-px steps). Optional later: physical sub-offset for smoother scrolling (an oracle exemption) | none | — |
| G7 | gamerend.cc:91-151, 648-708; objs/egg.cc:1069-1082; objs/barge.cc:710-734; shapes/shapeinf.cc:546-607 | Blackness, editor grid and outline, egg and barge debug lines, bbox lines | none. They become NN-replicated, S px thick. A thin variant would be an opt-in and an oracle exemption | none | M1 |
| G8 | gamewin.cc:1641-1735 | `view_*` scroll via `win->copy` by 8 px | none (covered by B4: an 8·S physical copy). The dirty shift stays in game px | none | M1 |
| G9 | effects.cc:1829-1873 | Earthquake buffer copy ±4 | Covered by B4. Optionally move the shake to a present-time offset | S | M1 |
| G10 | gamewin.cc:489-497, 1431, 2982-3002; game.cc:815 | `clear_screen`, saving dim, plasma, speech dim | none. They go through the scaled primitives, and plasma loops stay in game px | none | M1 |
| G11 | objs/chunkter.cc:248-277; chunkter.h:36-101 | `render_flats`, `rendered_flats`, MRU, `free_rendered_flats` | Build `src1x` (128², zero-filled, the exact engine render after P1-P4). If S>1, build `(128S)²` with precedence **per-terrain override (FNV-1a-64 of src1x)** → **per-tile override** `(shape, frame&31)` with 64-B source check → **NN**. Store (S, generation) per terrain; re-render on mismatch in `get_rendered_flats` (chunkter.h:95-101). Add a static `flush_all()` | L | M1 |
| G12 | objs/chunkter.cc:234-242 | `Figure_queue_size()` hard-coded to 100 (verified) | `(ceil(gw/128)+3)·(ceil(gh/128)+3)` from the current game area (a 30-entry working set at 320x200, 17.7 MB at S=6), bounded by a byte budget of `16384·S²` per entry, never below the working set | S | M1 |
| G13 | objs/chunkter.cc:193-216 | `set_flat` leaves the cache stale; `commit_edits` allocates outside the queue | Optionally free in `set_flat`; insert into the MRU list in `commit_edits` | S | M1 (opt) |
| G14 | objs/chunkter.cc:284-313 | `render_all` (terrain editor) `copy8` and `paint_shape` | Works through the scaled `copy8` (NN). Add a per-tile flat-override lookup; per-terrain overrides are not shown while editing | S | M1 |
| G15 | gamemap.cc:1690-1723 | `write_minimap` averages `rendered_flats` of every terrain (about 3,000) | Use `src1x` / a 1x path. Never build S× caches here | S | M1 |
| G16 | gamemap.cc:259-270 | `clear_chunks` | Call `Chunk_terrain::flush_all` | S | M1 |
| G17 | shapes/vgafile.cc:525-534 | `Shape_frame::paint` flat branch `copy8(…,8,8,x−8,y−8)` | NN through the scaled `copy8` in M1. Add a flat-override lookup for direct flat paints | S | M1 |
| G18 | exult.cc:2857-2912 | `BuildGameMap` uses a 2048² window at scale 1 | With D5, display/full = 1 gives S=1 automatically. Force S=1 even if the config says "force" | S | M1 |
| G19 | actors.cc:2078-2147, 2163-2234 | Actor outlines, weapon offsets | Offsets stay in game px (×S implicitly). Outlines are drawn from the hi-res mask when a payload is used. Optional `wihh_fine` sidecar ±(S−1) | M | M2 |

### 3.5 Shapes and the override store

| # | file:line | What | Required change | Diff | MS |
|---|---|---|---|---|---|
| H1 | new `shapes/hires_store.{h,cc}` | Store (dev: loose PNG; ship: companion VGA), palette validation, CRC gate, byte-budget LRU | M1 subset: terrain store (per-tile flats resident, about 9 MB BG / 10.8 MB SI; per-terrain by hash). M2: full store, LRU touch per paint, eviction at the frame boundary, generation per `Vga_file` | L | M1 / M2 |
| H2 | shapes/vgafile.h:47-163 (verified) | `Shape_frame` (data, extents, static `scrwin`, paint wrappers 97-120) | Add a slot `{store*, logical shape, frame, state: unknown/none/loaded/evicted/inactive, payload*}`. The wrappers dispatch on `scrwin->get_scale()`. The cache never frees `Shape_frame` objects (pointer stability) | M | M2 |
| H3 | shapes/vgafile.cc:483-699 | The 5 painters | If `win` scale > 1: an active payload of the same scale is painted through `phys_view()` at `(x·S, y·S)`; otherwise the NN kernels (B3). Outline and invisible use the **hi-res mask** when a payload is used. The early-out keeps the logical extents | M | M1 (NN) / M2 (payload) |
| H4 | shapes/vgafile.h:357-377 | `Vga_file::get_shape`, the only lazy-load choke point | **Stamp identity only, no I/O**: logical shape number (imports included), `frame&31` for flats, the requested frame for RLE. Add `set_hires_store()`. Never load the payload here (frame-0 sweep) | S | M2 |
| H5 | shapes/vgafile.cc:802-824, 915-918 | Reflection | Derive the hi-res `f\|32` by transposing the hi-res base (exact under the top-left mapping). It is evictable. An explicit `f\|32` is allowed only under rule R2 | M | M2 |
| H6 | shapes/vgafile.cc:1163-1183 | `U7load` | Open the companion pack as a **path** (`IFileDataSource`), never as a flex entry, which would be read whole into RAM | S | M2 |
| H7 | shapes/vgafile.cc:1008-1011, 1270-1306 | `reset`, `reset_imports`, `new_shape` | Bump the hi-res generation together with the low-res reset | S | M2 |
| H8 | shapeid.h:171-196 | `Shape_manager::paint_shape/paint_invisible/paint_outline` | Signatures unchanged. The `Shape_frame` hook does the work. Keep "remap disables translucency" (176-181) | S | M2 |
| H9 | shapeid.h:348-366; shapeid.cc:552-576 | `ShapeID::paint_*`, `cache_shape` | none. The remap LUT applies to indexed hi-res, and the raw pointers stay valid | none | M2 |
| H10 | shapeid.cc:150-382 | `Shape_manager::load` | Create one store per ShapeFile after the imports and `read_shape_info()`; read `config/video/hires/*` | M | M1 (terrain) / M2 |
| H11 | new `shapes/hires_rules.{h,cc}`; new `tools/exult_hirescheck` | Group derivation and rules G1-G6, R1-R4, A1-A2, W1-W4, B1-B2, T1-T3, P1-P4 (gap_5 §6) | One shared library for the engine's strict activation and the validator CLI, with pinning tests for the hard-coded engine tables | L | M2 |
| H12 | shapes/pngio.cc:45-171 | `Import_png8` (`fopen` without tag expansion; tRNS index shift) | Engine wrapper: `get_system_path`, raw-index convention, reject PNGs with tRNS on any index but 255, verify the palette, `HAVE_PNG_H` guard (off on Android/iOS) | M | M1 |
| H13 | files/crc.{h,cc}; Shape_frame | CRC is file-only | Buffer overload (or zlib `crc32`). `Shape_frame::content_crc()` over the canonical decoded frame (gap_6 §7.3) | S | M2 |
| H14 | gamemgr/modmgr.cc:56-85, 592-617 | Path tags | Add `<PREFIX_HIRES>` (`config/disk/game/<name>/hires_path`, default `$game_path/hires`). Search chain: `<PATCH>/hires/x6` → `<PREFIX_HIRES>/x6` → `<DATA>/hires/<game>/x6` | S | M1 |
| H15 | exult.cc:298-312, 986-989 | CLI | `--dump-art <dir>` modelled on `BuildGameMap`: walks the effective `Shape_manager` (imports, patch, fonts) and writes raw-index PNG plus a per-configuration manifest with CRC and provenance | M | M2 |
| H16 | tools/hires/u7art.py:33, 199-207, 296-310 | Interim extractor | Emulate `paint_tile` fill (after P1-P4); fix the v2 header (10-byte `FF FF FF FF "exlt" 00 00`); emit FNV-1a keys, CRC and provenance; refuse to run when a patch or mod is active | S | M1 |
| H17 | gamewin.h:747-755; gamerend.cc:624-632 | `Game_window::show` (`blits`), `paint_dirty` | Frame stamp and deferred LRU eviction point | S | M2 |
| H18 | gamemap.cc:1199-1206; effects.cc:321-349 | Superchunk read, `Sprites_effect` ctor | Optional prefetch (worker decode with its own file handles) | M | M2 (opt) |

### 3.6 Other buffer readers and consumers

| # | file:line | What | Required change | Diff | MS |
|---|---|---|---|---|---|
| R1 | shapeid.cc:643-654 (verified) | `ImageBufferPaintable` snapshot via `create_buffer(full)` plus `get/put` | Covered by B5/B6: a same-scale buffer and physical rows (2.3 MB at S=6) | none | M1 |
| R2 | drag.cc:413-448 (verified) | Legacy main-buffer drag path (map edit, bbox, dropping) | none. Scaled primitives in M1; it picks up hi-res art automatically in M2 | none | M1 |
| R3 | drag.cc:317-404 | Dragged item drawn into a 1x layer sized from the frame | Layer content scale S when the frame has a payload | M | M3 (M2 opt) |
| R4 | usecode/intrinsics.cc:1791-1840 (verified) | `display_area`: `clear_screen`, `paint_map_at_tile`, raw sprite 10 via `sman->paint_shape` | none. It works through scaled primitives and the `Shape_frame` hook | none | M1 |
| R5 | gumps/Gump_manager.cc:321-337, 1090-1118 (verified) | Modal backdrop painted to the window; gump `paint()` during events lands in the main buffer; full world repaint per modal event | none for correctness. Optional "UI-only repaint" path (skip `paint_map` when only layers changed) | M | later |
| R6 | gamewin.cc:3126-3139 | `create_mini_screenshot` | Covered by W14 | none | M1 |
| R7 | gamewin.cc:1028-1044 | `Send_location` sends `get_scale_factor()` | Keep the scaler scale; Studio ignores it (mapedit/locator.cc:320) | none | — |
| R8 | gamemgr/bggame.cc:690-710; flic/playfli.cc:116-363; txtscroll, menulist, exultmenu | Scene-buffer `get/put`, `SDL_SurfaceOwner(get_bits)` | none. Scene buffers stay at scale 1. Menus that draw straight into `get_ib8()` outside a `Scene_view` go through the scaled primitives | none | M1 |
| R9 | perf.h:91-118; perf.cc:119-154 | Perf overlay forces `set_all_dirty` | Per-pass scopes, an S label, an option not to force a repaint, a dev key binding | S | M1 |

### 3.7 Configuration, CLI, build, tests

| # | file:line | What | Required change | Diff | MS |
|---|---|---|---|---|---|
| C1 | exult.cc:3128-3304 (scale forcing 3203-3211) | `setup_video` | New keys under `config/video/hires/`: `render_scale` = auto \| 1..6 (cap) or `force:N`, `art_scale` = 6, `overrides` = on/off/identity (test), `cache_mb`, `partial` = strict \| lenient. Do not tie them to scaler forcing | S | M1 |
| C2 | gumps/VideoOptions_gump.cc:329-349, 543-621 | Video options | Show S_eff and offer the cap. The revert path calls `resized` twice, which is fine with generations | M | M1 (opt) |
| C3 | exult.cc:2744-2785 | Studio zoom `set_scaleval` (point only) | none. S_eff follows k through D5 (k=1 gives S=1) | none | M1 |
| C4 | exult.cc:289-312, 393-430, 825, 986-989 | CLI declare, validate, `setup_video` guard, dispatch | `--render-test "map,tx,ty,w,h,lift,scale,seed,save,effects,overrides,mode,ab,out"` modelled on `BuildGameMap`: in-process A/B through `push_render_target`, writes indices plus a digest | M | M1 |
| C5 | gamewin.cc:587-588 | `srand(SDL_GetTicks())` in `init_files` | Test hook to reseed after `init_files` (audio off: fmopl.cpp:514 uses `std::rand`) | S | M1 |
| C6 | Makefile.am files, Makefile.common:196-258, msvcstuff/vs2019/Exult.vcxproj(+filters), ios/Exult.xcodeproj | Four build descriptions | Register new sources in all four. Add `shapes/pngio.o` to `Makefile.common` `SHAPES_OBJS`. Add `tests/` (doctest, `check_PROGRAMS`, `AM_TESTS_ENVIRONMENT=SDL_VIDEO_DRIVER=dummy`). Ignore `data/shortcutbar*` | M | M1 |
| C7 | configure.ac:479-485 | `sdl3` without a version floor; no `SDL_VERSION_ATLEAST` anywhere (verified) | Guard the INDEX8 path with `#if SDL_VERSION_ATLEAST(3,4,0)` and a runtime fallback to ARGB, so upstream CI (3.2.14) still builds. Optionally set a 3.4 floor for the fork only | S | M1 |

### 3.8 UI (milestone 3)

| # | file:line | What | Required change | Diff | MS |
|---|---|---|---|---|---|
| U1 | shapes/font.cc:255-330, 472-545, 775-795 | Fonts ignore `win` and paint through `scrwin`; each `Font` has a private `Shape_file` | Override key `(font source spec, font#, glyph)`. Glyph width must equal S·w exactly (advance). Painted in a layer with content scale | M | M3 |
| U2 | mouse.cc:108-127, 198-262 | Pointers (`pointers.shp` / `exult.flx[5]`), mouse layer | Pointer overrides; mouse layer content scale | M | M3 |
| U3 | gumps/Gump_manager.cc:143-285; gumps/Paperdoll_gump.cc:137-160, 574-688; gumps/Gump_button.cc:76-78; ShortcutBar_gump, Face_stats | Gump layers, paperdoll composite, button pairs, HUD | Content scale k (S or `ceil(ui_scale)`). Group rules: paperdoll set, button pair | L | M3 |
| U4 | usecode/conversation.cc:52-53, 218-330, 760-830, 868-886 | Fixed 320x200 conversation layer, faces | Content scale; face overrides (whole shape is one group) | M | M3 |
| U5 | game.cc:89, exultmenu.cc, scene_layer.h | Menu shapes, scenes | Keep them at 1x; optional later | — | later |

### 3.9 Milestone exit criteria

* **M1 done when:**
  * the S=1 golden images are byte-identical (after P1-P4);
  * O1 (primitive fuzz) and O2 (static region `render_S == NN(render_1)`) pass for S ∈ {2,3,6};
  * O4a/O4b (identity and marker flats) pass;
  * O6 (repaint idempotence) and O7 (S-invariant scalars, including `mini_screenshot`) pass;
  * the windowed profile runs at S=6 1:1;
  * per-tile and per-terrain flat overrides load from `<PREFIX_HIRES>/x6`;
  * performance holds at -O2: world paint ≤ 1.6 ms and present ≤ 2 ms for 320x200 at S=6.
* **M2 done when:**
  * the hi-res slot, companion VGA, LRU, reflections, strict groups, CRC gate and `--dump-art` work;
  * O4a passes for RLE, translucent and reflected frames, and O5 (masked real overrides) passes;
  * `exult_hirescheck` is in CI.
* **M3 done when:**
  * layer content scale works and gumps, fonts, faces, cursor and the dragged item use overrides;
  * hit-testing is unchanged.

---

## 4. Constraints for override art

### 4.1 Pixel format and palette
* **8-bit indexed against the effective palette 0.** That means static `palettes.flx` plus `<PATCH>/palettes.flx` (palette.cc:141), per game. BG and SI hashes differ in `verify.cc`, but entries 0-8 and 10-12 are byte-identical on this install. Treat a pack as palette-bound anyway.
* RGBA override art is out of scope. It would bypass the palette swaps, cycling, xforms and ramp remaps (palette.md §6.2).
* 6→8-bit conversion matches the engine: `c8 = v·255/63` at brightness 100 and gamma 1 (iwin8.cc:84-90).
* Do **not** use ipack's export colours: they use ×4 with uint8 wrap, so 63 becomes 252 and entry 255 becomes (232,0,4).
* Quantize in palette-0 space only. The engine applies the day/night grades globally; no static index stays bright at night.

### 4.2 Index classes (normative)

| Range | Meaning | Allowed in override |
|---|---|---|
| `0x00` | black; transparent only for `copy_transparent8`, which has no engine callers | yes. Some pipelines use `0x01-0xDF` to avoid the duplicate colours 0/252; either is safe |
| `0x01-0xDF` | static colours, 16 ramps (palette.cc:581-654) | **the only output range of the quantizer.** Ramp of each pixel ≈ ramp of its 1x parent, so `PT_RampRemap` recolours correctly (rule P2) |
| `0xE0-0xE7`, `0xE8-0xEF`, `0xF0-0xF3`, `0xF4-0xF7`, `0xF8-0xFB`, `0xFC-0xFE` | palette cycling every 100 ms (gamewin.cc:1063-1068, verified). **0xFF is never cycled** | only where the 1x parent pixel (or a 1-px neighbour) is in the **same** range. Copy or NN the source index mask, or synthesise a phase field inside the same range (rule P4) |
| `0xEE-0xFE` | 17 translucent xforms (`blends.dat` count 17, shapeid.cc:306-334, verified), but **only** in frames painted translucently: TFA bit, **all actor frames** (actors.cc:2090), **all sprites.vga** (shapeid.cc:567-569). Elsewhere they cycle | keep the parent's index as a blend operator; the mask may be smoothed (rule P3). The comment at shapeid.h:87-88 (`0xf4 through 0xfe`) and docs/newgame.txt are stale |
| `0xFF` | border colour in game palettes (`border255`); RLE transparent key at encode time; see-through index of every UI layer | RLE: transparent only, never an opaque colour (no in-game frame uses it). **Forbidden in flats** |

* No partial alpha. Threshold AI alpha at about 50%. Soft edges are possible only through the frame's own translucent index, in translucent-painted frames.
* Water sparkles are sparse `E0-E7` single pixels, 1–17% of water flats. Re-place them as small 1–3 px glints at the original positions instead of 6×6 blocks.
* Dithering must be ordered or blue-noise with a **world-aligned** threshold map. Never use error diffusion across tile edges.

### 4.3 Geometry and hotspots (canonical mapping: top-left sub-pixel)

Game px `p` covers hi-res `[S·p, S·p+S−1]`. For a 1x frame with extents (xleft, xright, yabove, ybelow):

```
xleft_h  = S·xleft        xright_h = S·xright + S − 1      w_h = S·w
yabove_h = S·yabove       ybelow_h = S·ybelow + S − 1      h_h = S·h
painted at (S·xoff, S·yoff) in physical px
```

* **G1.** Flats and font glyphs must have exactly these extents. A flat is `8S×8S` (48×48), `xleft_h=yabove_h=8S`, `xright_h=ybelow_h=−1`, painted at `(S·xoff−8S, S·yoff−8S)`.
* **G2.** Every opaque hi-res pixel of an RLE frame lies inside the canonical S× bbox. A trimmed bbox is fine if the hotspot follows the mapping. Anything outside leaves dirty-rect trails and breaks z-order, because both stay at 1x (gamewin.h:788-798, objs.cc:1296-1322).
* **G3.** A PNG `oFFs` must be `(−xright_h, −ybelow_h)` (the ES/ipack convention scaled; flats `(0,0)`). Never auto-correct it.
* **G4.** A hi-res frame is empty iff the 1x frame is empty (actors.cc:803-823, ucinternal.cc:810-826).
* **G5.** The silhouette, downsampled by S ("any sub-pixel opaque"), lies within the 1-px dilation of the 1x mask, with ≥ 90% coverage. Picking stays on 1x (`has_point`, vgafile.cc:706-742).
* **G6.** Flats are fully opaque.
* Sizes: the largest shapes.vga frame is 72×72, i.e. 432×432 at S=6. The large frames are UI and effects: gumps up to 1842×888, full-screen sprites and SI faces 1920×1200. These require fix P6.

### 4.4 Reflected frames
* `f|32` is synthesised only when `f ≥ nframes` and bit 5 is set (vgafile.cc:915-918). Flats use `frame&31` and are never reflected.
* **R1.** Under the top-left mapping, transpose commutes with S× upscaling. Never ship hi-res for a synthesised reflection; derive it from the hi-res base.
* **R2.** An explicit hi-res `f|32` (to fix handedness) is allowed only if `f|32 ≥ nframes_1x` **and** the base `f` has hi-res art. It must satisfy G1-G5 against the transposed 1x frame. The weapon offsets are still swapped by the engine (actors.cc:2226-2228).
* **R3.** In shapes with more than 32 real frames (`no_bit5_frame_reflection`, shapeid.cc:122-131), `f|32` is a real frame. Never auto-transpose it.
* Only actors (west/east = 32-47 and 48-63) and barges (`get_rotated_frame`, shapeinf.cc:521-544) use reflections. No ifix record does.

### 4.5 Groups and partial overrides (strict activation by default)
A group is active only if **every** member frame has a valid override (G1-G6, P1, CRC). Otherwise the whole group falls back to NN. Groups:
* `Animation_info` cycles `[k·cnt,(k+1)·cnt)` (animate.cc:327-362);
* actor bodies, frames 0..31;
* weapon in-hand frames {1,2,3,4}, casting {1..7};
* barge quads, wheel {0..3}, draft horse {0..15} (barge.cc:640-648);
* missiles 8..23 (effects.cc:619-626, 743-749);
* whole shape for sprites.vga, faces, fonts and pointers;
* gump button pairs, paperdoll sets.

Lenient per-frame activation is for development only. Terrain flats have no frame animation; their coupling is spatial (§4.7).

### 4.6 Composites
* Actor + weapon: the hi-res offset is exactly `S·(wx−actor_x, wy−actor_y)`, swapped on reflection. That is exact for every mix of hi-res and NN.
* Sub-pixel refinement needs an optional sidecar `hires/x6/wihh_fine` with deltas in `[−(S−1), S−1]`, applied only when both actor and weapon are hi-res. `wihh.dat` cannot carry it: it is 1 byte, values 0..63 (shapevga.cc:807-838).

### 4.7 Terrain-specific
* Keys:
  * per-tile `(SF_SHAPES_VGA, shape, frame&31)`, carrying its 64-byte 1x source;
  * per-terrain = FNV-1a-64 of the zero-filled 16,384-byte 1x render after P1-P4, stored as `terrain/<16 hex>.png` (768×768, flat layer only, including the under-RLE fill).
* Never key on terrain number or `modified`: `swap/insert/delete_terrain` renumber terrains (gamemap.cc:1271-1468), and saving clears the flag (gamemap.cc:475).
* **Edge contract:** the outer band of B ≈ S/2 hi-res px of **every** override is a deterministic function D of that override's own 1x border pixels. This makes per-tile, per-terrain and NN neighbours meet at the original 1x step.
* Upscale in context (chunk windows or toroidal macro-textures). Context renders must emulate the `paint_tile` fill. The current `art_original/chunks` and `superchunks` have black index-0 holes under every RLE tile (gap_1 §6).

### 4.8 Identity, provenance, formats
* **Key:** `(ShapeFile, logical shape, frame)`. Imports are keyed by their logical numbers 2048+. Fonts are keyed by `(source spec, font, glyph)`.
* **Validity:** CRC32 of the canonical decoded frame: `u8 kind | i16 extents | index raster with transparent=255 | mask`. Computed by the same libshapes helper in `--dump-art` and in the runtime loader. On mismatch the engine falls back to NN, which covers mods, `<PATCH>`, FoV and SS.
* **Authoring PNG:**
  * 8-bit palette PNG with **raw engine indices**;
  * PLTE = effective palette 0;
  * tRNS only on 255 (RLE frames);
  * **no** ipack/ES `transp_to_0` +1 rotation (pngio.cc:215-252), because `Import_png8` reverses that rotation only when a tRNS entry is present, so mixing conventions shifts every index;
  * naming `SSSS_FF.png`;
  * JSON sidecar `{file, shape, frame, extents, class flat|rle|translucent-painted, crc, masks}`.
* **Shipping:** a sparse companion VGA of S× RLE frames (`<…>/hires/x6/shapes.vga`, gumps, sprites, faces …), opened as a path. It costs about 0.9–1.1 B per opaque pixel and has no decode hitch.
* Layout:
  * `<PATCH>/hires/x6/…` for mod-specific art;
  * `<PREFIX_HIRES>/x6/…` for the base pack, which survives mod activation (`<PATCH>` is replaced by the mod's dir, modmgr.cc:70-74).
* **Effective sources.** Extract with the engine's own source stacks, not only static `shapes.vga`. The engine draws from 8 `Vga_file`s plus fonts, pointers and menu shapes, with patch layering and SI imports (gap_6). `art_original` is correct today for flats 0..149, because no installed mod touches them. It is incomplete for everything else.

### 4.9 Budgets (S=6)
* World buffer: 2.3 MB (320x200 view).
* Flats cache: 576 KiB per terrain. Working set about 30 entries (17.7 MB); 100 entries = 59 MB.
* All hi-res flats: 9 MB BG / 10.8 MB SI.
* Map frame working set (10x10 chunks): p95 15 MB, max 21 MB.
* Peak scene, map plus actors plus UI: about 50 MB.
* All shapes.vga resident: about 220 MB BG / 270 MB SI.
* LRU default: 256 MB desktop, 64 MB 32-bit/Android.
* Upload per full frame: 2.3 MB (INDEX8) / 9.2 MB (ARGB).

---

## 5. Risks (ranked) and open questions

### 5.1 Risks

| Rank | Risk | Sev. | Where | Mitigation |
|---|---|---|---|---|
| 1 | Logical and physical units mixed in buffer fields lead to out-of-bounds writes. `BeginPaintIntoGuardBand` would write `ibuf->width = draw_surface->w − 6` (physical into logical). `copy()` is unclipped. The `show()` gate silently skips the world | Critical | imagewin.cc:1142-1252, 968; ibuf8.cc:43-68 | W8 first; B4 clipping; ASan builds; oracles O1, O6 |
| 2 | Fatal exception when a texture exceeds the renderer limit (16384 on D3D11/12/Vulkan): startup, VideoOptions or Studio zoom | Critical | imagewin.cc:555-561, 695-709 | D5 with `S_tex`; fail soft by lowering S |
| 3 | AI art breaks the index semantics (cycling, xform, 255, ramps): water blinks, shadows are wrong, holes appear | High | art pipeline | §4.2 quantizer limits, mask copy, `exult_hirescheck` P1-P4 |
| 4 | Terrain seams: about 80% of tile edges with per-tile art, a 768-px grid with per-terrain art | High | chunkter.cc | Edge contract, context upscaling, per-terrain content-hash overrides, seam QA over 64k adjacent pairs |
| 5 | P1-P4 change the 1x output (37% of used BG terrains), which invalidates hashes and goldens | High | chunkter.cc:104,119-126,259 | Land them before hashing any art; regenerate goldens once |
| 6 | Stale or wrong art under mods, `<PATCH>`, imports or ES reload | High | vgafile.cc:866-885; shapeid.cc:420-468 | CRC gate; per-`Vga_file` generation; P8 |
| 7 | Memory: the flats LRU is fixed at 100 (59 MB at S=6) and thrashes above 100 chunks; hi-res frames grow unbounded; 32-bit targets | High | chunkter.cc:234-242; vgafile | G12, D5 (large views give small S), byte-budget LRU |
| 8 | Painting has side effects, so any second render or 1x mirror changes game state | High | actors.cc:5133-5148; animate.cc | No dual write (D3); derive any 1x image by downsampling |
| 9 | A hi-res payload is evicted while a raw `Shape_frame*` or payload pointer is in use | High | shapeid.cc:552-576; vgafile.cc:802-824 | Never free `Shape_frame`; evict only at the frame boundary |
| 10 | `encode_rle` stack overflow on hi-res rows | High | vgafile.cc:295 | P6 before encoding anything |
| 11 | Performance: a -O0 build costs 6–23 ms per frame at S=6; modal loops repaint the world per event; large Auto views | Med | build-linux; Gump_manager.cc:1101-1111 | P9; D5 caps S×view at about the display; optional UI-only repaint |
| 12 | INDEX8 textures need SDL ≥ 3.4; LINEAR/PIXELART sampling of palettes is unverified on D3D11/12 (the Windows target) | Med | imagewin.cc present path | ARGB baseline; `SDL_VERSION_ATLEAST` plus runtime fallback; test on the 5070 Ti |
| 13 | `ibuf` means "current target", so present code can act on a pushed layer | Med | imagewin.cc:968, 1738-1781 | `main_ibuf` (W1/W6); W10 decoupling |
| 14 | Partial overrides pop inside animation, actor, barge and missile groups | Med | §4.5 | Strict activation; the validator names the blocking frames |
| 15 | Visual mismatch until M3: UI, icons, dragged item and cursor stay 1x; outlines and grids are S px thick | Low | drag.cc, layers | Accepted for M1/M2; layer content scale in M3 |
| 16 | Four build descriptions drift; no libpng on Android/iOS | Low | build files | C6; companion VGA as the runtime format |
| 17 | Downscale aliasing when r < 0.5 (no mipmaps); non-integer vertical scaling in ACF | Low | UpdateRect | The snap rule keeps r ∈ [0.5,1]; a halving chain only for forced S |
| 18 | HiDPI windowed: display in points underestimates S (macOS) | Low | imagewin.cc:663-671 | Use `SDL_GetCurrentRenderOutputSize` for S_eff |

### 5.2 Open questions that need a decision (with the recommended default)

| # | Question | Recommendation | Blocks |
|---|---|---|---|
| Q1 | Fixed S=6 or derived S_eff? | Derived (D5) with snap to {1,2,3,6}, `render_scale=auto`, plus `force:N` for authoring and screenshots. For the 3440x1440 fullscreen, suggest the presets `scale=6` Auto ACF (573x200, 4.13 Mpx) or `scale=5` (688x240) | M1 |
| Q2 | SDL floor: 3.4 (INDEX8) or keep ARGB? | ARGB-LUT baseline first; INDEX8 behind `SDL_VERSION_ATLEAST(3,4,0)` plus a runtime check. The Windows MSYS2 build ships 3.4.x anyway | M1 |
| Q3 | Fallback for frames without an override: NN on the fly or cached pre-upscaled? | On the fly (measured faster for sprites, no memory). Cache only flats (S× chunk cache) | M1 |
| Q4 | Terrain: S× per-terrain cache or per-tile blits every frame? | Cache, sized from the view plus a byte budget (0.04 ms vs 0.18 ms per frame) | M1 |
| Q5 | Edge band width B and function D | B = S/2 = 3, D = NN index replication for v1; A/B test a smooth-plus-dither D on grass, water and roads | M1 art |
| Q6 | Which terrains get per-terrain art? | Top 100–200 by usage (68–81% of BG map chunks) plus transition-rich ones (coasts, roads, towns) | M1 art |
| Q7 | Outline and grid thickness at S | NN-faithful (S px) by default, so the oracle needs no exemption. For hi-res payloads, the outline comes from the hi-res mask with a stroke of about S/2..S | M2 |
| Q8 | Sub-game-pixel lerp? | Defer. Opt-in later and exempt from the oracle | — |
| Q9 | Strict or lenient group activation by default? | Strict for players; lenient as a dev config key | M2 |
| Q10 | Reference configuration for the base pack: BG+FoV with or without SI configured? | Without SI for M1 (flats are unaffected). Decide before M2 (adds skins 2048–2059, SI paperdolls, gumps 2048/2049) | M2 |
| Q11 | Pack keying: per-configuration packs, or a CRC-addressed pool plus a key→CRC map? | CRC pool plus a per-configuration map (frames are shared across configurations) | M2 |
| Q12 | Hi-res reflections: pre-generated or derived at runtime? | Derived (about 0.2–1 ms per actor frame, evictable); explicit only under R2 | M2 |
| Q13 | UI-only repaint path in M1? | No. Revisit for low-end and large views | later |
| Q14 | Aspect-correct modes: does S follow `py` or `py/1.2`? | `py/1.2` (render at the horizontal ratio; the presenter stretches by 1.2 as today) | M1 |
| Q15 | Paletted screenshot and `--buildmap` at S? | Screenshot: S× (an authoring feature). `--buildmap`: force S=1. A full S=6 map is 151 MB per superchunk; use `--render-test` instead | M1 |
| Q16 | Stop destroying the SDL renderer on every `resized()`? | Not in M1. Revisit if GPU-side atlases appear | — |
| Q17 | Android/32-bit defaults | S ≤ 3, LRU 64 MB, companion VGA only (no libpng) | M2 |
| Q18 | Fix `Paperdoll_source_parser::erased_for_patch` (miscinf.cc:212-215, 330-332)? | Separate, optional commit; it changes behaviour for mods | — |

---

## 6. Contradictions resolved (checked in code)

| # | Claim A | Claim B | Code evidence | Verdict |
|---|---|---|---|---|
| C1 | ibuf.md: `rotate_colors` cycles 0xE0-**0xFF** | palette.md, shapes.md: 0xE0-**0xFE** | `Game_window::rotatecolours` calls `rotate_colors(0xfc,3)`, `(0xf8,4)`, `(0xf4,4)`, `(0xf0,4)`, `(0xe8,8)`, `(0xe0,8,1)` (gamewin.cc:1063-1068), so the last cycled index is 0xFE. 0xFF is the border/transparent index and is never cycled | **0xE0-0xFE.** ibuf.md is wrong. Also, gap_5's "xform F4..FE" and the comment at shapeid.h:87-88 are stale: `nblends = 17` (shapeid.cc:306-334), so the translucent range is **0xEE-0xFE** |
| C2 | build.md: master uses only SDL 3.2 APIs; CI pins 3.2.14 | present.md: local SDL is 3.4.18; recommends INDEX8 plus `SDL_SetTexturePalette` and `SDL_SCALEMODE_PIXELART` | `deps/prefix/lib/pkgconfig/sdl3.pc` says Version 3.4.18. `build-linux/config.log:2574-2575` links deps/prefix. `SDL_render.h:1016` declares `SDL_SetTexturePalette`; `SDL_surface.h:92` says PIXELART "available since SDL 3.4.0". configure.ac:480 has no version floor. There are no `SDL_VERSION_ATLEAST` uses | **Both true.** The fork must compile against 3.2.14, so the ARGB+LUT path is the baseline and INDEX8/PIXELART are optional under `SDL_VERSION_ATLEAST(3,4,0)` with a runtime fallback (C7, Q2) |
| C3 | world.md: `Shape_manager::cache_shape` is "a natural single lookup point" for all world shapes | shapes.md, ui.md: the hook must be on `Shape_frame`, set from `Vga_file::get_shape`, dispatched on the target buffer's scale | `ShapeID::paint_invisible` and `paint_outline` call `get_shape()`, not `cache_shape()` (shapeid.h:358-366). There are 95 `sman->paint_shape(…, Shape_frame*)` call sites with raw frames (grep, excluding mapedit and tools), for example intrinsics.cc:1816-1840 and chunkter.cc:308. Fonts call `shape->paint_rle` directly (font.cc:279, 318) | **Hook on `Shape_frame`** (H2, H3). `cache_shape` stays untouched. Refinement from gap_3 (see C7): `get_shape` only stamps the identity, and the payload is resolved on the paint path |
| C4 | ibuf.md (a′): `draw_surface` = S·logical + 2gb, logical API on the same `Image_buffer8`, fix about 120 scaler reads | present.md: keep `ibuf`/`draw_surface` and every game-px member unchanged, add a separate `hires_surface` and texture, bypass phases 1–2; dual write vs replacement left open. world.md and ui.md assume a per-buffer `render_scale` | The gate `ibuf->width + 2*guard_band == draw_surface->w` (imagewin.cc:968) fails silently if `draw_surface` becomes physical while `width` stays logical. `BeginPaintIntoGuardBand` writes `draw_surface->w − 3·gb/2` into `ibuf->width` (1198-1199). The 122 scaler reads are confirmed. `scale_layer_color` patches and restores the current target (1758-1779). Paint side effects rule out dual write | **Merged design (D2–D4).** Per-buffer scale with a logical API (ibuf/world/ui). The **existing `ib8` is mutated in place** and its memory is a physical `draw_surface` (no second surface, no dual write). Phases 1–2, the scalers and guard-band painting are **bypassed** when S>1, as present.md proposes, so the scaler reads need **no** change in M1. The gate is replaced by `ibuf == main_ibuf`. `create_surface` sets logical dims `(w−2gb)/S`. The present path uploads rect×S to `world_texture` |
| C5 | palette.md: the phase-1 scaler converts 8-bit to RGB | present.md: in default case A phase 1 is skipped and conversion happens in phase 2 | In case A (`scaler==fill_scaler` or `scale==1` or SDLScaler with point/bilinear, imagewin.cc:740-745) `inter_surface = draw_surface`, phase 1 is skipped (972), and phase 2 converts the **whole** surface (1017-1049) or SDL-blits it whole (1051-1093) | **present.md is right.** The RGB conclusion still holds: any downscale today averages RGB, not indices. Today a full-surface conversion runs on every `show()`, not per dirty rect, so a per-dirty-rect upload at S is not worse than today |
| C6 | ibuf.md: "only the game world goes to the main buffer" | ui.md: there are more main-buffer writers | Verified writers: the modal backdrop `background->paint()` straight to the window (Gump_manager.cc:323-326); the `ImageBufferPaintable` get/put snapshot (shapeid.cc:643-654); the legacy drag, bbox and drop path (drag.cc:418-440); gump `paint()` called while handling events, which lands in the game window (comment at Gump_manager.cc:1101-1105); non-layer gumps in `render_gumps_to_layer` (336, unreachable today because `uses_render_layer()` defaults to true); `display_area` with `clear_screen` and raw sprite 10 (intrinsics.cc:1812-1840); item-menu outlines; egg and barge debug markers; plasma; `clear_screen` | **ui.md is more accurate.** All of these go through the scaled primitives, so they work at S with no change (R1, R2, R4, G7, G10). Only `ImageBufferPaintable` needs B5/B6 |
| C7 | shapes.md §7.4: attach hi-res lazily **in `Vga_file::get_shape`** | gap_3: **never** in `get_shape`; resolve on the paint path | `read_shape_info` calls `get_num_frames(i)` for every shape (shapeid.cc:123-131), and that does `get_shape(n,0)` "to force it into memory" (vgafile.h:400-408). Loading payloads there would cost about 25 MB and 0.2–0.3 s at startup | **Split:** `get_shape` stamps the identity (no I/O, a few bytes). The payload is resolved on first paint into an S>1 target (D8, H4) |
| C8 | palette.md: the quantizer outputs `0x00-0xDF` | upscale-research/algorithmic.md: `0x01-0xDF` | Index 0 is opaque black everywhere except `copy_transparent8`, which has no engine callers (grep) | **Both safe.** `0x01-0xDF` is a conservative choice that avoids the duplicate colours 0/252 |
| C9 | Terrain cache at S=6: "56 MiB" (gap_1, shapes) vs "58/59 MB" (ibuf, world, gap_3) | — | 100 × 589,824 B = 58.98 MB = 56.25 MiB | Same number in different units |
| C10 | Chunks per 320x200 repaint: 20 (world, gap_4), 30 (gap_2), "4x3 visible" (gap_3) | — | 20 is a typical scroll position; 30 is the maximum over all 16 scroll residues of the `paint_map` range (gamerend.cc:206-219); 4x3 counts visible chunks only | Different definitions. Size the cache from the maximum: `(cw+3)(ch+3)` = 30 |
| C11 | Task goal "S=6 (1920x1200)" | gap_2: fixed S=6 is wrong for the active fullscreen profile and can crash | gap_2 §3 table, from the `get_draw_dims` maths (imagewin.cc:1315-1436) | Not a contradiction but a policy. S_art = 6, and S_eff is derived (D5), giving 6 for the windowed profile |
| C12 | shapes.md: hi-res flats as RLE frames painted with `paint_rle` | gap_1: compose raw 8S×8S tiles into the cache | `Shape_frame` asserts 8x8 for non-RLE frames (vgafile.cc:363, 384). The flat/RLE decision comes from the data | Compatible. **Containers** store hi-res flats as RLE (companion VGA) or PNG; the terrain store decodes them to raw 48×48 tiles at load and composes the cache with `memcpy` |

---

## Appendix: key measured numbers (sources)

| Item | Value | Source |
|---|---|---|
| 1x full world paint, 320x200, -O2 | 0.026–0.21 ms (the objects pass is 80–100% of it) | gap_4 §3.1 |
| S=6 world paint, 320x200, -O2 | NN on the fly 0.32–1.33 ms; S² data 0.37–1.57 ms | gap_4 §3.3 |
| S=6 world paint, -O0 (current build-linux) | 1.1–7.1 ms (NN), 6–23 ms (S² data) | gap_4 §3.5 |
| Present at 1920x1200 | LUT 8→32 0.90 ms; 2x2 box 0.76 ms; fused LUT + 3x3 area to 640x400 1.69 ms | present.md §7.5 |
| Frame budget (vsync 60 Hz plus a 10 ms Delay) | about 6.7 ms of work per iteration | gap_4 §4 |
| Override PNG decode / `encode_rle` | 140–240 Mpx/s / 200–380 MB/s; worst frame (1920x1200) about 10 ms | gap_3 §2.6 |
| Headless determinism | `--buildmap 2`: 144 PNGs in 19.5 s, byte-identical across runs | build.md §6.3 |
