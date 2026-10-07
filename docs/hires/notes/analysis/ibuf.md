# Image buffer core primitives — analysis for hi-res render scale

Scope: `imagewin/imagebuf.{h,cc}`, `imagewin/ibuf8.{h,cc}`, `imagewin/iwin8.{h,cc}` plus the parts of
`imagewin/imagewin.{h,cc}`, the scalers, and engine call sites that touch raw pixel memory or depend on
buffer geometry. Repository: `/home/simonea/ultima7_exult/exult-hires` (master `8b6ab6b43`).
All line numbers refer to that tree.

---

## 1. Class structure and ownership

```
Image_buffer (abstract, imagebuf.h:57)
  └─ Image_buffer8 (ibuf8.h:30)          -- the only concrete buffer class
Image_window (imagewin.h:64)              -- owns ONE Image_buffer* ibuf (+ SDL surfaces, layers)
  └─ Image_window8 (iwin8.h:42)           -- palette, gamma, layer->texture conversion; ib8 == ibuf
```

* `Image_window` *has-a* buffer: `Image_buffer* ibuf` (imagewin.h:321). It is not an `Image_buffer`.
  `Image_window8` keeps a typed alias `Image_buffer8* ib8` (iwin8.h:44) set in the ctor
  (iwin8.cc:64-65) from `new Image_buffer8(0, 0, nullptr)` — the private "window" ctor
  (ibuf8.h:34) with `bits_owned=false`.
* The window buffer's memory is the SDL `draw_surface` (8-bit INDEX8 with an SDL palette). It is
  wired in `Image_window::create_surface` (imagewin.cc:540-578) by writing the protected fields
  directly (Image_window is a friend, imagebuf.h:147):
  * `width/height = draw_surface->w/h - 2*guard_band` (564-568)
  * `line_width = draw_surface->pitch / pixel_size` (571)
  * `offset_x/offset_y = (full - game)/2` (573-574)
  * `bits = pixels + offset_x + offset_y*pitch + guard_band*(1+pitch)` (575-577)
  * `free_surface` nulls `ibuf->bits` (imagewin.cc:829); `~Image_window` deletes `ibuf` (520).
* `draw_surface` is created with size `inter_width/scale + 2*guard_band` (imagewin.cc:726-728);
  `guard_band` is a compile-time constant of **4 physical pixels** (imagewin.h:357).
* Owned buffers: `Image_buffer8(w,h)` allocates `w*h` (ibuf8.h:37-39), `line_width == width`,
  offsets 0, clip = full. Deleted by `~Image_buffer` (imagebuf.h:144-146) unless `bits_owned==false`
  (ibuf8.h:49-53 nulls `bits` first).
* Aliasing buffers: `Image_buffer8(w,h,pitch,bits,guard_band)` (ibuf8.h:43-47), used for overlay
  **layers** (imagewin.cc:2273-2277) on an INDEX8 SDL surface of `w+2gb x h+2gb` (imagewin.cc:1579).
* No copy ctor/assignment is deleted on `Image_buffer`; an accidental copy of an owned buffer would
  double-`delete[]`. A "view" buffer (see §5) must be created as non-owning.

### Render-target redirection (important for any hi-res work)

`Image_window8::set_render_buffer` (iwin8.h:63-68) swaps **both** `ib8` and the base `ibuf` to an
arbitrary `Image_buffer8` (layers, scratch buffers). `Game_window::push_render_target /
pop_render_target` (gamewin.cc:541-550) also switch `Shape_frame::scrwin` (vgafile.h:55,69).
While redirected, every `Image_window` method that reads `ibuf` geometry (`get_full_width`,
`get_start_x`, `show()`'s `ibuf->clip`, `create_buffer`, `scale_layer_color`) operates on the
redirected buffer, not the window buffer. In this fork **gumps, text gumps, conversation, mouse
cursor, text effects, scenes, cheat screen are drawn into layers** (Gump_manager.cc:181-201,
Text_gump.cc:49-75, conversation.cc:299-325, mouse.cc:198-230, effects.cc:1189-1200,
scene_layer.h:101-139). Only the **game world** (terrain, objects, sprites/effects, debug overlays)
is drawn into the main window buffer. That makes the main buffer the natural (and initially the
only) target for render scale S.

Cached raw pointers to the window buffer exist and survive resizes because the `ib8` object is
never replaced (only its fields): `Shape_frame::scrwin` (gamewin.cc:330, 921), `Game::ibuf`
(game.cc:98), `Cheat_screen::ibuf` (cheat_screen.cc:98,143,207), `ExultMenu::ibuf`
(exultmenu.cc:196). **Consequence: changing the render scale must mutate the existing object in
place; swapping in an object of another class would leave dangling pointers.**

---

## 2. Coordinate system and geometry fields

Fields (imagebuf.h:59-66): `width, height` (full buffer, pixels), `offset_x, offset_y`,
`depth` (always 8 here), `pixel_size` (1), `bits`, `line_width` (pitch in pixels), and the private
clip rect `clipx, clipy, clipw, cliph`.

* Origin `(0,0)` is the **top-left of the game area**, not of the buffer. `bits` points at that pixel.
  The valid pixel range is `x ∈ [-offset_x, width-offset_x)`, `y ∈ [-offset_y, height-offset_y)`
  (letterbox margins in Fit/Centre fill modes have negative coordinates). Address of a pixel is
  always `bits + y*line_width + x` — negative coordinates are legal.
* `Image_window::get_start_x() = -offset_x`, `get_full_width() = width`, `get_end_x() = full+start`
  (imagewin.h:624-654); `get_game_width()` is `game_width` (or scene size in scene mode, 640-646).
* Clip rect is in the same coordinates. `clear_clip()` → whole buffer (imagebuf.h:165-170);
  `set_clip()` clamps the request to the buffer (173-198); `get_clip()` (200-205);
  `is_visible(x,y,w,h)` (208-210); `ClipRectSave`/`SaveClip()` RAII (107-141).
* Protected clip helpers: `clip_internal` (69-89), `clip_x(srcx,w,destx,desty)` (92-94) for
  horizontal lines (rejects rows outside the clip), `clip(srcx,srcy,w,h,destx,desty)` (97-100)
  which clips a dest rect and advances the source offset accordingly.
* **Everything in this API is in game pixels; there is no notion of a physical scale.** Pixel
  memory, pitch, clip, offsets and dims are all in the same unit.

The guard band (4 px on each side of `draw_surface`) exists so scalers can read beyond the edges
(imagewin.h:353-357). `BeginPaintIntoGuardBand` temporarily enlarges `ibuf->width/height` to
`draw_surface->w/h - 1.5*gb` and `game_width/height` by `gb/2` (imagewin.cc:1142-1211);
`EndPaintIntoGuardBand` restores them (1213-1224); `FillGuardband` copies edge pixels into the
right/bottom guard band with raw `memcpy/memset` on `draw_surface` using `ibuf->width/height`
(1226-1252).

---

## 3. Complete primitive inventory

### 3.1 Non-virtual base API (imagebuf.h)

| Member | Lines | Notes |
|---|---|---|
| `get_bits()` | 149-151 | raw pointer to (0,0). **Bypass point.** |
| `get_width()/get_height()` | 153-159 | full buffer dims |
| `get_line_width()` | 161-163 | pitch. **Bypass point.** |
| `clear_clip/set_clip/get_clip/is_visible` | 165-210 | logical clip |
| `ClipRectSave`, `SaveClip()` | 107-141 | RAII |
| `clip_x`, `clip`, `clip_internal` | 69-100 | protected; `Image_window` is friend and calls `ibuf->clip()` in `show()` (imagewin.cc:903) |

### 3.2 Virtual API (pure in `Image_buffer`, implemented in `Image_buffer8`)

| Primitive | Decl | Impl (ibuf8.cc unless noted) | Behaviour |
|---|---|---|---|
| `fill8(val)` | imagebuf.h:216 | 140-146 | whole buffer incl. padding: `line_width*height` bytes from row `-offset_y`, col `-offset_x`; **no clip** |
| `fill8(val,w,h,x,y)` | 218 | 152-167 | clipped rect fill |
| `fill_hline8(val,w,x,y)` | 220 | 173-181 | `clip_x` + `memset` |
| `draw_line8(val,x0,y0,x1,y1,xform*)` | 223 | 185-354 | own clipping (`clipline`), 16.16 DDA walking a raw pointer (`inc1 = line_width`); with `xform` it does `*to = xf[*to]` |
| `copy8(src,w,h,x,y)` | 226 | 360-384 | clipped; **source stride == srcw** (no source pitch parameter) |
| `copy_hline8(src,w,x,y)` | 228 | 390-402 | clipped single row |
| `copy_hline_translucent8(src,w,x,y,first,last,xforms*)` | 230 | 408-434 | for `c∈[first,last]`: `out = xforms[c-first][*to]` (depends on existing dest pixel) |
| `fill_hline_translucent8(val,w,x,y,xform&)` | 235 | 440-456 | `*p = xform[*p]`, val ignored |
| `fill_translucent8(val,w,h,x,y,xform&)` | 237 | 462-481 | rect version |
| `copy_transparent8(src,w,h,x,y)` | 239 | 488-513 | **index 0** transparent. No engine callers outside the window wrappers (dead code). |
| `get_pixel8(x,y)` | 242 | ibuf8.h:99-101 | **no clip, no bounds check**. No engine callers outside wrappers. |
| `put_pixel8(pix,x,y)` | 244 | ibuf8.h:103-107 | clip-checked |
| `create_another(w,h)` | 249 | ibuf8.h:58-60 | new owned `Image_buffer8(w,h)` |
| `copy(sx,sy,w,h,dx,dy)` | 251 | 43-68 | `memmove` within buffer, overlap-safe, **no clipping at all**. Used for scrolling and earthquake |
| `get(dest,sx,sy)` | 253 | 74-99 | copies a `dest->width x dest->height` rect into `dest` (raw `dest->bits/line_width`); source rect is clipped against **this buffer's current clip rect** ("convoluted use of clip()") |
| `put(src,dx,dy)` | 255 | 105-110 | `Image_buffer8::copy8(src->bits, src->width, src->height, …)` — **qualified call**, assumes `src->line_width == src->width` |
| `fill_static(b,g,w)` | 257 | 115-134 | random noise over full buffer |
| `draw_beveled_box(...)` | 278 | 827-883 | composed of `put_pixel8/fill_hline8/draw_line8/fill8` (unqualified ⇒ virtual) |
| `draw_box(...)` | 291 (virtual, non-pure) | imagebuf.cc:58-89 | composed of `fill_hline8/draw_line8/fill8` |

### 3.3 Non-virtual `Image_buffer8` extras

| Member | Lines | Notes |
|---|---|---|
| `paint_rle(xoff,yoff,data)` | ibuf8.h:109, ibuf8.cc:516-669 | the hot path for every sprite; decodes U7 RLE directly into `bits` using `clipx..cliph` |
| `paint_rle_remapped(xoff,yoff,data,trans&)` | ibuf8.h:110, ibuf8.cc:672-825 | same with a 256-byte translation table |

RLE format (as consumed above and in vgafile.cc): sequence of `uint16 scanlen` (bit0 = encoded),
`sint16 x`, `sint16 y` relative to the frame origin; raw scans hold `scanlen` bytes; encoded scans
hold runs `bcnt` (7 bits after `>>1`, bit0 = repeat). Transparency = absence of pixels. Nothing in
the format limits resolution: at S=6 the offsets (≤ ±32767) and scan lengths (≤ 32767) still fit,
so **hi-res RLE frames can use the same format and the same decoder** at physical coordinates.

### 3.4 Composite painters outside the buffer (call per-line primitives)

`Shape_frame` (shapes/vgafile.cc):
* `paint_rle` 483-498 → `win->paint_rle` (non-virtual); `is_visible` culling only for frames ≥ 8 px.
* `paint_rle_remapped` 504-519 → `win->paint_rle_remapped`.
* `paint` 525-534: flats → `win->copy8(data, 8, 8, xoff-8, yoff-8)`.
* `paint_rle_translucent` 540-589 → `copy_hline_translucent8`, `fill_hline_translucent8`, `fill_hline8`
  (translucent indices `xfstart = 0xff - xfcnt … 0xfe`, 556).
* `paint_rle_transformed` 596-634 (invisible NPCs) → `fill_hline_translucent8`.
* `paint_rle_outline` 640-699 → `put_pixel8`, `fill_hline8`.
* `reflect()` 90-124 uses a **local** `Image_buffer8` as a transposition scratch (`copy8`, `fill8`,
  `get_bits`) — pure data processing that must stay unscaled.

Fonts: `Font::paint_text` (shapes/font.cc:255-285, 293-323) **ignores its `win` argument**
(`ignore_unused_variable_warning(win)`) and paints through `Shape_frame::paint_rle(x,y)`, i.e.
into the static `Shape_frame::scrwin`. `paint_text_box` does use `win` for clip (font.cc:97-99,
354, 456).

### 3.5 `Image_window` / `Image_window8` wrappers

* `Image_window` wrappers (imagewin.h:874-950) call `ibuf->X(...)` → **virtual** dispatch.
  `fill_translucent8` is itself virtual in `Image_window` (911) and not overridden by `Image_window8`.
* `Image_window8` **hides** them with wrappers that call **qualified** members:
  `ib8->Image_buffer8::fill8 / fill8(rect) / fill_hline8 / copy8 / copy_hline8 /
  copy_hline_translucent8 / fill_hline_translucent8 / copy_transparent8 / get_pixel8 / put_pixel8`
  (iwin8.h:94-149). Only `draw_line8` is unqualified (111-113). Since `Game_window::win` is an
  `Image_window8*` (gamewin.h:103, 426), the bulk of engine calls (`win->fill8`, `gwin->win->copy8`
  in gamerend.cc:364-382, 529, 664-700; gumps/Dynamic_container_gump.cc:96-302; …) are
  **statically bound to Image_buffer8's implementation** — a subclass override would be silently
  bypassed. Calls through an `Image_window*` (e.g. effects.cc:1838-1866) go virtual. Dispatch
  therefore depends on the static pointer type.
* Window-level depth-independent wrappers: `clear_clip/set_clip/copy/get/put` (928-950),
  `is_visible` (613-615), `create_buffer` → `ibuf->create_another` (imagewin.cc:848-852).

### 3.6 Palette functions (iwin8.cc)

* `set_palette(rgbs,maxval,brightness)` 96-120: builds `colors[768]` with brightness and static
  gamma tables (`GammaRed/Green/Blue`, 57-59, `get/set_gamma` 68-78), pushes 256 colours to the SDL
  palette of `paletted_surface` and `draw_surface`, then `mark_all_layers_dirty()`.
* `apply_gamma_palette(...)` 122-129: same conversion into a caller array (fixed UI palettes).
* `get_palette()` iwin8.h:83-85.
* `rotate_colors(first,num,upd)` 135-174: rotates RGB triplets in `colors[]`, pushes to SDL when
  `upd`, marks layers dirty. Called every 100 ms from `Game_window::rotatecolours` for ranges
  `0xFC+3, 0xF8+4, 0xF4+4, 0xF0+4, 0xE8+8, 0xE0+8` (gamewin.cc:1054-1079) and for plasma
  (gamewin.cc:3226).
* Fades live in `Palette::fade_in/fade_out` (palette.cc:391-482): repeated `win->set_palette` +
  `win->show()`.
* `mini_screenshot()` 181-226: **raw read** of `ibuf->get_bits()/get_line_width()`, averages 3x3
  blocks of a centred 288x180 region using `get_game_width/height` → 96x60 save thumbnail.
* Layer conversion: `layer_argb_pixel` 231-250, `fill_guardband` 252-274, `refresh_layer`
  281-322, `refresh_layer_scaled` 331-617 (consensus-difference matting through the game scaler);
  these read the layer **SDL surfaces** directly with `logw/logh`.

Palette consequence for hi-res: the whole pipeline remains *8-bit indexed until presentation*
(the scalers convert through `paletted_surface`'s SDL palette, e.g. scale_point.cc
`Manip8to32(paletted_surface_palette->colors, …)`). As long as the S-x buffer stays 8-bit indexed,
day/night palette swaps, fades, colour cycling (0xE0-0xFF) and Xform translucency keep working
**unchanged**. Override art must therefore be palette-indexed and must respect index semantics:
cycling ranges above; translucent indices `0xFF-nxforms … 0xFE` (= 0xEE…0xFE with the 17 default
blends, shapeid.cc:333-368) which overlap the cycling range (meaning depends on whether the shape is
drawn translucent); 0xFF used as "transparent" in fills/reflect (vgafile.cc:92); index 0 transparent
in `copy_transparent8`.

---

## 4. Raw pixel memory / geometry bypasses of the virtual API

Inside imagewin (friends reading fields directly):
1. `create_surface` sets fields (imagewin.cc:564-577).
2. `show()` (882-1114): `ibuf->clip()` (903), start offset subtraction (906-907), `buffer_w =
   get_full_width()+gb` (915-916), **gate `ibuf->width + 2*gb == draw_surface->w`** (968) that
   decides whether the scaling path runs, scaler calls with `x+gb,y+gb,w,h` in buffer pixels
   (977-1002), `get_full_width()` in phase 2 (1026-1027, 1060-1061).
3. All software scalers read `draw_surface->pixels`, `ibuf->line_width`, `ibuf->height + guard_band`
   and (bilinear) `ibuf->width/height` for `increase_area` — scale_point.cc, scale_2x.cc,
   scale_2xSaI.cc, scale_bilinear.cc:38-146, scale_hq{2,3,4}x.cc, scale_xbr.cc, scale_interlace.cc
   (~120 reads in total).
4. `Begin/EndPaintIntoGuardBand`, `FillGuardband` (1142-1252) mutate/read `ibuf->width/height`.
5. `scale_layer_color` (1738-1781) temporarily overwrites `ibuf->line_width` and `ibuf->height`
   (and `draw_surface`, `scale`) to reuse the member scalers on a layer surface.
6. `screenshot` (1254-1271) saves `draw_surface` with its guard band.
7. `screen_to_game/game_to_screen` (1277-1313): `gx = sx*inter_width/(scale*display_width) + start_x`.
8. `mini_screenshot` (iwin8.cc:181-226) as above.

Engine code:
9. `Game_render::paint_chunk_flats` (gamerend.cc:520-531): `gwin->win->copy8(cflats->get_bits(),
   128, 128, xoff, yoff)` — passes the **chunk cache's raw pixels**, assuming stride 128.
10. `Chunk_terrain::render_flats/paint_tile` (objs/chunkter.cc:85-131, 248-268): per-chunk
    `Image_buffer8(128,128)` cache, filled with `copy8(shape->get_data(), 8, 8, tx*8, ty*8)`;
    LRU of 100 buffers (`Figure_queue_size`, 234-242).
11. `Game_map::write_minimap` (gamemap.cc:1705-1722): iterates `get_bits()` of each
    `rendered_flats` assuming `line_width == width`, averaging colours.
12. Intro zoom `SDL_SurfaceOwner` (gamemgr/bggame.cc:690-710) wraps `get_bits/get_width/
    get_line_width` of an `Image_buffer` in an SDL surface.
13. `Shape_frame::reflect` (vgafile.cc:91-124) uses a local buffer's `get_bits`.
14. Static casts assuming `Image_buffer8`: `Game_window::get_layer_ibuf` (gamewin.h:438-439),
    Notebook_gump scratch (gumps/Notebook_gump.cc:512).
15. ExultStudio (`mapedit/shapedraw.cc:58-67, 490, 1208`, `mapedit/chunklst.cc:588`,
    `mapedit/shapelst.cc`) and tools (`tools/ipack.cc`, gimp/aseprite plugins,
    `exult_shp_thumbnailer`) use `Image_buffer8` and `get_bits()` as plain image containers. They
    link the same ibuf8.cc, so **default behaviour (scale 1) must be bit-identical**.

Hit-testing does **not** read the framebuffer: `get_pixel8` has no engine callers; object picking
uses `Shape_frame::has_point` on RLE data in game pixels (vgafile.cc:706+). Mouse mapping is done
by `screen_to_game` (point 7). So keeping the buffer API logical preserves picking/collision.

---

## 5. Assessment: how to implement a scaled 8-bit buffer

Goal recap: logical game pixels everywhere in game code; physical storage S×S per logical pixel
(S = 6 → 1920x1200 for 320x200); frames without override drawn with nearest-neighbour
replication; frames with an override drawn at native physical resolution; buffer stays
palette-indexed.

### 5.1 Option (a): scaled subclass with logical API, physical storage

`class Image_buffer8_scaled : public Image_buffer8` keeping `width/height/offset/clip` logical,
plus `S`, physical `pwidth/pheight/ppitch/pbits`. Each primitive clips in logical coordinates
with the existing helpers, then writes S×S blocks.

Per-primitive changes:

| Primitive | Scaled implementation (logical args) | Notes |
|---|---|---|
| `fill8(val)` | memset over physical rows (`ppitch*pheight`) | trivial |
| `fill8(rect)` | clip logical; memset `S*w` bytes on `S*h` rows at `(S*x, S*y)` | |
| `fill_hline8` | clip_x; memset `S*w` on `S` rows | |
| `draw_line8` | run the DDA on logical coords; plot each logical pixel as an S×S block (or xform block) | current implementation walks a raw pointer with `inc1=line_width` (ibuf8.cc:288-353) — must be restructured; only debug/cheat callers (shapeinf.cc:604, cheat_screen.cc:1367-1406) |
| `copy8` | clip; expand each source row (each byte → S bytes) into the first physical row, then `memcpy` it S-1 times | |
| `copy_hline8` | same, 1 logical row | |
| `copy_hline_translucent8` | per source pixel: opaque ⇒ replicate; translucent ⇒ apply `xforms[c-first][*to]` **to each of the S×S dest pixels individually** | dest underneath may be hi-res, so the result cannot be computed once and replicated |
| `fill_hline_translucent8`, `fill_translucent8` | apply xform to every physical pixel of the S-scaled rect | |
| `copy_transparent8` | replicate non-zero source pixels | dead code; implement for completeness |
| `get_pixel8` | read top-left physical sample | no engine callers |
| `put_pixel8` | clip; S×S block | |
| `create_another` | return a buffer with the **same S** (so `get`/`put` round-trips keep hi-res detail) | FLI decoding (flic/playfli.cc:116-363) and intro save-unders would then pay S² — offer `create_another_unscaled` or let `put()` accept a scale-1 source |
| `copy` (scroll) | multiply all coords by S, `memmove` `S*h` rows of `S*w` bytes | still unclipped; callers (gamewin.cc:1654-1722, effects.cc:1862-1866) pass in-range logical rects |
| `get` | clip logical (current convoluted clip), then: dest scale == S → physical copy; dest scale 1 → decimate | must use dest's physical pitch, not `dest->width` |
| `put` | src scale == S → physical blit honouring `src` pitch; src scale 1 → replicate (`copy8`) | current code assumes `line_width == width` |
| `fill_static` | per logical pixel replicate (keeps "chunky" static) | |
| `paint_rle` / `paint_rle_remapped` | decode in logical coords with logical clip, expand each run ×S horizontally and replicate S rows | currently **non-virtual** → must become virtual (or branch internally) |
| `draw_box`, `draw_beveled_box` | inherited; already composed of virtual calls | lines become S px thick (same look as upscale) |
| clip API, `is_visible`, `ClipRectSave` | unchanged (logical) | |
| `get_bits/get_line_width/get_width/get_height` | ambiguous — must add explicit `get_phys_*` and audit all raw users (§4) | |

Plus a **native-resolution path for hi-res frames**. Two equivalent ways:
* dedicated physical primitives, e.g. `copy8_phys(src, pw, ph, src_pitch, pdx, pdy)` and
  `paint_rle_phys(pxoff, pyoff, data)`, clipping against `clip*S`; or (preferred)
* a **physical view**: `Image_buffer8 phys_view()` returning a non-owning scale-1 `Image_buffer8`
  aliasing the same memory with `width/height/offset = physical` and clip = logical clip × S. All
  existing code — `Image_buffer8::paint_rle`, `Shape_frame::paint_rle_translucent / transformed /
  outline` (vgafile.cc:540-699), `copy8` for flats — then works verbatim on hi-res RLE/flat data at
  physical coordinates (`xoff*S`, `yoff*S`). The aliasing ctor at ibuf8.h:43-47 shows the pattern
  but subtracts a guard band; a ctor taking explicit offsets/clip is needed.

Problems specific to a *subclass*:
1. `Image_window8` wrappers use qualified calls (iwin8.h:94-149) and `put()` calls
   `Image_buffer8::copy8` qualified (ibuf8.cc:109) → overrides bypassed. Must be de-qualified.
2. `paint_rle`/`paint_rle_remapped` are non-virtual and called through `Image_buffer8*`
   (vgafile.cc:497, 518; mouse.cc:224-226) → must be made virtual.
3. The window buffer object is created once (iwin8.cc:64) and its pointer cached widely (§1);
   changing S at runtime (resize, option change) would require replacing the object → dangling
   `Shape_frame::scrwin`, `Game::ibuf`, `Cheat_screen::ibuf`, `ExultMenu::ibuf`. So the subclass
   would need to support S changing in place anyway — at which point it is just a field.
4. Scalers and `show()` read `ibuf->width/height/line_width` as physical (§4 items 2-5). With
   logical `width/height` in the scaled buffer, all ~120 scaler reads plus `show()`,
   guard-band code and `scale_layer_color` must switch to physical fields.

### 5.2 Option (a′) — recommended: scale as a field of `Image_buffer8`

Add to `Image_buffer8` (or `Image_buffer`) `int scale = 1` plus physical geometry
(`phys_width, phys_height, phys_pitch`, physical base pointer). Keep logical
`width/height/offset/clip`. Every primitive does `if (scale == 1) { original code } else { scaled
code }`. Advantages:
* Default behaviour is byte-identical (ExultStudio, tools, chunk-cache-at-S=1, reflect, layers).
* No virtual-dispatch hazards: the qualified `Image_buffer8::` calls in iwin8.h and `put()` keep
  working; non-virtual `paint_rle` branches internally.
* In-place change of S on the persistent window `ib8` → cached pointers stay valid.
* `static_cast<Image_buffer8*>` sites remain valid.
* The branch cost is negligible versus S² pixel work.

The physical view (§5.1) provides the native-res path. A helper
`blit(const Image_buffer8& src, int destx, int desty)` that understands both scales replaces the
`copy8(cflats->get_bits(), 128, 128, …)` bypass in `paint_chunk_flats`.

### 5.3 Option (b): physically S× buffer, coordinates scaled at call sites

Make `ibuf` a plain 1920x1200 `Image_buffer8` (S=1 semantics) and multiply coordinates by S where
the game draws. Cost: every drawing call site that targets the world buffer must change —
`paint_shape` (~150 call sites), `paint_text*` (~150), `->paint(` (~150), direct `win->fill8 /
fill_translucent8 / copy8 / set_clip / copy` (~66), `get_width/get_game_width` (~110),
`get_start_x/end_x/full_width` (~47), dirty rects, clip rects, scroll math, effects, plus the RLE
decoder would still need an "upscale native frame" mode because original frames are 1x. In practice
(b) degenerates into (a) with far more churn and a high risk of mixing units (game pixels are also
used for hit-testing, tile math `c_tilesize=8`, chunk math `c_chunksize=128`). Not recommended.
The only part of (b) worth keeping is at the **window level**: the SDL `draw_surface` *is* S× larger,
and `show()`/scalers operate on it in physical units.

### 5.4 Window-side consequences (any option)

* `create_surface` (imagewin.cc:540-578): allocate `draw_surface` as `S*logical + 2*gb`; set
  logical `width/height/offset` and physical fields; compute `bits` with physical offsets
  `offset*S`. Ensure the physical full size is an exact multiple of S (Fit/AspectCorrect modes
  produce arbitrary `inter_width/scale`, imagewin.cc:726-727).
* The existing "scaler scale" and S interact: effective scaler factor = `scale / S` when integral
  (e.g. window 1920x1200, S=6 → no scaler, fill path only); when the window is smaller than S×game
  the physical buffer must be **downscaled** in the fill stage (arb Bilinear supports arbitrary
  ratios, BilinearScaler.cpp:48-80; SDLScaler uses GPU linear filtering). This is presentation
  work outside the buffer but `show()` must convert its logical rect to physical (multiply by S
  after `ibuf->clip`, before the 4-px alignment at 917-944) and the gate at 968 must compare
  physical dims.
* Guard band: `guard_band` = 4 *physical* px. `BeginPaintIntoGuardBand` adds `gb/2 = 2` to
  `ibuf->width` — in logical units at S=6 that is 12 physical px, overflowing the 4-px band.
  Either disable guard-band painting when S>1 (point-style presentation) or recompute in physical
  units (`max(1, ceil(gb/2/S))` logical requires `gb ≥ 2S`). `FillGuardband` must use physical
  width/height.
* `scale_layer_color` must save/patch the physical fields the scalers actually read.
* `screen_to_game/game_to_screen` must keep returning **logical** coords: the denominator uses
  `scale`; if `scale` is redefined as physical-scaler scale, divide additionally by S.
* `mini_screenshot` must sample the physical buffer with stride S (or average 3S×3S).
* `screenshot(paletted)` will save the hi-res draw_surface — fine.
* Palette/rotation: unchanged API, but each rotation tick (100 ms) forces a re-convert of the
  whole physical buffer on `show()` — 2.3 Mpx at 1920x1200, acceptable; 9.2 Mpx for a 640x400
  logical view at S=6.

### 5.5 Where overrides hook in (buffer-relevant hooks)

1. **Terrain (first priority).** `Chunk_terrain::render_flats` (objs/chunkter.cc:248-268) creates
   the per-chunk cache; `paint_tile` (85-131) writes each 8x8 flat with `copy8`. Make the cache a
   scaled buffer (`128S x 128S`, 576 KB at S=6; ×100 LRU ≈ 58 MB — consider lowering
   `Figure_queue_size`, 234-242); in `paint_tile`, if `(shapes.vga, shape, frame)` has an override,
   physical-copy the `8S x 8S` tile, else replicate. `paint_chunk_flats` (gamerend.cc:520-531) must
   do a scale-aware blit instead of `copy8(get_bits(),128,128,…)`. `write_minimap`
   (gamemap.cc:1705-1722) must iterate physical dims (averaging still works).
   Cache invalidation: clear all `rendered_flats` when S or the override set changes.
2. **Flat frames drawn directly**: `Shape_frame::paint` (vgafile.cc:525-534).
3. **RLE sprites**: `Shape_frame::paint_rle / paint_rle_remapped / paint_rle_translucent /
   paint_rle_transformed / paint_rle_outline` (vgafile.cc:483-699) — if the frame has a hi-res RLE
   and the target buffer's S matches, paint it via the physical view at `(xoff*S, yoff*S)`; culling
   with `is_visible` stays logical (frame logical extents unchanged). Fonts reach these through
   `Shape_frame::scrwin` (font.cc:277-279).
4. Overrides must be stored S× with the hotspot at `(xleft*S, yabove*S)` so logical extents
   (`get_width/xleft/...`, used for hit-testing) stay the original ones.

### 5.6 Test strategy enabled by this design

`ibuf8.cc` and `imagebuf.cc` depend only on `endianio.h`/`common_types.h` (no SDL;
imagewin/Makefile.am builds them in the base `libimagewin` list). There is no unit-test suite in the
repo today. A standalone test binary can use a strong oracle: **apply the same random sequence of
primitive calls (fills, lines, copy8, hlines, translucent ops with real Xform tables, put_pixel,
copy/scroll, get/put, paint_rle, paint_rle_remapped, boxes, with random clip rects and negative
offsets) to an S=1 buffer and to an S=k buffer; assert `phys(S=k) == nearest_upscale(S=1)`.**
Nearest-neighbour upscale commutes with every per-pixel/xform operation when the background is
itself an upscale, so this checks clipping and replication exhaustively. Add: S=1 byte-identical
regression against the current implementation (golden), physical-view clip mapping, hi-res RLE
drawn through the view equals a reference rasterisation, guard-band bounds (ASan).

---

## 6. Summary of risks

* Statically bound qualified calls (iwin8.h:94-149, ibuf8.cc:109) silently bypass subclass
  overrides.
* Fields `width/height/line_width` have a dual role (logical API + scaler geometry); mixing units
  is the main bug source.
* Guard band constant is physical; guard-band painting adds a logical delta.
* Raw `get_bits()` users assume `line_width == width` and logical geometry (gamerend.cc:529,
  gamemap.cc:1706, iwin8.cc:190, bggame.cc:700).
* `set_render_buffer` swaps `ibuf`, so window-geometry functions report the redirected buffer.
* Translucency applied to replicated blocks must be per physical pixel.
* Override art must stay palette-indexed and honour cycling/translucent index ranges.
* Memory of the terrain cache grows S² (≈58 MB for 100 chunks at S=6).
* ExultStudio/tools share `ibuf8.cc`; S=1 must remain byte-identical.
