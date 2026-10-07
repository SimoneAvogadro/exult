# Gap #4: Baseline cost of world rasterization (`Game_window::paint`) and its measured growth at render scale S

Scope: measure how long the world renderer takes today at 1x, where that time goes, and how it actually grows when the same frame is rasterized at S = 2, 3, 4 and 6. Earlier reports only measured presentation (LUT conversion and downscale, about 0.9 ms). This report does not extrapolate the S-scaled cost. It measures it, with a harness that redirects every drawing primitive of the real renderer into an S-x buffer. The output was verified bit-exact against a nearest-neighbour upscale of the 1x frame.

Repo: `/home/simonea/ultima7_exult/exult-hires` (master `8b6ab6b43`). All `file:line` references point at that tree unless they say otherwise. **Nothing in the repo was modified.** The harness lives in a patched throw-away copy (`/home/simonea/ultima7_exult/tmp/gap4/exult-src`). Its sources, scripts and raw results are archived in `docs-hires/analysis/gap4_bench/`.

---

## 0. TL;DR

1. **At 1x the world paint is tiny, and object traversal dominates it, not pixels.** For a full 320x200 repaint (g++ -O2, Ryzen 7 9700X, quiet machine), the median is **0.026–0.21 ms** across 8 scenes.
   - The objects pass takes 80–100 % of that time.
   - Flat terrain takes about 1.4 µs: 9 `memcpy` blocks out of the per-terrain chunk caches.
   - Each frame visits 190–1230 objects. Only 60–160 of them reach the RLE blitter. The rest are culled or contribute only dependency work, at about 70–175 ns per object.
2. **At S=6 the same frames cost 0.32–1.57 ms (median), which is 6–16x the 1x cost, not 36x.** Two S=6 strategies were measured on real draw lists:
   - (A) nearest-neighbour (NN) expansion on the fly inside the primitives: **0.32–1.33 ms**.
   - (B) pre-upscaled cached RLE frames and pre-upscaled chunk flats, drawn with the *existing* `paint_rle`: **0.37–1.57 ms**. This needs 9–12 MB of cache per scene, and the first frame after a teleport takes 13–19 ms while the cache fills.
   - Cost grows roughly **linearly in S** for (A), because work per emitted row dominates, and as `a + b·S + c·S²` with a small `c` for (B) (§3.6).
3. **Cached pre-upscaled RLE is *not* faster than on-the-fly NN for sprites at -O2.** It is slower: it has S times more scanlines to parse and reads 36x more source bytes. Pre-scaling pays off **only for the flats**: 0.18 ms becomes 0.04 ms at S=6. Option (B) is still the right *model of cost* for real hi-res override art, which is also S² data, so **a scene where every frame is overridden costs about 0.4–1.6 ms of world paint at S=6 for a 320x200 view**.
4. **View area costs more than S does.** At 1x, a 1920x1080 game view (an "Auto" view) already costs **0.79–2.50 ms**, with 1840–3920 objects visited per frame. Measured costs for roughly 1920x1200 output pixels:
   - 320x200 at S=6: 0.32–1.57 ms;
   - 640x400 at S=3: 0.38–2.34 ms;
   - 1920x1080 at 1x: 0.79–2.50 ms.

   An ultrawide Auto view at S=2 (1720x720, giving 3440x1440) costs 1.1–4.2 ms. The design must cap `S x game_view` at about the display size. A large Auto view combined with S=6 is never sensible.
5. **Smooth scrolling (lerp) costs exactly one full paint per main-loop iteration.** `paint_lerped` is `paint()` with a sub-tile offset. The chunk margin it needs is always painted anyway (`gamerend.cc:212-216`). The measured LERP time equals the full-paint time. With lerp at S=6 you therefore pay about 60–100 full paints per second, at ≤1.6 ms each. That is affordable on this CPU.
6. **Partial paints are cheap but not proportional to area.** A 1-tile column strip (the non-lerp scroll path) costs 0.007–0.068 ms, which is up to a third of a full paint for 1/40 of the pixels. The whole chunk column's objects are still traversed. The `win->copy()` shift costs 0.8 µs at 1x, which is about 0.05–0.1 ms extrapolated to the 2.3 MB S=6 buffer.
7. **`build-linux` is an unoptimized (-O0) build.** `CXXFLAGS = -pthread` with no `-O` flag, `OPT_LEVEL` is empty, and configure reported "optimization level: from CXXFLAGS" (`build-linux/Makefile:457,540`, `config.log:1464-1465`).
   - At -O0 the 1x paint costs 0.36–1.50 ms, 7–15x slower than -O2.
   - At S=6 it costs 1.1–7.1 ms with (A) and **6–23 ms with (B)**, because `Image_buffer8::paint_rle` copies byte by byte.

   Any performance evaluation, and any player build, must use `--with-optimization=normal` (or `CXXFLAGS=-O2`).
8. **The frame budget is set by the main loop, not by drawing.**
   - `Delay()` always waits up to 10 ms, or less if an event is pending (`gumps/gump_utils.h:41-55`).
   - vsync is on by default (`imagewin/imagewin.cc:631-633`).

   With strict double-buffered vsync at 60 Hz, that leaves about 6.7 ms per iteration for events, tqueue, paint and show. A full S=6 frame (paint ≤1.6 ms plus about 1–2 ms of present, per `present.md`) fits. A -O0 build at S=6 does not.
9. **Decision guidance** (detail in §5):
   - **Use on-the-fly NN for frames without an override.**
   - **Use cached S-x data only where it is real art**, meaning override frames and optionally S-x chunk-flat caches.
   - **Keep lerp at S.**
   - **Defer a "UI-only repaint" path.** It is an optimization for -O0/low-end machines and for modal loops, not a requirement.
   - **Fix three hot spots:** the byte loops in `paint_rle`, the fixed 100-entry flats LRU, and per-run calls in the translucency path.

---

## 1. What was measured, and how

### 1.1 Existing instrumentation (read, not used for the numbers)

- `PerformanceTimer` (`perf.h:32-139`, `perf.cc:31-155`) keeps a linked list of named timers.
  - `start_phase()`/`end_phase()` measure **exclusive** time: when a nested scope ends, its delta is added to `start_time` of every active outer scope (`perf.h:104-118`).
  - The clock is `SDL_GetTicksNS()` (`perf.cc:56-58`).
  - Lookup is a linear string compare on every scope entry (`perf.cc:35-54`).
- Scopes in today's code:
  - `Game_window::paint` (`gamerend.cc:331`);
  - `Image_window::show` and its sub-phases (`imagewin.cc:886, 954, 973, 1019, 1053`);
  - `composite_layers` / per-layer (`imagewin.cc:2095, 2121`);
  - `UpdateRect` (`imagewin.cc:2171, 2176, 2213`).

  There is **no** scope inside `paint_map`, so terrain, objects and effects are not separated.
- The overlay is drawn by `paintPerfMetrics()`, called at the top of every `show()` (`imagewin.cc:883`).
  - It closes the "Frame" timer, prints every used timer, resets all of them and restarts "Frame" (`perf.cc:119-154`). Values are therefore **per shown frame** (show to show, including the 10 ms `Delay`).
  - When the overlay is on it calls `gwin->set_all_dirty()` (`perf.cc:147-148`). **This forces a full world repaint every frame, so the overlay changes what it measures**: it hides the dirty-rect savings of the non-lerp path.
  - The overlay is toggled by the `PERF_METRICS` action (`keys.cc:190`, `keyactions.cc:984-986`), which has **no default key** in `data/bg/defaultkeys.txt`. The user must bind it.

### 1.2 Harness used for the numbers

A copy of master was built with `g++ 9.4 -O2 -g` through the `Makefile.common` wrapper described in `build.md` Appendix A, plus an -O0 twin. Three files were patched in the copy:

- `gamerend.cc`: `std::chrono` phase accumulators around:
  - the flats loop (`gamerend.cc:227-233`);
  - the flat-RLE loop (`235-241`);
  - the object passes (`254-265`);
  - blackness (`267-269`);
  - `paint_map`, effects, border, gumps and lights inside `Game_window::paint` (`361-410`).

  It also counts chunks, objects (`paint_object`), flat RLE objects and light sources. Patch script: `gap4_bench/patch_gamerend.py`.
- `imagewin/ibuf8.cc`: two additions.
  - **Counters** of pixels actually written into the game buffer per primitive: `copy8`, `paint_rle`(+remapped), `fill*`, and the translucent paths.
  - A **hi-res redirect**. When a mode is set and `this == game ibuf`, every primitive draws into a separate S-x `Image_buffer8` instead, with coordinates and clip multiplied by S:
    - **mode 1, on-the-fly NN**: each RLE run or raw run is expanded into a line buffer and written to S rows with `fill_hline8`/`copy_hline8`. Chunk flats are expanded per row.
    - **mode 2, cached pre-upscaled**: on first use each RLE frame is re-encoded as an S-x NN-upscaled RLE stream and then drawn with the **unchanged** `Image_buffer8::paint_rle` at `(x·S, y·S)`. Chunk flats are cached as S-x images and drawn with `copy8`.
    - Translucent and remap paths expand per run in both modes.

  Patch script: `gap4_bench/patch_ibuf8.py`.
- `exult.cc`: three lines after `gwin->setup_game()` (`exult.cc:1141`) call `Run_paint_bench()` when `EXULT_PAINT_BENCH` is set (`gap4_bench/bench_paint.cc`).

What `Run_paint_bench()` does for each scene:
1. Teleports the party with `teleport_party(t, skip_eggs=true)` (`gamewin.cc:1969-2011`) and runs 30 warm-up paints.
2. Runs one counting pass.
3. Times N full `gwin->paint()` calls (the same `read_map_data` + `set_all_dirty` + `paint_dirty` path the game uses, `gamerend.cc:419-425`).
4. Times `set_all_dirty`+`paint_dirty`, a 1-tile column strip, the `win->copy()` scroll shift, an NPC-sized 48x64 rect, `paint_lerped(0x8000)` and `show(true)`.
5. Repeats the full paint in mode 1 and mode 2 for each S. Cold time and cache size are recorded for mode 2.

**Validation.** For every scene the S=6 buffer (both modes) was dumped and compared with a 6x NN upscale of the 1x frame. **The difference was 0 pixels in all 16 cases.** The S-x run therefore does the complete, real rasterization work of that frame.

**Environment.**
- CPU and OS: Ryzen 7 9700X, WSL2 (Linux 6.18).
- SDL: 3.2.14 with `SDL_VIDEO_DRIVER=dummy`, so the renderer is SDL's software renderer.
- Config: `skip_intro`, audio off, vsync 0, new BG game via `--bg --nomenu`.
- Single thread.
- Statistics are median and p90 over N=300 at 1x and N=50–150 at S>1.

**Caveats.**
- NPCs do not move, because the tqueue does not run during the bench.
- No combat sprite effects and no gumps are open.
- The dummy video driver makes `SHOW` numbers unrepresentative (§3.2).
- A first run made while other agents loaded the machine (load average about 13) gave numbers 1.6–2.2x higher, with p90 spikes of 4–12 ms (`gap4_bench/results/g320.txt`). The tables below use the quiet re-runs `q*` (load average 2–3). Two further quiet repeats (`g320r2`, `g320r3`) agree with each other within about 3 %.

### 1.3 Scenes

| id | tile (x,y,z) | content (verified from frame dumps) |
|---|---|---|
| start | 1079,2214,0 | Trinsic, new-game start: buildings, NPCs, walls |
| brit_st | 860,1380,0 | Britain street, roofs shown |
| brit_mkt | 880,1300,0 | Britain street with roofs and street furniture |
| castle | 940,1160,0 | Castle British gate: guards, carpet, banners, trees (many translucent and filled pixels) |
| throne | 936,1140,0 | Inside Castle British (roof skipped), furniture-heavy |
| forest | 600,600,0 | dense forest NW of Britain (trees, many flat RLE objects) |
| dung | 580,1200,0 | dungeon interior west of Britain (`in_dungeon=5`, blackness active) |
| brit_xlu | 860,1380,0 + stress | brit_st plus 63 spawned translucent objects (first translucent shape 177) and an **invisible avatar** (`paint_rle_transformed`, `actors.cc:2078-2090`). Translucency stress, used as a stand-in for "combat with translucent actors" |

---

## 2. Where the time goes in today's code

Call graph of one full repaint (`Game_window::paint()`, `gamerend.cc:419-425`):

1. `map->read_map_data()`, then `set_all_dirty()` (`gamewin.h:778-780`), then `paint_dirty()` (`gamerend.cc:624-632`), then `paint(x,y,w,h)` (`gamerend.cc:328-414`).
2. `win->BeginPaintIntoGuardBand` (may enlarge the rect by the 4-pixel guard band), then `set_clip`.
3. `Game_render::paint_map` (`gamerend.cc:192-291`):
   - **Chunk range** (`206-219`): one chunk of margin on the top/left and two on the bottom/right ("increased by 1 to support Smooth Scrolling", `212-216`). That is **20 chunks for 320x200** (5x4), 42–48 for 640x400 and about 190 for 1920x1080.
   - **Pass 1, flats** (`227-233`): `paint_chunk_flats` copies the chunk's pre-rendered 128x128 terrain (`Chunk_terrain::get_rendered_flats`, `objs/chunkter.h:95-101`) with `copy8` (`gamerend.cc:520-531`). The terrain cache is an LRU of **100 `Chunk_terrain`s** (`objs/chunkter.cc:234-241, 248-268`). `render_flats` re-copies all 256 tiles of a terrain when it is (re)built.
   - **Pass 2, flat RLE terrain** (`235-241`): `Flat_object_iterator` calls `obj->paint()` for each object.
   - **Pass 3, objects**, diagonally NE (`254-265`), through `paint_chunk_objects` (`557-586`):
     - it walks the chunk's light lists;
     - it calls `paint_object()` for every non-flat object not yet painted in this `render_seq`;
     - `paint_object()` first paints all dependencies recursively (`592-618`), then calls `Game_object::paint()`, which calls `get_shape_location` and `ShapeID::paint_shape` (`objs/objs.cc:959-963`, `shapeid.h:348-356`). Dispatch goes to `paint_rle`, `paint_rle_remapped` or `paint_rle_translucent` (`shapeid.h:171-183`).
     - Actors take `Actor::paint` (`actors.cc:2078-2110`), which adds weapon and outline drawing, plus `Npc_actor::paint` side effects: it wakes dormant schedules and adds the NPC to the nearby list (`actors.cc:5133-5149`).
   - **Dungeon blackness** (`267-269`, `648-708`): a `fill8` per run of blacked-out tiles.
4. `effects->paint()` (sprite effects into the world buffer), then the border fill, then `gump_man->paint(false/true)`, `dragging->paint()` and `effects->paint_text()`. These last ones go to **layers**, which are not world pixels (`present.md` §0). Finally the party light sum and `clock->set_light_source` (`gamerend.cc:394-410`).

Pixel primitives (`imagewin/ibuf8.cc`):
- `Shape_frame::paint_rle` culls frames of at least 8x8 px with `is_visible` (`shapes/vgafile.cc:483-501`).
- It then calls `Image_buffer8::paint_rle` (`ibuf8.cc:516-670`), which clips per scanline and per run. Its inner loops copy **byte by byte** (`Write1(dest, Read1(in))` / `Write1(dest, col)`), not with `memcpy`/`memset`.
- Translucent frames take a different route (`vgafile.cc:540-594`): one virtual call per run (`copy_hline_translucent8`, `fill_hline_translucent8`, `ibuf8.cc:408-460`) with a per-pixel `Xform_palette` lookup.
- Invisible actors use `paint_rle_transformed` (`vgafile.cc:596-638`).

**Measured split at 1x (q320, mean per paint):** flats about 0.0014 ms, flat RLE 0.0002–0.022 ms, **objects 0.021–0.215 ms**, blackness 0.004 ms (dungeon only), effects/gumps/lights ≤0.001 ms. The objects pass is 80–100 % of the frame.

Pixel and object counts per 320x200 frame:

| scene | chunks | objects visited (`paint_object`) | RLE blits reaching `Image_buffer8::paint_rle` | px written (K) | translucent px (K) | filled px (K) | light sources |
|---|---|---|---|---|---|---|---|
| start | 20 | 704 | 115 | 126 | 2.2 | 0.6 | 4 |
| brit_st | 20 | 661 | 111 | 138 | 2.5 | 0.1 | 8 |
| brit_mkt | 20 | 523 | 97 | 120 | 1.6 | 0.7 | 0 |
| castle | 20 | 1230 | 153 | 189 | 23.0 | 22.8 | 6 |
| throne | 20 | 1027 | 157 | 184 | 20.5 | 19.7 | 5 |
| forest | 20 | 190 (+156 flat RLE) | 63 | 99 | 0.6 | 0.1 | 0 |
| dung | 24 | 328 | 61 | 98 | 1.0 | 0.1 | 1 |
| brit_xlu | 20 | 714 | 110 | 184 | 41.2 | 7.4 | 15 |

The flats always write 64,000 px. Overdraw is 1.5–3x the 64,000-pixel frame. Only about 10–30 % of the visited objects end up as RLE blits (61–157 blits, against 190–1230 visited objects plus 0–156 flat RLE objects). The cost per visited object (objects phase divided by objects) is about 70–175 ns at -O2 and about 0.9–1.8 µs at -O0. Translucent pixels are the most expensive per pixel: in `brit_xlu`, 41 K translucent px add about 0.01 ms in the flat-RLE pass at 1x and 0.17–0.19 ms at S=6.

---

## 3. Results

### 3.1 1x baseline, 320x200 (q320, -O2, ms)

| scene | full paint med | p90 | `set_all_dirty`+`paint_dirty` | `paint_lerped` | 1-tile strip | 48x64 NPC rect | scroll `win->copy` |
|---|---|---|---|---|---|---|---|
| start | 0.060 | 0.088 | 0.059 | 0.059 | 0.012 | 0.013 | 0.0008 |
| brit_st | 0.058 | 0.085 | 0.058 | 0.059 | 0.017 | 0.011 | 0.0008 |
| brit_mkt | 0.044 | 0.057 | 0.044 | 0.044 | 0.020 | 0.016 | 0.0008 |
| castle | 0.210 | 0.251 | 0.211 | 0.208 | 0.068 | 0.055 | 0.0008 |
| throne | 0.170 | 0.205 | 0.172 | 0.177 | 0.051 | 0.036 | 0.0008 |
| forest | 0.026 | 0.029 | 0.026 | 0.026 | 0.007 | 0.006 | 0.0008 |
| dung | 0.026 | 0.027 | 0.026 | 0.026 | 0.008 | 0.006 | 0.0008 |
| brit_xlu | 0.124 | 0.158 | 0.125 | 0.131 | 0.018 | 0.019 | 0.0008 |

`paint_lerped` equals the full paint, so lerp itself costs nothing extra per paint. Its cost is that the main loop calls it on every iteration while scrolling (`exult.cc:1497-1502`), where otherwise only a 1-tile strip would be painted after a `win->copy` shift (`gamewin.cc:1641-1735`).

### 3.2 `show()` (reference only)

With the dummy driver's software renderer, 1280x800 display, point x4 and vsync off, `show(true)` costs about 0.51–0.60 ms at 320x200, 1.3 ms at 1280x800 and 2.5–3.0 ms at 1720x720 / 1920x1080. These are CPU-side renderer copies and **not** representative of a GPU renderer. With vsync on (the default) the dummy renderer blocks for about 16.7 ms per present, which a first probe confirmed. For the S-x present path use the measurements in `present.md` §0: LUT 8 to 32 for 1920x1200 is 0.90 ms, plus a 9.2 MB ARGB upload or a 2.3 MB INDEX8 upload.

### 3.3 Measured S-scaled world paint, 320x200 view (q320, -O2, median ms)

Mode 1 = NN expansion on the fly. Mode 2 = cached pre-upscaled RLE frames and S-x chunk flats, drawn with the existing `paint_rle`/`copy8`.

| scene | S=1 | S=2 m1 / m2 | S=3 m1 / m2 | S=4 m1 / m2 | **S=6 m1 / m2** | S6/S1 m1 / m2 | m2 cache @S6 | m2 cold first frame @S6 |
|---|---|---|---|---|---|---|---|---|
| start | 0.060 | 0.224 / 0.159 | 0.297 / 0.265 | 0.382 / 0.388 | **0.545 / 0.733** | 9.1 / 12.3 | 11.6 MB | 16.8 ms |
| brit_st | 0.058 | 0.220 / 0.143 | 0.298 / 0.266 | 0.385 / 0.414 | **0.553 / 0.806** | 9.5 / 13.9 | 10.7 MB | 15.2 ms |
| brit_mkt | 0.044 | 0.164 / 0.096 | 0.228 / 0.185 | 0.304 / 0.290 | **0.442 / 0.611** | 10.1 / 13.9 | 8.7 MB | 12.9 ms |
| castle | 0.210 | 0.535 / 0.461 | 0.717 / 0.690 | 0.894 / 0.934 | **1.326 / 1.573** | 6.3 / 7.5 | 12.4 MB | 18.7 ms |
| throne | 0.170 | 0.466 / 0.414 | 0.639 / 0.625 | 0.810 / 0.865 | **1.186 / 1.481** | 7.0 / 8.7 | 12.3 MB | 17.9 ms |
| forest | 0.026 | 0.130 / 0.061 | 0.181 / 0.125 | 0.233 / 0.200 | **0.355 / 0.411** | 13.7 / 15.9 | 11.3 MB | 15.4 ms |
| dung | 0.026 | 0.124 / 0.059 | 0.164 / 0.110 | 0.209 / 0.178 | **0.323 / 0.367** | 12.4 / 14.1 | 9.3 MB | 12.6 ms |
| brit_xlu | 0.124 | 0.422 / 0.358 | 0.593 / 0.563 | 0.774 / 0.806 | **1.226 / 1.468** | 9.9 / 11.9 | 10.7 MB | 15.6 ms |

Phase breakdown at S=6 (mean ms):
- **Flats:** 0.18–0.22 in m1 (per-frame expansion of 1x chunk caches) against 0.035–0.041 in m2 (`memcpy` of 768x768 S-x caches).
- **Objects:** 0.11–1.19 in m1 against 0.33–1.55 in m2.
- **brit_xlu flat RLE (translucent fields):** 0.17 in m1 and 0.19 in m2.

Mode 2 is *slower* for objects because its RLE streams have S times more scanlines and 36 times more payload. The original 1x RLE stays L1-resident, while the S-x streams are about 10 MB per scene.

### 3.4 Larger game views (Auto-like), -O2, median ms

| scene | 320x200 @1x | 640x400 @1x | 1280x800 @1x | 1720x720 @1x | 1920x1080 @1x | 640x400 **@S3** (1920x1200 out) m1/m2 | 1720x720 **@S2** (3440x1440 out) m1/m2 | 1280x800 @S2 m1/m2 |
|---|---|---|---|---|---|---|---|---|
| start | 0.060 | 0.221 | 0.804 | 0.949 | 2.168 | 0.862 / 0.751 | 2.686 / 2.102 | 2.226 / 1.778 |
| brit_st | 0.058 | 0.184 | 0.734 | 0.906 | 1.544 | 0.874 / 0.797 | 2.600 / 1.964 | 2.183 / 1.614 |
| brit_mkt | 0.044 | 0.262 | 0.768 | 0.952 | 1.295 | 0.979 / 0.961 | 2.880 / 2.237 | 2.369 / 1.771 |
| castle | 0.210 | 0.576 | 1.184 | 1.369 | 2.498 | 2.343 / 2.314 | 4.189 / 3.510 | 3.903 / 3.264 |
| throne | 0.170 | 0.552 | 0.981 | 1.114 | 2.281 | 2.291 / 2.192 | 3.530 / 2.835 | 3.249 / 2.615 |
| forest | 0.026 | 0.100 | 0.562 | 0.661 | 1.059 | 0.703 / 0.561 | 2.266 / 1.589 | 1.982 / 1.347 |
| dung | 0.026 | 0.075 | 0.306 | 0.427 | 0.790 | 0.490 / 0.379 | 1.671 / 1.113 | 1.239 / 0.778 |
| brit_xlu | 0.124 | 0.279 | 0.838 | 1.003 | 1.691 | 1.175 / 1.103 | 2.816 / 2.192 | 2.420 / 1.835 |

- At 1920x1080 a frame visits 1840–3920 objects across 187–198 chunks, with 1360–2430 RLE blits and 3.3–4.1 Mpx written.
- **The flats LRU thrashes there.** In start, castle and throne the flats phase rises to **0.70–0.80 ms per frame**, against 0.04 ms elsewhere, because more than 100 distinct `Chunk_terrain`s are visible and `render_flats()` re-renders evicted terrains every frame (`objs/chunkter.cc:234-268`).
- At S>1 with large views, mode 1 spends 0.7–0.85 ms per frame expanding flats. Mode 2 spends about 0.1 ms.

### 3.5 -O0 build (what `build-linux` produces), 320x200, median ms

| scene | S=1 | S=6 m1 | S=6 m2 | -O0 / -O2 at S=1 |
|---|---|---|---|---|
| start | 0.737 | 1.90 | 13.64 | 12x |
| brit_st | 0.819 | 1.80 | 16.39 | 14x |
| brit_mkt | 0.618 | 1.48 | 12.18 | 14x |
| castle | 1.497 | 7.10 | 22.85 | 7x |
| throne | 1.403 | 6.11 | 22.21 | 8x |
| forest | 0.396 | 1.14 | 8.15 | 15x |
| dung | 0.358 | 1.08 | 6.13 | 14x |
| brit_xlu | 1.159 | 7.07 | 21.63 | 9x |

At -O0, S-x RLE data drawn through `Image_buffer8::paint_rle` (mode 2, which models real override art) costs **6–23 ms per frame**, close to the 36x pixel ratio. That is the per-byte `Write1(dest, Read1(in))` loop at `ibuf8.cc:516-670` running unoptimized. Mode 1 escapes most of it because it writes rows with `memset`/`memcpy` (`fill_hline8`/`copy_hline8`).

### 3.6 Cost model (fit on q320 medians, S ∈ {1,2,3,4,6})

- Mode 1 (on-the-fly NN): `T(S) ≈ b·S`, with `b` = 0.07–0.27 ms per unit of S (largest for castle, throne and brit_xlu). The quadratic term is about 0, and the maximum fit error is ≤0.05 ms. Cost is dominated by work per emitted row and per run (calls and clipping), not by bytes. S=8 would cost about 0.4–1.6 ms.
- Mode 2 (S² data through the existing blitter): `T(S) ≈ b·S + c·S²`, with `c` = 0.009–0.018 ms and `b` = 0.01–0.18 ms. That is about 0.3–0.65 ms of pure byte cost at S=6. S=8 would cost about 0.6–2.3 ms.
- Rule of thumb for other CPUs: scale by single-thread speed. A machine 4x slower than a 9700X lands at about 1.5–6.5 ms per S=6 world paint, which still fits in a 60 Hz frame.

---

## 4. Frame budget in the real loop

- **Main loop** (`exult.cc:1348-1524`):
  - `Delay()` waits 10 ms in 1 ms sleeps, or returns early if an event is pending (`gumps/gump_utils.h:41-55`);
  - then events, `tqueue->activate`, gump updates;
  - then **either** `paint_lerped(factor)` every iteration while within `2·mswait` of the last step (`exult.cc:1445-1503`), **or** `paint_dirty()` when the dirty rect is non-empty (`exult.cc:1504-1510`);
  - then `rotatecolours()`, which marks the frame painted every 100–200 ms, so palette cycling needs a full present but **no** repaint (`gamewin.cc:1054-1079`);
  - then `show()`.
- **vsync** defaults to 1 (`imagewin.cc:631-633`). With a present that blocks on vsync, the iteration period is about `max(16.7 ms, 10 ms + work)`. The work budget before dropping to 30 Hz is therefore about **6.7 ms**. With S=6 that work is about 0.3–1.6 ms of paint plus about 1–2 ms of present: OK. A -O0 S=6 build at up to 23 ms is not.
- **Dirty rects** are merged into **one bounding rect** (`gamewin.h:782-784`). Once two NPCs move at opposite sides of the screen, the non-lerp path paints almost the whole view anyway. The full-paint number is therefore the relevant number in busy scenes even without lerp.
- **Modal loops repaint the whole world on every event**:
  - the gump modal loop (`gumps/Gump_manager.cc:1090-1118`, `gwin->paint()` at 1108 when `ran || dirty || got_event`, so mouse motion counts);
  - the conversation sprite-wait loop (`usecode/conversation.cc:566-574`).

  At S=6 that is ≤1.6 ms per event cycle at -O2: acceptable, but this is where a "world unchanged, repaint UI only" shortcut would help low-end and -O0 builds.
- **Painting has game-logic side effects.** `Npc_actor::paint` resumes dormant schedules and registers nearby NPCs (`actors.cc:5133-5149`). The *game-pixel* area that gets painted must therefore not change with S. Hi-res must only change the raster density, never the clip in game pixels.

---

## 5. Implications for the four open design choices

1. **NN expansion on the fly in `paint_rle` (frames without an override).** **Recommended as the default path.**
   - 0.32–1.33 ms at S=6 for a 320x200 view, with no memory cost and no cold-cache hitch.
   - Bit-exact with the "render at 1x, then point-scale" output, so all palette effects keep working.
   - The kernel should clip once per scanline and write S rows with `memset`/`memcpy`, as the harness's `nn_paint_rle` does.
   - Translucent, transformed and remapped variants need the same treatment. They are per-run virtual calls today (`vgafile.cc:540-638`).
2. **Cached pre-upscaled RLE frames.**
   - **Not justified for performance** for frames without an override: they are 15–45 % slower than on-the-fly NN at S=6, cost about 10 MB per scene and stall 13–19 ms on first use.
   - **Justified for flats**: the S-x chunk-flat cache costs 0.04 ms against 0.18 ms. But each terrain is 576 KB at S=6, so the 100-entry LRU could reach about 58 MB. The alternative is to draw S-x 8x8 tiles straight into the frame, which is about 1,100 tiles of 2,304 B per frame. Either way `Figure_queue_size()` (`objs/chunkter.cc:234-241`) must become view-size and S aware: the comment already sketches `(cw+3)*(ch+3)`.
   - Real override art behaves like mode 2. Budget **≤1.6 ms per frame at S=6 for a 320x200 view (-O2)** when *everything* is overridden.
   - Load or decode override frames ahead of time, or asynchronously, to avoid the cold-start hitch.
   - Optimize `Image_buffer8::paint_rle`'s byte loops with `memcpy`/`memset`. This matters a lot at -O0 and somewhat at -O2.
3. **Keeping lerp at S.** **Affordable.** Lerp paint equals full paint (§3.1), so smooth scrolling at S=6 costs 60–100 x ≤1.6 ms of world paint per second on this CPU. No sub-pixel re-architecture is needed. At S the lerp offset can even become S-x finer (`scrolltx_lo` in S-pixels) for smoother scrolling at the same cost.
4. **UI-only repaint path.** **Not required for S=6 at 320x200 on desktop -O2.** Worth doing later for:
   - modal loops that repaint the world on every mouse event (§4);
   - large Auto views at S≥2, where a full frame costs 1.1–4.2 ms;
   - low-end and Android targets.

   The layer architecture (`present.md`) already separates UI pixels, so the shortcut is "skip `paint_map` when only layers changed".

Additional constraints the numbers impose:
- **Cap S by display size** (`S·game_w ≤ display_w`). Objects in view, not S, dominate cost, and a large Auto view at S=6 has no visual benefit.
- **Ship and evaluate only optimized builds.** `build-linux` should be reconfigured with `--with-optimization=normal`, or `CXXFLAGS="-O2 -g"`.
- **Add perf scopes inside `paint_map`** (flats, flat RLE, objects, blackness) and an S-aware label to the overlay. Stop the overlay from forcing `set_all_dirty()` when the user wants to measure the dirty-rect path (`perf.cc:147-148`). Bind `PERF_METRICS` to a default key in a dev keymap.

---

## 6. Touchpoints

| where | what | change needed |
|---|---|---|
| `gamerend.cc:192-291` (`paint_map`) | chunk range plus 3 passes; objects pass is 80–100 % of 1x cost | S does not change traversal; keep the clip in game pixels; add perf scopes per pass |
| `gamerend.cc:328-414` (`Game_window::paint`) | single perf scope; border fill; gumps go to layers | border fill and guard band in S-pixels; make paint skippable for UI-only updates |
| `gamerend.cc:434-514` (`paint_lerped`) | full paint with sub-tile offset | at S, allow offsets in S-pixels; no extra cost |
| `gamerend.cc:520-531` + `objs/chunkter.cc:234-268` | flats `copy8` from a 100-entry terrain LRU | S-x terrain cache or direct S-x tile blits; LRU size from view and S (thrashes at 1920x1080 already: 0.7–0.8 ms/frame) |
| `imagewin/ibuf8.cc:516-670` (`paint_rle`, `_remapped`) | byte-by-byte run copies | NN S-x variant (row `memset`/`memcpy`) for frames without override; `memcpy`/`memset` for runs of S-x override data (6–23 ms at -O0 otherwise) |
| `imagewin/ibuf8.cc:360-383` (`copy8`), `152-183` (`fill8`, `fill_hline8`) | flats and fills | S-x variants or S-aware callers |
| `shapes/vgafile.cc:540-638` | translucent and transformed paths, one virtual call per run | S-aware row-batched variants (brit_xlu: +0.17 ms at S=6) |
| `exult.cc:1348-1524` + `gumps/gump_utils.h:41-55` + `imagewin.cc:631-633` | fixed 10 ms `Delay`, vsync, lerp paint every iteration | frame-time-aware delay if S>1 work grows; budget about 6.7 ms per iteration at 60 Hz |
| `gumps/Gump_manager.cc:1090-1118`, `usecode/conversation.cc:566-574` | full world repaint per event in modal loops | optional UI-only repaint when the world is unchanged |
| `gamewin.h:778-795`, `gamewin.cc:1641-1735` | single bounding dirty rect; scroll by `win->copy` plus strip paint | at S: copy the S-x buffer (2.3 MB `memmove`, ≤0.1 ms) and paint the strip at S |
| `actors.cc:5133-5149` | painting wakes NPC schedules | keep the painted game-pixel area independent of S |
| `perf.h:91-118`, `perf.cc:35-58, 119-154`, `keys.cc:190` | exclusive timers, overlay forces full repaint, no default key | per-pass scopes, optional no-force mode, dev key binding |
| `build-linux/Makefile:457,540` | -O0 build | reconfigure with optimization for any performance work |

---

## 7. Risks and open questions

- Numbers come from one fast desktop CPU under WSL2. WSL scheduling produced 4–12 ms outliers under load, so treat p90 numbers as noisy. There are no Windows-native (MSYS2) or Android numbers.
- There are no live combat sprite effects, moving NPCs or open gumps. Effects drawn into the world buffer use the same RLE primitives and should scale like objects. The tqueue's own cost was not measured.
- In mode 2 the pre-scaled flat cache is keyed by buffer pointer. That is correct for the static benchmark (validated bit-exact at 320x200) but is not a design.
- Real hi-res art will have fewer long repeat runs than NN-upscaled frames, so more raw bytes. The `c·S²` term (0.3–0.65 ms at S=6) is the part that will grow with it.
- `show()` was measured only on SDL's software renderer. The real S-x present cost must come from `present.md`, or from a GPU run with the perf overlay.

## 8. Reproduce

```bash
G=/home/simonea/ultima7_exult/tmp/gap4          # patched copy + -O0 twin + configs
$G/run_all.sh                                   # q320 q640 q1280 q1720 q1920 q320O0
python $G/summarize.py q320 q640 q1280 q1720 q1920 q320O0
# single run:
EXULT_PAINT_BENCH=1 EXULT_BENCH_N=300 EXULT_BENCH_S=2,3,4,6 EXULT_BENCH_SCENES="$(cat $G/results/scenes.txt)" \
EXULT_BENCH_SHOTS=$G/shots SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
  $G/exult-src/exult --bg --nomenu -c $G/cfg/g320.cfg
```

Archived copies are in `docs-hires/analysis/gap4_bench/`:
- harness sources: `bench_paint.cc`, `bench_hooks.h`, `patch_*.py`;
- scripts: `make_cfg.sh`, `run_all.sh`, `summarize.py`;
- raw outputs: `results/*.txt`;
- tables: `results/summary_quiet.txt`.
