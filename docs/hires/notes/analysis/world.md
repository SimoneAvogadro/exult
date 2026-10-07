# World rendering analysis (terrain, objects, effects) — Exult master @ 8b6ab6b43

Scope: `gamerend.cc/.h`, `gamewin.cc/.h`, `objs/chunkter.*`, `objs/chunks.*`, `objs/objs.cc`,
`objs/iregobjs.cc`, `objs/animate.cc`, `objs/egg.cc`, `objs/barge.cc`, `actors.cc`, `monsters.cc`,
`effects.cc/.h`, `gamemap.cc`, `drag.cc`, `cheat.cc`, plus the buffer primitives they call
(`imagewin/ibuf8.*`, `imagewin/iwin8.h`, `shapes/vgafile.*`, `shapeid.*`) as far as needed to follow
the data flow. All references are `file:line` in `/home/simonea/ultima7_exult/exult-hires`.

Goal context: render the world at integer render scale **S** (target 6) into a physical buffer
S times larger, keep all game logic / hit-testing in game pixels, allow per-(file,shape,frame)
hi-res overrides, and downscale at present time when needed.

---

## 1. Coordinate systems in use

| Space | Units | Where defined / computed |
|---|---|---|
| World tile | `tx,ty` 0..3071 (`c_num_tiles`), `tz` lift 0..15 | `exult_constants.h:28-38` (`c_tilesize=8`, `c_tiles_per_chunk=16`, `c_chunksize=128`, `c_num_chunks=192`) |
| Chunk | `cx,cy` 0..191; superchunk 0..11 | same |
| Scroll | `scrolltx, scrollty` = top-left tile of the view | `gamewin.h:137`; set in `set_scrolls` `gamewin.cc:1085` |
| Smooth-scroll sub-tile offset | `scrolltx_lo/scrollty_lo` in **game pixels** (0..7), `avposx_ld/avposy_ld` camera-actor pixel delta | `gamewin.h:975-981`, computed `gamerend.cc:434-514` |
| Game ("screen") pixels | origin = top-left of game area; may be negative inside the letterbox/"full" area | `get_shape_location` `gamewin.cc:1259-1313`: `x = (tx+1-scrolltx)*8 - 1 - 4*tz - scrolltx_lo (+avposx_ld)` |
| Buffer pixels | **identical to game pixels today**; `Image_buffer::bits` is pre-offset so (0,0) = game area top-left (`offset_x = (full_w - game_w)/2`) plus a 4-px scaler guard band | `imagewin/imagewin.cc:564-577`, `get_start_x()` = `-offset_x` `imagewin.h:624` |
| Layer pixels | each overlay layer has its own logical (game-pixel sized) 8-bit buffer; placed with a dest rect in **screen pixels** computed via `game_to_screen` | `effects.cc:1163-1215`, `drag.cc:317-407` |
| Screen pixels | window/renderer pixels | `Image_window::screen_to_game/game_to_screen` `imagewin.cc:1277-1313` |

**Central fact:** every world-drawing call site computes positions in game pixels and passes
them unchanged to `Image_buffer8` primitives, which write one byte per game pixel. There is no
abstraction between "game pixel" and "buffer pixel" anywhere in the world renderer. `c_tilesize`
is used 264 times in 22 non-tool files; changing the tile size is not an option; S must be an
orthogonal factor applied at the buffer level.

---

## 2. Buffer ownership and the drawing choke points

* `Game_window::win` is an `Image_window8*` (`gamewin.h:103`), created in the ctor
  `gamewin.cc:326` with `(width, height, gwidth, gheight, scale, fullscreen, scaler, fillmode, fillsclr)`.
* `Image_window8::ib8` / `ibuf` is the active 8-bit target (`iwin8.h:44,55`). It can be swapped with
  `set_render_buffer` (`iwin8.h:63-68`) — used by `Game_window::push_render_target/pop_render_target`
  (`gamewin.cc:541-550`) to redirect drawing into overlay layers (text effects, dragged item, gumps).
* `Shape_frame::scrwin` is a **static** render target (`shapes/vgafile.h:55,69`), set to
  `win->get_ib8()` in the Game_window ctor (`gamewin.cc:330`) and after resize (`gamewin.cc:921`),
  and swapped by push/pop_render_target. All `Shape_frame::paint_*` (x,y) overloads paint into it
  (`vgafile.h:98-120`). `Font::paint_text` ignores its `win` argument and paints into `scrwin`
  too (`shapes/font.cc:260,276-279`).

Consequently, **every pixel the world renderer writes goes through a small set of
`Image_buffer8` methods** (`imagewin/ibuf8.h:63-114`):
`copy` (unclipped memmove, `ibuf8.cc:43-68`), `fill8`, `fill_hline8`, `draw_line8`, `copy8`
(src stride = srcw, `ibuf8.cc:360-384`), `copy_hline8`, `copy_hline_translucent8`,
`fill_hline_translucent8`, `fill_translucent8`, `copy_transparent8`, `put_pixel8`, `paint_rle`,
`paint_rle_remapped`, `get/put` (ImageBufferPaintable), `fill_static` (intro only).
Shape painting reaches them through `Shape_manager::paint_shape / paint_invisible / paint_outline`
(`shapeid.h:171-196`) → `Shape_frame::paint_rle / paint_rle_remapped / paint / paint_rle_translucent /
paint_rle_transformed / paint_rle_outline` (`shapes/vgafile.cc:483-699`).

Because the render scale must differ between the world buffer and the 1x layer buffers that
share these code paths (push/pop_render_target swaps both), **S has to be a property of the
target `Image_buffer8` instance**, not a global.

Since the recent "overlay layers" work (git log: `f9c7508ae Turning UI elements into overlay
layers`, `9d296a88e`, `e29aa959b` …), gumps (`Gump_manager::paint` → `render_gumps_to_layer`,
`gumps/Gump_manager.cc:831-836`), the mouse pointer, text effects and the dragged item live in
separate layers composited on top of the main image. **The main buffer now essentially contains
only the world + world effects + letterbox border**, which makes it a good candidate to become the
S-x buffer while layers stay 1x.

---

## 3. Order of painting a frame

Main loop (`exult.cc:1385-1523`):

1. Events, then `tqueue->activate(ticks)` (`exult.cc:1391`): animators, NPC steps, effects'
   `handle_event`. These mostly only call `add_dirty`, **but some write the buffer immediately**:
   * actor/camera movement → `scroll_if_needed` (`gamewin.cc:1165`) → `view_left/right/up/down`
     (buffer copy + strip repaint, see §5);
   * `Earthquake::handle_event` copies the buffer, `show()`s, copies back (`effects.cc:1829-1873`);
   * `Rain_effect::handle_event` only adds dirty + `set_painted` (`effects.cc:1412-1439`).
2. Smooth scrolling enabled (`config/gameplay/smooth_scrolling`, `gamewin.cc:387`): every frame
   within the lerp window `paint_lerped(factor)` (`exult.cc:1445-1503`) → full repaint
   (`gamerend.cc:434-514` → `paint()` `gamerend.cc:419-425`).
   Otherwise, if dirty: `paint_dirty()` (`exult.cc:1504-1507`, `gamerend.cc:624-632`).
3. `Mouse::show()` (layer), `rotatecolours()` (palette rotation of 0xE0..0xFE,
   `gamewin.cc:1054-1079`), `gwin->show()` → `win->show()` (scaler + layer composite)
   (`gamewin.h:747-755`).

`Game_window::paint(x,y,w,h)` (`gamerend.cc:328-414`), in game pixels:

1. `win->BeginPaintIntoGuardBand(&x,&y,&w,&h)` — may enlarge the rect and temporarily grow
   `ibuf->width/height` and `game_width/height` by `guard_band/2` = 2 px on right/bottom
   (`imagewin.cc:1142-1211`); undone by `EndPaintIntoGuardBand` (`imagewin.cc:1213-1224`).
2. Clamp to the game rect → `win->set_clip(gx,gy,gw,gh)` (`gamerend.cc:339-357`).
3. `render->paint_map(gx,gy,gw,gh)` if there is a main actor, else `win->fill8(0)` (`:361-365`).
4. `effects->paint()` — all `Special_effect::paint()` (sprites, projectiles, rain, clouds) still
   clipped to the game rect (`:367`, `effects.cc:286-290`).
5. `win->set_clip(x,y,w,h)` (full requested rect) and `fill8(border_index)` the four letterbox
   strips (`:369-384`).
6. `gump_man->paint(false)` (layers), `dragging->paint()` (layer, or legacy direct paint),
   `effects->paint_text()` (layers), `gump_man->paint(true)` (modal layers) (`:386-391`).
7. If the repaint covered the entire game rect: sum party carried light + `light_sources` returned
   by `paint_map`, expire light spell, `clock->set_light_source(...)` → **palette** change only
   (`:394-410`).
8. `EndPaintIntoGuardBand`, `clear_clip` (`:412-413`).

`Game_render::paint_map(x,y,w,h)` (`gamerend.cc:192-291`):

* `render_seq++`, `painted = true`.
* Chunk range: start = 1 tile left/up of the rect; stop = **2 chunks + 8 tiles** beyond the
  right/bottom edge (`:206-219`) so that objects whose origin is down-right but which extend
  up-left (lift: 4 px per lift level; large shapes) are painted.
* `skip_lift == 0` → terrain-editor mode `paint_terrain_only` (`:220-223`, `:157-183`,
  `Chunk_terrain::render_all` `objs/chunkter.cc:284-312`).
* Pass 1 — flats: for each chunk `paint_chunk_flats(cx,cy,xoff,yoff)` (`:227-233`) →
  `win->copy8(cflats->get_bits(), c_chunksize, c_chunksize, xoff, yoff)` (`:520-531`).
  `xoff/yoff = Figure_screen_offset(...) - scroll*_lo` (`:74-85`).
* Pass 2 — flat RLE terrain objects (`Flat_object_iterator`, `obj->paint()`) (`:235-241`, `:537-549`).
* Map-editor chunk outlines: `fill8` 1-px lines + `paint_text` (`:243-251`, `:91-103`).
* Pass 3 — non-flat objects, chunks traversed diagonally NE (`:254-265`); per chunk
  light-source counting then `Nonflat_object_iterator` + `paint_object` (`:557-586`) which
  recursively paints dependencies first (`:592-618`), optionally drawing bbox back/front via
  `Shape_info::paint_bbox` → `draw_line8` (`shapes/shapeinf.cc:546-607`).
* Dungeon blackness `paint_blackness` (`:267-269`, `:648-708`): tile-granular `fill8` with index 0
  (73 in ice dungeons), offset by `in_dungeon<<2` px.
* Outline of cheat-selected objects `paint_outline(HIT_PIXEL)` (`:272-279`).
* Map-editor tile grid (`fill_translucent8` 1-px lines, `:109-125`) and selected chunks
  (`fill_translucent8` chunk rects, `:131-151`).

Object `paint()` variants (all compute `get_shape_location` in game px then go through
`ShapeID::paint_shape / paint_invisible / paint_outline`, `shapeid.h:348-365`):
`Game_object::paint` (`objs/objs.cc:959-964`), `Ireg_game_object::paint` (invisible via xform,
`objs/iregobjs.cc:57-66`), `Animated_*::paint` (calls `animator->want_animation()` first,
`objs/animate.cc:592-660`), `Egglike_game_object::paint` / `Egg_object::paint` (egg area lines
with `draw_line8`, `objs/egg.cc:650-654`, `1041-1084`), `Field_object`, `Mirror_object`
(`objs/egg.cc:1581-1586, 1726-1728`), `Barge_object::paint` (only debug outline via `fill8`,
`objs/barge.cc:710-734`), `Actor::paint` (`actors.cc:2078-2113`: `dont_render` flag, invisible
NPCs → `paint_invisible` (xform), otherwise `paint_shape(...,true)` = forced translucency,
`paint_weapon` (`:2118-2147`), status outline `paint_outline(HIT/CHARMED/PARALYZE/PROTECT/CURSED/POISON_PIXEL)`),
`Npc_actor::paint` (`actors.cc:5133-5148`) and `Monster_actor::paint` (`monsters.cc:365-371`).

**Paint has game-logic side effects**: `Npc_actor::paint` resumes dormant schedules and adds the
NPC to the "nearby" list (`actors.cc:5135-5147`); `Animated_*::paint`, `Egg_object::paint`,
`Field_object::paint`, `Monster_actor::paint` start animators (`want_animation`). They are
idempotent per frame, but **the world must not be rendered twice per frame through the object
paint path** to produce e.g. a 1x mirror; derive any 1x image by downsampling instead.

---

## 4. Dirty-rectangle logic

* One single bounding rectangle `TileRect dirty` (`gamewin.h:139`); `add_dirty(r)` unions
  (`gamewin.h:782-784`, `TileRect::add` `rect.h:73`), `set_all_dirty()` = whole **full** buffer
  incl. letterbox (`gamewin.h:778-780`), `clear_dirty()` sets `w=0` (`:757-759`).
* `add_dirty(const Game_object*)` (`gamewin.h:788-798`): `get_shape_rect(obj)`
  (`gamewin.cc:1223-1253`, uses the 1x `Shape_frame` extents, ignores lerp offsets), enlarged by
  `1 + c_tilesize/2` = 5 px, clipped to the full rect; **returns false when off-screen**.
* That boolean drives logic: `Frame_animator` and `Wiggle_animator` stop animating when
  `add_dirty` fails (`objs/animate.cc:439-452, 552-555`); Npc movement code uses it for
  dormancy (`actors.cc:5268, 5291`). Dirty rects must therefore stay computed in game pixels.
* Other producers: `Actor::add_dirty` adds weapon rect (`actors.cc:763-796`), `Barge_object::add_dirty`
  (box + stretch, `objs/barge.cc:334-353`), `Sprites_effect::add_dirty` (enlarge 12 px,
  `effects.cc:358-369`), `Projectile_effect/Homing_projectile::add_dirty` (`effects.cc:703-714, 899-907`),
  particles (`effects.cc:1293-1322`), clouds (`effects.cc:1704-1725`), `Text_effect::add_dirty`
  (`effects.cc:1039-1043`), drag (`drag.cc:249-250, 279-293`).
* `paint_dirty()` (`gamerend.cc:624-632`) → `effects->update_dirty_text()`, clip dirty to full rect,
  `paint(box)`, `clear_dirty()`. Dirty rects added *during* paint (rain particles add dirty inside
  `paint`, `effects.cc:1320`) are discarded.
* Because the dirty area is a single union, two animations at opposite corners repaint nearly
  the whole view; with S=6 that repaint costs 36x the pixels (see §8).

---

## 5. Scrolling

* **Tile-granular scrolling** (default): `view_right/left/down/up` (`gamewin.cc:1641-1731`) change
  `scrolltx/ty` by one tile, then (unless gumps are showing → full `paint()`) call
  `map->read_map_data()`, `win->copy(...)` to shift the **game area** (not the letterbox) by
  `c_tilesize` px, `paint()` the newly exposed 8-px strip, and shift `dirty` by 8 px.
  `Image_buffer8::copy` is an **unclipped** `memmove` per line (`ibuf8.cc:43-68`).
* **Smooth scrolling / lerp** (`paint_lerped`, `gamerend.cc:434-514`): interpolates between the
  previous and current scroll tile, splits into whole tiles (`scrolltx`) and a pixel remainder
  `scrolltx_lo/scrollty_lo` (0..7 game px), plus `avposx_ld/avposy_ld` so the camera actor, party
  and boarded barge glide (`get_shape_location`, `gamewin.cc:1278-1307`). Every lerped frame is a
  **full repaint**. Sub-pixel granularity is limited to 1 game pixel; at S=6 it could be 1/6 game
  pixel if offsets were carried in physical units (optional improvement).
* No other sub-tile scrolling exists. The guard band (`imagewin.h:357`, `guard_band = 4`) is a
  scaler aid, not a scrolling margin.
* **Shake**: `Earthquake::handle_event` (`effects.cc:1829-1873`) shifts the whole game area by a
  random −4..+4 game px with `win->copy`, `show()`s, then copies back.

---

## 6. Terrain: chunk flats cache (`objs/chunkter.*`)

* Data: `Chunk_terrain::shapes[256]` (ShapeIDs, `chunkter.h:36`), shared by all `Map_chunk`s using
  the same terrain number (`num_clients`, `gamemap.cc:361-369`, `objs/chunks.cc:702-741`).
* `Map_chunk::set_terrain` turns **RLE** flats into `Terrain_game_object` / `Animated_object`
  objects (`objs/chunks.cc:726-740`) which are drawn in pass 2. Only **non-RLE** flats (raw 8x8,
  e.g. shapes 0..149 of shapes.vga) go to the cache.
* Cache: `Image_buffer8* rendered_flats` of `c_chunksize x c_chunksize` = 128x128 = 16,384 B,
  created lazily in `render_flats()` (`chunkter.cc:248-268`) by `paint_tile` for all 256 tiles:
  `rendered_flats->copy8(shape->get_data(), 8, 8, tx*8, ty*8)` (`chunkter.cc:86-133`). Under an
  RLE flat it copies a neighbouring flat to avoid black gaps (ice caves; note the bound check
  `tiley + y > 0` at `:104` should be `>= 0`).
* MRU circular list `render_queue` with `queue_size`; `get_rendered_flats()` moves to front
  (`chunkter.h:95-101`); when `queue_size > Figure_queue_size()` the tail's buffer is freed
  (`chunkter.cc:250-258`). `Figure_queue_size()` is hard-coded **100** (`chunkter.cc:234-242`,
  the commented-out code suggested `(cw+3)*(ch+3)` from window size). Max memory today ≈ 100 x
  16 KiB = 1.6 MB.
* Invalidation: only on edits (`commit_edits` → `render_flats`, `chunkter.cc:208-216`), destruction,
  or eviction. Cache holds **palette indices**, so palette changes (day/night, lightning, fog,
  cycling) never invalidate it — this must be preserved at S (keep it 8-bit indexed).
* Consumers: `Game_render::paint_chunk_flats` (`gamerend.cc:520-531`) and
  `Game_map::write_minimap` which averages all pixels of every terrain's cache
  (`gamemap.cc:1690-1745`, `ibuf->get_bits()` at `:1706`).
* **Waste:** `paint_chunk_flats` calls `get_rendered_flats()` for every chunk in the paint range,
  which extends 2 chunks + 8 tiles beyond the right/bottom edge. With a 320x200 view and
  `scrolltx=scrollty=5` the range is 5x4 = 20 chunks of which only 3x2 = 6 are visible; caches for
  ~14 fully off-screen chunks are built anyway (copy8 then clips them away). Harmless at 16 KiB,
  expensive at S=6.

Memory per cached chunk at scale S (8-bit): 16,384·S² B.

| S | chunk cache | 100 entries | main buffer for 320x200 view |
|---|---|---|---|
| 1 | 128² = 16 KiB | 1.6 MB | 64,000 B |
| 2 | 256² = 64 KiB | 6.6 MB | 256,000 B |
| 3 | 384² = 144 KiB | 14.7 MB | 576,000 B |
| 4 | 512² = 256 KiB | 26.2 MB | 1,024,000 B |
| 6 | 768² = 576 KiB (589,824 B) | **59.0 MB** | 1920x1200 = 2,304,000 B |

A full hi-res flat tile set at S=6 (48x48 = 2,304 B per frame; a few thousand frames) is only
~4-7 MB, so an alternative to caching at S is drawing per tile from a hi-res tile atlas.

---

## 7. Effects (`effects.cc/.h`)

| Effect | How it draws | Position units | Hi-res treatment |
|---|---|---|---|
| `Sprites_effect` / `Explosion_effect` | `sprite.paint_shape` (SF_SPRITES_VGA, `has_trans=true` for sprites, `shapeid.cc:563-566`) `effects.cc:452-460` | tile + int px offset/deltas | override via sprites.vga key; replicate otherwise. Bug: y uses `get_scrolltx_lo()` (`effects.cc:459`) |
| `Projectile_effect`, `Homing_projectile` | `paint_shape` at tile granularity minus lift px (`effects.cc:855-863, 998-1003`) | tiles | override; replicate |
| `Rain_effect<Raindrop/Snowflake/Sparkle>` | 200 `Particle`s (ShapeIDs in sprites.vga frames 3-7, 13-20, 21-27) via `paint_shape` (`effects.cc:1254-1457`); bounds from `win->get_game_width/height()` (`:1422-1423, 1450-1451`) | absolute game px; rain moves 6 px/100 ms | override sprites; replicate; bounds stay in game px |
| `Clouds_effect`/`Cloud` | sprites.vga shape 2 (translucent shadow) `paint_shape` (`effects.cc:1732-1739`), moved 1-3 px per 100 ms | absolute game px | good override candidate (large, soft); xform translucency works on indexed hi-res |
| `Lightning_effect` | palette `PALETTE_LIGHTNING` (`effects.cc:1497`) | — | none needed |
| `Fog_effect` | `gclock->set_fog` palette + sparkles (`effects.cc:1611-1634`) | — | none |
| `Earthquake` | buffer `copy` ±4 px, `show`, copy back (`effects.cc:1829-1873`) | game px | scale copy by S, or move shake to presentation offset |
| `Text_effect` | own overlay layer, `paint_to_layer` (`effects.cc:1163-1215`), anchored with `game_to_screen` | game px → screen px | independent of world buffer if `game_to_screen` stays correct |
| `Fire_field_effect` | creates world objects (`effects.cc:1879-1902`) | — | via object path |

Other direct pixel writes into the world buffer:
* `paint_blackness` `fill8` (tile-granular) — replicate exactly.
* `Shape_frame::paint_rle_outline` (status/selection outlines): `put_pixel8` at run ends and
  `fill_hline8` on first/last line (`vgafile.cc:640-699`) — 1 game px thick; at S either S-px thick
  (faithful) or derived from the override's alpha mask.
* `paint_rle_transformed` (invisible): applies `invis_xform` over the shape coverage (`vgafile.cc:596-634`).
* `paint_rle_translucent`: indices `0xff - xfcnt .. 0xfe` are translucent via `xforms`
  (`vgafile.cc:540-589`; `xfcnt = nblends`, 17 by default, `shapeid.cc:306-360`) — overlaps the
  palette-cycling range 0xE0..0xFE (`gamewin.cc:1063-1068`), so override art must respect both
  conventions (and 0xFF = transparent for RLE).
* Map-editor/debug: tile grid, selected chunks, chunk outline + text (`gamerend.cc:91-151`),
  egg-area lines (`objs/egg.cc:1069-1082`), barge outline (`objs/barge.cc:724-732`), bboxes.
* `Game_window::write` "Saving game" dim + text (`gamewin.cc:1431-1432`), `Game::waitforspeech`
  dim (`game.cc:815`), `plasma` load screen (`gamewin.cc:2982-3001`, `w*h*6` random 3-px crosses).
* `fill_static` is only used by the BG intro (`gamemgr/bggame.cc`), not by the world.

Palette effects (day/night, light sources via `clock->set_light_source`, fades in `palette.cc`,
lightning, fog, infravision, invisibility palette, color cycling) never touch buffer pixels, so
they keep working unchanged **as long as the S-x buffer stays 8-bit palette-indexed**.

---

## 8. Readers of buffer pixels / S-sensitive consumers

* `Game_window::create_mini_screenshot` → `paint_map` then `Image_window8::mini_screenshot`
  (`gamewin.cc:3126-3139`, `imagewin/iwin8.cc:181-226`) reads `ibuf->get_bits()` with game-px
  indices (288x180 centre, 3x3 average) — must sample an S x S area per game px.
* `ImageBufferPaintable` snapshots/restores the whole buffer with `create_buffer(full_w, full_h)`
  + `get/put` (`shapeid.cc:643-656`).
* `Game_map::write_minimap` reads every terrain cache (`gamemap.cc:1704-1724`); works at S but
  builds every terrain at S (thrashing the 100-entry queue, 36x work) — prefer a 1x path.
* `--buildmap` (`exult.cc:2860-2908`) renders 2048x2048 game px per superchunk via
  `paint_map_at_tile`; at S=6 that is 12288² ≈ 151 MB per buffer — force S=1 (or make it an
  explicit hi-res export option).
* `UI_view_tile`/crystal ball paints the map into a 320x200 sub-rect via `paint_map_at_tile`
  (`usecode/intrinsics.cc:1837`) — fine with a scaled-buffer API.
* Screenshots (`save_screenshot.cc`) — imagewin subsystem.

---

## 9. Every "1 game pixel = 1 buffer pixel" assumption (world side)

1. All `Image_buffer8` primitives take buffer px; callers pass game px (§2).
2. `Chunk_terrain::rendered_flats` sized `c_chunksize` (`chunkter.cc:259`); `paint_tile` copies 8x8
   raw data at `tx*8,ty*8` (`:91,130`).
3. `paint_chunk_flats` passes `c_chunksize` as both width and **source stride** to `copy8`
   (`gamerend.cc:529`; stride = srcw in `ibuf8.cc:371-382`).
4. `Shape_frame::paint` non-RLE: `copy8(data, 8, 8, x-8, y-8)` (`vgafile.cc:525-534`);
   `Chunk_terrain::render_all` same (`chunkter.cc:300-302`).
5. RLE decoders write one byte per source pixel (`ibuf8.cc:516+`, `vgafile.cc:540-699`).
6. Scrolling copies by `c_tilesize` px (`gamewin.cc:1654,1676,1700,1722`); Earthquake ±4 px.
7. Lerp offsets `scrolltx_lo/avpos*_ld` are integer game px (`gamerend.cc:475-506`).
8. Clip rectangles (`set_clip`) and `is_visible` culling (`vgafile.cc:491-495`) in game px.
9. Guard-band painting extends `game_width/height` by 2 px (`imagewin.cc:1184-1199`).
10. `mini_screenshot`, `ImageBufferPaintable`, `write_minimap`, `--buildmap` (§8).
11. Dragged-item layer is created at the 1x frame size (`drag.cc:322-336`) and its screen scale is
    derived from `game_to_screen` (`drag.cc:367-405`).
12. 1-px debug lines / outlines / grids become S-px thick under naive replication.

Assumptions that are **not** a problem: hit-testing (`find_object` uses `get_shape_rect` +
`Shape_frame::has_point` on 1x frames, `gamewin.cc:2078-2135`), off-screen detection (§4),
chunk/superchunk range computation (`gamerend.cc:206-219`, `gamemap.cc:305-338`), object
ordering/dependencies — all purely game-px/tile logic and stay valid if positions keep being
computed in game px.

---

## 10. What must change for an S x physical world buffer

### 10.1 Recommended shape of the change: a scaled render target
Give `Image_buffer8` (or a subclass) a `render_scale` (default 1). Keep every existing primitive
signature in **logical (game) px**; implementations map to physical px:
* rects/clip: `(x,y,w,h) → (x·S, y·S, w·S, h·S)`; store clip in physical px; `is_visible` scaled;
* `copy` (scroll, quake) scaled — and add clipping (it is currently unclipped);
* 1x-source primitives (`copy8`, `copy_hline8`, `copy_transparent8`, `paint_rle*`,
  `copy_hline_translucent8`, `fill_hline_translucent8`) upscale nearest-neighbour; translucent /
  transformed paths apply the xform per **physical** destination pixel (correct even over a
  hi-res background);
* `fill8/fill_hline8/fill_translucent8` scaled rects; `put_pixel8` → S x S block;
  `draw_line8` → scaled endpoints, thickness S (faithful) or 1 (crisp debug) via a flag.
* **New** primitives that take a source already at physical resolution and a game-px anchor
  (plus optional physical sub-offset): opaque blit (flats/chunk cache), keyed blit (index 0xFF),
  translucent-range blit, xform-coverage blit (invisible), remapped blit
  (`palette_transform` tables, `shapeid.h:348-356`), outline-from-mask.

With this, `gamerend.cc`, `gamewin.cc`, `effects.cc`, `actors.cc`, `objs/*` need almost no
coordinate changes; the world keeps thinking in game px. Layers keep `render_scale = 1`, so
`push_render_target` keeps working.

### 10.2 Override hook points (world)
* **Flats:** `Chunk_terrain::paint_tile` (`chunkter.cc:86-133`) knows the `ShapeID` of each tile —
  look up override (SF_SHAPES_VGA, shnum, frnum); else upscale the 8x8 raw tile. Same for
  `render_all` (`chunkter.cc:284-312`).
* **Everything else** goes through `ShapeID::paint_shape/paint_invisible/paint_outline`
  (`shapeid.h:348-365`) → `Shape_manager::paint_*` (`shapeid.h:171-196`). `Shape_manager::cache_shape`
  is already keyed by `(shape_kind, shapenum, framenum)` (`shapeid.cc:551-572`); extending
  `Cached_shape` with an optional hi-res frame pointer gives a single lookup for objects, actors,
  weapons, sprites, particles, clouds, projectiles. Calls that pass a bare `Shape_frame*`
  (`chunkter.cc:308`, `intrinsics.cc:1840`, fonts) need either a key or a frame→override map.
* Overrides must cover exactly the S-scaled 1x bounding box (origin `xleft·S, yabove·S`); larger art
  would escape dirty rects (computed from 1x extents) and break occlusion ordering.

### 10.3 Terrain cache at S
* Allocate `rendered_flats` at `c_chunksize·S`; pass physical size/stride to the new opaque blit.
* Make `Figure_queue_size` dynamic (visible chunks × margin) and/or a byte budget; at S=6, 100 entries
  = 59 MB.
* Skip `get_rendered_flats` for chunks whose 128x128 game-px rect does not intersect the clip
  (saves ~2/3 of builds for a 320x200 view, §6). Alternatively drop the cache at high S and blit
  per tile from a hi-res tile store (256 blits of 48x48 per visible chunk).
* Keep the cache indexed (palette effects/cycling unaffected); `write_minimap` should use 1x data.

### 10.4 Scrolling, shake, lerp, guard band
* `view_*`: copy by `c_tilesize` game px through the scaled `copy` (8·S physical); strip repaint and
  dirty shift unchanged. Earthquake: scaled copy, or better a presentation-time offset.
* Lerp: works as-is with integer game-px offsets (6-physical-px steps). Optional: carry
  sub-game-pixel physical offsets for smoother scrolling at S (needs a physical origin shift in the
  buffer and consistent handling in sprite/cloud paints).
* Guard band (`BeginPaintIntoGuardBand`) mixes game px (`game_width += 2`) with buffer px — must be
  redefined in physical px for the S buffer or disabled when the present stage is a downscale.

### 10.5 Effects: hi-res vs replication
* Replicate exactly: dungeon blackness, border fills, map-editor overlays, plasma (keep iteration
  count in game px), "saving"/speech dimming (fill_translucent8).
* Override-capable via sprites.vga/shapes.vga keys: sprites/explosions, projectiles, rain/snow/
  sparkle particles, clouds, weapons, actors.
* Needs hi-res-aware algorithms when an override exists: outline (derive from override alpha),
  invisible xform coverage (use override coverage), translucency (indexed override using the
  translucent index range), palette_transform remap (apply same table).
* Palette-only effects: unchanged.

### 10.6 Consumers to adapt
`mini_screenshot` (sample S x S), `ImageBufferPaintable` (physical snapshot), `--buildmap` (force S=1),
drag-item layer (create at w·S x h·S when override exists, keep screen dest rect),
text-effect layers (only need correct `game_to_screen`), ExultStudio `Send_location` sends
`get_scale_factor()` (`gamewin.cc:1038`).

---

## 11. Risks / gotchas found
* Paint-time side effects (NPC dormancy/nearby list, animator start) → never double-render.
* `Image_buffer8::copy` is unclipped; scaling bugs there corrupt memory.
* Static `Shape_frame::scrwin` + `ib8` swapping: render scale must travel with the buffer.
* Off-screen flats caching becomes expensive at S; queue cap 100 = 59 MB at S=6.
* Single-union dirty rect + per-frame full repaints under smooth scrolling → 36x pixel work at S=6
  (≈2.3 MB per full repaint for 320x200; scales with game area).
* Override art bigger than the 1x box breaks dirty rects/occlusion; outline/invisible/translucency
  semantics must be honoured by indexed overrides (0xFF transparent, `0xff-xfcnt..0xfe`
  translucent in RLE, 0xE0-0xFE cycling in flats).
* Existing small bugs: `Sprites_effect::paint` y offset uses `scrolltx_lo` (`effects.cc:459`);
  `paint_tile` neighbour check `tiley + y > 0` (`chunkter.cc:104`).
* `--buildmap` and `write_minimap` blow up in time/memory if S applies to them.
