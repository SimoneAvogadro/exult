# Hi-res art overrides — Analysis: shape data, loading, caching, mods/patches

Scope: `shapes/vgafile.{h,cc}`, `shapeid.{h,cc}`, `shapes/shapevga.*`, `shapes/font*.{h,cc}`, `shapes/pngio.*`,
`files/` (U7file manager, Flex, path tags, `utils.cc`), `fnames.h`, the mod system (`gamemgr/modmgr.cc`, `game.cc`,
`exult.cc`), plus the places that consume raw frame pixels (`objs/chunkter.cc`, `gamerend.cc`, `mouse.cc`, `gamewin.cc`).
Repo: `/home/simonea/ultima7_exult/exult-hires` (git master, last commit `8b6ab6b43`). All line numbers refer to that tree.

---

## 1. On-disk formats

### 1.1 Flex container (all `*.vga`, `*.flx`)
* 128-byte header: 80-byte title, `magic1 = 0xffff1a00`, `count` at offset `0x54`, `magic2`, padding
  (`files/Flex.h:33-60`). Index table at `0x80`: `count` pairs of `(u32 offset, u32 length)`.
* `Vga_file` does **not** use `U7FileManager`/`Flex` objects for shapes. It opens a raw `IDataSource` per source
  and reads the table itself: shape `n` entry at `0x80 + n*8` (`shapes/vgafile.cc:863`, `:873-876`). An entry with
  `offset==0 || length==0` means "no shape here" (`:878`, `:893`), which is what makes *sparse* patch files work.

### 1.2 Shape entry (one Flex object per shape)
Two encodings, distinguished heuristically in `Shape_frame::read()` (`shapes/vgafile.cc:398-450`):
* **RLE shape**: `u32 dlen` (total length) followed by `u32 frame_offset[nframes]`; `hdrlen = frame_offset[0] = 4+4*nframes`,
  so `nframes = (hdrlen-4)/4` (`:411-418`). It is RLE iff `dlen == shapelen` or (`shapelen` even and `dlen == shapelen-1`) (`:415`).
* **Flat (8x8 tile) shape**: just `nframes * 64` raw palette indices; `nframes = shapelen / 64` (`:443-449`).
  The engine decides flat-vs-RLE **only from the data**, not from the shape number. ES uses `IS_FLAT(shnum) = shnum < 0x96`
  (`mapedit/shapelst.cc:73`, `c_first_obj_shape` in `shapes/shapevga.h:36`) but shapes < 150 *can* be RLE (terrain
  objects such as ice-cave pieces; see `objs/chunkter.cc:92-131` and `Game_render::paint_chunk_flat_rles`, `gamerend.cc:537-550`).

### 1.3 RLE frame
On disk (`get_rle_shape`, `shapes/vgafile.cc:456-477`): `u16 xright, u16 xleft, u16 yabove, u16 ybelow` (note the order,
read into `short`s), then the scanline stream. In memory `data` holds only the scanline stream (`datalen = framelen-8`).

Scanline stream (decoders: `Image_buffer8::paint_rle`, `imagewin/ibuf8.cc:516+`; encoder: `Shape_frame::encode_rle`,
`shapes/vgafile.cc:281-347`):
```
repeat:
  u16 hdr            ; 0 terminates the frame
  len  = hdr >> 1    ; pixels in this scanline segment
  enc  = hdr & 1
  s16 x, s16 y       ; position RELATIVE TO THE FRAME ORIGIN (x - xleft, y - yabove at encode time)
  if !enc: len raw palette bytes
  else: runs until len pixels consumed:
        u8 b; cnt = b >> 1
        if (b & 1) u8 pixel  -> cnt copies
        else       cnt raw bytes
```
* Pixel value `255` is transparent only at *encode* time (`Skip_transparent`, `:135-145`): transparent pixels are never stored,
  so painting never tests for 255.
* Run counts are capped at 127 per byte (`:319`, `:327`).
* Frame extents: width = `xleft + xright + 1`, height = `yabove + ybelow + 1` (`vgafile.h:124-130`). The origin
  `(0,0)` is the "hot spot" — for world objects it is the bottom-right of the footprint at the object's lift
  (`Game_window::get_shape_location`, `gamewin.cc:1278-1313`).

### 1.4 Flat frame in memory
`xleft = yabove = 8`, `xright = ybelow = -1` (`vgafile.cc:444-445`) — i.e. the origin is one pixel *past* the bottom-right
corner, and `paint()` does `win->copy8(data, 8, 8, xoff-8, yoff-8)` (`:525-534`). `rle=false`, `datalen=64`.
The `Shape_frame` constructors assert `w == h == c_tilesize` for non-RLE frames (`vgafile.cc:363`, `:384`).
`c_tilesize=8`, `c_num_tile_bytes=64`, `c_chunksize=128` (`exult_constants.h:28-32`).

---

## 2. Class model, ownership, memory

| Class | Owns | Notes |
|---|---|---|
| `Shape_frame` (`vgafile.h:47-163`) | `unique_ptr<uint8[]> data`, `datalen`, 4 `short` extents, `bool rle` | Has a **virtual** destructor (`:158`) -> vptr; ~32 bytes + data. Non-copyable, movable. Static `scrwin` = current render target (`:55`, `vgafile.cc:55`). |
| `Shape` (`vgafile.h:168-268`) | `vector<unique_ptr<Shape_frame>> frames`, `num_frames`, `modified`, `from_patch` | `num_frames` excludes reflections; reflected frames are stored in the same vector at index `framenum|32` (`vgafile.cc:817-823`), so `frames.size()` can exceed `num_frames`. |
| `Shape_file` (`vgafile.h:274-293`) | one `Shape`, all frames preloaded | Used for fonts (`shapes/font.cc:789`), mouse pointers (`mouse.h:36`), save-game thumbnails (`gamewin.cc:3126-3133`). |
| `Vga_file` (`vgafile.h:298-412`) | `shape_sources` (vector of `(unique_ptr<IDataSource>, is_patch)`), `shape_cnts`, `vector<Shape> shapes`, plus a parallel *imports* set (`imported_sources`, `imported_shapes`, `imported_shape_table`) | Sources stay **open for the life of the file**; frames are read lazily via `seek`. No eviction: frames are only freed on `reset()`/`load()`/`new_shape()`. Has no name/identity besides `u7drag_type`. |
| `Shapes_vga_file` (`shapes/shapevga.h:41-90`) | `Vga_file` + `map<int, Shape_info>` | `init()` loads `SHAPES_VGA` + `PATCH_SHAPES` (`shapevga.cc:972-979`). |
| `Fonts_vga_file` (`shapes/fontvga.h:39`) | `vector<shared_ptr<Font>>`, each `Font` owns a `Shape_file` | The `Vga_file` base is unused for painting; each font is copied into its own `Shape_file` (`font.cc:775-795`). |
| `Shape_manager` (`shapeid.h:75-222`) | `Shapes_vga_file shapes`, `Vga_file files[SF_COUNT]`, `fonts`, `xforms`, `translucency_argb`, `special_pixels`, `shape_cache[SF_COUNT]` | Singleton (`sman`). `files[SF_SHAPES_VGA]` is never loaded — shapes.vga lives in the separate `shapes` member. |

`ShapeFile` enum (`shapeid.h:39-52`): `SF_SHAPES_VGA, SF_GUMPS_VGA, SF_PAPERDOL_VGA, SF_SPRITES_VGA, SF_FACES_VGA, SF_EXULT_FLX,
SF_GAME_FLX, SF_SHORTCUTBAR_VGA, SF_OTHER`. Fonts are explicitly "not yet" in this enum.

**Frame cache in Shape_manager.** `shape_cache[file]` is a `std::map<pair<shape,frame>, Cached_shape{Shape_frame*, has_trans}>`
(`shapeid.h:77-80`, `:100-101`). `ShapeID::get_shape()/paint_shape()` -> `cache_shape()` -> one `std::map` lookup per call
(`shapeid.cc:552-583`). On a miss it calls `Vga_file::get_shape()` and stores the **raw pointer**. `has_trans` comes from
`Shape_info::has_translucency()` for shapes.vga and is forced `true` for sprites.vga (`:562-569`). The cache is cleared by
`load()` (`:152-154`), `load_gumps_minimal()` (`:387-388`) and `reload_shapes()` (`:423`).

**What gets loaded where** (`Shape_manager::load`, `shapeid.cc:150-382`):
* gumps: `GUMPS_VGA` + `PATCH_GUMPS` (`:177`); in BG, optionally imports SI gump shapes from `<SERPENT_STATIC>/gumps.vga` (`:198-216`).
* paperdolls: the sources come from `Shapeinfo_lookup::GetPaperdollSources()` (`:179`).
* BG multiracial: imports SI skin shapes into `shapes` (`:218-232`).
* sprites: `SPRITES_VGA` + `PATCH_SPRITES` (`:235`), plus an SIB special case (`:236-255`).
* faces: `FACES_VGA` (+ BG `files/mrfacesvga` resource) + `PATCH_FACES` (`:257-265`).
* `exult.flx` (`:267`), game flx `bg_data.flx`/`si_data.flx` (`:269-271`), shortcut bar = `exult.flx[EXULT_FLX_SHORTCUTBAR_VGA]` + `PATCH_SHORTCUTBAR_VGA` (`:273-279`).
* shapes.vga via `read_shape_info()` -> `shapes.init()` (`:116-144`, `:281`). It also sets `no_bit5_frame_reflection` for every shape with >32 frames (`:122-131`).
* fonts: from `exult.flx` (original/serif) or `fonts.vga`, with a per-font patch file (`:283-304`).
* xforms/blends (`:306-360`), `invis_xform` (`:360`), `translucency_argb` (`:367-381`).
* Outside the manager: `Game::menushapes` = `MAINSHP_FLX` + `PATCH_MAINSHP` (`game.cc:89`); `pointers.shp` (`mouse.cc:108-127`);
  menu fonts from mainshp/intro.dat (`game.cc:275-296`); a local `Vga_file exult_flx` (`game.cc:623`); the cheat-map minimaps
  `PATCH_MINIMAPS` (`cheat.cc:1223`).

---

## 3. Lazy loading, reflection, imports, patch layering

### 3.1 Lookup chain
`Vga_file::get_shape(shapenum, framenum)` (`vgafile.h:357-377`):
1. If `shapenum` is in `imported_shape_table` -> `imported_shapes[pointer_offset].get(imported_sources, realshape, framenum, imported_cnts, source_offset)`.
2. Else `shapes[shapenum].get(shape_sources, shapenum, framenum, shape_cnts, -1)`.

`Shape::get()` (`vgafile.h:224-228`) returns `frames[frnum]` if it is present, otherwise it calls `Shape::read()`.

`Shape::read()` (`vgafile.cc:855-920`):
* It walks the sources **from last to first** (the patch is last) and takes the first one whose table entry for this shape
  is non-empty (`:866-885`). Granularity is therefore **whole shape**: a patch shape replaces *all* frames.
  `from_patch` records whether the winner was a `<PATCH>` file (`:881`, flag set in `U7load`, `:1169`).
* It reads that frame only (`frame->read`), and on the first read it creates the `frames` list sized `nframes` (`:909-911`).
* **Flats:** `if (!frame->is_rle()) framenum &= 31;` (`:912-914`), and `Shape_frame::read` also masks (`:443`). If the frame
  index is still `>= frames.size()`, `store_frame` prints an error and returns `nullptr` (`:980-983`).
* **Reflection:** `if (framenum >= nframes && (framenum & 32)) return reflect(..., framenum & 0x1f, ...)` (`:915-918`).
  `Shape::reflect` reads the normal frame, calls `Shape_frame::reflect()`, and stores the result at index `framenum|32`
  (`:802-824`). `Shape_frame::reflect()` is a **transpose across the NW–SE diagonal** (x<->y): `xleft'=yabove,
  yabove'=xleft, xright'=ybelow, ybelow'=xright`. It decodes into a `max(w,h)^2` `Image_buffer8` and re-encodes RLE (`:73-126`).
  So "frames >= 32 are reflections" only holds when the shape has fewer than 33 real frames. Shapes with >32 frames are
  flagged `no_bit5_frame_reflection` (`shapeid.cc:127-128`, `shapeinf.h:753-767`), and for them index 33 is a real frame.
* Callers encode the reflect bit in the frame number: `objs/objs.cc:105` ("Bit 5=S, Bit6=reflect"), `:519-520`.
  `ShapeID::framenum` is a `signed char` (`shapeid.h:230`). A 2-byte IREG/chunk ID gives 6 frame bits (`:239-246`).
  v2 chunks put a full byte in it (`objs/chunkter.cc:150-152`), which overflows for frames >127.

### 3.2 Imports
`import_shapes()` (`vgafile.cc:1239-1268`) maps *logical* shape numbers in this file to `realshape` numbers in another
file (for example BG skins from SI `shapes.vga`). Imported frames are cached in `imported_shapes`. A caller of
`get_shape(logical, f)` cannot tell whether the frame was imported.

### 3.3 Patch layering summary
* Shapes: `Vga_file` source vector, back-to-front, per **shape** (above).
* Other resources: `U7multifile`/`U7multiobject` also search in reverse order (`files/U7file.cc:96-107`; `U7obj.cc:57-66`), per **object**.
* `File_data::patch` is computed as `!spec.name.compare(1, 8, "<PATCH>/")` (`files/U7file.cc:26`), which starts at index 1.
  It is therefore false for real `<PATCH>/...` names (this is an existing quirk, so do not rely on it). `Vga_file::U7load`
  does it correctly (`vgafile.cc:1169`).

### 3.4 Reload / invalidation
`Shape_manager::reload_shapes(shape_kind)` (`shapeid.cc:420-468`) is called with a **u7drag** kind (`U7_SHAPE_*`,
`shapes/u7drag.h:49-55`) but indexes `shape_cache[shape_kind]`, which is indexed by `ShapeFile`. These enums only agree for
SHAPES (0) and GUMPS (1). FONTS=2 clears the PAPERDOL cache, and FACES=3 clears the SPRITES cache while leaving dangling
faces pointers. This is an existing bug, but any hi-res reload hooked into the same path inherits it.
`U7FileManager::get_ptr()->reset()` is called there as well.

### 3.5 Latent hazards worth knowing
* Requesting a flat with a frame index >= 32 is never cached: `frames[33]` does not exist, so `read()` runs again and
  **replaces `frames[f&31]`**. That frees the frame that `shape_cache` may still point to (`vgafile.h:227` + `vgafile.cc:912-919`, `:987`).
* `get_rle_shape` with `len==8` (an empty frame) writes `data[0]`, `data[1]` on a null `unique_ptr` (`vgafile.cc:467-470`).
* **`encode_rle` uses a fixed `unsigned short runs[200]` per scanline segment (`vgafile.cc:295`).** Worst case is about 2 runs
  per 3 pixels (alternating 2-pixel repeats and single pixels), so segments wider than about 300 px can overflow the stack
  buffer. Original art rarely gets that wide. **6x art (often 400–1800 px wide, noisy after AI upscaling) will.** This must be
  fixed (for example a `std::vector<unsigned short>(w+1)`) before encoding any hi-res RLE in the engine or in tools (ipack/ES use the same code).
* `Shape::reflect` resize test uses `frames.size() - 1` (`vgafile.cc:819`). This is harmless but odd.

---

## 4. Painting code paths

### 4.1 Central dispatch
`Shape_manager::paint_shape(xoff, yoff, Shape_frame*, translucent, trans)` (`shapeid.h:171-183`):
* non-RLE -> `Shape_frame::paint` (copy8 8x8),
* remap table -> `paint_rle_remapped`,
* opaque -> `paint_rle`,
* translucent -> `paint_rle_translucent(xforms.data(), xforms.size())`.

Also `paint_invisible` -> `paint_rle_transformed(*invis_xform)` (`:185-189`) and `paint_outline` ->
`paint_rle_outline(get_special_pixel(..))` (`:192-196`).
`ShapeID::paint_shape` (`shapeid.h:348-356`) builds an optional 256-entry remap table from `palette_transform`
(`Get_palette_transform_table`, `shapeid.cc:603-641`) and forwards. All these overloads paint into the **static
`Shape_frame::scrwin`** (`vgafile.h:97-120`).

`scrwin` is set by `Game_window`: at construction (`gamewin.cc:330`), on resize (`:921`), and by
`push_render_target/pop_render_target` (`:540-550`). Those also redirect `Image_window8::ib8`
(`imagewin/iwin8.h:58-71`, which says "Callers must also point Shape_frame::set_to_render at the same buffer").
Master already draws UI pieces (gumps, mouse, text…) into **separate 8-bit layer buffers** this way. **The render scale
must therefore be a property of the target buffer, not a global.** The game-view buffer would be at S x while (initially) UI layers stay at 1x.

### 4.2 Frame painters (`shapes/vgafile.cc`)
| Function | Lines | Behaviour |
|---|---|---|
| `paint_rle` | 483-498 | visibility early-out (only if w or h >= 8), then `Image_buffer8::paint_rle` (`ibuf8.cc:516+`, clip-aware memcpy/fill) |
| `paint_rle_remapped` | 504-519 | same with `trans[pix]` (`ibuf8.cc:672+`) |
| `paint` | 525-534 | RLE -> `paint_rle`; flat -> `copy8(data, 8, 8, xoff-8, yoff-8)` |
| `paint_rle_translucent` | 540-589 | pixels in `[0xff - xfcnt, 0xfe]` go through `xforms[pix - xfstart]` (`copy_hline_translucent8`/`fill_hline_translucent8`) |
| `paint_rle_transformed` | 596-634 | ignores pixel values and applies one xform over the covered pixels (invisible NPCs) |
| `paint_rle_outline` | 640-699 | 1-pixel outline: both ends of each segment plus the full first/last row |
| `has_point` | 706-742 | hit test in **origin-relative game pixels**, with 1-pixel tolerance |

All painters take `xoff,yoff` as **buffer pixel coordinates of the origin**. The RLE stream stores coordinates relative to
the origin, so the same painters work unchanged on a hi-res RLE frame whose extents are in hi-res pixels, as long as the
caller passes `xoff*S, yoff*S` and paints into the S x buffer. This is the key reuse opportunity.

### 4.3 Direct consumers that bypass `paint_shape`
* **Terrain flats:** `Chunk_terrain::paint_tile` copies `shape->get_data()` (raw 64 bytes) into a per-chunk
  `rendered_flats` `Image_buffer8(c_chunksize, c_chunksize)` (`objs/chunkter.cc:86-131`, `:248-267`). An MRU queue of up to
  100 chunks is kept (`:233-240`). It is blitted with `win->copy8(cflats->get_bits(), c_chunksize, c_chunksize, xoff, yoff)`
  in `Game_render::paint_chunk_flats` (`gamerend.cc:520-531`). `Chunk_terrain::render_all` (terrain editor) uses `copy8`
  and `sman->paint_shape` (`chunkter.cc:284-313`). `gamemap.cc:1705` averages `rendered_flats` pixels for minimap
  colours. It uses `get_width/height`, so it is size-agnostic but slower at S x.
* **Fonts:** `Font::paint_text*` calls `shape->paint_rle(x, yoff)` on its own `Shape_file` frames and ignores the `win` argument
  (`shapes/font.cc:260-322`, `:498`, `:538`). Layout uses `get_width()` in game pixels.
* **Mouse:** `cur->paint_rle(lb, hot_x, hot_y)` into a UI layer buffer (`mouse.cc:217-231`).
* **Raw-frame callers of `sman->paint_shape`** (no `ShapeID` identity): intro/menus (`gamemgr/bggame.cc` ~67 sites,
  `sigame.cc`), `exultmenu.cc:408/517/560`, `menulist.cc:71/163`, `txtscroll.cc:181`, `browser.cc:380/387`, `cheat.cc:1275`,
  `usecode/intrinsics.cc:1501/1840`, `usecode/conversation.cc:768/923` (faces), `chunkter.cc:308`.
  **A hi-res hook placed only in `ShapeID`/`Cached_shape` would miss all of these.** A hook on the `Shape_frame` itself does not.

### 4.4 Geometry that must stay in game pixels
* Hit testing: `Game_window::find_object` uses `get_shape_rect(obj)` and `Shape_frame::has_point(x-ox, y-oy)` (`gamewin.cc:2104-2112`).
* Dirty rects: `get_shape_rect(s,x,y)` from low-res extents (`gamewin.h:826-828`), enlarged by `1 + c_tilesize/2` (`:789-791`).
  **Hi-res art must not extend beyond S x the low-res bounding box** (plus at most the margin of about 5 game px), or it leaves trails.
* `Shape_frame::get_width/xleft/...` are used throughout gumps/containers for layout (for example `gumps/Dynamic_container_gump.cc:406-407`).
* Conclusion: the low-res `Shape_frame` stays **authoritative for geometry, hit tests and layout**. The hi-res variant is purely visual.

### 4.5 Palette-dependent semantics (why overrides should be 8-bit indexed with the game palette)
* Transparent = 255 (encode-time) (`vgafile.cc:140`, `:164`).
* Translucency indices: `xfstart = 0xff - nxforms` .. `0xfe` (`vgafile.cc:556`; `shapeid.cc:333-368`). BG ships 17 blends, so 0xEE..0xFE.
* Palette cycling: `rotate_colors` on 0xE0-0xE7, 0xE8-0xEF, 0xF0-0xF3, 0xF4-0xF7, 0xF8-0xFB, 0xFC-0xFE (`gamewin.cc:1063-1068`).
* Palette transforms / remaps (`shapeid.cc:603-641`), special/cycling pseudo-pixels (`shapeid.h:55-70`, `:121-128`), day/night palettes.

An indexed hi-res frame that uses the **same index semantics** inherits all of this for free. RGB art would need re-quantisation.
The existing scalers can do 8->8 only for point scaling (`Scale_point` with `ManipBaseDest<uint8>`, `imagewin/scale_point.h:35-80`,
`manip.h:173`). hq2x/xBR blend colours and only produce 16/32-bit output, so they cannot produce indexed hi-res frames.

---

## 5. Files, path tags, mods

### 5.1 Path tags
* `path_map` (`files/utils.cc:98`). `add_system_path` strips trailing slashes (`:129-140`). `clone_system_path` (`:142-148`).
  `store/reset_system_paths` (`:101-107`).
* `get_system_path` expands nested `<TAG>` prefixes up to 10 levels (`:168-230`). An unknown tag is left as-is.
* `U7open_in` tries the name as given, then upper-cases path components from the end, one at a time (`:312-341`).
  `U7exists` = open as file or as directory (`:467-487`). `U7ListFiles(mask)` expands tags (`files/listfiles.cc:44`).
* Defaults: `<STATIC>`/`<GAMEDAT>`/`<PATCH>`/`<SAVEGAME>`/`<MODS>` set to relative placeholders in `exult.cc:564-569`;
  `<DATA>` via `setup_data_dir` (`utils.cc:838-917`); `<BUNDLE>` on macOS/iOS.
* Per game (`ModManager::ModManager`, `gamemgr/modmgr.cc:378-633`): `config/disk/game/<cfgname>/{path, static_path, mods, patch, source}`,
  defaulting to `$game_path/{static,mods,patch,source}`. These are registered as `<PREFIX_STATIC>`, `<PREFIX_MODS>`, `<PREFIX_PATCH>`… (`:592-617`).
* Per mod (`ModInfo::ModInfo`, `modmgr.cc:95-238`): `<mods>/<title>.cfg` with `<mod_info>` keys `mod_title, display_string,
  required_version, codepage, patch` (default `__MOD_PATH__/patch` = `<PREFIX_MODS>/<title>/patch`), `source`,
  `gamedat_path`, `savegame_path`, `skip_splash`, `menu_*`, `force_digital_music`. These are registered as `<PREFIX_TITLE_PATCH>` etc.
  Examples: `content/bgkeyring/Keyring.cfg` (`patch = __MOD_PATH__/data`), `content/islefaq/islefaq.cfg`.
* Activation: `BaseGameInfo::setup_game_paths()` (`modmgr.cc:56-85`, called from `Game::create_game`, `game.cc:118-119`,
  and `exult.cc:936/991`) clones `<STATIC>`, `<MODS>`, `<GAMEDAT>`, `<SAVEGAME>`. It then sets **`<PATCH>` to the mod's patch
  dir only (or the base game's patch dir when no mod is active), or clears it** (`:70-74`).

**Consequence:** while a mod is active, the base game's `patch/` is *not* searched. A hi-res pack installed in the base game's
patch dir would silently disappear when the user plays Keyring or SF Island. `Shapes_vga_file::init` also checks
`is_system_path_defined("<PATCH>")` before using it (`shapevga.cc:973`).

### 5.2 U7FileManager
`U7FileManager::get_file_object(File_spec)` (`files/U7fileman.cc:44-88`) caches `U7file` objects (Flex/IFF/Table/Flat) by
`File_spec{name,index}` (`U7obj.h:37-56`). `IExultDataSource` reads a whole object into memory (`databuf.h:383-402`).
`IFileDataSource` is a stream over `U7open_in` (`databuf.h:197-214`). `Vga_file` keeps `IFileDataSource` streams open
(`vgafile.cc:1163-1183`), and `IExultDataSource` buffers when the source is a resource index (for example `exult.flx`).

### 5.3 PNG I/O
`shapes/pngio.{h,cc}` is compiled into `libshapes.la` unconditionally (`shapes/Makefile.am:15-19`), but its body is guarded by
`HAVE_PNG_H` (`pngio.h:29`, `pngio.cc:30`). `configure.ac:487-500` sets this when libpng is found and forces it **off on
Android**. MinGW and MSVC define it (`Makefile.mingw:514`).
`Import_png8(name, transp_index, w, h, rowbytes, xoff, yoff, pixels, palette, pal_size)` (`pngio.cc:45-171`):
* uses `fopen(name)` directly, so **no tag expansion and no case fallback**. The caller must pass `get_system_path(...)`.
* only accepts `PNG_COLOR_TYPE_PALETTE` (`:97-101`) and unpacks depth <8 (`:102-104`);
* reads `oFFs` (pixel units) into `xoff,yoff` (`:118-126`);
* for every **fully transparent** tRNS entry `i`: pixels `==i` become `transp_index`, and **pixels `>i` are decremented**
  (the entry is removed from the palette) (`:147-166`). This round-trips ES/ipack exports, which rotate the palette so that
  transparency is index 0 (`Export_png8(..., transp_to_0=true)`, `:178-245`). A palette PNG whose index 0 is merely *marked*
  transparent without the rotation would get **every index shifted by one**.
* returns the PNG palette. The engine has no palette-conversion helper; ES has a file-static `Convert_indexed_image` +
  `Find_closest_color` (`mapedit/shapelst.cc:1028-1050`).

Tool conventions to stay compatible with:
* ES/ipack `oFFs = (-xright, -ybelow)` for RLE frames and `(0,0)` for flats (`mapedit/shapelst.cc:671-677`, `tools/ipack.cc:452-461`).
  On import: `xleft = w + xoff - 1`, `yabove = h + yoff - 1` (`shapelst.cc:1100-1103`).
* ipack `all: <base>` extracts every frame as `<base>SSSS_NN.png` (`tools/ipack.cc:100-120`, `:450`, `tools/ipack.txt`).
  `ipack -c` writes `empty_object()` for unlisted shapes, which produces **sparse** flex files (`tools/ipack.cc:660-690`).
  ipack/ES force flats to 8x8 (`shapelst.cc:1091-1099`, `ipack.txt` "flat").

---

## 6. What breaks if the game-view buffer becomes S x larger (shape-subsystem view)

1. Every painter receives game-pixel `xoff,yoff`. Something must multiply by S and choose between a hi-res frame and an upscaled low-res frame. Today nothing does.
2. Flat path: `paint_tile`/`copy8(..., c_tilesize, c_tilesize, tilex*c_tilesize, ...)`, `rendered_flats(c_chunksize, c_chunksize)`,
   `paint_chunk_flats(copy8 c_chunksize)` and `Shape_frame::paint` for flats all hard-code 8/128.
   (`chunkter.cc:91,130,259,300-302`; `gamerend.cc:529`; `vgafile.cc:532`). At S=6: 768x768 = 576 KiB per chunk buffer;
   100 cached chunks = about 56 MiB (the queue size is a constant, `chunkter.cc:233-240`).
3. Low-res RLE frames have to be drawn magnified: either an on-the-fly "paint_rle_scaled" (each segment -> S rows, each run x S)
   or a lazily generated, cached nearest-neighbour hi-res `Shape_frame`. The second doubles as the "pre-scaled" fallback cache,
   but costs S^2 x memory.
4. `paint_rle_outline` draws 1 hi-res pixel, which is visually 1/S of a game pixel. It needs an S-thick variant or
   upscale-then-outline. `paint_rle_transformed`/`translucent`/`remapped` are per-pixel and scale-invariant.
5. `Shape_frame::paint_rle`'s visibility early-out uses `c_tilesize` as a size threshold. That is harmless.
6. Fonts and UI painted via `scrwin` must keep working at 1x on 1x layers. That again means a per-buffer scale.
7. `encode_rle`'s `runs[200]` overflow at hi-res widths (§3.5).
8. Hit tests, dirty rects and layout stay correct only if they keep using the **low-res** frame (§4.4).

---

## 7. Design assessment

### 7.1 Representation of a hi-res variant
**Recommended: reuse `Shape_frame` as the hi-res container, and attach it to the low-res frame with a non-owning pointer.**
* A hi-res variant is a `Shape_frame` with `rle=true` whose extents and RLE coordinates are in **physical (S x) pixels**.
  All existing painters (opaque, remapped, translucent, transformed, outline) then work unchanged on the S x buffer with
  `(xoff*S, yoff*S)`. Hi-res **flats** are stored as RLE too: a 48x48 opaque frame with `xleft=yabove=8S, xright=ybelow=-1`.
  This is exactly what `Shape_frame(pixels, 8S, 8S, 8S, 8S, true)` produces (`vgafile.cc:355-369`). No flat-specific
  hi-res code is needed, apart from `paint_tile`, which must call `paint_rle` instead of `copy8` (or a decode-to-raw fast path).
* Geometry mapping (low-res pixel `p` covers hi-res `[p*S, p*S+S-1]`):
  `xleft_h = xleft*S`, `xright_h = xright*S + S - 1`, `yabove_h = yabove*S`, `ybelow_h = ybelow*S + S - 1`,
  so `w_h = w*S`, `h_h = h*S`, and the hi-res origin is the top-left hi-res pixel of the low-res origin pixel.
  The loader should **validate** `w_h == w*S && h_h == h*S` (or "contained within") and reject anything else.
* Attachment: add to `Shape_frame`
  `Shape_frame* hires = nullptr; uint8 hires_scale = 0; uint8 hires_state = 0 /*unknown|none|present*/;`.
  This makes **every** path that ends up with a `Shape_frame*` hi-res-capable, including the about 100 raw-frame call sites in §4.3.
  `Shape_manager::paint_shape` gets the per-target scale (§7.6) and picks `shape->hires` when `hires_scale == S`.
  The `Cached_shape` map does not need to change.
* Ownership: the hi-res frames live in a **separate store** (§7.2), not inside the low-res `Shape`. That keeps `Shape::write`,
  ES and the save paths untouched, and allows dropping or reloading hi-res independently. Invariant: whenever a low-res
  `Vga_file` is reset or reloaded, its hi-res companion is reset too (and vice versa: clear the `hires` pointers), because the
  frames hold raw pointers.
* The alternative registry-only design (`map<(file,shape,frame), HiRes>` looked up at paint time) needs identity at paint time.
  The raw-frame callers do not have it, and it adds a map lookup per paint. It is only useful as the *index* behind the store.

### 7.2 Where the hi-res data comes from — two formats, one store
**(A) Companion VGA (recommended runtime/distribution format):** `<…>/hires/x6/shapes.vga`, `gumps.vga`, `faces.vga`,
`sprites.vga`, `paperdol.vga`, `exult.flx`… These are ordinary Exult Flex VGA files that contain S x RLE frames, sparse
(empty entries = no override). They load with an unmodified `Vga_file`:
* lazy per-frame reads, compact RLE, no libpng at runtime (**Android has no PNG**), and multi-source layering for free
  (`Vga_file::load(vector<sources>)`, back-to-front per shape: base pack, then the mod's own hi-res patch);
* automatic hi-res reflection: the companion `Shape::read` transposes its own S x frame (`vgafile.cc:915-918`);
* buildable with `ipack -c` from PNGs (no `flat` flag; offsets per ES convention scaled by S), once the `runs[200]` bug is fixed.

**(B) Loose PNG directory (development/AI-pipeline format, `#ifdef HAVE_PNG_H`):** per-frame granularity, easy to regenerate.
Suggested layout and naming (matching ipack `SSSS_NN`):
```
<hires-root>/x6/shapes/0012_03.png      # shape 12, frame 3
<hires-root>/x6/shapes/0857_33.png      # explicit reflected frame (optional; otherwise auto-transposed)
<hires-root>/x6/gumps/0034_00.png
<hires-root>/x6/faces/0291_01.png
<hires-root>/x6/fonts/<fontsource>/<font#>/0065.png   (later)
```
Format: 8-bit palette PNG with the **game palette in ipack/ES convention** (index 0 = transparent after rotation, or
index 255 transparent with no tRNS on other entries), size exactly `S*w x S*h`. `oFFs` is optional: if present it must equal
`(-(xright*S+S-1), -(ybelow*S+S-1))`. Load with `Import_png8(get_system_path(path).c_str(), 255, ...)`, then verify the palette
(byte-compare with palette 0). If the palette differs, remap *non-special* indices by nearest colour and refuse or flag
pixels that would map into 0xE0–0xFE unless the source pixel at the same low-res position was already in that range.
Port the logic of ES `Convert_indexed_image`. After loading, `Shape_frame(pixels, wS, hS, xleft*S, yabove*S, true)` RLE-encodes it.
Directory listing via `U7ListFiles` once at startup gives a cheap existence set, so there is no `fopen` per miss.

Both feed one `Hires_store` (per ShapeFile + per scale): `get(shape, frame)` -> `Shape_frame*` or nullptr. Precedence:
loose PNG > companion VGA.

### 7.3 Locating the files (path tags)
Because `<PATCH>` is replaced by a mod's patch dir (§5.1), use a **dedicated tag chain**, searched in this order:
1. `<PATCH>/hires/x<S>/…`, the active patch dir (mod-specific or game-specific art).
2. `<PREFIX_HIRES>`: a new per-game config key `config/disk/game/<cfgname>/hires_path` (default `$game_path/hires`),
   registered next to `_PATCH` in `ModManager::ModManager` (`modmgr.cc:607-612`). Here the big base-game pack lives, and it stays
   visible while a mod is active.
3. Optionally `<DATA>/hires/<game>/x<S>` for packs shipped with the build.

Mod-aware safety: when a mod (or the user patch) replaced a shape (`Shape::get_from_patch()`, `vgafile.h:211`) or the
low-res frame differs from what the hi-res pack was made from, base-pack art would be wrong. **Store a CRC32 of the source
low-res frame data** in the pack (a sidecar index or a Flex entry), compare it at attach time, and fall back to upscaling on a
mismatch. `files/crc.cc` only exposes a file-based `crc32()`, so a buffer overload is needed (or zlib's `crc32`).

### 7.4 Attaching lazily — the hook
* Make `Vga_file::get_shape` (`vgafile.h:357-377`) the single choke point. After it obtains `r`, if
  `hires_store && r && r->hires_state == unknown`: compute the **lookup key**:
  * flats (`!r->is_rle()`): `frame & 31` (mirrors `vgafile.cc:912-914`);
  * RLE: `frame` as requested; if `frame < get_num_frames(shape)` the companion must have that *explicit* frame (no
    auto-reflect). Otherwise this is a low-res reflection, so request `frame` and let the companion transpose it
    (`frame&31` explicit -> reflect), or transpose the attached hi-res of `frame&31` directly.
  
  Then set `r->hires`/`hires_state`.
* `Vga_file` needs an identity for this (it has none). Add `void set_hires(Hires_store*)` that `Shape_manager::load()`
  calls for each `files[SF_*]` and for `shapes`. Fonts (`Font::load_internal`, `font.cc:775-795`), `pointers.shp` (`mouse.cc:108-127`)
  and `Game::menushapes` (`game.cc:89`) are separate hook points for later phases.
* Imported shapes: key by the **logical** shape number (what the player and modder see), which is what `get_shape` receives.
* Reflection inside a hi-res `Shape_frame` is just `Shape_frame::reflect()` on the hi-res frame. It works for any size
  (transpose), with a `max(w,h)^2` temporary buffer. That is about 3 MiB at 1800 px, which is fine.

### 7.5 Fallback for frames without an override
* Nearest-neighbour: either (a) a new `Image_buffer8::paint_rle_scaled(xoff, yoff, data, S, …)` family (opaque, remap,
  translucent, transformed, outline), which has zero memory cost and is the most code; or (b) lazily build an upscaled RLE
  `Shape_frame` (`decode -> Scale_point 8->8 -> encode_rle`) and attach it as `hires` with `hires_state=generated`, which
  reuses all painters at the cost of memory (S^2 x raw, RLE-compressed). (b) is simpler and gives a uniform paint path.
  It needs an LRU cap, because hi-res memory for all of shapes.vga at 6x is on the order of hundreds of MiB.
* "Pre-scaled with an existing scaler" is only meaningful if the output is re-quantised to palette indices while preserving
  the special ranges (§4.5). Treat it as an offline tool feature, not a runtime one.

### 7.6 Passing the render scale
* Put `int render_scale = 1` on `Image_buffer` (or `Image_buffer8`) and set it to S only for the game-view buffer (and for
  `rendered_flats_hi`). `Shape_frame::paint*` and `Shape_manager::paint_*` then read `scrwin->get_render_scale()`. Callers keep
  passing game pixels, and the painter multiplies. This preserves all 100+ call sites and the `push_render_target` mechanism
  (§4.1). UI layers stay 1x until gumps, fonts and faces get their own phase.
* Clip rects and `copy8`/`fill8` on an S x buffer are a rendering-layer decision (see the ibuf/world analyses). The shape layer
  only requires that "draw origin `(x,y)` in game px" maps to "buffer px `(x*S, y*S)`".

### 7.7 Memory / performance notes
* Terrain flats: 150 shapes; a full x6 set is only a few MiB (48x48 = 2.3 KiB per frame). They can be preloaded at startup
  to avoid hitches.
* Large sprites at 6x: decode + RLE-encode the PNG on first paint can take milliseconds per frame. A companion VGA avoids the
  encode cost, so prefer it at runtime.
* `Shape_frame` size grows by about 16 bytes (pointer + 2 bytes, padded). That is negligible.

### 7.8 Testability
There is no unit-test framework, but `files/Makefile.am` builds `noinst_PROGRAMS = rwregress`, which is a precedent for a
small regression program. A `shapes/hiresregress` linking `libshapes.la` + `libu7file.la` can test without game data:
RLE encode/decode round-trip at large widths (`runs[200]`), the geometry mapping, reflection (transpose) of hi-res vs low-res,
flat `&31` keying, sparse companion-VGA fallback, PNG palette/tRNS/oFFs validation, and CRC mismatch fallback.

---

## 8. Touchpoints (summary)

| Where | What | Change |
|---|---|---|
| `shapes/vgafile.h:47-163` `Shape_frame` | frame container | add `hires` ptr + scale + state; scale-aware paint wrappers using `scrwin` scale |
| `shapes/vgafile.h:357-377` `Vga_file::get_shape` | lazy load choke point | attach hi-res (key normalisation flats `&31`, reflect rule); add `set_hires()` identity |
| `shapes/vgafile.cc:281-347` `encode_rle` | RLE encoder | fix `runs[200]` overflow before encoding hi-res |
| `shapes/vgafile.cc:802-824,855-920` `Shape::reflect/read` | reflection / flats | companion-VGA reflection semantics; flat `&31` |
| `shapes/vgafile.cc:525-534` `Shape_frame::paint` | flat 8x8 copy | hi-res/scaled path |
| `shapes/vgafile.cc:640-699` `paint_rle_outline` | outline | S-thick outline at S x |
| `imagewin/ibuf8.cc:516+,672+` | RLE painters | optional `paint_rle_scaled*` if not caching upscaled frames |
| `shapeid.h:171-196` `Shape_manager::paint_*` | dispatch | choose hi-res/upscaled by target scale |
| `shapeid.cc:150-382` `Shape_manager::load` | file loading | create/load Hires_store per SF file; hook shapes, gumps, faces, sprites, paperdol, exult.flx |
| `shapeid.cc:420-468` `reload_shapes` | reload | reset hi-res with low-res; fix U7_SHAPE vs SF index mismatch |
| `objs/chunkter.cc:86-131,248-267` | terrain prerender | S x `rendered_flats`, use hi-res flats |
| `gamerend.cc:520-531` | flats blit | `c_chunksize*S`, offsets x S |
| `shapes/font.cc:260-322,775-795` | fonts | later phase: font hi-res key + scale |
| `mouse.cc:108-127,217-231`, `game.cc:89` | pointers, menu shapes | later phase |
| `shapes/pngio.cc:45-171` | PNG import | engine-side wrapper: system path, palette check/remap, no index shift surprises, HAVE_PNG_H guard |
| `gamemgr/modmgr.cc:56-85,592-617` | path tags | add `<PREFIX_HIRES>` (+ optional mod `hires` key); hi-res search chain |
| `files/crc.{h,cc}` | CRC | buffer overload for frame CRC validation |
| `gamewin.cc:330,540-550,921` | render target | propagate per-buffer render scale |

## 9. Open questions
* Is the base pack keyed by CRC of the low-res frame (robust against mods) or by `(file, shape, frame)` only?
* Is a companion-VGA build step (ipack) acceptable for distribution, with loose PNGs only in dev builds?
* Should non-overridden frames be upscaled on the fly or cached (memory budget)?
* For hi-res art that legitimately wants to exceed the low-res bbox (antialiased edges, shadows): allow up to the dirty margin, or never?
* What hi-res font key should be used, given that the font source depends on the `config/gameplay/fonts` setting (`shapeid.cc:283-304`)?
