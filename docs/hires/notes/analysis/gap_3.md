# Gap 3: Resident memory budget of hi-res override frames

Repo: `/home/simonea/ultima7_exult/exult-hires` (master `8b6ab6b43`). Data: `/mnt/e/Games/RolePlayingGames/{ultima7,Serpent}`.
All sizes are in MB = 10^6 bytes. S = render scale; numbers are for S = 6 unless stated (memory scales with S²).
Measurement tools and raw results: `docs-hires/analysis/gap_3_tools/` (`memstat.py`, `memstat.json`, `hrbench.cc`, see Appendix).

## 0. Summary

* **Today frames are never evicted, and nothing needs it.** Every frame of BG `shapes.vga` together costs 6.9 MB (SI: 8.4 MB) in its
  original RLE form. Frames are freed only by `Shape::reset`/`Vga_file::reset` on load, reload or `new_shape`. At S=6 the same
  "load once, keep forever" policy grows about 32x: **BG shapes.vga is about 190-220 MB, SI about 230-270 MB.** All game VGA files together are about **290 MB (BG)
  and 405 MB (SI)**, before reflections, the 59 MB terrain cache and the surfaces.
* **The working set is small.** On the real world map (terrain, ifix and ireg objects), the distinct frames needed for a 10x10-chunk
  neighbourhood are **p50 7 MB, p95 15 MB, max 21 MB (BG)** and p50 5.5, p95 12.5, max 18 MB (SI). Add actors (0.2-0.7 MB per
  distinct actor shape, 2.2 MB max) and UI (gumps, faces, paperdolls: a few MB each scene). **A 64 MB LRU covers the worst measured
  case at S=6.** Without eviction, a long session drifts towards 86-126 MB for the map, plus up to 93-115 MB for actors, plus the UI files.
* **Correction to the gap statement:** the largest `shapes.vga` frame is **72x72** (432x432 at S=6, about 0.19 MB). No world object is
  300x200, because U7 buildings are assembled from many small shapes. The frames that cost 1-2.3 MB at S=6 are UI and effect art:
  gumps (BG/SI gump 32/27, 307x148), full-screen sprites (320x200) and SI full-screen faces (320x200).
* **The "noisy art barely compresses" assumption is right for RLE but not for PNG.** In a simulation of AI-like art re-quantised to
  the U7 palette, Exult RLE costs **0.93-1.08 bytes per opaque pixel**, so it is effectively raw. Indexed PNG costs **0.44-0.75 B/px**.
  A cached nearest-neighbour upscale RLE costs only 0.13-0.26 B/px.
* **Hitches.** Single-thread on a Ryzen 7 9700X: PNG decode runs at 140-240 Mpx/s and `encode_rle` at 200-380 MB/s. The worst single frame
  (1920x1200) costs about **10 ms** (decode plus encode), and a 432x432 world frame about **1 ms**. A cold scene (teleport or game load) costs
  **20-30 ms at p50 and 100-160 ms at max**. With a pre-encoded companion VGA, the decode and encode cost disappears and only a
  file read remains. ARM/Android should be assumed 2-4x slower.
* **Hard constraints for the design:**
  1. `Shape_frame*` pointers are cached raw. That includes `Shape_manager::shape_cache`, menus, the mouse and every paint call, so
     the `Shape_frame` objects must stay alive. Only a separate hi-res payload can be evicted.
  2. Frame 0 of every shape is forced resident at startup. If hi-res attachment happens inside `Vga_file::get_shape`, that is
     about 25 MB and about 0.2-0.3 s at startup.
  3. A hi-res source opened as an Exult FLX resource is read **entirely** into RAM.
  4. `encode_rle`'s `runs[200]` overflows on real hi-res data. The simulation hit 34 BG gump frames and 35 SI sprite frames, with up to 561 runs per scanline.

---

## 1. How frame residency works today

### 1.1 Ownership and lazy load
* `Shape_frame` owns its pixels as `std::unique_ptr<unsigned char[]> data` plus `datalen`, extents and an `rle` flag
  (`shapes/vgafile.h:47-55`). It has a virtual destructor (`vgafile.h:158`), so the object costs about 32 B plus data.
* `Shape` owns `std::vector<std::unique_ptr<Shape_frame>> frames` (`vgafile.h:170`). `Shape::get()` returns the cached frame or
  calls `read()` (`vgafile.h:224-228`).
* `Vga_file` owns `std::vector<Shape> shapes` and keeps every `IDataSource` open for its lifetime: `shape_sources`
  and `imported_sources` (`vgafile.h:306-313`). `Vga_file::get_shape()` (`vgafile.h:357-377`) is the only lazy-load path.
* A read allocates exactly one heap block per frame: `Shape::read` (`vgafile.cc:855-920`) calls `Shape_frame::read`
  (`vgafile.cc:398-450`), which calls `get_rle_shape` (`vgafile.cc:456-477`, `make_unique` + `read` at 473-474). For flats it uses `readN(64)` (`vgafile.cc:446-448`).
  The frame is stored forever by `store_frame` (`vgafile.cc:973-989`).
* **Nothing evicts.** Frames are freed only by:
  * `Shape::reset()` (`vgafile.cc:1008-1011`), which is called from `Shape::load` (`vgafile.cc:1017-1032`) and `Vga_file::new_shape` (`vgafile.cc:1292-1306`).
  * `Vga_file::reset()` (`vgafile.cc:1270-1274`), which is called from every `Vga_file::load` (`vgafile.cc:1185-1187`).
  * `Vga_file::reset_imports()` (`vgafile.cc:1276-1281`).
  * Callers: `Shape_manager::load()` (`shapeid.cc:150-382`), `load_gumps_minimal()` (`shapeid.cc:385-414`) and the ExultStudio
    `reload_shapes()` (`shapeid.cc:420-468`).

### 1.2 Where frames are forced resident (not on paint)
* **Startup frame-0 sweep.** `Shapes_vga_file::read_info` creates an `info` entry for *every* shape index, because it reads `tfa.dat` for
  `i < shapes.size()` (`shapes/shapevga.cc:775-778`). Then `Shape_manager::read_shape_info()` calls `shapes.get_num_frames(i)` for
  every shape that has info (`shapeid.cc:123-131`). `get_num_frames` does `get_shape(shapenum, 0)` "to force it into memory"
  (`vgafile.h:400-408`). Result: **frame 0 of all 1024 (BG) or 1036 (SI) shapes is loaded at startup.** Animation info does
  the same for animated shapes (`shapevga.cc:795-800`).
* **Eager whole-shape loads:** `Vga_file::extract_shape` (`vgafile.h:379-397`, used by menus and credits: `exultmenu.cc:590,601`,
  `gamemgr/bggame.cc:1957,1965`, `gamemgr/sigame.cc:1364,1372`). Also `Shape_file` / `Shape::load` (`vgafile.cc:1017-1032`), used for
  fonts (`shapes/font.cc:775-795`, `font_shapes = std::make_unique<Shape_file>(&data)`) and `pointers.shp` (`mouse.h:36`). Non-flex
  `Vga_file`s preload their single shape (`vgafile.cc:1213-1219`).
* **Metadata-only users.** There are 117 `get_shape()` call sites in the engine (excluding studio and tools), and 16 of them read only extents
  (`get_xleft`, `get_width`, …). Hit-testing calls `Shape_frame::has_point` on the low-res frame (`gamewin.cc:2108-2112`,
  `vgafile.cc:706-742`).

**Consequence:** if hi-res payloads are attached inside `get_shape`/`Shape::read`, startup pays for all frame-0 overrides. That is
24.9 MB at S=6 for BG shapes.vga (23.1 MB SI) and about 0.2-0.3 s of PNG decode plus RLE encode on a fast desktop CPU. Metadata queries would also pull
hi-res data. **Attach hi-res only on the paint path** (see §5.2).

### 1.3 Raw `Shape_frame*` holders (pointer stability)
* `Shape_manager::shape_cache` is a `std::map<pair<shape,frame>, Cached_shape{Shape_frame*, has_trans}>` per file
  (`shapeid.h:77-80,100-101`). It is filled by `cache_shape` (`shapeid.cc:552-576`) and cleared only in `load()` (`shapeid.cc:151-154`),
  `load_gumps_minimal()` (`shapeid.cc:387-388`) and `reload_shapes()` (`shapeid.cc:423`). Every `ShapeID::paint_shape` goes through
  it (`shapeid.h:348-356`).
* Long-lived raw pointers: `Mouse::cur` (`mouse.h:44`), `MenuTextEntry`/`MenuGameEntry` (`menulist.h:95,140`), and
  `menulist.cc:65`.
* Paint routines hold `Shape_frame*` on the stack across nested calls, for example `Shape::reflect` calls `get()` for the
  base frame and then allocates the reflection (`vgafile.cc:802-824`).

**Consequence:** an eviction policy must **never destroy `Shape_frame` objects**. Only a hi-res payload that hangs off the frame (or sits in
a side table keyed by frame identity) may be freed, and only at a safe point (§5.3).

### 1.4 Reflections are extra resident frames
`Shape::read` turns a request for `frame|32` beyond `nframes` into `Shape::reflect` (`vgafile.cc:915-918`). That stores a new
transposed frame at `frames[framenum|32]` (`vgafile.cc:817-823`), after building a `max(w,h)²` temporary
`Image_buffer8` (`vgafile.cc:91`). Measured on the data:
* Static map objects **never** use reflected frames. In 37,542 BG and 71,986 SI ifix records, no frame number is ≥ 32. No RLE
  shape has more than 32 frames, so every frame ≥ 32 is a reflection.
* Reflections come from **actors**: west and east frames are 32-47 and 48-63 (`actors.cc:136,143`). Barges use
  `Shape_info::get_rotated_frame` (`shapes/shapeinf.cc:521-544`), which XORs bit 5.
* So at S=6 a reflected actor frame doubles that frame's cost. A hi-res reflection must be a transposed *hi-res* frame, so it is
  just as large as the base.

### 1.5 Data sources: streaming vs whole-buffer
`Vga_file::U7load` (`vgafile.cc:1163-1183`) opens a plain path with `IFileDataSource`, which streams and seeks (`vgafile.cc:1168`). It opens a
`(file, index)` resource with **`IExultDataSource`, which reads the whole object into a heap buffer** (`vgafile.cc:1171-1172`,
`files/databuf.h:381-400`). A hi-res pack shipped as an *entry inside an Exult FLX* would therefore be **fully resident**
(hundreds of MB) as soon as it is opened. The pack must be a standalone file opened via a path. The codebase has no mmap
support (no `mmap`/`MapViewOfFile` in the engine).

### 1.6 Other S-dependent resident memory (for the total budget)
* **Terrain cache:** `Chunk_terrain::render_flats` keeps an LRU of up to 100 `Image_buffer8(c_chunksize, c_chunksize)`. The
  constant is in `Figure_queue_size()` (`objs/chunkter.cc:234-242`), eviction at `chunkter.cc:250-258`, allocation at 259, and queue insertion before
  render at `chunkter.h:95-101` + `chunkter.cc:41-59`. At S=6 each chunk is 768x768 = 589,824 B, so **59 MB**. That is 2.5-4x the
  measured hi-res frame working set. A 320x200 view touches at most 4x3 chunks.
* Game-view surface at 1920x1200: 2.3 MB INDEX8, plus a 9.2 MB ARGB texture or upload (see `present.md`).
* `ImageBufferPaintable` copies the whole window buffer (`shapeid.cc:643-648`), which is 2.3 MB per instance at S=6.
* The low-res frames themselves (all files fully loaded) are about 8-11 MB. They are tiny and must stay (§5.1).

---

## 2. Measured data

### 2.1 Method
* `memstat.py` parses every Flex VGA (RLE scanlines decoded exactly as `Image_buffer8::paint_rle` reads them). It counts opaque pixels,
  bounding boxes, horizontal and vertical spans, exact NN-upscaled RLE cost, frame-0 floor and reflection cost. It also builds the world
  (`u7map` → `u7chunks` templates, `u7ifixNN`, `gamedat/u7iregNN` top-level objects) and slides chunk windows over the 192x192
  chunk world. Cost models:
  * *noisy RLE (model):* `opaque·S² + spans·S·6` (raw pixels plus one 6-byte header per hi-res row span), with flats at 64·S².
  * *bbox8:* `w·h·S²`, an uncompressed indexed bitmap.
  * *RGBA:* bbox8 × 4.
* `hrbench.cc` simulates "AI art re-quantised to the palette": bilinear RGB upscale, Gaussian noise (σ = 8, 16 or 24 in 8-bit units),
  then nearest colour among indices 0x00-0xDF, keeping indices ≥ 0xE0 (cycling and translucent) by NN. It then runs a **verbatim copy of
  Exult's `encode_rle`** (with an unbounded `runs` buffer), a copy of the `paint_rle` inner loop, and libpng 1.6.37 indexed PNG
  write and read. Single thread, `-O2`, Ryzen 7 9700X. **Caveat:** real diffusion or ESRGAN output may have more high-frequency detail
  than σ=24. Treat RLE ≈ 1.1 B/px and PNG ≈ 0.9 B/px as pessimistic bounds.

### 2.2 Per-file totals at S=6 (all frames resident, no reflections)

| Game / file | RLE frames | flats | orig. resident | opaque px (S=1) | noisy RLE model | simulated RLE (σ) | NN-RLE | bbox8 | RGBA |
|---|---|---|---|---|---|---|---|---|---|
| BG shapes.vga | 10,286 | 3,885 | 6.90 MB | 5.58 M | **221.7** | 187.7 (8) / 217.2 (24) + 9.0 flats | 61.5 | 323.6 | 1,294 |
| BG faces.vga | 296 | – | 0.63 | 0.60 M | 22.4 | 23.0 (16) | 5.3 | 24.6 | 98 |
| BG gumps.vga | 175 | – | 0.27 | 0.57 M | 20.6 | 19.5 (8) | 2.2 | 24.8 | 99 |
| BG sprites.vga | 230 | – | 0.28 | 0.33 M | 12.5 | – | 2.0 | 33.7 | 135 |
| BG fonts.vga | 1,012 | – | 0.10 | 0.04 M | 1.7 | – | 0.7 | 3.2 | 13 |
| SI shapes.vga | 13,081 | 4,690 | 8.40 | 6.68 M | **266.2** | – | 75.1 | 396.5 | 1,586 |
| SI faces.vga | 184 | – | 0.77 | 1.06 M | 38.8 | 30.7 (16) | 6.6 | 45.6 | 182 |
| SI gumps.vga | 267 | – | 0.36 | 0.74 M | 27.0 | – | 2.8 | 32.6 | 130 |
| SI sprites.vga | 520 | – | 1.07 | 1.71 M | 63.3 | 55.1 (16) | 8.1 | 148.7 | 595 |
| SI paperdol.vga (also used by BG) | 458 | – | 0.28 | 0.19 M | 7.4 | – | 2.3 | 19.3 | 77 |
| SI fonts.vga | 1,317 | – | 0.13 | 0.04 M | 2.1 | – | 0.9 | 4.1 | 16 |

Exult's own `data/exult.flx` and `exult_bg.flx` shapes add about 6 + 4 MB (partial parse; the largest is 354x204).
**All files resident: BG ≈ 286 MB, SI ≈ 405 MB** in RLE, or about 1.2-1.6 GB if stored as RGBA. **RGBA-in-RAM is not viable.**

Empirical compression ratios (hrbench, per opaque pixel at S=6):

| encoding | shapes.vga σ=8 | shapes.vga σ=24 | gumps σ=8 | SI faces σ=16 | SI sprites σ=16 |
|---|---|---|---|---|---|
| Exult RLE of noisy art | 0.934 | 1.080 | 0.957 | 0.800 | 0.896 |
| indexed PNG (zlib default) | 0.442 | 0.702 | 0.473 | 0.522 | 0.434 |
| Exult RLE of NN upscale | 0.261 | 0.261 | 0.106 | 0.172 | 0.131 |

### 2.3 Frame size distribution: there are no "huge world frames"
* `shapes.vga` (both games): **max frame 72x72** → 432x432 at S=6 (0.12-0.19 MB). Bucketed by noisy-RLE size: 9,622 frames < 64 KB and
  664 frames between 64 and 256 KB (BG); 12,449 / 632 (SI). 2,398 BG frames become wider than 200 px at S=6.
* Large frames are UI and effects only:

| frame | S=1 | S=6 | model MB |
|---|---|---|---|
| BG gump 32 / SI gump 27 | 307x148 | 1842x888 | 1.5 |
| BG gump 3 | 244x160 | 1464x960 | 1.3 |
| BG sprites 10 (and 22, 199x200) | 320x200 | 1920x1200 | 1.3-1.4 |
| SI faces 256, 293-296 (full-screen portraits) | 320x200 | 1920x1200 | 2.3 (sim. 0.44-0.92) |
| SI sprites 17, 40-43 (full-screen effects) | 320x200 | 1920x1200 | 0.09-0.13 (sparse) |
| BG face 277 | 119x142 | 714x852 | 0.43 |
| SI paperdol 123 | 133x144 | 798x864 | 0.58 |

### 2.4 Working sets on the real map (S=6, noisy-RLE model; terrain + ifix + ireg, NPCs excluded)
Windows slide in chunk steps over the 192x192-chunk world. 5x4 chunks covers a 320x200 view (at most 4x3 chunks) plus a margin.
10x10 is about the terrain-cache extent. 16x16 is one superchunk, which is the unit `read_map_data` loads (`gamemap.cc:304-336`).

| window | BG p50 / p95 / max | BG distinct frames p50 / max | SI p50 / p95 / max | SI frames p50 / max |
|---|---|---|---|---|
| 5x4 chunks | 2.5 / 7.4 / 13.0 MB | 401 / 1,177 | 1.6 / 5.3 / 9.6 MB | 193 / 1,029 |
| 10x10 chunks | 7.0 / 15.4 / 21.2 MB | 920 / 1,891 | 5.5 / 12.5 / 17.9 MB | 506 / 1,595 |
| 16x16 chunks | 13.7 / 22.7 / 27.0 MB | 1,341 / 2,402 | 11.1 / 20.3 / 28.7 MB | 917 / 2,376 |
| whole map (never evict) | 86.3 MB, 5,997 frames | | 126.4 MB, 8,242 frames | |

**Actors** (tfa shape class 12/13, `shapes/shapeinf.h:892-911`; BG 124 shapes, SI 156): all 32 frames plus 32 reflections per shape cost
median **0.71 / 0.76 MB**, max **2.19 MB** (BG shape 230) / 2.03 MB (SI shape 832). All actor shapes together cost 93 / 115 MB. A walking NPC
touches roughly 3 frames × 4 directions, and combat touches more. 10-20 distinct actor shapes on screen therefore cost about 3-15 MB.

**UI:** an open backpack or paperdoll gump costs 0.3-1.5 MB, a face 0.05-0.4 MB (SI full-screen 0.5-2.3 MB), and fonts 2 MB total.

**Estimated peak working set at S=6:** about 21 MB map + 15 MB actors + 10 MB UI + 5 MB sprites ≈ **50 MB**. Typical play needs
10-25 MB.

### 2.5 Startup floor and reflection worst case
* Frame-0 sweep (§1.2) if hi-res is attached in `get_shape`: **24.9 MB (BG) / 23.1 MB (SI)** for shapes.vga.
* Reflections if every reflectable frame were mirrored: +212.7 MB (BG) / +255.1 MB (SI). This is theoretical, because only actors and barges mirror.

### 2.6 Time costs (desktop, single thread)

| operation | throughput | biggest frame (1920x1200) | 432x432 world frame |
|---|---|---|---|
| libpng indexed decode | 141-237 Mpx/s (opaque) | 5.2-6.7 ms | ~0.8 ms |
| `encode_rle` (vgafile.cc:281-347) | 197-381 MB/s output | 3.4-5.0 ms | 0.3-0.4 ms |
| `paint_rle` copy loop (ibuf8.cc:516+) | 530-1,060 MB/s | 0.7-1.5 ms | 0.1 ms |
| PNG encode (offline tool only) | ~10-20 MB/s | | |

Cold scene (teleport or load), PNG source: decode plus encode of the 5x4 window takes **~20-30 ms at p50, ~55-90 ms at p95 and ~95-160 ms
at max**. Opening the largest gump or SI portrait adds a ~10 ms hitch. With a **pre-encoded companion VGA** (S× RLE stored as is) the
same scene is a file read of 2.5-13 MB: a few ms from the page cache and no CPU transform. ARM phones are 2-4x slower.

---

## 3. What breaks or degrades if nothing changes
1. **Unbounded growth.** Memory rises monotonically with exploration: about 86-126 MB of map frames, plus actors, UI and reflections, so
   up to 300-400 MB. There is no budget, LRU or config to cap it.
2. **32-bit builds.** Android's default ABIs include `armeabi-v7a` and `x86` (`configure.ac:399-405,1425`). Windows snapshots build
   `mingw32 i686` (`.github/workflows/snapshots-windows.yml:49`), and no `--large-address-aware` flag was found, so assume 2 GB of address space. 300-400 MB of
   small heap blocks next to SDL, audio and the 59 MB terrain cache is risky for address-space fragmentation. Android's low-memory killer
   targets apps with large anonymous RSS.
3. **Startup cost** if hi-res is attached in the lazy-load choke point (§1.2).
4. **Encoder overflow.** `unsigned short runs[200]` (`vgafile.cc:295`) overflows on hi-res scanlines. The simulation measured up to 561 runs
   (BG gumps), 523 (SI sprites) and 287 (SI faces). shapes.vga peaked at 169 with this noise model, but its 432 px rows can reach about 290 runs with
   denser art. The same encoder is used by `Shape_frame::reflect` → `create_rle` (`vgafile.cc:124,239-244`).
5. **Whole-file buffering** if the pack is an FLX entry (§1.5).
6. Latent bug: a frame whose data is only its 8-byte header writes `data[0]` into a null `unique_ptr` (`vgafile.cc:467-471`). A pack writer must
   never emit empty frames, or the reader must fix this.

---

## 4. Budget arithmetic per S (BG; SI ≈ +20 %)

| S | game view | terrain cache (100 chunks) | all shapes.vga RLE (noisy) | 10x10 p95 working set | suggested LRU default |
|---|---|---|---|---|---|
| 1 | 320x200 | 1.6 MB | 6.9 MB | 0.4 MB | n/a |
| 2 | 640x400 | 6.6 MB | ~25 MB | 1.7 MB | unlimited |
| 3 | 960x600 | 14.7 MB | ~55 MB | 3.8 MB | unlimited / 64 MB |
| 4 | 1280x800 | 26.2 MB | ~99 MB | 6.8 MB | 64-128 MB |
| 6 | 1920x1200 | 59.0 MB | ~222 MB | 15.4 MB | 128-256 MB desktop, 64 MB 32-bit/mobile |

---

## 5. Recommendations for the design

### 5.1 Two-tier frames: low-res stays the truth, hi-res is an evictable payload
* Keep every low-res `Shape_frame` loaded exactly as today. Together they are under 11 MB, and they serve hit-testing (`has_point`), extents, `is_empty`,
  the frame-0 sweep, ExultStudio and the NN fallback.
* Add a payload slot to `Shape_frame` (`vgafile.h:47-163`), e.g. `Hires_entry* hires; uint8 hires_state` with
  {unknown, none, loaded, evicted}, or keep it in a side table keyed by `(file id, shape, frame)`. The payload is an S× RLE stream
  (same format, so every painter works unchanged) plus its scale. The `Shape_frame` object itself is never freed by the cache
  (§1.3).

### 5.2 Attach on the paint path only
* Resolve hi-res in `Shape_manager::paint_shape` / `paint_invisible` / `paint_outline` (`shapeid.h:171-196`) and in terrain
  `paint_tile` (`objs/chunkter.cc:86-133`). Do it only when the render target has scale > 1. Do **not** resolve it in `Vga_file::get_shape`
  (`vgafile.h:357-377`), `get_num_frames` (`vgafile.h:400-408`) or `cache_shape` (`shapeid.cc:552-576`). This avoids the 25 MB
  startup sweep and keeps metadata queries cheap.
* The `none` state (no override exists) is cached per frame, so the miss costs one byte compare.

### 5.3 One byte-budgeted LRU for all files
* Use a global `Hires_cache` with an intrusive LRU list (prev/next in the entry), `bytes_used` and `budget`. Add a config key, e.g.
  `config/video/hires/cache_mb`. Suggested defaults are 256 MB on 64-bit desktop and 64 MB on 32-bit builds and Android. The floor is about 48 MB at S=6,
  below which scrolling thrashes.
* **Touch** on every paint, stamped with the current frame number. `Game_window::show()` already counts `blits` (`gamewin.h:747-755`).
* **Evict only at a frame boundary.** Run eviction at the end of `Game_window::show()` (or after `paint_dirty`, `gamerend.cc:624-632`), skipping
  entries touched in the current frame. Pointers taken during the current paint then stay valid. Insertions may overshoot the
  budget temporarily, so add a hard cap to protect 32-bit builds.
* **Invalidate** together with the low-res frames, in `Vga_file::reset()` / `reset_imports()` / `new_shape()` (`vgafile.cc:1270-1306`) and
  `Shape_manager::load` / `load_gumps_minimal` / `reload_shapes` (`shapeid.cc:150-154,385-388,420-423`). Bump a per-`Vga_file`
  generation number so stale side-table entries die.
* Entries are whole frames: at most 2.3 MB, and every shapes.vga frame is under 256 KB. Fragmentation is acceptable. An arena or slab per size class is
  optional.

### 5.4 Terrain flats
* All hi-res flats are cheap: 3,885 × 2,304 B = **9 MB (BG)** and 4,690 × 2,304 = **10.8 MB (SI)**. They are read only by `render_flats`.
  Either keep them resident outside the LRU, or put them through the LRU with a priority.
* The **terrain cache is the largest fixed cost** (59 MB). Make `Figure_queue_size()` (`chunkter.cc:234-242`) depend on the view and S, or
  on a byte budget. The commented `(cw+3)*(ch+3)` formula gives 6x5 = 30 chunks at 320x200, which is **17.7 MB**. Alternatively, draw flats per tile from
  the hi-res flat set (see `world.md`).

### 5.5 Fallback for frames without an override
* Preferred: scale at paint time (`paint_rle_scaled*`). This costs **0 bytes**.
* If the fallback is cached as an NN-upscaled RLE, it costs only 0.13-0.26 B/px (52 MB for all BG shapes). It must go through the same LRU and be
  regenerable (state `generated`).

### 5.6 Reflections
* Hi-res reflections should be **derived and evictable**: transpose the base hi-res frame, with the `max(w,h)²` temp, about 0.2 MB for world frames.
  Do not store them as permanent extra frames. Only actors and barges need them (§1.4).
* Alternatively, ship explicit mirrored art for asymmetric actor art. It costs the same memory but goes through the same LRU.

### 5.7 Pack format implications
| option | on-disk | RAM after load | load CPU | notes |
|---|---|---|---|---|
| **Sparse companion VGA, S× RLE** (Flex, standalone file) | ~0.8-1.1 B/px | = file bytes per frame | read only | fits `Vga_file`/`Shape_frame::read` almost unchanged. Must be opened as a **path** (`IFileDataSource`), never as an FLX resource (§1.5). Best runtime choice. |
| PNG per frame (loose, or Flex of PNG blobs) | 0.44-0.75 (≤0.9) B/px | 0.8-1.1 B/px after RLE encode | decode 140-240 Mpx/s + encode 200-380 MB/s | good dev and authoring format; hitches; needs an in-memory PNG reader for blobs (`build.md`) |
| PNG/zstd blobs kept in RAM + small decoded LRU | 0.44-0.75 B/px resident | + LRU | decode on every re-entry | saves only 30-55 % against RLE; not worth the CPU at runtime |
| mmap'd companion VGA (RLE painted in place) | as companion | OS page cache (reclaimable) | none | best for Android RSS, but needs new mmap plumbing (none exists); 32-bit VA of 220-270 MB per game |

Recommendation: use PNG as the **authoring** format, and have a build step (`ipack`-like, see `build.md`) produce a **sparse companion VGA at S×**.
Ship that. Keep loose-PNG loading for development, with the same LRU.

### 5.8 Hitch mitigation
* Prefetch the distinct frames of a superchunk when it is read (`Game_map::get_superchunk_objects`, `gamemap.cc:1199-1206`).
  Prefetch all frames of a sprite when a `Sprites_effect` starts (`effects.cc:321-349`).
* For PNG sources, decode on a worker thread with its own file handle, because `IDataSource` is not thread-safe. Encode RLE there too, and attach the result on
  the main thread at the frame boundary. While a frame is missing, paint the NN fallback.

---

## 6. Touchpoints

| where | what | change |
|---|---|---|
| `shapes/vgafile.h:47-163` | `Shape_frame` | add hi-res payload slot and state; never freed by the cache |
| `shapes/vgafile.h:357-377,400-408` | `get_shape`, `get_num_frames` | must **not** attach hi-res (frame-0 sweep) |
| `shapes/vgafile.cc:802-824,915-918` | reflection | derive hi-res reflection lazily, make it evictable |
| `shapes/vgafile.cc:281-347` (295) | `encode_rle` `runs[200]` | size the buffer by width (overflow measured up to 561 runs) |
| `shapes/vgafile.cc:456-477` | `get_rle_shape` empty frame | fix null write, or never emit empty frames |
| `shapes/vgafile.cc:1163-1183` | `U7load` | open the hi-res pack as a path (streaming), never as an FLX resource |
| `shapes/vgafile.cc:1008-1011,1270-1306` | reset / new_shape | invalidate hi-res entries (generation bump) |
| `shapeid.h:77-101`, `shapeid.cc:552-576` | `shape_cache` raw pointers | keep pointer stability; optional hi-res pointer next to `Cached_shape` |
| `shapeid.h:171-196` | `paint_shape` / `paint_invisible` / `paint_outline` | hi-res resolve, LRU touch |
| `shapeid.cc:116-144,150-382,385-414,420-468` | info sweep, load, reload | create the cache, budget config, invalidation |
| `objs/chunkter.cc:86-133,234-268` | flats render, queue size | hi-res flats; view- or budget-based queue size (59 MB → ~18 MB) |
| `gamewin.h:747-755`, `gamerend.cc:624-632` | `show` / `paint_dirty` | frame stamp and deferred eviction point |
| `gamemap.cc:1199-1206`, `effects.cc:321-349` | superchunk read, sprite effect | prefetch hooks |
| `shapes/font.cc:775-795`, `mouse.h:36-44`, `menulist.h:95,140` | eager `Shape_file`s, raw pointers | later phase; pin or exempt from eviction |
| `configure.ac:399-405`, `.github/workflows/snapshots-windows.yml:49` | 32-bit targets | lower default budget or S |

## 7. Risks
* The noise model may underestimate real AI art entropy. Budget for RLE at ≈1.1 B/px and PNG at ≈0.9 B/px.
* Eviction while a pointer is in use (nested paint, `reflect`). Mitigate by evicting only at the frame boundary, never on insert.
* Thrash if the budget is under the scene working set (~48 MB at S=6). Log hit and miss counters and make the budget visible in the cheat screen.
* ExultStudio's live reload must invalidate hi-res entries too. Otherwise stale art keeps being painted with new low-res geometry.
* Prefetching on a worker thread adds thread-safety work around `IDataSource` and the `Shape` vectors.
* On Android and 32-bit builds, address-space fragmentation from many small allocations and the 59 MB terrain cache. A lower S may be the only safe option there.

## 8. Open questions
* Default S and budget per platform: S=6 on desktop only, S=3-4 on mobile?
* Should flats and fonts be pinned, or go through the LRU with a priority?
* Should hi-res reflections be pre-generated in the pack (disk cost) or derived at runtime (CPU cost of about 0.2-1 ms per actor frame)?
* Is an mmap-backed companion VGA worth the new I/O plumbing for the Android RSS benefit?
* Should the cache be shared across `Vga_file`s (one global budget, recommended) or set per file?

## Appendix: reproduction
```
python3 docs-hires/analysis/gap_3_tools/memstat.py            # ~25 s, writes memstat.json next to it
g++ -O2 -std=c++17 hrbench.cc -I<zlib/libpng include> -L<libdir> -lpng16 -lz -o hrbench
./hrbench <static>/shapes.vga <static>/palettes.flx 6 <sigma> [maxframes] [pngframes]
```
`hrbench` reports per-frame and total RLE, NN-RLE and PNG sizes, the maximum runs per scanline (the `runs[200]` check), and encode, paint and PNG decode
timings. It was built against the zlib and libpng extracted in the session scratchpad (`root/usr`).
