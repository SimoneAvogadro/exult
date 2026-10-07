# Palette effects, lighting, translucency and special pixels (current master)

Scope: how colour is produced in Exult today and what that implies for a hi-res (S x, goal S=6) world buffer with optional per-frame hi-res override art.
Repo: `/home/simonea/ultima7_exult/exult-hires`, master @ `8b6ab6b43` (2026-09-15). All references are `file:line` in that tree.

---

## 1. Executive summary

* **The world is 8-bit indexed until presentation.** Every game drawing primitive writes palette indices into `Image_buffer8`, whose bits are the pixels of `draw_surface` (an SDL INDEX8 surface; `imagewin/imagewin.cc:571-577`, `paletted_surface = draw_surface` at `imagewin/imagewin.cc:762`). RGB is produced only in `Image_window::show()` by the scaler functions (`Manip8to16/555/565/32` lookups through `paletted_surface`'s SDL palette, e.g. `imagewin/scale_point.cc:39-87`, `imagewin/scale_xbr.cc:45-184`; dispatch at `imagewin/imagewin.cc:974-1001`).
* **All global colour effects are palette operations.** Day/dusk/night, weather, light sources, the light spell, invisibility, infravision, the red "hit" flash, lightning, fades and cross-fades all change the 256-entry palette (`palette.cc`, `gameclk.cc:34-160`). Palette cycling (water, lava, fire, magic) rotates palette entries `0xE0-0xFE` every 100 ms (`gamewin.cc:1054-1077`, `imagewin/iwin8.cc:135-174`). None of this touches buffer pixels, so **hi-res art stored as palette indices joins in automatically with no extra code.**
* **Per-pixel effects exist, and they all work in index space.** Translucency, shadows, blood and glass use `Xform_palette` lookup tables applied to the *destination* index (`imagewin/ibuf8.cc:408-485`). The same goes for invisible NPCs (`shapes/vgafile.cc:596-634`), gump/conversation shading (`shapeid.cc:503`, `usecode/conversation.cc:72`) and the save/grid overlays (`gamewin.cc:1431`, `gamerend.cc:110-150`). There are also source-index remaps (NPC palette transforms, `shapeid.cc:603-641`; cursor remap, `mouse.cc:224`) and single-index outlines (`shapes/vgafile.cc:640-702`). Each is a per-pixel table lookup or write, so it works the same at any resolution as long as the buffer stays indexed.
* **There is no per-pixel lighting and no "natural light" work.** Lighting picks one of the palettes 7/11/12/6 from a scalar light level (`gameclk.cc:56-82`). That level is summed over light-source objects found during a full repaint (`gamerend.cc:293-305`, `gamerend.cc:394-410`). `git log --all` has no light-map or natural-light branch or commit. The only related commits are `b2f841394`, which makes the light spell use palette 6, and `5201df498`/`0e2f03358`, which add extra light-source shapes.
* **Recommendation: hi-res override art must be 8-bit, quantized to palette 0 (DAY) of the game's own `palettes.flx`, with reserved-index rules (section 7).** True-colour art would need a new 32-bit buffer class and an emulation of every palette effect. It would also force a full scene re-render on every palette change (10 Hz cycling, 60-step transitions), and it would lose cycling and ramp remapping. Section 6 gives the details.

---

## 2. How the final RGB is produced (data flow)

1. **Palette storage.** `Palette` (`palette.h:47-188`) holds `pal1[768]` (current) and `pal2[768]` (fade target, normally black). The values are **6-bit VGA (0..63)** and `max_val = 63` (`palette.cc:57-62`). The palette is loaded from `<STATIC>/palettes.flx`, overridable by `<PATCH>/palettes.flx` (`fnames.h:80-81`, `palette.cc:141`, `palette.cc:366-369`). A 768-byte entry is a simple palette. A 1536-byte entry is an interleaved pair `pal1/pal2` (`palette.cc:316-333`).
2. **Apply.** `Palette::apply()` (`palette.cc:169-194`) temporarily replaces entry 255 with the configured border colour when `border255` is set (section 3.4). It then calls `Image_window8::set_palette(pal1, max_val, brightness)`.
3. **Convert to 8-bit RGB with brightness and gamma.** `Image_window8::set_palette` (`imagewin/iwin8.cc:96-120`) computes `colors[i] = Gamma[ v*brightness*255/(100*maxval) ]` (`Get_color8`, `imagewin/iwin8.cc:84-90`). Gamma tables come from `config/video/gamma/*`, default 1.0 (`exult.cc:829-832`). The result is pushed into the SDL palette of `paletted_surface`/`draw_surface`, and every UI layer is marked dirty (`mark_all_layers_dirty`, `imagewin/iwin8.cc:119`).
4. **Present.** `Image_window::show(x,y,w,h)` (`imagewin/imagewin.cc:882-1110`) runs the selected scaler from the 8-bit `draw_surface` to a 16/32-bit `inter_surface`/screen texture through the SDL palette (phase 1). It then optionally runs an arbitrary "fill scaler" (bilinear/point) or SDL GPU scaling to the window size (phase 2, `imagewin/imagewin.cc:1016-1094`). **Phase 2 already works on RGB data**, so a downscale from an S-x buffer to a smaller window would average real colours, not indices.
5. **UI layers.** Gumps, conversation, the cursor, text effects and the shortcut bar have their own 8-bit buffers. `Image_window8::refresh_layer` (`imagewin/iwin8.cc:281-322`) converts them to ARGB textures. The conversion uses the live `colors[]`, or a per-kind *fixed palette* override that keeps the UI readable at night (`palette.cc:234-282`, `imagewin/iwin8.cc:231-250`). The layer's transparent index (default 255, `imagewin/imagewin.h:701`) becomes alpha 0. An optional `index_argb[256]` table renders translucent indices with real alpha (`imagewin/iwin8.cc:235-244`; built from `blends.dat` in `shapeid.cc:362-381`).

Consequence: **palette state is global and is applied at the very end.** The game buffer has no RGB anywhere, so it can grow S times and every effect still works. Performance is the only change: each palette change (rotation, transition step, fade step) needs a full re-conversion of the S-x buffer (section 8).

Minor observation: `uses_palette` is always set to true (`imagewin/imagewin.cc:541`). As a result `Game_window::rotatecolours()` never calls `set_painted()` (`gamewin.cc:1073-1075`). The rotated palette becomes visible at the next `show()`, which other animation triggers frequently. `Game_window::show()` only blits when `painted` (`gamewin.h:747-755`).

---

## 3. Special palette indices

### 3.1 Map of the 256 indices

| Index range | Role | Where defined |
|---|---|---|
| `0x00` | Black. Used as the transparent key by `copy_transparent8` (`imagewin/ibuf8.cc:488-514`); never part of a ramp (`palette.h:153-156`) | `palette.cc:673-675` |
| `0x01-0xDF` (1..223) | Static colours, about 17 brightness ramps (`Palette::get_ramps`, `palette.cc:581-654`). **The only range Exult's own colour matching searches** (`find_color(..., last=0xe0)`, `palette.cc:485-503`), and the only range `mini_screenshot` uses (`imagewin/iwin8.cc:214`) | `palette.h:105` |
| `0xE0-0xE7` | Palette cycling, 8 entries ("cyan-white" magic cycle, `MAGICCYCLE_PIXEL = -226`) | `gamewin.cc:1068`, `shapeid.h:64` |
| `0xE8-0xEF` | Palette cycling, 8 entries ("yellow-red" fire cycle, `FIRECYCLE_PIXEL = -236`) | `gamewin.cc:1067`, `shapeid.h:65` |
| `0xF0-0xF3` | Palette cycling, 4 entries (green, `-242`) | `gamewin.cc:1066`, `shapeid.h:66` |
| `0xF4-0xF7` | Palette cycling, 4 entries (magenta, `-246`) | `gamewin.cc:1065`, `shapeid.h:67` |
| `0xF8-0xFB` | Palette cycling, 4 entries (white-yellow, `-249`) | `gamewin.cc:1064`, `shapeid.h:68` |
| `0xFC-0xFE` | Palette cycling, 3 entries (red-yellow-black, `-252`) | `gamewin.cc:1063`, `shapeid.h:69` |
| `0xEE-0xFE` | **Also** the 17 *translucent* indices, but only inside RLE frames painted with translucency (section 3.3) | `shapeid.cc:368`, `shapes/vgafile.cc:556` |
| `0xFF` | **Border colour** in game palettes. Transparent key for all RLE-encoding tools and all UI layers. Not cycled | `palette.cc:92,124,169-184`; `shapes/vgafile.cc:92,140,164`; `imagewin/imagewin.h:701` |

The colour names in the table come from the `Pixel_colors` enum. They are confirmed by the real data (section 9.1), which gives the day-palette contents of each range:

* `E0-E7`: white, then light blue to deep blue and back. Used for **water sparkles** and the "void" tile.
* `E8-EF`: white, then yellow, orange, red and dark red (**fire/lava**).
* `F0-F3`: greens.
* `F4-F7`: pink to magenta.
* `F8-FB`: white, then yellow to orange.
* `FC-FE`: black, brown, light orange.

### 3.2 Palette cycling (water, lava, fire, "void")

* `Game_window::rotatecolours()` (`gamewin.cc:1054-1077`) is called once per main-loop iteration (`exult.cc:1518`). Every `rot_speed = 100 ms` (200 ms if `!fast_palette_rotate()`, which never happens now; `imagewin/imagewin.h:608-610`) it rotates the six ranges above. Only the last call has `upd=1` and pushes the result to SDL.
* `Image_window8::rotate_colors` (`imagewin/iwin8.cc:135-174`) rotates the **already gamma-converted `colors[]`**, not `Palette::pal1`. With `num>0` it moves the colour of entry *j* to *j+1* (`std::rotate(start, finish-3, finish)`). A pixel with fixed index *j* therefore shows the colours of *j, j-1, j-2, ...* over time. **Spatial gradients of cycling indices produce bands that move toward increasing index.** This is how flowing water and lava are authored.
* Any later `Palette::apply()` (time-of-day change, flash) reloads the palette and resets the cycle phase. This is harmless.
* Flat terrain uses cycling directly. `Chunk_terrain::paint_tile` explicitly skips shape 12 frame 0, the "palette cycling void tile" (`objs/chunkter.cc:105-109`).
* In the real data, shape 12 frame 0 is 64 x `0xE2`. **Water flats are mostly static blues**: the `0x50` ramp and `0x8E-0x93`. Only about 1-17% of their pixels are `E0-E7` "sparkles" (BG shapes 7, 13, 84, 95: 1-8%; shape 10: 17%). U7 water "flows" through scattered cycling glints, not full-surface cycling.
* The ranges are hard-coded in three places that must stay consistent: `gamewin.cc:1063-1068`, `palette.cc:607-609` (ramp detection) and `shapeid.h:64-69`. The docs in `docs/newgame.txt:166-178` are stale: they list only five groups and say the translucent range is `0xf4-0xfe`.
* The load-screen plasma uses its own rotations on other ranges: BG 128..207, SI 16..111 (`gamewin.cc:3140-3233`). These run only on the 320x200 load screen and are not relevant to world art.

### 3.3 Translucency (Xform tables)

* **Tables.** `Shape_manager::load()` reads `blends.dat` (first byte = count; Exult ships 17 entries for both games: `data/bg/blends.dat`, `data/si/blends.dat`, identical). That count is `nxforms`, and `xforms.resize(nblends)` (`shapeid.cc:306-334`).
  * If `<STATIC>/xform.tbl` exists (always true with real game data; `verify.cc:193`), table *i* of `xform.tbl` is stored at `xforms[nxforms-1-i]` (`shapeid.cc:336-351`). So `xform.tbl[0]` applies to `0xFE`, as `docs/u7tech.txt:478-488` describes.
  * Otherwise the tables are computed from palette 0 with `create_trans_table`, which only outputs indices `< 0xE0` (`shapeid.cc:352-358`, `palette.cc:548-560`).
* **Range.** `xfstart = 0xFF - xfcnt`, which is `0xEE` for 17 tables. Pixels `0xEE..0xFE` in a frame painted translucently are **not written**: the destination pixel is replaced by `xforms[pix - xfstart][dest]` (`shapes/vgafile.cc:540-590`, `imagewin/ibuf8.cc:408-438`). The stale comment at `shapeid.h:87-88` ("0xf4 through 0xfe") is wrong.
* **Meaning of each index** (blend colour and opacity from `blends.dat`; the real game uses the matching `xform.tbl` table):

| idx | RGB | opacity | likely use |
|---|---|---|---|
| EE | 208,216,224 | 75% | light/glass |
| EF | 136,44,148 | 78% | purple |
| F0 | 248,252,80 | 83% | yellow |
| F1 | 144,148,252 | 97% | blue |
| F2 | 64,216,64 | 79% | green |
| F3 | 204,60,84 | 55% | pink-red |
| F4 | 144,40,192 | 50% | violet |
| F5 | 96,40,16 | 50% | brown |
| F6 | 100,108,116 | 75% | grey (smoke) |
| F7 | 68,132,28 | 50% | green |
| F8 | 255,208,48 | 25% | warm glow |
| F9 | 28,52,255 | 50% | blue |
| FA | 8,68,0 | 50% | dark green |
| FB | 255,8,8 | 46% | **blood** (the "7th" in `docs/newgame.txt:166-170`) |
| FC | 255,244,248 | 50% | white |
| FD | 56,40,32 | 50% | **dark brown, shadows** |
| FE | 228,224,214 | 32% | light grey; **same table as `invis_xform`** (`shapeid.cc:360`) |

* **Which frames are painted translucently.** The dispatch is in `Shape_manager::paint_shape` (`shapeid.h:171-183`):
  * Flat 8x8 tiles: never. They go through `copy8`, which writes all 64 indices raw (`shapes/vgafile.cc:525-535`).
  * RLE terrain in chunk pass 2: never (`objs/chunkter.cc:303-308` uses the default `translucent=false`).
  * `shapes.vga` objects: only if the TFA translucency bit is set (`shapes/shapeinf.h:868-870`, `shapeid.cc:564`).
  * **Every actor frame**, whatever its TFA bit (`actors.cc:2090` passes `force_trans=true`).
  * **Every `sprites.vga` frame** (`shapeid.cc:567-569`): explosions, rain, snow, sparkles, clouds (`effects.cc:1258`, `effects.cc:1645`).
  * Gumps, faces, fonts, paperdolls: not translucent in the world path. In UI layers, faces and the shortcut bar are drawn *raw* and their translucent indices become real alpha through `index_argb` (`usecode/conversation.cc:218-249`, `usecode/conversation.cc:919-923`, `gumps/Gump_manager.cc:271-283`).
* In a non-translucent frame, indices `0xEE-0xFE` are written raw and **cycle**, because they fall inside the cycling ranges.

### 3.4 Index 255 and the border

* `border255` is true for palettes 0..12 except 9 (`palette.cc:92`, `palette.cc:124`, `palette.cc:429`). It is forced true for cross-fade intermediates (`palette.cc:538`). While it is set, entry 255 is replaced at apply time by the border colour (`config/video/game/border/*`, `exult.cc:855-865`, default black). The fade loops do the same (`palette.cc:391-483`).
* The area outside the map is filled with `get_border_index()`, which is 255 (`palette.h:144-146`, `gamerend.cc:370-383`).
* **So a raw 255 in the world buffer always shows as the border colour, not as the palette's real colour 255.** Exult's encoder treats 255 as transparent (`Skip_transparent`/`Find_runs`, `shapes/vgafile.cc:135-188`; `reflect()` fills with 255, `shapes/vgafile.cc:92`). ipack exports with transparent index 255 (`tools/ipack.cc:435-466`) and imports with 255 as the transparent index (`tools/ipack.cc:568`, `tools/ipack.cc:628`). Every UI layer uses 255 as its see-through index (`mouse.cc:205`, `usecode/conversation.cc:51`, `effects.cc:1194`, `drag.cc:351`).

### 3.5 Special pixels (outlines, status colours)

* `Shape_manager::load()` (`shapeid.cc:158-175`) computes `special_pixels[]` from **palette 0** with `find_color` (static range only): POISON (green), PROTECT (light grey), CURSED (yellow), CHARMED (light blue), HIT (red), PARALYZE (purple), BLACK.
* Negative `Pixel_colors` values return a fixed cycling index (`get_special_pixel`, `shapeid.h:121-128`).
* **Outlines.** `Shape_frame::paint_rle_outline` (`shapes/vgafile.cc:640-702`) puts one pixel at both ends of every RLE scanline and fills the first and last scanline. That gives an outline **1 buffer pixel thick, derived from the RLE data.**
  * Used by `Actor::paint` for hit and status outlines (`actors.cc:2093-2111`), `Game_object::paint_outline` (`objs/objs.cc:970-976`), the item menu (`gumps/ItemMenu_gump.cc:180-183`) and the map editor (`gamerend.cc:277`).
  * The red "hit" outline on the target (`gamewin.cc:2392`) complements the palette-wide `flash_red()` used when the Avatar is hit (`palette.cc:107-114`, `actors.cc:2959`).
* Map-editor/debug colours: bounding-box indices `{15,0,22,38,5,64,80,94}` (`gamerend.cc:310-321`), chunk outlines with `HIT_PIXEL` (`gamerend.cc:89-103`).

---

## 4. Global colour effects (all palette-level)

| Effect | Mechanism | Ref |
|---|---|---|
| Time of day | Hour picks palette NIGHT(2) for hour < 5 or in a dungeon, DAWN(1) at 5, DAY(0) from 6 to 19, DUSK(1) at 20, then NIGHT. Hour changes produce 60-step `Palette_transition` cross-fades (per-channel linear interpolation, `palette.cc:520-541`) | `gameclk.cc:42-54`, `gameclk.cc:154-159` |
| Weather | Fog gives FOG(5); overcast gives OVERCAST(4), both only in the day palette, with a 20-step transition | `gameclk.cc:74-80`, `gameclk.cc:134-141` |
| Light sources | In a dark palette: light < 224 gives CANDLE(7), < 640 gives SINGLE_LIGHT(11), otherwise MANY_LIGHTS(12) | `gameclk.cc:63-73` |
| Light spell | SPELL(6) until `special_light` expires | `gameclk.cc:65-66`, `gamewin.cc:721-728`, `gamerend.cc:401-406` |
| Invisible Avatar | INVISIBLE(3), applied immediately | `gameclk.cc:57-58`, `gameclk.cc:128-132` |
| Infravision | Forces DAY(0) | `gameclk.cc:60-61` |
| Avatar hit | `flash_red()`: RED(8) for 100 ms, a blocking `SDL_Delay` | `palette.cc:107-114` |
| Lightning | LIGHTNING(10) while flashing | `effects.cc:1460-1500` |
| Fade in/out | Busy loop, 20 ms per step, interpolating `pal1` toward `pal2` (black). Calls `win->show()` each step, with frame skipping | `palette.cc:82-101`, `palette.cc:391-483` |
| Brightness / gamma | Brightness % in `Get_color8`; gamma tables | `imagewin/iwin8.cc:84-129` |
| UI readability at night | Per-layer-kind fixed palette when the live palette is not DAY | `palette.cc:197-282` |

**Lighting has no per-pixel mask.** The light level is computed only after a *complete* repaint (`gx==0 && gy==0 && gw==get_width() && gh==get_height()`, `gamerend.cc:394-410`). It sums `paint_chunk_objects()` light counts (`gamerend.cc:203-290`) and party-carried light, each weighted by `max(0, 75 - 2*dx - 3*dy) * brightness` in tiles (`gamerend.cc:293-305`). Note for the hi-res design: the full-repaint test uses `get_width()/get_height()`, which must stay in game pixels.

---

## 5. Per-pixel operations in the 8-bit buffer (all index-local)

| Operation | What it does per pixel | Code | Hi-res implication |
|---|---|---|---|
| Translucent RLE paint | `dest = xform[pix-xfstart][dest]` for `pix` in 0xEE..0xFE, otherwise `dest = pix` | `imagewin/ibuf8.cc:408-438`, `shapes/vgafile.cc:540-590` | Works unchanged on hi-res indexed data. Each hi-res pixel blends with the hi-res destination. |
| Invisible NPC | `dest = invis_xform[dest]` under the shape's RLE mask | `shapes/vgafile.cc:596-634`, `shapeid.h:185-189`; callers `actors.cc:2088,2140`, `objs/iregobjs.cc:62`, `drag.cc:356,426` | Needs a hi-res mask, from the override's RLE data. |
| Rect shading | `dest = xform[dest]` over a rectangle | `imagewin/ibuf8.cc:462-485`; gump text shading `shapeid.cc:503`; conversation `usecode/conversation.cc:72`; save dimming `gamewin.cc:1431` and `game.cc:815` (xform 8); tile grid (xform 16) and selected chunks (xform 13) `gamerend.cc:110-150,181,284-287`; perf overlay `perf.cc:136-143`; cheat screen `cheat_screen.cc:1922-1926` | Rectangles and line widths are in game pixels and must be scaled by S. |
| Source remap | `dest = table[pix]` (shift, xform-as-remap, or ramp remap) | `imagewin/ibuf8.cc:672+`, `shapeid.cc:603-641`, `palette.cc:697-724`; cursor `mouse.cc:224` | Works on hi-res indexed art if the art keeps the palette-0 ramp structure. |
| Outline | Writes a single special index at scanline ends | `shapes/vgafile.cc:640-702` | At S=6 this becomes a 1/6-game-pixel line. A thickness rule is needed. |
| Border fill | `fill8(255)` outside the map | `gamerend.cc:370-383` | Coordinates must be scaled. |
| Static | Random 0/7/15 (BG intro only) | `imagewin/ibuf8.cc:115-133`, `gamemgr/bggame.cc:781` | Scene layer at 320x200, out of scope. |
| Mini screenshot | Reads `ibuf` at game coordinates, 3x box average in RGB, nearest of indices 0..223 | `imagewin/iwin8.cc:181-226` | **Breaks with an S-x buffer.** It must sample with stride S, or render a 1x copy. |

---

## 6. Is palette-indexed hi-res art the right choice?

### 6.1 Indexed (recommended)

What it preserves, with no extra code:

* Day/night, weather, light, spell, invisible, red flash, lightning, fades and transitions, all as global palette swaps.
* Cycling water and lava. The override places cycling indices; there is even an opportunity for smoother hi-res flow patterns (section 7.4).
* Translucency, shadows, blood and invisibility via the existing xform tables. These are index-to-index, so they are resolution-agnostic.
* NPC ramp remaps and shifts, cursor remaps, special-pixel outlines.
* **Caches stay valid across palette changes.** `Chunk_terrain::rendered_flats` stores indices (`objs/chunkter.cc:245-267`), so a palette change only needs a re-present, not a re-render. This matters a lot: palette changes happen at 10 Hz (cycling) and constantly during transitions.
* Presentation already applies the palette *before* any downscale (section 2, step 4). Dithered 6x art therefore averages into smoother colours on smaller windows, which raises the effective colour depth.

Costs and limits:

* A 224-colour static palette with about 17 ramps limits gradients. At 6x, ordered or blue-noise dithering hides this well, especially after downscale.
* Translucency is quantized through xform tables (the same look as the original).
* No anti-aliased alpha edges on RLE frames: a pixel is either opaque or transparent (255). Soft edges are only possible with translucent indices, and only in translucent-painted frames.

### 6.2 True-colour (RGBA) hi-res art: what would break or need emulation

1. **No 32-bit buffer exists.** `Image_buffer8` is the only implementation (`imagewin/ibuf8.h`, `imagewin/imagebuf.h:57-64`). A parallel `Image_buffer32` would need all primitives: `copy8`, `paint_rle*`, the translucent fills, `draw_line8`, text, beveled boxes and so on.
2. **Palette swaps.** The palettes in `palettes.flx` are per-index *colour grades*, not uniform multipliers. In the night palette, reds go to black, blues are mostly kept, dark ends of ramps are crushed and highlights are kept relatively more (section 9.1). Mapping RGB under palette *k* needs a fitted transform, for example a 3D LUT per palette built from the 224 `(pal0[i] -> palk[i])` pairs, with LUT blending for transitions.
   * In favour: in both games the static range is a *function* of the day colour, so no day colour maps to two different night colours.
   * Against: art colours far from the 224 sample points would be extrapolated, and the grade would drift from the original look.
3. **Cycling is lost** unless the art carries an extra index or phase channel.
4. **Translucency** would have to switch from xform lookups to RGBA blending (`translucency_argb` already exists, `shapeid.cc:362-381`). Translucent sprites over *indexed* destinations would then need RGB destinations, so the whole buffer becomes RGB.
5. **Ramp remaps and palette shifts** (`PT_Shift`, `PT_RampRemap`) have no RGB equivalent beyond hue-shift approximations.
5b. **Mixed content.** Frames without an override are indexed. Putting them into an RGB buffer means resolving the palette at draw time. **Every palette change (10 Hz cycling, 60-step transitions, fades) would then force a full scene re-render and invalidate the flats cache**, instead of today's cheap re-present.
6. Screenshots, mini screenshots and UI layers all assume indexed sources.

Verdict: **indexed is the right primary format.** If true-colour detail is wanted later, the only reasonable route is a GPU path with an index texture plus a palette texture, and possibly a separate RGB "detail" layer modulated by a per-palette LUT. That is a separate project.

---

## 7. Rules for the art pipeline (what override images must satisfy)

### 7.1 Target palette

* Quantize against **palette 0 (DAY) of the game's own `palettes.flx`**, using `<PATCH>/palettes.flx` if the mod has one.
  * BG and SI palettes differ: `verify.cc:172` (BG, sha1 `a2306ce...`) versus `verify.cc:1290` (SI, `7b42754...`). **Overrides are per game.**
  * Palette 0 is the engine's reference palette: special pixels are computed from it (`shapeid.cc:159-175`), so are the algorithmic xforms (`shapeid.cc:353`) and the ramps (`palette.cc:590-602`).
* Convert 6-bit to 8-bit the way the engine does: `c8 = v*255/63` with integer floor at brightness 100 and gamma 1.0 (`imagewin/iwin8.cc:84-90`).
* Use a perceptual distance (OKLab or similar) for quality. Note that Exult itself uses squared RGB distance on 6-bit values (`palette.cc:486-503`).

### 7.2 Allowed indices, by frame class

| Frame class | Static colours (quantizer output) | Cycling `0xE0-0xED` | `0xEE-0xFE` | `0xFF` |
|---|---|---|---|---|
| Flat 8x8 terrain (`shapes.vga` 0..149, non-RLE) | `0x00-0xDF` | Only where the original pixel was cycling (copy semantics, section 7.4) | Cycling colours (flats are never translucent) | **Forbidden**: flats are fully opaque, and 255 shows as the border colour |
| RLE frame, *not* painted translucently | `0x00-0xDF` | Original mask only | Cycling colours, original mask only | **Transparent key** (not inside opaque pixels) |
| RLE frame painted translucently (TFA bit, **all actors**, **all `sprites.vga`**) | `0x00-0xDF` | Original mask only | **Blend operators**: keep the original index; the mask may be smoothly reshaped | Transparent key |
| Gumps, faces, fonts (UI layers) | `0x00-0xDF` | Original only | Faces and shortcut bar: rendered as real alpha via `index_argb` | Transparent (layer see-through index) |

* **Never let the quantizer produce indices `>= 0xE0`.** Exult itself restricts `find_color` to `< 0xE0` for this reason (`palette.cc:485-490`).
* Index 0 is usable as black in flats and opaque pixels. Avoid it only in content drawn through `copy_transparent8`, which treats 0 as transparent (`imagewin/ibuf8.cc:488-514`).
* No partial alpha. Threshold AI alpha at about 50% into opaque or 255. For translucent-capable frames you may instead map soft edges to the frame's own translucent index.

### 7.3 Geometry

* An override for frame *(file, shape, frame)* must be exactly `S*w x S*h`, where `w,h` come from the original `xleft+xright`/`yabove+ybelow` (`shapes/vgafile.cc:456-480`). The hot-spot is at `(S*xleft, S*yabove)`.
* Hit-testing (`Shape_frame::has_point`, `shapes/vgafile.cc:706-745`), outlines-by-mask and collision keep using the original frame in game pixels. A silhouette that differs a lot from the original makes clicks feel wrong.

### 7.4 Cycling regions (water, lava, void, fire)

* Build a mask of original pixels in `0xE0-0xFE`. Leave out `0xEE-0xFE` when the frame is translucent-painted; those are blend operators.
* Baseline: nearest-neighbour upscale the *index* values inside the mask, and do not AI-upscale those pixels. This keeps the animation exactly as it was.
* Water sparkles are sparse single pixels (section 3.2). A 6x6 nearest-neighbour block of `E0-E7` will look like a large blinking square. Prefer re-placing them as smaller hi-res glints (for example 1-3 px) at the original positions, keeping the original index. An optional extra is a few new glints in the same range.
* Better (optional): synthesize a hi-res *phase field* (for example a smoothly interpolated `index - range_start` modulo range length) so bands flow smoothly at 6x. Indices must stay inside the **same range** as the original pixel. The ranges are 8/8/4/4/4/3 entries.
* Do not dither across range boundaries. A cycling index adjacent to a static index is fine.

### 7.5 Translucent regions

* Keep each translucent pixel's index (its blend operator) and only reshape the mask, with nearest-neighbour as the baseline.
* Shadows (`0xFD`) and blood (`0xFB`) cover large areas on actors. Smooth their *masks* at hi-res; never quantize them into static colours.

### 7.6 Dithering and tiling

* Flats tile edge-to-edge (8x8 becomes 48x48). Use **ordered or blue-noise dithering with a world-aligned threshold map**, or error diffusion with wrap-around, so dither patterns don't seam at tile edges.
* Process tiles that belong together (whole terrain chunks or atlases) together, so the AI keeps them consistent.

### 7.7 PNG I/O caveat

`Export_png8(..., transp_to_0=true)` (used by ipack, `tools/ipack.cc:464`) **rotates the palette so 255 becomes index 0**: every stored index is `(i+1)%256` (`shapes/pngio.cc:215-252`). `Import_png8` undoes this only when it finds a fully transparent tRNS entry (`shapes/pngio.cc:127-166`).

The extraction tool for `art_original` should therefore write **raw indices** with the game palette as PLTE and tRNS only on 255 (`transp_to_0=false`). It should also store a JSON sidecar with the frame class (flat, RLE, translucent-painted), the hot-spot, and the cycling/translucent masks.

### 7.8 Emissive and special indices (checked against the data)

I computed, for each index `i`, the luminance ratio `palk[i]/pal0[i]` (section 9.1):

* **No static index stays bright in the NIGHT palette (2).** The highest ratio is about 0.64, at ramp tops; the median is 0.38. There are no "self-lit window" indices to reserve, so ordinary quantization to `0x00-0xDF` is safe.
* The light palettes (7, 11, 12) keep some ramps near or above day luminance: the blue ramp `0x4F-0x52`, the dark reds `0x1A-0x1C` and the purples `0x5E-0x61`. This is a global grade and needs no special handling in art.
* Palettes 3 (invisible), 5 (fog), 8 (red) and 10 (lightning) are also global grades.

**Rule:** quantize in palette-0 space only and let the engine apply the grades. Never pick colours by how they look under another palette.

---

## 8. Touchpoints for an S-x world buffer, from the palette and colour side

1. `Image_window8::set_palette` and `rotate_colors` (`imagewin/iwin8.cc:96-174`): no change needed, since the palette is resolution-independent. Each call invalidates the presented image, and the next `show()` re-converts the whole buffer. At S=6 that is about 2.3 M lookups per full frame, which is fine for point/LUT conversion (a few ms) but not for hq/xBR. **Use point conversion at scale 1 plus an RGB downscale (fill scaler or SDL linear), never hqNx/xBR on the S-buffer.** Optional later: GPU palette lookup.
2. Fades (`palette.cc:391-483`) busy-wait 20 ms per step and call `show()` each step. They rely on frame skipping (`get_frame_skipping`) if conversion is slow.
3. `Shape_manager::paint_shape`, `paint_invisible`, `paint_outline` (`shapeid.h:171-196`) are the single dispatch point for opaque, remapped, translucent, invisible and outline painting. This is the natural override hook. Hi-res variants of `paint_rle`, `paint_rle_translucent`, `paint_rle_remapped`, `paint_rle_transformed` and `paint_rle_outline` must keep the same index semantics.
4. `Chunk_terrain::paint_tile` and `render_all` (`objs/chunkter.cc:86-133`, `objs/chunkter.cc:284-312`): flat copy into the `rendered_flats` cache. The cache stays indexed, so it is palette-proof, but each chunk grows S^2 times.
5. `fill_translucent8` callers (section 5): scale rectangles and decide line widths (grid lines of S pixels).
6. Border fill (`gamerend.cc:370-383`): scale coordinates; keep 255.
7. `Image_window8::mini_screenshot` (`imagewin/iwin8.cc:181-226`): sample with stride S or from a 1x render.
8. Light-source pass (`gamerend.cc:394-410`): the full-repaint test must keep comparing in game pixels.
9. `Shape_manager::load` xforms, `translucency_argb` and special pixels (`shapeid.cc:158-175`, `shapeid.cc:306-381`): no change; they are resolution-free.
10. UI layers (`imagewin/iwin8.cc:231-617`): unaffected while they stay at UI resolution. If hi-res gump, face or font overrides are added, the same index rules apply, with 255 transparent and `index_argb` for translucent indices.

---

## 9. Data checks against the real game files

I found the game data at `/mnt/e/Games/RolePlayingGames/ultima7/static` (BG) and `/mnt/e/Games/RolePlayingGames/Serpent/static` (SI). I parsed it read-only with small Python Flex/RLE readers kept in the session scratchpad.

### 9.1 Results

* **`palettes.flx`** has 16 entries in both games.
  * BG: entries 0-8 and 10-12 are 768 bytes; **entry 9 is empty**, which explains `palette != 9` in `palette.cc:92,124` and the BG resource list skipping 9 (`gamemgr/bggame.cc:173`); 13-15 are empty.
  * SI: entries 0-12 are present; 13-15 are empty.
  * **Entry 255 holds non-6-bit garbage in every palette** (for example `(250,64,1)` in day), which confirms that 255 is not a usable colour.
* **`xform.tbl`** has 20 entries in both games: **17 tables of 256 bytes plus 3 empty**. This matches `blends.dat` (17), so `nobjs = 17` and the translucent range is `0xEE-0xFE`. `docs/u7tech.txt:482` ("11 tables") is wrong.
* **`shapes.vga`**:
  * **Shapes 0-149 are all flat**: BG has 3885 and SI 4690 8x8 frames, and there are 0 RLE frames in that range.
  * **No RLE frame contains index 255 inside a slice** (BG 10286 and SI 13081 RLE frames scanned). 255 is safe to use as the hi-res transparent key.
  * Flats use cycling indices `E0-E7` (water sparkles, void) and, rarely, `0xFE` (BG shape 68) and `0xF4` (SI). There are no flat uses of `E8-EF`.
  * TFA-translucent shapes mostly use `F6, FB, FC, FD, FE` (smoke, blood, white, shadow, light grey) plus some `E0-E7`, which are cycling even inside translucent shapes. Non-translucent shapes use the whole `E0-FE` range as cycling colours.
* **Day palette, cycling ranges**: the contents are listed in section 3.1, identical in BG and SI.
* **Palette grades**:
  * Median luminance ratio against day: dusk 0.72, night 0.38, invisible 1.00, overcast 0.85, fog 1.37, spell 0.86, candle 0.66, red 1.27, lightning 1.43, single light 0.68, many lights 0.74.
  * Night: no static index keeps 85% or more of its day luminance (see 7.8). The day-to-night mapping over the static range has no conflicts.
* **Exult's ramp detector** (`palette.cc:581-654`) applied to BG palette 0 gives 16 static ramps (`01-0E, 0F-1E, 1F-2E, 2F-3A, 3B-48, 49-57, 58-65, 66-75, 76-85, 86-93, 94-A2, A3-B1, B2-BF, C0-C7, C8-D0, D1-DF`) plus 6 cycling ramps. Art that stays inside these ramps remaps correctly with `PT_RampRemap`.

### 9.2 Still open

* Whether any mod used with the fork ships `<PATCH>/palettes.flx` or `xform.tbl`. If so, overrides must be quantized against it.
* Final thickness rule for outlines and grid lines at S=6. This is a design choice, not a data question.
