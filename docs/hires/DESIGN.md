# Exult hi-res render scale: final design

Status: **final for M1**, outline for M2 (sprites) and M3 (UI). Date: 2026-10-04.
Revision 2 (2026-10-04): revised after the adversarial review of four reviewers (code claims, memory
safety, present/SDL, tests/build). Section 12 lists all 40 findings and what was done with each.
Revision 2.1 (2026-10-04): WP-00 corrected the sanitizer and build-lane commands (§6.1, §6.4, §6.5,
§7.1) after building the lanes and reviewing them, and its third review round made the buildmap
harness independent of the lane (§6.4); §12.1 lists the changes.
Repo: `/home/simonea/ultima7_exult/exult-hires`, fork base = upstream master `8b6ab6b43`.
Every `file:line` anchor refers to that commit. The lead architect re-checked the anchors in code; the
analysis documents in `docs-hires/analysis/` back the other measured facts.

Inputs:
* `analysis/00_architecture_map.md` (the "map": decisions D1-D10, touchpoints P/B/W/G/H/R/C/U);
* the three proposals `design/proposal_{A_minimal,B_perf_gpu,C_art_modder}.md`;
* the judges' verdicts. Average scores: A 7.8, C 6.7, B 5.5;
* `upscale-research/00_recommendation.md` (art pipeline).

This design **starts from proposal A**. It grafts in parts of B and C and fixes every must-fix item the
judges raised. Section 11 gives the reasons and lists what was rejected.

Terminology:
* **game px**: logical pixels, 320x200 for the classic view;
* **S** (`world_scale`): the effective render scale of the world buffer;
* **S_art** = 6: the scale the override art is authored at;
* **physical px** = game px × S;
* **NN**: nearest-neighbour replication.

---

## 1. Goals, non-goals, user decisions honoured

### 1.1 User decisions and how each is honoured

| User decision | How the design honours it |
|---|---|
| Fork locally from master | Branch `hires` from `8b6ab6b43` in `exult-hires`. A second branch, `upstream-fixes`, holds the 1x-changing bug fixes as separate, upstreamable commits (§11 D-02). |
| Target 6x detail | `S_art = 6`. The windowed profile (1920x1200, 320x200 Fit) renders at S=6 and presents 1:1 with NEAREST. |
| "Render high-res, downscale at the end if the target is lower" | Default policy `art`: S is the **largest** divisor of 6 (6, 3, 2, 1) that fits the texture limit and the pixel budget, whatever the window size. Downscaling happens only at present time: LINEAR for ratios in [0.5, 1), and a GPU halving chain below 0.5. NEAREST is never used to downscale (§3.2.4). |
| Terrain first; later RLE sprites; later UI | M1 = the world at S plus terrain-flat overrides (per tile, per group, per terrain). M2 = sprites (§3.6 outline). M3 = UI (§3.7 outline). |
| Selective per-tile / per-group overrides | Single tile = one `SSSS_FF.png`. Group = a sub-directory, activated all-or-nothing; a `.off` suffix disables it. Whole terrain = one `terrain/<T1 key>.png`. A dev toggle switches all overrides on/off live (§5). |
| AI art produced later (Windows RTX 5070 Ti, `E:\Dati\Ultima7_Upscale`) | The Windows engine reads packs from `E:\Dati\Ultima7_Upscale\packs\bg`. WSL tools write the canonical copy on ext4 (`/home/simonea/ultima7_exult/packs/bg`) and publish it to `E:` with `publish.sh`. Generated art ships as one `flats.bundle` file per pack, so neither side opens thousands of files over drvfs (measured at 17.6 s from WSL, §5.7). Dev-mode hot reload works on each side. Phase B covers the GPU routes (§8). |
| Deliverable = code + test suites + 6x tiles | §6 lists the tests per work package. §8 Phase A delivers a complete, validated 6x pack of all 3,885 BG flats from the CPU route, built here without user involvement. |
| Game logic, hit-testing and picking stay in game px | The logical API is unchanged (§2 invariant I1). Mouse mapping `screen_to_game` (`imagewin.cc:1277-1313`) is not touched. |

### 1.2 Goals (M1)

* **G1.** The world renders into the single main 8-bit buffer at S ∈ {1,2,3,6}, chosen by policy; all engine geometry stays in game px.
* **G2.** Frames without an override are pixel-identical to today's point ×S (NN).
* **G3.** Terrain flats can be overridden per tile, per group and per terrain with indexed 6x art, checked at load; anything missing or rejected falls back to NN.
* **G4.** Presentation at any window size: exact at integer ratios, filtered when downscaling, never fatal (fail-soft to S=1).
* **G5.** S=1 output is **byte-identical to upstream + P3** (§11 D-02); the feature is off by default in code. The fork's other prerequisite commits (main-buffer wiring, P11 palette alpha, the `Import_png8` leak) do not change output, and O0 proves that.
* **G6.** Automated tests: SDL-free unit tests and a data-free present test in `make check`, plus headless golden and oracle runs on real BG data here; the unit tests also run on Windows.
* **G7.** A Windows build (MSYS2 UCRT64) for the user's PC, measured on D3D11, D3D12 and Vulkan.
* **G8.** A complete 6x BG flat pack that passes the QA gates (Phase A), and a pipeline to replace families with AI art (Phase B).

### 1.3 Non-goals for M1

* Overrides for RLE sprites (M2) and for UI, fonts, faces and cursor (M3).
* RGBA or true-colour art. Art is indexed against palette 0; palette effects stay engine-side.
* Sub-game-pixel scrolling, the earthquake as a present-time offset, frame pacing, and the GPU area shader. All are possible post-M1 options (§11 D-19).
* A VideoOptions UI entry for S. S is shown in the resize toast only.
* Overrides on Android and iOS (no libpng there). The NN world at S still works on those platforms.

### 1.4 Visual mismatches accepted by design in M1 and M2 (user documentation)

The user guide (`docs/hires.md`, WP-18) carries this list verbatim:

1. **UI stays 1x until M3.** That covers gumps, text, the cursor, the dragged item, conversation faces and the shortcut bar. They are composited at display resolution by the existing UI scalers, exactly as today.
2. **Objects stay NN until M2.** That covers NPCs, trees, buildings and RLE terrain tiles such as shore pieces. Next to smooth 6x flats they look blocky.
3. Selection and combat outlines, editor grids, and egg, barge and bbox debug lines become **S px thick**, because each game px is drawn as an S×S block.
4. The terrain editor (`render_all`) and direct flat paints (`Shape_frame::paint`, flat branch) show **NN only**. Per-terrain overrides are not shown while a terrain is being edited.
5. Scrolling still moves in whole game-px steps (S physical px). The earthquake shakes by game px.
6. A paletted screenshot is an **S× indexed image**. That is an authoring feature.
7. Save-game thumbnails (`mini_screenshot`) take the top-left sample of each S×S block.

---

## 2. Architecture overview and invariants

### 2.1 Overview

```
            game px everywhere: logic, clip, dirty rects, mouse, picking, draw order ─────────┐
                                                                                             │
 shapes.vga ─► Vga_file ─► Shape_frame (1x, authoritative) ──► painters (unchanged) ──┐      │
 u7chunks ─► Chunk_terrain ─► find_flat_source() ─┐                                   │      │
                    T1 key ─────────────┐         │                                   ▼      │
 <PATCH>/hires, <HIRES> ─► Hires::Store │ per-terrain ► per-tile ► NN                 │      │
   (PNG, rules, groups,      ▲          ▼         ▼                                   │      │
    guards, reduction)       │   rendered_flats: Image_buffer8(128,128, pixel_scale=S)│      │
                             │          │ blit()                                      │      │
                     Hires glue         ▼                                             ▼      │
                     (config, palette,  Image_buffer8 main (logical API, pixel_scale=S) ◄────┘
                      dev keys, log)    bits = draw_surface INDEX8 (full·S + 2·4 px guard)
                                        scaled branches → Write_tracker (logical bbox)
                                                │ show(): world_scale>1 → show_world_scaled()
                                                ▼   upload tracked_to_phys(rect): (x+off)·S, clamped
                         World_presenter: world_texture  INDEX8+shared SDL_Palette (SDL≥3.4)
                                                         | ARGB8888 via LUT (SDL 3.2 baseline)
                                          [INDEX8 → RGB resolve target, NEAREST 1:1, if filtering]
                                          filter ladder: NEAREST | PIXELART/LINEAR | halving chain
                                                ▼  into the same logical display rect as today
                         composite_layers() (UI at 1x, unchanged) ─► SDL_RenderPresent
```

Nothing changes when `world_scale == 1`. The window then runs the upstream pipeline: phase 1/2 scalers,
`screen_texture`, `screen_texture_a`, `UpdateRect`.

### 2.2 Invariants

| # | Invariant | Enforced by |
|---|---|---|
| I1 | **Logical API, physical storage.** `width`, `height`, `offset_x/y`, the clip rect, every coordinate argument and every dirty rect stay in game px. Only `bits` (the physical address of logical (0,0)) and `line_width` (the physical pitch) are physical. | Code review; O1, O2 |
| I2 | **The S=1 path is byte-identical to upstream + P3.** Each primitive's existing body runs untouched when `pixel_scale == 1`. | `test_ibuf_golden` (digests recorded on unmodified code); `buildmap_golden.sh` against `build-upstream` |
| I3 | **NN equivalence.** With overrides off, `phys(render_S) == NN_S(render_1)` for every primitive and for whole world regions. | O1 (fuzz), O2 (regions) |
| I4 | **No dual write.** The world is painted once per frame, into the main buffer. The painted game-px area does not depend on S, because paint has game-logic side effects (`actors.cc:5133-5148`). | Design; O6 |
| I5 | Hit-testing, picking, emptiness, draw order and dirty extents use **1x frames**. | Unchanged code paths |
| I6 | **Every `show()` at S>1 takes `show_world_scaled()`.** That includes scene mode and pushed render targets. Upstream's `screen_texture`, `screen_texture_a`, `UpdateRect` and phase 1/2 are never touched at S>1, and they are not even created. | Hook at `imagewin.cc:899`; data-free present test |
| I7 | **Index semantics.** Overrides are raw indices against palette 0. Cycling indices (0xE0-0xFE) appear only where the 1x parent pixel or one of its neighbours is in the same cycle range. 0xFF never appears in a flat. | Loader rules (§5.5); validator |
| I8 | **Overrides never change game state.** `overrides=no` reproduces the NN output exactly. | O4a, toggle test |
| I9 | **No art key depends on the 1x fill heuristic.** Per tile: `(shape, frame&31)`. Per terrain: the T1 key over the terrain's own flats only. | §5.2; `test_hires_rules` vectors |
| I10 | **Determinism.** No uninitialised memory: P3 zero-fill; every new buffer is value-initialised; palette alpha is always 255 (P11). No asynchronous loading. `--render-test` output is reproducible run to run. | ASan build; `render_regions.sh` run twice |
| I11 | **Fail soft.** Any hi-res failure falls back to S=1 (window) or NN (tile), with a log line. It never throws, never calls `free_surface()` from the hi-res path, and latches the failure so the retry does not loop. `std::bad_alloc` and libpng errors are caught at the store boundary. | `create_world_scaled_surfaces` failure path; store states |
| I12 | **The main buffer always describes `draw_surface`.** After every `create_surface`: `draw_surface->w == main_ibuf->width·pixel_scale + 2·guard_band` (and the same for h); `pixel_scale == world_scale`; the tracker is set iff S>1. Every "main buffer" field write in `Image_window` goes through `main_ibuf`, never through `ibuf` (the current target). | Debug assert at the end of `create_surface`; S-cycle and pushed-resize oracles under ASan (§6.4) |

---

## 3. Detailed changes per component

The rule for rebase safety: **every S>1 behaviour lives in new functions or new files. Existing functions
gain only one-line hooks.** The scaled primitive bodies go into a new `imagewin/ibuf8_scaled.cc`, so
upstream edits to `ibuf8.cc` conflict only on the hook lines.

### 3.1 Image buffer core (`imagewin/`)

**Data** (`imagewin/imagebuf.h:57-66`, class `Image_buffer`):

```cpp
protected:
    int pixel_scale = 1;              // physical px per game px; affects storage only
public:
    int get_pixel_scale() const { return pixel_scale; }
    Image_buffer(const Image_buffer&) = delete;              // defensive: owned bits, double delete[]
    Image_buffer& operator=(const Image_buffer&) = delete;
    struct Write_tracker {            // logical bbox of writes since the last take()
        int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
        void add(int x, int y, int w, int h);  bool empty() const;  TileRect take();  void reset();
        void mark_all(int x,int y,int w,int h);   // reset(), then assign exactly this rect (never a union)
    };
    Write_tracker* tracker = nullptr; // set only on the main buffer when S>1
```

* The physical address of logical (x,y) is `bits + y·S·line_width + x·S`. Negative logical coordinates keep working.
* **Tracker coordinates are logical, relative to the logical origin**, so they are often negative: borders and `clear_screen` paint from `get_start_x() = −offset_x`. The tracker never holds physical or texture coordinates. The single conversion to the world texture is `tracked_to_phys` (§3.2.4).
* The name `pixel_scale` avoids a clash with `Layer::render_scale` (`imagewin.cc:2130`).

**Constructors** (`imagewin/ibuf8.h:36-47`):
* `Image_buffer8(unsigned w, unsigned h, int scale)`: owned, `w·S × h·S`, **value-initialised**, `line_width = w·S`.
* `Image_buffer8(unsigned char* origin, int line_width, int w, int h, int off_x, int off_y, int scale)`: a non-owning view. The tests use it for negative offsets and foreign pitches; M2 uses it as the physical view.

The existing scale-1 constructors stay byte-for-byte as they are.

**Hooks.** Each primitive in `ibuf8.cc` gets one first line,
`if (pixel_scale != 1) { s_<name>(args); return; }`. The scaled implementations live in
`ibuf8_scaled.cc` and share the logical clip helpers (`imagebuf.h:69-100`) plus three internal helpers:
* `phys(x,y)`, the physical pointer;
* `rep_row(dst, src, w)`, which expands each byte ×S;
* `dup_rows(first, bytes)`, which copies the first physical row into the next S−1 rows.

Every physical write goes through these helpers. The helpers also call `tracker->add()` with the
**clipped logical rect**, so the tracker cannot miss a write.

| Primitive (`ibuf8.cc` lines) | Scaled branch |
|---|---|
| `fill8(pix)` 140-146 | memset `S·height` physical rows of `S·width` bytes from the physical origin `(−offset·S)`. Tracker: whole buffer. |
| `fill8(rect)` 152-167, `fill_hline8` 173-181 | logical clip, then memset `S·w` bytes on `S·h` (resp. S) rows |
| `copy8` 360-384, `copy_hline8` 390-402 | logical clip, then `rep_row` + `dup_rows` per source row |
| `copy_hline_translucent8` 408-434 | per source pixel: opaque gives an S×S block; translucent applies `xforms[c−first][*p]` **to every physical pixel**, because the destination may already hold hi-res detail |
| `fill_hline_translucent8` 440-456, `fill_translucent8` 462-481 | `xform[*p]` on every physical pixel of the S-scaled span |
| `copy_transparent8` 488-513 | non-zero source pixels become S×S blocks (no engine callers; implemented for completeness) |
| `get_pixel8` / `put_pixel8` (`ibuf8.h:99-107`, inline) | read the top-left physical sample / logical clip, then an S×S block |
| `draw_line8` 185-354 | Bresenham in game px; each logical pixel is plotted as `fill8(…,1,1)` or `fill_translucent8(…,1,1)`. Lines are S px thick (§1.4) |
| `copy` 43-68 (scroll, earthquake) | **clipped**: the source and destination rects are both intersected with the logical extent `[−offset_x, width−offset_x) × [−offset_y, height−offset_y)` (shifting the other rect accordingly), then scaled ×S and memmoved over `S·h` rows. The guard band is only 4 physical px (under one game px at S=6), so an unclipped bad rect would overrun by S×. The S=1 body stays unclipped, as upstream, so out-of-range rects are tested on S>1 buffers only (§6.2). |
| `fill_static` 115-134 | one `std::rand()` per **logical** pixel, then an S×S block. RNG consumption equals S=1 |
| `paint_rle` 516-669, `paint_rle_remapped` 672-825 | each RLE scan is one contiguous span, so decode the scan (applying the remap if any) into a row buffer, then call the scaled `copy_hline8(row, len, x, y)`. Correct by construction because it reuses the clip. **Row buffer:** a reusable heap `std::vector<uint8_t>` (static per process, single-threaded engine), grown to `scanlen` before decoding. Scan lengths are 15-bit (up to 32,767, `ibuf8.cc:522-525`), so a fixed stack array would overflow on wide mod or hi-res frames. Each run is clamped to `min(run count, scanlen − b)` when it is copied, while the input pointer still advances by the full encoded size so parsing matches upstream. Malformed data therefore cannot write past the row buffer. The `memset` fast path for runs comes in WP-16 |
| `create_another` (`ibuf8.h:58-60`) | returns an owned buffer of **the same scale**, so `ImageBufferPaintable` (`shapeid.cc:643-654`) round-trips exactly |
| `draw_box`, `draw_beveled_box` | unchanged; they are built from the primitives above |

**Defined behaviour across scales.** This is a judge must-fix. `create_buffer` inherits the scale of
the *current* target (`imagewin.cc:848-852`; callers include `shapeid.cc:646`, `playfli.cc:116`,
`mouse.cc:177`, `bggame.cc`), so a buffer can meet a target of another scale. Rule: **a physical row
copy happens only between equal scales, and it always uses each buffer's own `line_width`.**

| Operation | Same scale | Source S → destination 1 | Source 1 → destination S | Other mixes |
|---|---|---|---|---|
| `get(dest, x, y)` (74-99) | physical rows, both pitches honoured | top-left sample per logical px | replicate S×S | sample, then replicate |
| `put(src, x, y)` (105-110) | at S=1 with both scales 1: today's `copy8` call, unchanged; otherwise `blit` | via `blit` | via `blit` | via `blit` |
| `blit(src, x, y)` (new) | copies src's **storage extent**: src logical `[−off_x, w−off_x) × [−off_y, h−off_y)`, placed so that src logical (0,0) lands at dest (x,y); logical clip against dest; physical rows honouring `src.line_width` | sample | replicate | sample + replicate |

* **Why the storage extent.** Upstream `put` reads `w×h` from `src->bits` and ignores the source offsets (`ibuf8.cc:105-110`). For a view with `offset_y > 0`, the last `offset_y` rows would lie past its storage, which is an out-of-bounds read. For zero-offset sources, which covers every `create_buffer` result and the flats cache, the storage extent equals `(0,0,w,h)`, so the behaviour is the same. The scaled and mixed `get` paths define the destination side the same way. The S=1 same-scale `get`/`put` bodies stay as upstream (I2).
* A mixed-scale call logs once per process (`[hires] mixed-scale get/put …`), because it loses quality. It is never undefined.

**Other new members.**
* `put_phys(const uint8* src, int pw, int ph, int src_pitch, int px, int py)`: a physical copy, clipped against both `clip × S` of the destination and the source size `pw × ph`. It writes through the tracked helpers, so the tracker sees the logical bbox, using floor for the start and ceil for the end. Used by terrain composition.
* `set_tracker(Write_tracker*)`.

**Raw readers audited.** grep over `get_bits()`, `get_line_width()`, `->bits`, `create_buffer`, `->get(`, `->put(`:
* `gamerend.cc:529` changes to `blit` (§3.3);
* `gamemap.cc:1706` reads a local 1x buffer (§3.4);
* `iwin8.cc:190-191` (`mini_screenshot`) uses `ib8->get_pixel8(X,Y)` in place of the raw read. That is identical at S=1 and a top-left sample at S>1;
* `vgafile.cc:124` (reflect) uses a scale-1 buffer. Unchanged.
* `bggame.cc:690-700` (`SDL_SurfaceOwner` wraps `get_bits()` with logical w/h and the physical pitch). It is safe only for scale-1 buffers. Today that holds only because `Scene_view` pushes an S=1 scene layer; if `create_layer` fails (`imagewin.cc:1575-1586`), the intro paints into the S>1 main buffer. Fix: a new `Image_window::create_buffer_1x(w,h)` (map B6), used by the `SDL_SurfaceOwner` sources at `bggame.cc:1278-1280` and `1453-1455` and by `playfli.cc:116`. `SDL_SurfaceOwner` asserts `get_pixel_scale() == 1`.
* `Newfile_gump.cc:214` (`create_buffer` + `get`, later `put`) is a same-scale round trip, so it is correct at any S. The static scratch buffer in `Notebook_gump.cc:501-516` keeps the scale it was created with across S changes. It is only a discard target for `push_render_target`, which is self-consistent at any scale, so it stays unchanged. The other `create_buffer` callers (`mouse.cc:177`, `shapeid.cc:646`, the `bggame.cc` backups) are same-scale `get`/`put` round trips.

### 3.2 Window, present and downscale path (`imagewin/`)

#### 3.2.1 State (`imagewin/imagewin.h:320-409`, constructor `569-578`)

```cpp
Image_buffer*  main_ibuf;            // = ib in the ctor init list (before create_surface runs)
int            world_scale = 1;      // S_eff; 1 = upstream pipeline
bool           world_scaled_failed = false;  // latched by a failed S>1 setup; cleared by a config change
World_presenter presenter;           // new: imagewin/world_present.{h,cc}
Image_buffer::Write_tracker world_writes;
public: int get_world_scale() const { return world_scale; }
        void hires_config_changed() { world_scaled_failed = false; }  // render_scale key, --render-scale, dev reload
```

#### 3.2.2 Scale policy (new, header-only, SDL-free `imagewin/world_scale.h`)

```cpp
enum class World_policy { Off, Art, Auto, Force };
struct World_scale_in { World_policy policy; int force_n; int full_w, full_h; double aspect_y;
                        int out_w, out_h;          // letterbox rect in OUTPUT PIXELS (HiDPI-correct)
                        int s_art /*6*/; int max_tex /*0 → 16384*/; double max_world_mpx /*10*/; };
int compute_world_scale(const World_scale_in&);
```

| Policy | S chosen |
|---|---|
| `Off` | 1 |
| `Art` (default when enabled) | the largest d ∈ divisors(S_art) = {6,3,2,1} with `full·d ≤ max_tex` on both axes (the world texture size) and `full_w·full_h·d² ≤ max_world_mpx·10⁶` |
| `Auto` | the map's D5: take `p = max(out_w/full_w, out_h/(full_h·aspect_y))`, snap `ceil(p)` up to a divisor of S_art, then apply the same caps, **stepping down through divisors only** |
| `Force` (N in 2..8) | N; only the texture limit applies, stepping down by 1. Used for tests and screenshots. An N that is not a divisor of 6 gets NN only unless an `x<N>` folder exists |

S never steps 6→5→4; that would silently drop all ×6 art. The policy table is unit-tested with the
gap_2 configurations:

| Configuration | full | S | r = output/(full·S) | Filter |
|---|---|---|---|---|
| Windowed 1920x1200, 320x200, Fit | 320x200 | **6** | 1.0 × 1.0 | NEAREST (exact) |
| Fullscreen 3440x1440, scale 4, Auto, ACF (the user's active profile) | 860x300 | **6** (9.29 Mpx ≤ 10) | 0.667 × 0.8 | LINEAR |
| Fullscreen preset: scale 6, Auto, ACF | 573x200 | **6** (4.13 Mpx) | 1.0 × 1.2 | PIXELART (SDL ≥ 3.4, shader renderers) / LINEAR |
| Window 1280x800, 320x200 | 320x200 | **6** | 0.667 | LINEAR |
| Window 640x400, 320x200 | 320x200 | **6** | 0.333 | 1 halving + LINEAR |
| Fullscreen scale 2, ACF (1720x600) | 1720x600 | **3** (6 would be 37 Mpx) | 0.667 × 0.8 | LINEAR |
| Studio zoom x1 on 1920x1200 | 1920x1200 | **2** (9.2 Mpx) | 0.5 | LINEAR (exact 2:1 box) |
| Fullscreen scale 1, Fill, 3440x1440 | 3440x1440 | **1** | — | upstream path |
| `--buildmap` | 2048² | **1** (forced) | — | upstream path |

The `max_world_mpx` default (10) is provisional. WP-15 sets it from a Windows measurement (§9 WP-15
decision rule). The user's view is never changed; only S drops (6→3) if the budget fails.

#### 3.2.3 Surface creation

The hook goes in `create_scale_surfaces` (`imagewin.cc:585-770`), **after** the initial clear block
(675-683) and **before** `screen_texture` is created at line 690:

```cpp
if (create_world_scaled_surfaces(w, h)) return true;   // S>1 handled entirely there
```

`create_world_scaled_surfaces(w, h)` is new:
1. Read the request: `config/video/hires/render_scale` plus the session override `--render-scale`. If `world_scaled_failed` is set, set `world_scale = 1` and return false at once.
2. Take the max texture size from `SDL_GetRendererProperties(screen_renderer)` → `SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER` (0 → 16384). Take the letterbox rect L in output pixels from `SDL_GetRenderLogicalPresentationRect`, queried while the window is the render target, right after `SDL_SetRenderLogicalPresentation` (662/670). If L is empty, use `SDL_GetCurrentRenderOutputSize` instead. L is only an input to the `auto` policy here; the filter is re-chosen on every frame (§3.2.4).
3. Set `S = compute_world_scale(...)`, with `full = inter_width/scale × inter_height/scale` (the same `draw_width/height` that line 726 computes) and `aspect_y = 1.2` for the aspect-correct fill modes. If S == 1, set `world_scale = 1` and return false; the upstream code continues unchanged.
4. Create `draw_surface` as INDEX8 of size `full·S + 2·guard_band` per axis (`guard_band` = 4, `imagewin.h:357`, physical), plus its palette.
5. Call `presenter.create(screen_renderer, full_w·S, full_h·S, format)`.
6. Set `inter_surface = paletted_surface = draw_surface`. Apply the two `set_ui_layer_config` calls exactly as lines 766-767 do. Set `world_scale = S` and return true.
7. **On any failure**, release only what this function created:
   * destroy the S× `draw_surface` (its palette is refcounted with the INDEX8 texture, `SDL_render.c:1981`) and null it;
   * call `presenter.destroy()`;
   * leave `inter_surface` and `paletted_surface` untouched (they are still null at this point), and leave the renderer and window alone. **Never call `free_surface()`**: it destroys `screen_renderer` (`imagewin.cc:831-834`), and the upstream code that runs next needs it (690-704);
   * set `world_scale = 1` and `world_scaled_failed = true`, log once, and return false.

   The upstream path then builds a normal S=1 setup. If that fails too, `create_surface`'s point-scaler retry (555-561) re-enters the hook, sees the latch and goes straight to the upstream path again, so it cannot loop on S>1. The latch is cleared only by `hires_config_changed()`. This replaces the fatal throw for S>1 failures (invariant I11).

`screen_texture` and `screen_texture_a` are **not created** at S>1, and every remaining reference to them
is guarded (§3.2.6). The reason is rebase isolation (D-04), not memory. Upstream sizes both from the display
and inter size (`imagewin.cc:691-693, 703-704`): about 20.4 MB each on the 3440x1440 scale-4 profile. The
S>1 world textures replace them and are larger: INDEX8 `world_texture` 9.3 MB, plus `world_rgb` 37.2 MB
when the ladder needs a resolve. `world_rgb` and the halving targets are therefore created **lazily**,
only when the current ratio needs them (§3.2.4).

**Buffer wiring** (`create_surface`, `imagewin.cc:539-578`), a judge must-fix that the review widened.
`ibuf` is the *current* render target (`iwin8.h:63-68`). Upstream writes "main buffer" fields through
`ibuf` in three places, and all three must use `main_ibuf` instead:
* `create_surface` (564-577) runs under an RAII guard, `Ibuf_scope guard(ibuf, main_ibuf);`. It swaps `ibuf = main_ibuf` at the top, **before** its own `free_surface()` call at 541, and restores it in the destructor, so the throw at 560 cannot leave `ibuf` pointing at the wrong buffer;
* `free_surface` (811-842): `main_ibuf->bits = nullptr` replaces `ibuf->bits = nullptr` (829). `resized()` (866) and `toggle_fullscreen()` (1136) call `free_surface()` **before** `create_surface`, outside any guard, and a pushed layer's bits point into its own surface, which stays valid. Writing through `ibuf` there would null a pushed layer or scene buffer instead;
* `~Image_window` (518-520): `delete main_ibuf`, not `delete ibuf`. `ibuf` may be a layer buffer owned by its `Layer`.

This is a separate commit with no change at S=1, and it is offered upstream. The tail of
`create_surface` then always runs:

```cpp
// the upstream statements (564-577), now acting on main_ibuf through the guard; at S>1 replaced by:
if (world_scale > 1) {
    const int S = world_scale;
    main_ibuf->width  = (draw_surface->w - 2*guard_band) / S;     // logical
    main_ibuf->height = (draw_surface->h - 2*guard_band) / S;
    main_ibuf->line_width = draw_surface->pitch;                  // physical
    main_ibuf->offset_x = (get_full_width() - get_game_width()) / 2;   // logical, as today
    main_ibuf->offset_y = (get_full_height() - get_game_height()) / 2;
    main_ibuf->bits = pixels + guard_band*(pitch + 1) + offset_y*S*pitch + offset_x*S;
}
// unconditional, on every path (S>1, S=1, and after a fail-soft fallback):
main_ibuf->pixel_scale = world_scale;
main_ibuf->set_tracker(world_scale > 1 ? &world_writes : nullptr);
world_writes.mark_all(-main_ibuf->offset_x, -main_ibuf->offset_y, main_ibuf->width, main_ibuf->height);
guardband_paint_active = false;
assert(draw_surface->w == main_ibuf->width * world_scale + 2*guard_band &&
       draw_surface->h == main_ibuf->height * world_scale + 2*guard_band);           // I12
```

Setting the scale unconditionally is a **blocker fix**. Without it, a switch from S=6 to S=1 (VideoOptions,
fullscreen toggle, or the I11 fallback) leaves `pixel_scale = 6` on a 1x-sized `draw_surface`. The next
`fill8(0)` then writes S² times past the surface. `guardband_paint_active` is reset as well, so a flag left
from an earlier S=1 frame cannot make `EndPaintIntoGuardBand` (1213-1224) write physical sizes into the
logical fields. `game_width/height` are **not** reset from `saved_*`, because `resized()` has just assigned
the new game size.

`free_surface` gets `presenter.destroy()` **before** `free_layer_textures()` and `SDL_DestroyRenderer`
(831-834). `SDL_DestroyRenderer` frees every texture of that renderer (`SDL_render.c:5672`), so destroying
the presenter afterwards would leave it holding dangling pointers. `World_presenter::destroy()` nulls
every texture pointer, and `create()` asserts that it gets the renderer that is current. Resizes keep
rebuilding everything, as today (map Q16).

#### 3.2.4 Present (`show()`, new `show_world_scaled`, new `World_presenter`)

**Hook** at `imagewin.cc:899`, right after `EndPaintIntoGuardBand();` and before the clip at 902:
`if (world_scale > 1) { show_world_scaled(x, y, w, h); return; }`. Because it sits after the
scene-mode preamble (`mark_all_layers_dirty`, 894-896), it applies to **every** show: scene mode,
pushed targets and the main buffer alike (invariant I6).

`show_world_scaled` first consumes the presenter's reset flags (§3.2.5). Every branch then starts with
`SDL_SetRenderTarget(screen_renderer, nullptr)` and `SDL_RenderClear` (black), as upstream's
`UpdateRect` does (`imagewin.cc:2198`). SDL3 leaves the backbuffer undefined after a present, and the
world covers only the letterbox rect L, so without the clear the bars would show stale swapchain content.

| Case | Behaviour |
|---|---|
| `scene_mode` | Clear, `composite_layers()`, `SDL_RenderPresent`; **no world is drawn**. This matches what upstream shows today, although by a different mechanism than revision 1 claimed. Upstream either skips phase 1/2, because the scene buffer's size differs from `draw_surface` (`imagewin.cc:968`), or it draws the main buffer, when the sizes match: a 320x200 scene on a 320x200 view, as in the user's windowed profile. In that second case the main buffer is black, because `Scene_view::begin_frame` calls `clear_screen()` before the push (`scene_layer.h:106-112`), and the opaque `Fill` scene layer covers it. Either way nothing of the world is visible. |
| `ibuf != main_ibuf` (pushed layer, not a scene) | The world did not change. Clear, draw the last world texture, composite, present. No upload and no tracker consumption. |
| main buffer | `rect = world_writes.take()`; if `EXULT_HIRES_FULL_UPLOAD=1`, the whole buffer instead. In ARGB mode, a palette change makes it the whole buffer. `phys = tracked_to_phys(rect, …)`, `presenter.upload(draw_surface, guard_band, phys)`, then clear, `presenter.draw()`, `composite_layers()`, `SDL_RenderPresent`. |

The upload is driven by the **write tracker**, not by the requested rect. `Game_window::show()` always
requests the full window (`gamewin.h:747-755` → `imagewin.h:683-685`), so the requested rect carries no
information. Uploading the tracked rect is exact by construction: every S>1 write goes through the
tracked helpers. A frame with no writes (mouse move over UI, palette tick) uploads nothing.

**Coordinate conversion** (pure, in `world_scale.h`, unit-tested with offsets ≠ 0). The tracker holds
logical rects relative to the logical origin (§3.1), and the world texture starts at logical
`(−offset_x, −offset_y)`:

```cpp
SDL_Rect tracked_to_phys(TileRect r, int off_x, int off_y, int S, int full_w, int full_h) {
    // texture space: phys = (logical + offset) · S, then clamped to [0, full·S)
    int x0 = (r.x + off_x) * S, y0 = (r.y + off_y) * S, x1 = x0 + r.w * S, y1 = y0 + r.h * S;
    x0 = max(x0, 0); y0 = max(y0, 0); x1 = min(x1, full_w * S); y1 = min(y1, full_h * S);
    return (x1 > x0 && y1 > y0) ? SDL_Rect{x0, y0, x1 - x0, y1 - y0} : SDL_Rect{0, 0, 0, 0};
}
// source pointer for phys: pixels + (guard + phys.y) * pitch + guard + phys.x
```

`World_presenter::upload` clamps `phys` to `[0,tex_w) × [0,tex_h)` again before any `SDL_LockTexture`
or `SDL_UpdateTexture` call, and asserts in debug builds that it did not need to. `SDL_LockTexture`
does not clip (SDL 3.4.18 `SDL_render.c:2629-2667`; the software and D3D11 backends compute the
pointer unchecked), so an out-of-range rect would be a heap write. Revision 1 said only "rect × S".
Taken literally, that gives a shifted rect whenever `offset_x/y ≠ 0`, for example Fit with a 320x200
game on a 1920x1080 window (full 355x200, offset_x = 17).

**`World_presenter`** (new `imagewin/world_present.{h,cc}`). It depends only on SDL, so the data-free
present test can link it without the engine.

```cpp
class World_presenter {
public:
    enum class Format { Argb, Index8 };
    bool create(SDL_Renderer*, int pw, int ph, Format want);     // falls back Index8 → Argb; registers the event watch
    void destroy();                                               // nulls every texture; removes the event watch
    void upload(const SDL_Surface* draw8, int guard, SDL_Rect phys);   // clamps phys; INDEX8: SDL_UpdateTexture; ARGB: lock+LUT
    bool palette_changed(const SDL_Surface* draw8);                   // ARGB only: compares 768 B of RGB
    void draw(const SDL_FRect* logical_dst /*nullptr = logical presentation rect*/, Filter_override);
    Reset_state consume_resets();                                     // flags set by the event watch (§3.2.5)
    Format format() const;
};
```

**Texture state is always explicit.** SDL creates ARGB8888 textures with `SDL_BLENDMODE_BLEND`
(`SDL_render.c:1545`) and every texture with the renderer's default scale mode, which is LINEAR
(`:1546/:1207`). The engine's palette alpha is also indeterminate today: `set_palette` and
`rotate_colors` fill a stack `SDL_Color colors2[256]` with r/g/b only (`iwin8.cc:102-107, 154-159`),
and `SDL_SetPaletteColors` copies the garbage alpha verbatim (`SDL_pixels.c:1203-1207`). A reviewer's
probe on SDL 3.4.18 showed that an INDEX8 resolve with the default blend modes and palette alpha 0
turns all 256,000 pixels black. Therefore:
* **P11** (a prerequisite commit, offered upstream) sets `colors2[i].a = 255` in both functions (I10). At S=1 it does not change any saved image, because the PNG writers copy only r/g/b (`save_screenshot.cc:129-131, 258-260`), and O0 proves it on the 432 buildmap PNGs. At most it makes the live S=1 display deterministic too;
* the presenter sets `SDL_BLENDMODE_NONE` on `world_texture`, `world_rgb` and every halving target, sets the scale mode of every texture explicitly before each use (`SDL_SCALEMODE_NEAREST` on the INDEX8 texture and on any 1:1 pass), and builds the ARGB LUT with alpha forced to `0xFF`, whatever the palette holds.

* **ARGB (baseline; SDL 3.2 API only).** `world_texture` is ARGB8888 STREAMING at `full·S`. `upload` locks the clamped physical rect and converts it through a 256-entry LUT built from the RGB of `SDL_GetSurfacePalette(draw_surface)->colors`. `set_palette` and `rotate_colors` already write that palette (`iwin8.cc:108-117, 160-169`), so `iwin8.cc` needs only P11. A palette change, detected by comparing the 768 RGB bytes with the last LUT (never the alpha bytes, and never `SDL_Palette::version`, which SDL marks internal), forces a full conversion: about 0.9 ms at 1920x1200 and about 3.6 ms at 5160x1800.
* **INDEX8 (SDL ≥ 3.4; WP-14).** Compiled under `#if SDL_VERSION_ATLEAST(3,4,0)`. `world_texture` is INDEX8 STREAMING and gets `SDL_SetTexturePalette(world_texture, SDL_GetSurfacePalette(draw_surface))`. SDL syncs a texture's palette to the shared `SDL_Palette` version at draw time (verified in SDL 3.4.18 `SDL_render.c`), so a palette tick costs no upload. Uploads are raw indices: 2.3 MB per full frame at 1920x1200 against 9.2 MB for ARGB, measured 0.11-0.37 ms against 1.0-1.9 ms (proposal B §3.1).
  * Runtime fallback to ARGB if `SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER` lacks INDEX8, or if creating the texture or setting the palette fails.
  * `present_format=argb` forces the baseline.
* **Resolve before filtering.** An INDEX8 texture is only ever sampled with NEAREST. When the ladder below needs LINEAR, PIXELART or halving, `draw` first renders `world_texture` 1:1 with NEAREST and blend NONE into an ARGB TARGET texture `world_rgb`, then filters that. One extra GPU pass removes the dependence on palette-aware LINEAR or PIXELART sampling, which has not been run on D3D11/12 on the 5070 Ti (map risk 12). `world_rgb` is created lazily on the first frame that needs it.

**Filter ladder** (`choose_world_filter` in `world_scale.h`, pure and unit-tested). It is evaluated
**in every `draw()`**, not cached at surface creation. SDL recomputes the logical presentation
asynchronously on `WINDOW_RESIZED` and `PIXEL_SIZE_CHANGED` (`SDL_render.c:928-934`): async fullscreen
on X11, Wayland and WSLg, and DPI changes. Exult does not rebuild its surfaces on those events
(`exult.cc:2084-2099`). `rx = L.w/tex_w` and `ry = L.h/tex_h`, where L is the letterbox rect in
**output pixels** (`SDL_GetRenderLogicalPresentationRect`; HiDPI-correct, a correctness graft from B):
* L is queried only while the window is the render target. The call returns 0x0 while a texture target is current (`SDL_render.c:3011-3020`, confirmed by probe).
* If `L.w < 1` or `L.h < 1`, the world draw is skipped for that frame.
* The halving depth is bounded: `k ≤ 6`. With r = 0 the unbounded loop would never terminate.
* `world_rgb` and the halving targets are (re)created lazily whenever the required sizes change.

| Condition | Filter |
|---|---|
| `rx == ry`, integer ≥ 1 | NEAREST (exact) |
| `min(rx,ry) ≥ 1`, fractional (ACF 1.2, HiDPI 1.25) | PIXELART if SDL ≥ 3.4 **and** the renderer implements it in a shader, else LINEAR |
| `0.5 ≤ min(rx,ry) < 1` | LINEAR (exactly a 2x2 box at 0.5) |
| `min(rx,ry) < 0.5` | **halving chain**: k exact 2:1 LINEAR passes into ARGB TARGET textures of `ceil(w/2^i)` until `min(r)·2^k ≥ 0.5` (k ≤ 6), then LINEAR into the logical rect |

**PIXELART gating.** The software renderer and `direct3d` (D3D9) silently map PIXELART to NEAREST
(`SDL_stretch.c:88-89`, `SDL_render_d3d.c:1009-1012`; a probe found 0 differing pixels). Non-integer
NEAREST is exactly what the ladder exists to avoid. `choose_world_filter` therefore takes a
`pixelart_ok` input, which is true only when `SDL_GetRendererName` is one of `direct3d11`,
`direct3d12`, `vulkan`, `opengl`, `opengles2`, `gpu` or `metal`. Those renderers implement PIXELART in
shaders. PIXELART output is verified only in the WP-15 Windows matrix; `hires_present` runs on the
software renderer and cannot exercise it.

`present_filter=nearest|linear|pixelart` forces a filter for A/B comparisons. **NEAREST is never
chosen for a downscale**: B measured up to 3.9x the reference shimmer for it. The halving chain costs
about 40 lines and ships in M1 (user must-fix: r < 0.5 is reachable, for example a 640x400 window).

**Drawing** goes into the same logical display rect that `screen_texture_a` covers today
(`SDL_RenderTexture(..., src=content, dst=nullptr)` under the existing LETTERBOX logical
presentation, `imagewin.cc:662/670`). `screen_to_game` / `game_to_screen` (`imagewin.cc:1277-1313`)
use `inter_width/scale`, which equals `full_w`, so mouse mapping is unchanged (map I1).

#### 3.2.5 Palette ticks and device events

* **`Game_window::rotatecolours`** (`gamewin.cc:1054-1079`). Today the blit is forced only when the window is not palettized. `uses_palette` is always true (`imagewin.cc:541`), so the forced blit never happens. Change line 1073 to `if (!win->is_palettized() || win->get_world_scale() > 1) set_painted();`. That gives one present per 100 ms tick at S>1, so cycling water stays visible in idle scenes. The cost is zero bytes with INDEX8, or one LUT conversion with ARGB. S=1 is unchanged.
* **Render resets, through an event watch.** `Handle_event` runs only in the main loop (called at `exult.cc:1386`). About 14 other loops drain the SDL queue themselves: modal gumps (`Gump_manager.cc:1096`), menus, the cheat screen, the intro, scrollers, and `intrinsics.cc:2580`. A reset during a conversation or a menu, which is the likely moment after alt-tab, would be consumed and lost. Upstream uses `SDL_AddEventWatch` for exactly this reason (`exult.cc:815-820`). `World_presenter::create` therefore registers an event watch, and `destroy` removes it. The watch only sets `std::atomic` flags:
  * `SDL_EVENT_RENDER_TARGETS_RESET` → `targets_reset`;
  * `SDL_EVENT_RENDER_DEVICE_RESET` → `device_reset`;
  * `SDL_EVENT_RENDER_DEVICE_LOST` → `device_lost`. SDL 3.4.18 emits the reset and lost events from D3D11, D3D12 and Vulkan (`SDL_render_d3d11.c:1107`, `d3d12:1483`, `vulkan:2537`).
* **Consuming the flags.** `show_world_scaled` calls `consume_resets()` first:
  * targets reset → force a full upload; the resolve and halving targets are re-rendered anyway;
  * device reset → recreate the presenter's textures, call `free_layer_textures()` (the UI layer textures are invalid too, and `composite_layers` recreates them lazily), and force a full upload;
  * device lost → fail soft. Skip drawing for this frame and ask `Game_window` to rebuild the window, which calls `resized()` with the same parameters and creates a new renderer. If the rebuild fails, the I11 latch drops to S=1.

  All of this acts only while `world_scale > 1`. S=1 is unchanged, and the same gap at S=1 is a possible upstream report.

#### 3.2.6 Guard band, screenshots, scalers

* `ShouldPaintIntoGuardband()` (`imagewin.h:801-827`) gets `if (world_scale > 1) return false;` as its **first** line, before `screen_texture->w` is dereferenced. `Begin/EndPaintIntoGuardBand` (`imagewin.cc:1142-1224`) then never activate, so no physical value ever lands in a logical field.
* `FillGuardband()` (1226) returns early when `world_scale > 1`. It memcpys logical widths into physical rows and is called from `menulist.cc:306`.
* `screenshot()` (1254-1271):
  * paletted: `SaveIMG_RW(draw_surface, …, guard_band)` gives an S× indexed PNG cropped by 4 physical px, and needs no change;
  * non-paletted, at S>1: `present_world_frame(/*for_screenshot*/true)` replaces `UpdateRect(nullptr, nullptr, true)` at line 1258. It does the window target, clear, draw and composite, with no present, and is followed by the existing `SDL_RenderReadPixels`.
* **Scalers and `scale_layer_color`** (`imagewin.cc:1738-1781`) are unchanged. There is **no** `assert(scale==1)` in the scalers (map W11 is rejected). `scale_layer_color` runs them with the current target's `line_width/height` patched, and such an assert would fire at S>1.

#### 3.2.7 SDL version handling

* `configure.ac:479-485` stays without a version floor. The fork must compile against upstream CI's SDL 3.2.14.
* Everything from 3.4 (`SDL_SetTexturePalette` on textures, `SDL_SCALEMODE_PIXELART`) sits behind `#if SDL_VERSION_ATLEAST(3,4,0)` inside `world_present.cc` and `world_scale.h`, with runtime checks.
* The local Linux SDL is 3.4.18. MSYS2 ships 3.4.18 too. Both builds therefore compile INDEX8 and PIXELART. PIXELART is used only on shader renderers (§3.2.4), so WSLg with the software or OpenGL renderer decides per renderer. A 3.2.14 build uses ARGB and LINEAR. `exult --version` prints the presenter capabilities.
* **Local SDL 3.2 coverage.** Because both local SDLs are 3.4.18, nothing local would ever compile the `!SDL_VERSION_ATLEAST(3,4,0)` branches. WP-00 therefore builds SDL 3.2.14 into `deps/prefix-3.2` (cmake/ninja, about 5 s, build.md §4.A), and `ci.sh` keeps a `build-sdl32` tree that runs a full build plus `make check` (the ARGB cases of `hires_present`). The optional GitHub fork job is no longer the only guard.

### 3.3 Game window and world renderer

| Anchor | Change |
|---|---|
| `gamewin.cc:541-550` `push/pop_render_target` | No logic change: S travels with the buffer. Add `Image_buffer8* get_main_render_target()` for the harness. |
| `gamewin.cc:916-937` `Game_window::resized` | The toast at 930 appends `" x%d"` with `win->get_world_scale()` when it is > 1. Flats caches detect a scale mismatch per terrain on access, so no flush is needed. A new `request_window_rebuild()` sets a flag; the main loop and `show_world_scaled` act on it by calling `resized()` with the current parameters (device-lost path, §3.2.5). |
| `gamerend.cc:192-291` `paint_map` | (a) Perf scopes per pass (flats, flat RLE, objects, blackness) via the existing `PerformanceTimer` (`perf.h:91-118`). (b) A test-only `Game_render::test_passes` mask (default: all). When it is `PASS_FLATS`, return right after the flats loop (ends at line 233). Only `--render-test passes=flats` sets it. |
| `gamerend.cc:520-531` `paint_chunk_flats` | `Image_buffer8* tgt = gwin->get_win()->get_ib8(); if (auto* c = olist->get_rendered_flats(tgt->get_pixel_scale())) tgt->blit(*c, xoff, yoff);`. Chunks whose 128x128 game-px rect misses the clip are skipped before the cache is touched. Because the scale comes from the **current target**, the in-process A/B harness works through `push_render_target`. |
| `gamerend.cc:328-414` `paint` | No change. The guard band is off at S>1; the border fill goes through the scaled `fill8`. |
| `gamewin.cc:1641-1735` `view_*`, `effects.cc:1829-1873` earthquake | No change. They reach the clipped scaled `copy`. |
| `exult.cc:2857-2912` `BuildGameMap` | `config->set("config/video/hires/render_scale", "off", false)` before the window is created. |
| `exult.cc:1518` main loop | In dev mode, `Hires::poll_reload_trigger()` (one `stat` per 500 ms) right before `gwin->rotatecolours()`. |

### 3.4 Terrain cache (`objs/`)

**Pure fill rule** (new `objs/flat_source.h`, header-only, no engine types):

```cpp
enum class Tile_kind : uint8_t { None, Flat, Flat_void /*12/0*/, Rle };
// Index (0..255, row-major) of the tile whose flat frame is painted at (tx,ty), or -1.
template <class KindFn> int find_flat_source(int tx, int ty, KindFn kind_of);
```

It is a byte-for-byte port of `paint_tile`'s selection (`objs/chunkter.cc:86-133`), **including the
quirks**: the `tiley + y > 0` bound (P1) and the missing 12/0 skip in the full-chunk scan (P2).
`test_flat_source` pins both, so a later upstream merge of P1/P2 becomes a deliberate test update.

**Composition** (`Chunk_terrain`, `objs/chunkter.h:36-101`, `chunkter.cc:248-268`):

```cpp
void Chunk_terrain::paint_flats(Image_buffer8& dst, bool overrides) {
    dst.fill8(0);                                              // P3 (mandatory, also at S=1)
    const int S = dst.get_pixel_scale();
    if (S > 1 && overrides && Hires::terrain(t1_key(), S, dst)) return;   // per-terrain (WP-17); checks dst dims
    for (int ty = 0; ty < 16; ++ty) for (int tx = 0; tx < 16; ++tx) {
        const int src = find_flat_source(tx, ty, kind_of_tile);  if (src < 0) continue;
        const ShapeID& sid = shapes[src];  const Shape_frame* f = sid.get_shape();
        const Hires::Tile_view hi = (S > 1 && overrides)
                ? Hires::flat(sid.get_shapenum(), sid.get_framenum() & 31, S) : Hires::Tile_view{};
        if (hi.px && hi.side == 8*S) dst.put_phys(hi.px, hi.side, hi.side, hi.side, tx*8*S, ty*8*S);
        else    dst.copy8(f->get_data(), 8, 8, tx*8, ty*8);       // NN at S; identical call at S=1
    }
}
```

* At S=1 the write sequence equals today's `paint_tile` loop, plus the leading `fill8(0)` (P3).
* A view whose `side` does not match `8·S` is a bug: it would happen if a store built for another scale survived a resize. It asserts in debug builds and falls back to NN in release, so it can never cause an S_a²-vs-S_b² overrun.
* `paint_tile` becomes a thin wrapper, or is removed; the terrain editor keeps working.
* Precedence: **per-terrain → per-tile → NN**.

**T1 key** (WP-17). The terrain caches `uint64 t1` and `bool t1_valid`; `set_flat` (193) and
`commit_edits` (208-216) invalidate them. The key is computed by `Hires::terrain_key_t1(own_bitmap,
own_pixels)` (§5.2) from the terrain's own tiles. It never uses terrain numbers or the `modified`
flag, because `swap/insert/delete_terrain` renumber terrains (`gamemap.cc:1271-1468`).

**Cache** (`chunkter.h:43, 95-101`):
* `get_rendered_flats(int scale = 1)` re-renders when `rendered_flats->get_pixel_scale() != scale` or `rendered_gen != Hires::generation()`.
* `render_flats(scale)` allocates `Image_buffer8(128, 128, scale)` (value-initialised) and calls `paint_flats(*rendered_flats, true)`.
* `Map_chunk::get_rendered_flats` (`objs/chunks.h:228-230`) forwards the scale.
* `commit_edits` re-renders at the cache's current scale.
* `Figure_queue_size` (`chunkter.cc:234-242`) returns `max(100, (cw+3)·(ch+3))` from the current game area. That is the formula the original author left in comments. It changes performance only, never pixels, and fixes the thrash above 100 chunks. At S=6 the working set is 30 entries (17.7 MB) at 320x200 and 60 (35 MB) at 860x300; the cache may grow to 100 entries (59 MB) and only exceeds that for views whose working set is larger, which the pixel budget keeps rare.

**Other consumers.**
* `Game_map::write_minimap` (`gamemap.cc:1690-1723`) renders each terrain into a local `Image_buffer8 tmp(128,128)` with `paint_flats(tmp, false)` and averages that. It never builds 3,072 S× caches.
* `Game_map::clear_chunks` (`gamemap.cc:259-270`) needs nothing new: destructors free the caches.
* `render_all` (terrain editor, `chunkter.cc:284-313`) and `Shape_frame::paint`'s flat branch (`shapes/vgafile.cc:525-534`) get NN through the scaled `copy8` (§1.4).

### 3.5 Override store and loaders

**SDL-free core** (compiled under `HAVE_PNG_H`; without it, stubs return "no PNG support" and the engine
runs NN):

| File | Content |
|---|---|
| `shapes/hires_png.{h,cc}` | `Png_status read_indexed_png(const std::string& sys_path, int expect_w, int expect_h, Indexed_png& out)`. Uses libpng directly: IHDR, PLTE, tRNS presence, `tEXt` from both info structs (before and after IDAT), depth < 8 expanded with `png_set_packing`, **raw indices with no rotation**. Hardening: (1) `png_set_user_limits(png, 128·S_art, 128·S_art)` and `png_set_chunk_malloc_max` (1 MB) before reading; (2) after `png_read_info`, the IHDR width and height must equal `expect_w × expect_h` (F3), checked **before** `png_read_update_info` and before any row buffer is allocated, so a stray huge PNG in a pack dir cannot cause a giant allocation; (3) all libpng calls live in one function whose post-`setjmp` locals are POD or `volatile`, and every heap buffer is owned by a struct constructed **before** `setjmp`, so a `longjmp` skips no destructor and leaks nothing on truncated or partly written files. `write_indexed_png(...)` (tEXt support) is for the tools inside the engine and the tests. `Import_png8`'s own leak is fixed separately by an upstreamable commit: the `setjmp` handler at `pngio.cc:80-84` returns without freeing `palette` (allocated at 107) or `pixels`/`image` (137-138). The fix frees and nulls them there; they are reference parameters, so they survive the `longjmp`. |
| `shapes/hires_rules.{h,cc}` | CRC32 (IEEE, table-driven, no zlib dependency), FNV-1a-64, `terrain_key_t1`, the cycle-range table (`gamewin.cc:1063-1068`), the rule checks of §5.5 (one `p4_violations()` shared with `hirescheck.py` through fixtures), and `reduce_mode(src, S_from, S_to, parent1x)` (§5.6). |
| `shapes/hires_bundle.{h,cc}` | Reader for `x<S>/flats.bundle` (§5.7): a single read, then size and header checks before any entry is used. |
| `shapes/hires_store.{h,cc}` | `Hires::Store`, below. |

```cpp
namespace Hires {
struct Root { std::string sys_path; std::string label; };      // precedence order
struct Pal8 { uint8_t rgb[768]; };                              // effective palette 0, min(255, v*255/63) (= Get_color8)
using Src_provider = std::function<const uint8_t*(int shape, int frame)>;  // 64 B or nullptr (not a flat / out of range)
struct Tile_view { const uint8_t* px = nullptr; int side = 0; };  // side = 8·S of the store that produced it
struct Report { int tiles=0, loaded=0, rejected=0, groups_skipped=0, unguarded=0, warnings=0, terrains=0, bundled=0;
                std::vector<std::string> lines; double ms=0; };
class Store {
public:
    Report load(const std::vector<Root>&, int scale, int s_art, const Pal8&, const Src_provider&);
    Tile_view flat(int shape, int frame) const;                 // {8S×8S indices, 8S} or {}
    bool  terrain(uint64_t key, uint8_t* dst, int dst_w, int dst_h, int dst_pitch);
          // decodes on demand (≈1.2 ms per 768²); returns false unless the post-reduction size == dst_w × dst_h
    const Entry_info* explain_flat(int shape, int frame) const; // state, path, rule, detail
    const Entry_info* explain_terrain(uint64_t key) const;
};
}
```

* **Loading is eager, synchronous and deterministic.** For each root, `x<S>/flats.bundle` is read first if present, then the loose files in `x<S>/flats` (or `x<S_art>/…` plus reduction, §5.6) are scanned with `std::filesystem` (already used, `gamedat.cc:50`) and the names are sorted. A loose file overrides a bundle entry with the same key. Every tile is fully validated, including the guard and cycle rules, using the `Src_provider`. Group strictness (loose directories only) is decided before activation.
* `x<S>/terrain/*.png` is only **indexed** (name parsed); files are validated when decoded on first use, and a reject falls back to per-tile/NN with a log line.
* `Store::load`, `Store::terrain` and `reload()` catch `std::bad_alloc` and reader errors at their boundary. A failure rejects the entry (or disables the root) with a log line and never propagates into paint (I11).
* BG flats take about 9 MB at S=6. The load-time target is under 0.5 s. That holds for a bundle from any disk and for loose files on local disks (Windows reading `E:`: 0.25-0.31 s for 3,885 files), but **not** for loose files over drvfs from WSL (17.6 s; §5.7). WP-15 measures the result (decision rule there).

**Engine glue** (new top-level `hires_glue.{h,cc}`):
* Reads config (§4) and builds roots from path tags.
* Loads the effective palette 0 once per `Shape_manager::load` with a temporary `Palette::load(PALETTES_FLX, PATCH_PALETTES, 0)` (pattern at `palette.cc:141`; no `apply()`, so no border-colour swap) and `get_red/green/blue` (`palette.h`). It converts with the engine's own clamp, `min(255, v·255/63)` (`Get_color8` at brightness 100, `iwin8.cc:84-90`). This matters for index 255, whose 6-bit values (250, 64, 1) are out of range in BG: a uint8 wrap would give a different CRC (0x78d19732 instead of 0xc9c2c0e7; §5.4).
* Provides the `Src_provider` through `sman->get_shapes().get_shape(shape, frame)`. That is the effective `shapes.vga`, patch included; it rejects RLE frames as "not a flat". It **bound-checks first**: `if (shape < 0 || shape >= shapes.get_num_shapes()) return nullptr;`, because `Vga_file::get_shape` indexes `shapes[shapenum]` without a range check (`vgafile.h:357-376`), and the file-name pattern admits 0-9999.
* Owns the `Store` per scale, exposing `Hires::flat`, `Hires::terrain`, `generation()`, `invalidate()`, `reload()`, `set_enabled()` and `explain_at(tx,ty)`.
* Logs with the prefix `[hires]` to stdout (Exult's `stdout.txt` on Windows).

**Hooks:**
* `Hires::invalidate()` at the start of `Shape_manager::load` (`shapeid.cc:150`, game or mod switch) and in `Shape_manager::reload_shapes` (`shapeid.cc:420`, ExultStudio).
* The store reloads lazily on the next `flat()` call and bumps the generation.

**Path tag** (`gamemgr/modmgr.cc`):
* At 592-617, register `<{PREFIX}_HIRES>` from `config/disk/game/<name>/hires_path` (default `$game_path/hires`), next to `_PATCH`.
* In `setup_game_paths` (56-85), `clone_system_path("<HIRES>", "<" + path_prefix + "_HIRES>")`. It is **not** mod-specific, so the base pack survives mod activation; `<PATCH>` is replaced by the mod's dir (`modmgr.cc:70-74`).
* Roots in precedence order: `<PATCH>/hires`, then `<HIRES>` (only those defined and existing).

### 3.6 Shapes and sprite hook (M2, outline)

The smallest hook that can carry real sprite art (from proposal A §7), upgradable to map D8:

1. **Fixes first:** P6 (`encode_rle` `runs[200]` → vector, `vgafile.cc:295`), P7 (empty-frame null write, `vgafile.cc:467-470`), P8 (`reload_shapes` kind mapping, `shapeid.cc:420-468`). Each is an upstreamable commit with a test.
2. **Store:** `x6/shapes/<vga>/SSSS_FF.png`, raw index, tRNS only on 255, `oFFs = (−xright_h, −ybelow_h)` (map §4.3 G1-G5). At load, each key is resolved with `get_shape(shape, frame)`, its canonical geometry validated, encoded to RLE and recorded in a **side table** keyed by `(vga file id, shape, frame)` together with the store generation. The table is **not** keyed by a raw `Shape_frame*`. Frames are freed and reallocated, often at the same address, by paths that do not call `Hires::invalidate()`: `Shape::reset/set_frame/del_frame` (`vgafile.cc:1008, 1038, 1064`) and transient owners such as the stack `Mouse` of the menu (`game.cc:582`). A pointer key would then return a stale entry with the wrong geometry. The paint hook passes the identity it already has: `ShapeID`, or the `(file, shape, frame)` of the cache lookup. There is still no I/O in `Vga_file::get_shape` and no slot in `Shape_frame`.
3. **Paint:** in `Shape_frame::paint_rle*` (`vgafile.cc:483-699`), `if (win->get_pixel_scale() > 1) if (auto* h = Hires::frame(id, S)) { paint h through a physical view at (x·S, y·S); return; }`. Culling stays logical. The physical view is a second write path, so it must not bypass the invariants:
   * its clip is `main.clip × S`, so dirty-rect painting stays correct;
   * it carries the main buffer's tracker, converting the physical bbox to logical with floor for the start and ceil for the end, so D-06 ("cannot miss a write") still holds;
   * O1's `tracker_complete` property is extended to view writes.
4. **Reflections** are derived by transposing the hi-res base (map R1). An explicit `f|32` is allowed only under rule R2.
5. **CRC gate is mandatory**: mods patch 70-263 shapes (gap_6). Canonical-frame CRC32; a mismatch falls back to NN.
6. **Strict groups** use the same directory rule. The map's derived groups (animation cycles, actor frames 0-31, missiles, barges) become validator suggestions.
7. **Memory:** eager while packs are partial. Above about 200 MB, switch to the map's D8 (lazy payload, frame-boundary LRU). The side table keeps that change local.
8. `--dump-art` is extended to all eight `Vga_file`s (map H15). `--render-test mode=dynamic` uses pinned saves. Outlines come from the hi-res mask.

### 3.7 UI (M3, outline)

* Per-layer **content scale k** (map D9): the layer buffer is `logw·k × logh·k` with `pixel_scale = k`, the logical size is unchanged, and the composite source rect is physical.
* Fonts, gumps, faces, the cursor and the dragged item use the M2 `Shape_frame` hook through `push_render_target`.
* Fix the layer width mismatch (`imagewin.cc:2141` vs `iwin8.cc:343,403`).
* Optionally, INDEX8 layer textures, so palette ticks stop re-converting every layer (B §4.8).

### 3.8 Developer loop (M1b)

Enabled by `config/video/hires/dev=yes`. Three `Action::cheat_keys` entries go into the `keys.cc`
table (pattern at `keys.cc:129-145`), with the handlers in a new `hires_dev.cc`. Default bindings are
appended to `data/bg/defaultkeys.txt` and the SI file; these combinations are unused today.

| Action | Key | Effect |
|---|---|---|
| `HIRES_TOGGLE` | Ctrl-Alt-O | `Hires::set_enabled(!enabled)`, generation++, `set_all_dirty()`. An A/B at the same S. |
| `HIRES_RELOAD` | Ctrl-Alt-R | `Hires::reload()`: rescan and revalidate all roots, generation++, `set_all_dirty()`, then a toast with the summary (`loaded 3880, rejected 5 (see log)`). |
| `HIRES_INSPECT` | Ctrl-Alt-I | Mouse → `screen_to_game` → tile (tx,ty), then terrain number, T1 key, cell, own (shape:frame), effective source (from `find_flat_source`) and the result (`TERRAIN <path>`, `TILE <path>`, or `NN (<state>: <rule> <detail>)`). Shown as `center_text`, written to stdout, and copied to the clipboard (`SDL_SetClipboardText`). |

* **Trigger file:** every 500 ms in dev mode, the `mtime` of `<root>/x<S>/.reload` is checked; a change calls `reload()`. Each engine watches its own pack copy (§5.7). The Windows engine reads `E:` natively, so `publish.sh` and ComfyUI tools touch `E:\…\x6\.reload`. A WSL engine reads the ext4 copy, so WSL tools touch the ext4 `.reload`. Art made in WSL or ComfyUI therefore shows up in whichever engine is running.
* **Reload is the single sync point.** A partly written PNG fails to read, is rejected, and is retried on the next reload. Tools write to `*.tmp` and rename.
* Not included (C's extras, see §11): in-game template export (`--dump-art` writes templates instead), fallback overlay, recorder, named sets.

### 3.9 CLI entry points (new `render_test.cc`, `dump_art.cc`)

Declared next to `--buildmap` (`exult.cc:289-312`), skipped by the `setup_video` guard (`exult.cc:825`),
dispatched next to it (`exult.cc:986-989`), and modelled on `BuildGameMap` (`exult.cc:2857-2912`); see §4.2.

---

## 4. Configuration keys, CLI flags, defaults

### 4.1 Configuration (`config/video/hires/…`, read in `setup_video`, `exult.cc:3128-3304`)

| Key | Values | Default | Meaning |
|---|---|---|---|
| `render_scale` | `off` (or `1`) \| `art` \| `auto` \| `force:N` | `off` | Policy (§3.2.2). The user's configs set `art`. |
| `art_scale` | 6 | 6 | S_art; must match the pack folders |
| `max_world_mpx` | number | 10 (set finally by WP-15) | Pixel budget for `full·S²` |
| `overrides` | `yes` \| `no` | `yes` | `no` = pure NN (oracles, A/B) |
| `present_format` | `auto` \| `argb` \| `index8` | `auto` | `auto` = INDEX8 when compiled and accepted at runtime |
| `present_filter` | `auto` \| `nearest` \| `linear` \| `pixelart` | `auto` | Force a filter for A/B comparison and screenshots |
| `dev` | `no` \| `yes` | `no` | Dev keys, `.reload` poll, verbose `[hires]` log |
| `config/disk/game/<g>/hires_path` | path | `$game_path/hires` | `<HIRES>` root. Windows config: `E:\Dati\Ultima7_Upscale\packs\bg`. WSL interactive config: the ext4 copy `/home/simonea/ultima7_exult/packs/bg`. Test configs: scratch dirs (§6.4). Never a loose pack on `/mnt/e` from WSL (§5.7). |

These keys are independent of `scale`, `scale_method` and `fill_scaler`, which keep their meaning: game
area divisor, mouse mapping and the UI-layer scalers. When `world_scale > 1`, the world scaler is
simply not used. Changing `render_scale` (VideoOptions revert, `--render-scale`, dev reload) calls
`hires_config_changed()`, which clears the fail-soft latch (§3.2.3).

**The user's recommended configurations:**
* windowed 1920x1200, 320x200 Fit, `render_scale=art` → S=6 at 1:1;
* fullscreen 3440x1440, scale 4, Auto, ACF, `render_scale=art` → S=6 (or 3, per WP-15) on the user's own 860x300 view.

The 573x200 preset (scale 6) is documented as a sharper alternative, not as a requirement.

Environment (debug): `EXULT_HIRES_FULL_UPLOAD=1` disables the write tracker.

### 4.2 CLI

| Flag | Purpose |
|---|---|
| `--render-scale <off\|art\|auto\|force:N>` | Session override of `render_scale` (tests, the Windows matrix) |
| `--render-test "<k=v,…>"` | Headless region renderer and oracles (below) |
| `--dump-art <dir>` | Writes the engine's reference set for art production (below) |

**`--render-test` keys:**

| Key | Values | Meaning |
|---|---|---|
| `tx,ty,w,h` | | Top-left tile and region size in game px |
| `lift` | 16, 10, 5 | Roof mode, as in BuildGameMap |
| `scales` | `2:3:6` | List of S values |
| `mode` | `nn` \| `plain` | `plain` writes images only (QA previews, art review) |
| `overrides` | `no` \| `yes` | |
| `passes` | `all` \| `flats` | `flats` makes marker predictions exact |
| `repaint` | N | O6 random sub-rect repaints |
| `present` | 0 \| 1 | Windowed run through the real present path with read-back |
| `format` | `argb` \| `index8` | With `present=1` |
| `filter` | `auto` \| … | With `present=1` |
| `window` | `WxH` | Window size for `present=1` (default: 1:1 with `full·S`). `1280x800` gives a non-1:1 geometry (r = 0.667, LINEAR) |
| `game` | `WxH` | Game area inside the full area. For example `game=320x200` in a 355x200 full area gives `offset_x = 17`, to test the offset paths |
| `resize` | `force6:off:force3:force6` | After the first render, cycle S through these policies with `Game_window::resized` (fullscreen off), re-render and re-check NN equality after each step. This is the S-transition regression (I12) |
| `pushed_resize` | 0 \| 1 | Push a layer buffer, call `resized()` (and the fullscreen toggle path), pop, then assert that the layer's `bits` are unchanged and the main buffer satisfies I12 |
| `bench` | N | Median and p95 of world paint, and of upload and present when `present=1` |
| `inspect` | `tx:ty` | JSON of `explain_at` |
| `expect` | `nn` \| `identity` \| `marker:<idx>` | The oracle to assert |
| `seed` | 1 | `srand` after `init_files`, overriding `gamewin.cc:587-588` |
| `out` | DIR | Output directory |

Outputs:
* `ref_1x.png` and `hi_S.png` (indexed, raw);
* `diff.png` on failure;
* `digest.json` (FNV-1a-64 per image, light-source count, mini-screenshot digest, timings).

Exit 0 means pass, 1 means fail. The sequence follows gap_7 §5: gamma 1, `Game_window` at scale 1, `create_game`, `init_files(false)`, `srand(seed)`, static map, palette 0. A/B goes through `push_render_target` with `Image_buffer8(w,h)` and `Image_buffer8(w,h,S)`.

**`--dump-art <dir>`** (M1 subset: flats and terrain; M2 extends it to every `Vga_file`):

```
<dir>/ref.txt              game, mod, engine git rev, palette CRC32, "png=raw-index,no-rotation", "terrain_key=T1", "crc=C1"
<dir>/palette/pal0.gpl     GIMP/Aseprite palette (8-bit, v*255/63); pal0.act (768 B); classes.txt (index → static|cycle:E0-E7|…|reserved)
<dir>/flats/SSSS_FF.png    1x raw-index flats, PLTE = pal0, tEXt Exult-Src-CRC32
<dir>/flats.txt            shape frame crc32 map_uses cycle_px used_on_map
<dir>/templates/x6/flats/SSSS/SSSS_FF.png   NN×6 templates with guard (= the identity pack; modder starting point)
<dir>/terrain/<t1>.png     1x 128² flat layer painted by the engine (paint_flats, overrides off): AI context
<dir>/terrain.txt          tnum t1 uses own_cells rle_cells missing_cells duplicate_of
<dir>/terrain_tiles.bin    "U7HR" v1; per terrain 256×{own shape u16, frame u8, kind u8} + 256×{effective source u16,u8,u8}
<dir>/terrain_map.bin      "U7HR" v1; per map 192×192 u16 terrain numbers
```

The dump makes **the engine the single source of truth** for art inputs (fill, keys, CRCs). The Python
re-implementation of `paint_tile` in `u7art.py` survives only as a parity cross-check.

---

## 5. Override files: layout, naming, manifest, validation, packaging, hot reload

### 5.1 Directory layout

```
<root>/                                  <PATCH>/hires  or  <HIRES> (= hires_path; user: E:\Dati\Ultima7_Upscale\packs\bg)
  pack.txt                               optional manifest (§5.4)
  x6/
    .reload                              dev trigger (touched by tools)
    flats.bundle                         generated art, one file, entries independent (§5.7)
    flats/                               loose files override bundle entries with the same key
      0123_04.png                        single-tile override
      0019/                              strict group (tools group by shape by default)
        0019_00.png … 0019_31.png
      water/                             any name = custom material group
      0042.off/                          disabled group (skipped)
      _work/, *.json, *.txt, README      ignored by the engine (templates, provenance sidecars)
    terrain/
      9a1c0e44b2d6f001.png               per-terrain override, 768×768, key = T1 (lowercase hex)
  x3/ …                                  optional hand-made art for S=3 (otherwise reduced from x6)
```

### 5.2 Keys and naming

* **Per tile:** `^(\d{4})_(\d{2})\.png$` gives shape and frame (< 32). The key is `(shape, frame & 31)`, matching `vgafile.cc:912-914`. The shape bound is not a constant: the provider rejects anything `≥ get_num_shapes()` of the effective `shapes.vga` (N1), because DEVEL games can have fewer shapes and mods more.
* **Per terrain:** `^[0-9a-f]{16}\.png$`. **T1 key** = FNV-1a-64 (offset `0xcbf29ce484222325`, prime `0x100000001b3`) over the byte stream `"U7TK" | 0x01 | own[32] | pix[16384]`.
  * `own` is a 256-bit map, LSB first, in row-major tile order. Bit t is set when tile t resolves to a non-RLE frame (`get_shape()` is non-null and `!is_rle()`, `objs/chunkter.h:85-86`).
  * `pix` is the 128×128 raster in which own cells hold their 64 flat pixels and every other cell holds zeros.
  * The key does not depend on the fill heuristic (P1/P2). It changes on any flat-pixel edit, terrain edit, or flat/RLE change.
  * Known limitation: terrains with identical own flats but different RLE objects share one key. `terrain.txt` lists them as `duplicate_of`, and art tools must make the under-RLE fill fit all of them, or skip that key.
* Test vectors for CRC32, FNV and T1 are shared by doctest and pytest in `tests/data/hires/hash_vectors.txt`.

### 5.3 PNG contract

* 8-bit palette PNG (colour type 3; depths 1/2/4 are accepted and expanded).
* **Raw engine indices**: no ipack/Studio +1 rotation (`pngio.cc:215-252`).
* PLTE = effective palette 0 at `v·255/63` for every index used.
* Size `8S × 8S` (flats) or `128S × 128S` (terrain).
* No `oFFs` needed for flats or terrain.
* Optional `tEXt`:
  * `Exult-Src-CRC32` (8 hex): CRC32 of the 64-byte 1x flat the tile was made from (the guard);
  * `Exult-Terrain-Key`: must equal the file name if present;
  * `Exult-Origin`: free text (route).
* The guard is **optional**. Aseprite and GIMP drop unknown `tEXt` on save, so a hand-edited tile simply becomes "unguarded" (accepted, counted in the report). `tools/hires/hirescheck.py --restamp` adds it back from the dump.

### 5.4 Manifest (optional `pack.txt`, `key=value` lines)

`game=BG`, `scale=6`, `palette_crc32=…`, `edge=none|nn3`, `route=…`, `title=…`.

The engine reads only `palette_crc32`. A mismatch with the effective palette 0 **disables the whole
root** with one clear message, which catches a BG pack used with SI. The tools use the other keys.

`palette_crc32` is the CRC32 of the 768 bytes `Pal8.rgb` (§3.5): palette 0 from `Palette::load`
without `apply()`, converted with `min(255, v·255/63)` per channel, index 255 included. For BG that is
`0xc9c2c0e7`; a uint8 wrap at index 255 would give `0x78d19732`. The 768-byte vector and its CRC go
into `tests/data/hires/hash_vectors.txt`. `mkpack` **copies** `palette_crc32` from the dump's `ref.txt`
and never recomputes it, so the engine stays the single source of truth.
Without `pack.txt`, a root is loaded file by file. **The directory structure is the manifest**: there
are no named sets, families or priorities in M1.

### 5.5 Validation rules (engine, at load or first decode)

| ID | Rule | Severity | Typical cause (and hint printed) |
|---|---|---|---|
| N1 | Name matches §5.2; frame < 32; `0 ≤ shape < get_num_shapes()`; the provider returns a flat for (shape, frame) | reject | typo; RLE shape number; shape beyond the file |
| F1 | Colour type 3 (palette) | reject | "RGB/RGBA image: quantize to palette 0 first (tools/hires/quantize)" |
| F2 | For every **used** index i: first `i < num_palette` (otherwise reject, detail "index i ≥ PLTE size n"), then `PLTE[i] == pal0_8bit[i]` | reject (names the first bad index) | GIMP "remove unused colors" reorders or shortens the colormap; editor palette optimisation; ipack ×4 colours (hint: "load palette/pal0.gpl, keep indices"). libpng only warns about out-of-range indices, so such images do load |
| F3 | Exact size, checked on IHDR before any pixel buffer is allocated | reject | |
| F4 | `tRNS` present | **warning** only; indices are read raw | Aseprite indexed export with a transparent colour set |
| P0 | Index 0xFF in a flat or terrain | reject | border/transparent colour |
| P4 | **One definition everywhere** (engine, `hirescheck.py`, QA A3, the route-3 generator): a pixel in cycle range R (E0-E7, E8-EF, F0-F3, F4-F7, F8-FB, FC-FE) is compliant iff its 1x parent pixel or one of the parent's 8 neighbours is in R. Neighbours are taken **inside the override's own 1x source and clamped at its edge**: the 8×8 flat for a tile, the 128×128 flat layer of that terrain (passed in by the glue at decode) for a terrain override | engine: reject if > 0.5 % of pixels are non-compliant, else warning. Generated art must have 0 (QA A3) | AI output, wrong quantizer, context upscaling that pulls cycling pixels across tile borders |
| G1 | `Exult-Src-CRC32` present and ≠ CRC32 of the live 1x flat | reject ("stale: built against different shapes.vga") | mod or patch changed the flat |
| G2 | Any reject inside a group directory | the whole group is skipped and named | strictness (loose groups only; bundle entries are independent) |
| R1 | `pack.txt` `palette_crc32` ≠ CRC32 of `Pal8.rgb` (§5.4) | root disabled | wrong game |
| B0 | Bundle header invalid (magic, version, scale ≠ S, `palette_crc32` ≠ R1's value) or file size ≠ header + count × entry size | bundle disabled; loose files still load | truncated copy, wrong game |

**Offline only** (in `hirescheck.py`, because they need ramps or neighbours):
* P2: the ramp of each block equals the ramp of the 1x parent (`Palette::get_ramps`);
* E1: the edge contract declared in `pack.txt`;
* the QA metrics of §8.3.

The engine's rule IDs and the validator's are identical. Fixture PNGs with expected rule IDs live in
`tests/data/hires/rules/`; both test suites use them.

**Edge contract.** `nn3` means the outer `B = S/2 = 3` px band of every flat (and the outer band of
every 768² terrain override) equals the NN replication of that override's own 1x border pixels, with
corners taking the corner pixel. Per-tile, per-terrain and NN neighbours then meet at the original 1x
step. The pack declares `none` or `nn3`. The choice is made in Phase A by in-engine A/B plus seam
metrics (§8.2 A4).

### 5.6 Art for S_eff < S_art

* If `x<S>` exists in a root, it is used.
* Otherwise, when `6 % S == 0`, the x6 art is reduced at load by m = 6/S with the **class-preserving mode filter** (`hires_rules.cc::reduce_mode`), per m×m block:
  * if the 1x parent pixel is in cycle range R and the block contains indices in R, take the most frequent index among those;
  * otherwise take the most frequent non-cycling index (the most frequent index if none);
  * ties go to the smallest index.
* The filter is deterministic and index-safe, and `reduce(NN6(x)) == NN_{6/m}(x)` (unit test).
* Validation (including G1 and P4) runs on the x6 data, before reduction.

### 5.7 Packaging and hot reload

* **Measured constraint.** From WSL, opening a file on `/mnt/e` (drvfs over 9p) costs about 4.5 ms. Reading the 3,885 loose flat PNGs (3.44 MB) took **17.6 s**, the same warm or cold. The same files take 0.03 s on ext4, and 0.25-0.31 s for native Windows reading `E:`. Bulk reads over 9p are fast: 12 superchunk PNGs, 3.9 MB, in 0.06 s. Per-file open cost dominates, so a loose-file pack must never be loaded by a WSL engine from `/mnt/e`.
* **Two copies, one source of truth.** WSL tools write the canonical pack on ext4 (`/home/simonea/ultima7_exult/packs/<name>/`). `tools/hires/publish.sh <name>` mirrors it to `E:\Dati\Ultima7_Upscale\packs\<name>` (`rsync --checksum` of the bundle plus the changed loose files), then touches `.reload` on both sides. The Windows `exult.cfg` points at `E:`; the WSL interactive config points at the ext4 copy; test configs use scratch dirs (§6.4).
* **Bundle (M1b, built in WP-08/WP-12; no longer a WP-15 contingency).** Generated art is written by `mkpack.py` as `x6/flats.bundle`:
  * header `"U7HB"`, version 1, scale, `palette_crc32`, entry count;
  * then N × `{shape u16, frame u8, flags u8 (bit0: guard present), guard u32, 8S·8S indices}`, sorted by key;
  * read in **one** call, about 9 MB, so it is fast from any disk.

  Every entry is validated like a loose tile (N1, P0, P4, G1). F1-F3 hold by construction, and F2 is covered by the header CRC (B0). **Entries are independent**: a bundle has no strict groups, so one bad tile never drops a whole shape. Loose PNGs override bundle entries with the same key, so hand edits stay simple. Strict groups (G2) are a loose-directory feature for curated sets.
* **Development and personal use:** loose PNG directories for hand-made or curated art, the bundle for generated art. EA-derived packs are never committed or distributed. The pipeline and tools are.
* **Hot reload:** dev mode only (§3.8). Ctrl-Alt-R or a touched `.reload`. Tools write `*.tmp`, rename, then touch `.reload`.
* **M2 shipping format:** the sparse companion VGA (map D8), required only where libpng is missing.

---

## 6. Test strategy

### 6.1 Framework and layout

* **C++:** doctest, a single header (MIT; works with g++ 9.4, mingw and MSVC), vendored as `tests/doctest.h` with its version and SHA-256 recorded in `tests/README`.
* **Python:** pytest in the existing `tools-venv`. It is **not installed yet** (`import pytest` fails). WP-01 installs it pinned (`~/.local/bin/uv pip install --python tools-venv/bin/python -r tools/hires/requirements.txt`, with `pytest==<pinned>` next to the numpy and Pillow pins), and `ci.sh` repeats that bootstrap.
* **Game-data tests:** shell drivers that exit 77 (skip) when `U7_BG_STATIC` is unset.

```
tests/
  doctest.h  README  Makefile.am
  unit/      main.cc, test_*.cc, rle_writer.h (test-only RLE encoder: any width, malformed runs on demand)
  present/   test_present.cc
  data/hires/ hash_vectors.txt, rules/*.png + expected.txt, editor/*.png (synthetic palette & content only)
  data/ibuf_golden.txt      digests recorded on unmodified upstream ibuf8.cc (WP-01)
  game/      test.cfg.in, buildmap_golden.sh, render_regions.sh, resize_cycle.sh, regen_goldens.sh, regions.txt,
             golden/*.sha256 (hash lists only, never pixels), perf_baseline.json
```

**Link plan** (a judge must-fix, corrected by the review). The tests link the existing **libtool
convenience libraries**, as `tools/ipack` already does (`tools/Makefile.am:80-84`; `build-linux/tools/ipack`
links today without engine objects). A convenience archive contributes only the members a program
references, so `imagewin.o` and the engine never get pulled in. Revision 1's wrapper translation units
are dropped.
* `hires_unit`: unit/*.cc; `LDADD = ../shapes/libshapes.la ../imagewin/libimagewin.la ../files/libu7file.la $(PNG_LIBS) $(ZLIB_LIBS)`, in dependency order. `Shape_frame(pixels,…)` and `encode_rle` live in `shapes/vgafile.cc`, which needs `Flex`, `U7exists`, `IFileDataSource` and friends from `libu7file`. A reviewer's link test showed that `vgafile.cc` + `ibuf8.cc` + `imagebuf.cc` + `libu7file.a` link and run with no SDL; the `sdlrwops` objects are not pulled in.
* `hires_present`: present/*.cc; `LDADD = ../imagewin/libimagewin.la $(SDL_LIBS)`.
* New sources go into the libraries' unconditional parts: `ibuf8_scaled.cc` and `world_present.cc` into `libimagewin_la_SOURCES` next to `ibuf8.cc` (outside `if BUILD_EXULT`), and `hires_{png,rules,bundle,store}.cc` into `libshapes_la_SOURCES` next to `vgafile.cc`. `ibuf8_scaled.cc` must be registered **everywhere `ibuf8.cc` is compiled**, because the hooks in `ibuf8.cc` reference it: libimagewin, `Makefile.common`, the vcxproj, Xcode and the ExultStudio/tools lists.
* `test_ibuf_golden` uses `Shape_frame(pixels,…)` only for small, realistic frames (width ≤ 64; `encode_rle`'s `runs[200]`, P6, stays out of reach). Wide scans (≥ 4096 px) and malformed runs come from `rle_writer.h` and go to `Image_buffer8::paint_rle` directly.
* Autotools: `tests/Makefile.am` with `check_PROGRAMS = hires_unit hires_present`, `TESTS = $(check_PROGRAMS)`, `AM_TESTS_ENVIRONMENT = SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software SDL_AUDIO_DRIVER=dummy; export SDL_VIDEO_DRIVER SDL_RENDER_DRIVER SDL_AUDIO_DRIVER;` and `LOG_COMPILER = @TEST_WRAPPER@`. `TEST_WRAPPER` is an `AC_ARG_VAR`, empty by default; `build-asan` sets it to `env ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 setarch x86_64 -R timeout 900` (§6.5, §7.1). Add the directory to `SUBDIRS` (`Makefile.am:11-13`) and `AC_CONFIG_FILES` (`configure.ac:1351-1399`). `check-game` is a separate target that runs `tests/game/*.sh`.
* `Makefile.common` has no convenience archives, so it lists objects explicitly. `HIRES_UNIT_OBJS` = `imagewin/{ibuf8,imagebuf,ibuf8_scaled}.o`, `shapes/{vgafile,pngio,hires_png,hires_rules,hires_bundle,hires_store}.o`, plus `$(FILE_OBJS)`. On Windows that also links the `sdlrwops` objects and therefore `$(SDL_LIBS)`, which is harmless there. `HIRES_PRESENT_OBJS` = `imagewin/world_present.o`. The targets are `hires_unit$(EXEEXT)`, `hires_present$(EXEEXT)` and `check-hires`, so `Makefile.mingw` builds and runs them on Windows. Also add `shapes/pngio.o` and the new hi-res objects to `SHAPES_OBJS` (`Makefile.common:231-258`).
* `hires_present` exits 77 when `SDL_Init(VIDEO)` or software-renderer creation fails.

### 6.2 Unit tests (`make check`, CI-safe, synthetic data)

| Test | Asserts |
|---|---|
| `test_ibuf_golden` | A fixed-seed stream of 20k ops over all primitives (random clips, layer-ctor pitches, real-shaped xform tables, small RLE frames built with `Shape_frame(pixels,…)`) on S=1 buffers. The FNV digest after every 1k ops must equal `tests/data/ibuf_golden.txt`, **recorded on unmodified upstream code** in WP-01. Proves I2 for the core. `copy` rects in this stream are always in range, because upstream's S=1 `copy` does not clip (`ibuf8.cc:43-68`) and the recorder must not run into undefined behaviour. |
| `test_ibuf_scaled` (O1) | The same in-range op stream on an S=1 reference and on S=k buffers (k ∈ {2,3,6}, 5 seeds, both owned and view ctors with negative offsets). After every op, `phys(S) == NN(ref)` over the whole buffer. Also covers: get/put/blit for **every scale pair** (1↔S sample/replicate, same-scale physical), with blit over the **storage extent** for views with non-zero offsets (§3.1); `put_phys` clipping against destination and source size; `get_pixel8` sampling; `fill_static` RNG parity; `create_another` scale; **`tracker_complete`**: every changed physical pixel lies in the tracked logical rect × S. **S>1-only cases** (no S=1 reference, checked by canaries in a padded allocation): `copy` with out-of-range rects; RLE scans of ≥ 4096 px and malformed run lengths from `rle_writer.h` (counts beyond `scanlen`), all under ASan. |
| `test_world_scale` | Every row of the §3.2.2 table; texture-limit and budget edges; `force:N`; never a non-divisor step under `art`/`auto`; `choose_world_filter` for every ladder row, including mixed axes, HiDPI 1.25, `pixelart_ok = false` (→ LINEAR), `L = 0x0` (→ skip) and the k ≤ 6 bound; `tracked_to_phys` with negative logical rects, `offset_x/y ≠ 0` (full 355x200, game 320x200) and rects partly outside the texture (clamped, never negative). |
| `test_flat_source` | Pins the legacy fill: the row-0 neighbour is ignored; 12/0 is skipped in the 3x3 pass but not in the full scan; row-major first hit; all-RLE gives −1; null shapes; the void tile as its own tile is painted. |
| `test_hires_rules` | CRC32, FNV, T1 and the palette-0 vector (`0xc9c2c0e7`) against `hash_vectors.txt`; T1 independent of fill and dependent on own pixels and the own bitmap; P4 on crafted tiles, including a cycling pixel whose only in-range neighbour lies in the next tile (non-compliant, since neighbours are in-tile); `reduce_mode` class preservation, and `reduce(NN6(x)) == NN3(x)` and `== NN2(x)` for random tiles. |
| `test_hires_png` | Palette 8-bit, depth 4 expanded, RGB rejected (F1), tRNS flagged, `tEXt` before and after IDAT, truncated and garbage files → `Read_error` **without leaks** (run under ASan in `build-asan`); an IHDR of 30000×30000 rejected (F3) **before** any row allocation (checked with a counting allocator hook); a used index ≥ PLTE size rejected (F2). |
| `test_hires_store` | Uses temp dirs and a fake provider and palette. Covers: valid tile; F2/F3/P0/P4/G1/N1 rejects with the right rule IDs, including a shape number beyond the provider's range; unguarded accepted; group strictness; `.off`; root precedence (`<PATCH>` over `<HIRES>`); duplicates (warning); frame `&31`; `x3` folder preferred over reduction; reduction from x6; `pack.txt` palette mismatch disables the root; terrain indexing and lazy decode reject → fallback; `terrain()` with a wrong destination size returns false; `Tile_view.side` equals 8S for every scale; generation bump on reload. **Bundle:** valid; truncated (B0, loose files still load); header CRC mismatch (B0); one bad entry rejected alone (no group effect); a loose file overrides a bundle entry. |
| `test_editor_fixtures` | PNGs that mimic real editor output, synthetic content and palette: Aseprite indexed with and without a transparent index (tRNS → F4 warning, accepted); GIMP indexed with "remove unused colors" (→ F2 reject naming the index); full-palette GIMP export (accepted); `tEXt` dropped (accepted, unguarded); RGB export (F1). In WP-15 the user's own exports from Aseprite and GIMP on Windows (same synthetic pattern) are added to `tests/data/hires/editor/real/` with their expected outcomes. |

### 6.3 Data-free present test (`hires_present`, `make check`)

`SDL_VIDEO_DRIVER=offscreen`, `SDL_RENDER_DRIVER=software`:
* a synthetic indexed buffer at S ∈ {2,6}, presented 1:1 (NEAREST): the read-back equals `LUT(NN(indices))` exactly, for ARGB and (SDL ≥ 3.4) INDEX8;
* a palette change without writes: ARGB re-converts, INDEX8 uploads 0 bytes, and both read-backs equal the new LUT;
* a partial upload changes only the tracked rect; an upload rect partly outside the texture is clamped (no ASan report);
* downscale at r = 0.5: the read-back is the exact 2x2 box of the LUT image (±1 LSB);
* r = 0.25 runs the halving chain. The reference is a CPU emulation of the **cascaded** per-pass 2x2 box, truncating as the software scaler does, with ±1 tolerance (GPU backends may round). Against a single exact 4x4 box the error reaches 2, as the reviewers measured. A direct LINEAR draw at 0.25 is kept as a negative control: it must be off by far more (measured up to 104-137);
* **palette alpha = 0** (every `SDL_Color.a` zeroed) at r = 0.667 (LINEAR resolve) and r = 0.25 (halving), for ARGB and INDEX8: the read-back must equal the alpha-255 run. This catches a missing `BLENDMODE_NONE` or LUT alpha (§3.2.4);
* letterbox: with the logical rect smaller than the output, the bars read back black after a frame drawn over a non-black backbuffer;
* the window is resized after `create`, with no surface rebuild: the next `draw()` re-chooses the filter for the new L (async presentation change);
* resets: `SDL_PushEvent` of `SDL_EVENT_RENDER_TARGETS_RESET` and of `SDL_EVENT_RENDER_DEVICE_RESET` reaches the event watch, and the next draw gives an identical read-back;
* `choose_world_filter` agrees with the textures actually created. PIXELART is not testable here: the software renderer maps it to NEAREST (§3.2.4).

### 6.4 Game-data oracles (local and nightly; `make check-game`)

All runs use `env -u DISPLAY -u WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy` (offscreen
and software for `present=1`) and `tests/game/test.cfg`, generated from `test.cfg.in`: `static_path` is
`/mnt/e/Games/RolePlayingGames/ultima7/static`; `path`, `patch`, `mods`, `savegame_path`, `gamedat_path`
and `hires_path` are scratch dirs under `/home/simonea/ultima7_exult/tmp/test-*` (otherwise `game.cc:521`
creates `<game_path>/patch` in the user's install); audio is off and gamma is 1.

| Oracle | Run | Assertion |
|---|---|---|
| O0 / buildmap golden | `--buildmap 0\|1\|2` (432 PNGs) with `render_scale=art` in the config | SHA-256 list equals that of `build-upstream` (upstream `8b6ab6b43` + P3), which also proves the S=1 forcing |
| O2 | `render_regions.sh`: each region × S ∈ {2,3,6}, `overrides=no` | `phys(S) == NN(ref)` |
| O4a (tiles) | identity pack = `--dump-art` templates (rescaled for x2/x3) | output equals O2 exactly. Proves loader, lookup, `put_phys`, substitutes, cache |
| O4a (terrain) | identity terrain overrides for the terrains in the regions (`mkpack_identity.py --terrain`) | equals O2 |
| O4b | marker pack (templates with sub-pixel (0,0) of every S×S block = M = 0x01), `passes=flats` | the image equals the **predicted** image exactly: M at the top-left sub-pixel of every logical pixel whose cell has a flat source, NN elsewhere. With `passes=all`, only "difference count > 0" is asserted, because translucent shadows and objects modify markers. |
| O6 | `repaint=64` | random logical sub-rect repaints at S leave the buffer unchanged |
| O7 | always | the `mini_screenshot` digest is identical for ref and S (NN modes) |
| Present | `present=1`, S=6 at 1:1, ARGB and INDEX8 | read-back == `LUT(NN(ref))`; `screen_to_game` on a 16×16 grid equals the S=1 mapping |
| Present, non-1:1 | `present=1,window=1280x800` (r = 0.667, LINEAR), ARGB and INDEX8 | read-back within ±1 of a CPU bilinear reference of `LUT(NN(ref))`; ARGB == INDEX8 ± 1 |
| Offsets | `game=320x200` in a 355x200 full area (`offset_x = 17`), with and without `present=1` | O2 equality, plus a present read-back that is exact at 1:1 (tests `tracked_to_phys`) |
| S cycle (I12) | `resize_cycle.sh`: `resize=force6:off:force3:force6`, under ASan | every step renders NN-equal; no ASan report; the I12 assert holds |
| Pushed resize | `pushed_resize=1`, under ASan | the pushed layer's `bits` are unchanged; I12 holds; no ASan report |
| Toggle | `overrides=yes` then `no` in one process | the second render equals O2 (I8) |
| Determinism | each script runs twice | identical digests |
| Dump | `--dump-art` twice | identical trees; flat CRCs equal `u7art.py` for all 3,885 flats; T1 keys equal the Python port |
| Perf | `bench=200` at -O2 | world paint 320x200 S=6 p95 ≤ 2.0 ms; cold `render_flats` with per-tile art ≤ 1 ms; failure if more than 20 % worse than `perf_baseline.json` (per host) |

**Regions** (`tests/game/regions.txt`, chosen in WP-07 from the superchunk renders): open grass; coast
with water sparkles (cycling); dense forest (large RLE plus translucency); Britain at lift 16/10/5;
mountains; a superchunk border; world wrap (tx near 0 and 3071); a terrain containing P3 cells (flat
48/8).

**Goldens:** only SHA-256 and FNV **hash lists** are committed (`tests/game/golden/*.sha256`); pixels
derived from EA data never enter the repo. `make regen-goldens` rewrites them, and its commit message
must name the cause (for example "upstream merged P1").

**One harness per comparison** (measured in WP-00). The output depends on heap address order:
`Game_object::dependencies` is a `std::set<Game_object*>` (`objs/objs.h:93-98`), and `paint_object`
paints dependencies in pointer order (`gamerend.cc:598-603`), so ambiguous overlaps resolve by address.
With glibc malloc a list is reproducible for one binary **and one harness**. The same upstream binary
run from a sandbox path about 60 characters longer changed 2 of 144 superchunks, and the ASan allocator
changes them with the environment and the path as well. Therefore:
* O0 compares runs made by the same script, with the same sandbox path length and the same config text. When the harness changes, the `build-upstream` list is regenerated in the new harness, not reused.
* The script keeps the lane and the caller out of the run, so that runs of different lanes really get the same config text and environment. It links `<build>/exult` and `<build>/data` into the sandbox, and argv[0] and the config's `data_path` name those links, never `<build>` (so the config is written per sandbox, not by configure). It runs `exult` from the sandbox under `env -i` with a fixed environment: `PATH=/usr/bin:/bin`, `HOME`, the SDL drivers and the wrapper's sanitizer options. SDL3 copies every environment variable onto the heap in `SDL_Init`, and the lanes find their libraries through RUNPATH/RPATH. WP-07's `tests/game/buildmap_golden.sh` does the same as WP-00's `tmp/wp00/buildmap_golden.sh`. What the binary brings remains: its code, and the paths compiled in from `--prefix` (`EXULT_DATADIR`, kept as `<GAMEHOME>`).
* The ASan lane is never compared by hash across processes. Its runs must end without any sanitizer report, and its NN equalities are checked inside one process (O1, O2).

### 6.5 Sanitisers and manual acceptance

* **ASan on this machine needs ASLR off.** The g++ 9.4 ASan runtime is unreliable under the WSL2 6.18 kernel's ASLR. A reviewer's deliberate heap overflow hung in the `AddressSanitizer:DEADLYSIGNAL` loop in 11 of 40 runs, and the lead architect's re-run hung in 6 of 12. Under `setarch x86_64 -R` it was detected 40/40 (and 12/12), and LeakSanitizer works too. `vm.mmap_rnd_bits` cannot be changed without root. Therefore **every ASan process runs under `setarch x86_64 -R` and a `timeout`**: `make check` through `TEST_WRAPPER` (§6.1), and the game scripts and the interactive run through `EXULT_WRAPPER="env ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 setarch x86_64 -R timeout 1800"`, which they prefix to every `exult` call. The build runs ASan programs too (configure's test programs, and `expack` and the other tools in the data build), so `configure`, `make` and `make check` in `build-asan` also run under `setarch x86_64 -R timeout …` with `ASAN_OPTIONS=detect_leaks=0` (§7.1). `ci.sh` additionally wraps every step in `timeout`.
* **UBSan is fatal.** By default g++'s UBSan prints `runtime error:` and continues, so a run with undefined behaviour still exits 0. `build-asan` therefore compiles with `-fno-sanitize-recover=undefined`, the wrappers set `UBSAN_OPTIONS=halt_on_error=1`, and the game scripts fail when their log contains `runtime error:` or any sanitizer report, whatever the exit code.
* `build-asan`: `make check`, then `render_regions.sh`, `resize_cycle.sh` and the pushed-resize run, then one interactive session at S=6 (`ASAN_OPTIONS=detect_leaks=0` for the engine, 1 for the unit tests).
* Manual checklist (WSLg and Windows, `docs-hires/test/manual_checklist.md`):
  * walk and scroll with lerp on and off;
  * gumps open, drag and close; cheat screen (`ImageBufferPaintable`);
  * rain, clouds, earthquake; one combat round;
  * save (thumbnail) and load;
  * VideoOptions scale change and fullscreen toggle, including **S=6 → S=1 → S=6** transitions (switch `scale` to 1 with Fill on 3440x1440, then back);
  * terrain editor;
  * night palette and fades; water cycling in an idle scene;
  * BG intro and endgame (scene mode: no stale world in the letterbox bands), both at 860x300 and at the **320x200 windowed profile with a 320x200 scene** (the case where upstream draws the black main buffer);
  * Windows only: alt-tab and a display-mode change **while a conversation or modal gump is open** (render reset outside the main loop), on D3D11 and D3D12;
  * dev keys O/R/I.

### 6.6 CI integration

* **Local:** `tools/hires/ci.sh` does the following, with every step under `timeout`:
  * bootstraps pytest in `tools-venv`;
  * builds `build-o2`, `build-asan` (under `setarch -R`) and `build-sdl32` (SDL 3.2.14 from `deps/prefix-3.2`, ARGB-only) with the §7.1 commands, one tree at a time, and runs `make check` in all three. It asserts that `build-sdl32` resolved SDL 3.2.14 (`pkg-config --modversion sdl3` with the lane's variables, and `SDL_CFLAGS` in its Makefile);
  * runs the **build-list lint** `tools/hires/check_build_lists.py`. Every `.cc/.h` listed in `Makefile.common` and the `*/Makefile.am` files must appear in `Exult.vcxproj`, `Exult.vcxproj.filters` and `ios/Exult.xcodeproj/project.pbxproj`, apart from an allowlist of known exceptions. Neither project can be built here, and `Makefile.mingw` never reads the vcxproj (`Makefile.mingw:521` includes `Makefile.common`), so this lint is their only local check;
  * if `U7_BG_STATIC` is set, runs `make check-game` in `build-o2`, the ASan game scripts in `build-asan`, and `pytest tools/hires/tests`.

  It writes `tmp/ci-<date>.log` and a one-line summary.
* **GitHub fork (optional):** add `make check` after the build step of `ci-linux.yml`; that job pins SDL 3.2.14, like the local `build-sdl32`. Add `hires_unit.exe` to `ci-windows.yml`. `ci-msvc.yml` does not run in forks, so the vcxproj gets only the local lint until a maintainer's CI builds it.

### 6.7 Test cases per work package

| WP | Tests that must pass |
|---|---|
| WP-00 | reference buildmap hashes reproducible over 2 runs; `build-sdl32` builds; the ASan smoke binary detects its overflow 10/10 under `setarch -R` |
| WP-01 | `make check` green on **unmodified** upstream (in `build-o2`, `build-asan` and `build-sdl32`); `test_ibuf_golden` recorded; `import pytest` works in `tools-venv`; the build-list lint passes on upstream (the allowlist is seeded here) |
| WP-02 | probe JSON: INDEX8 read-back exact on D3D11/D3D12/Vulkan; timings recorded |
| WP-03 | `test_flat_source`; buildmap golden == build-upstream (+P3), which also proves the main-buffer wiring and P11 output-neutral; `test_ibuf_golden` unchanged; resize-while-pushed regression (manual here, automated as `pushed_resize=1` in WP-07) |
| WP-04 | `test_ibuf_scaled` (O1, mixed scales, storage-extent blit, S>1-only copy clipping, wide and malformed RLE scans, `tracker_complete`); `test_ibuf_golden` unchanged; ASan clean (under `setarch -R`) |
| WP-05 | `test_world_scale` (including `tracked_to_phys` and the ladder edge cases); `hires_present` (all §6.3 cases on ARGB, including palette alpha 0, letterbox clear, resize-after-create and pushed reset events); manual S=6 under WSLg, plus a manual S=6 → 1 → 6 VideoOptions cycle |
| WP-06 | play at S=6 (NN) under WSLg; buildmap golden unchanged |
| WP-07 | O2, O6, O7, present read-back (1:1 and non-1:1), offsets, S cycle and pushed resize under ASan, determinism, perf baseline recorded → **M1a gate** |
| WP-08 | `test_hires_png`, `test_hires_rules`, `test_hires_store` (including the bundle cases), `test_editor_fixtures` |
| WP-09 | O4a (tiles), O4b, toggle test → **M1b engine gate** |
| WP-10 | manual: O/R/I keys; **per side**: the Windows engine (reading `E:` natively) shows a `publish.sh` change within 2 s of the `.reload` touch, and a WSL engine (reading the ext4 copy) shows a WSL tool's change within 2 s; `inspect=` golden JSON for 3 known tiles |
| WP-11 | dump determinism, CRC parity with `u7art.py`, T1 parity with Python |
| WP-12 | pytest: the validator reproduces the expected rule IDs on `tests/data/hires/rules` (P4 identical to the engine on the shared fixtures); `mkpack` round trip for loose files and bundle (the engine's `test_hires_store` reads a bundle written by `mkpack`); `palette_crc32` copied from `ref.txt`; quantizer never emits ≥ 0xE0 outside the in-tile P4 mask |
| WP-13 | `hirescheck` 0 errors on the full pack (P4: 0 non-compliant pixels); QA gates §8.3 as recalibrated (B1 aggregate and floor, histogram in the report); engine loads the 3,885-entry bundle with 0 rejects |
| WP-14 | `hires_present` INDEX8 cases; present read-back INDEX8 == ARGB on BG regions; a palette tick uploads 0 B |
| WP-15 | `hires_unit.exe` and `hires_present.exe` green on Windows; renderer matrix including PIXELART on D3D11/D3D12/Vulkan; the alt-tab and mode-change reset checks in a modal gump; the perf and load-time decision rules applied and recorded |
| WP-16 | O1 still green; a `fast_paths` test (the legacy byte loop kept in the test as reference) is byte-identical; perf gates |
| WP-17 | O4a (terrain); precedence terrain > tile > NN (`test_hires_store` plus a region check); T1 invariance under P1/P2 (run against the `upstream-fixes` build) |

---

## 7. Build

### 7.1 Linux (this machine, user space)

```bash
source /home/simonea/ultima7_exult/deps/env.sh            # autotools, pkg-config, SDL3 3.4.18 (deps/prefix); needed for every step
cd /home/simonea/ultima7_exult/exult-hires
git switch -c hires 8b6ab6b43                             # fork branch (once); upstream-fixes branch likewise
autoreconf -v -i                                          # after Makefile.am / configure.ac edits (generated files stay untracked)
COMMON="--disable-exult-studio --disable-gimp-plugin --disable-aseprite-plugin --disable-shp-thumbnailer"
D=/home/simonea/ultima7_exult/deps
SYSROOT_PC=$D/sysroot/usr/lib/x86_64-linux-gnu/pkgconfig:$D/sysroot/usr/share/pkgconfig

# Performance and game tests (-O2). Never measure on build-linux (-O0, Makefile:457).
mkdir -p ../build-o2 && cd ../build-o2
../exult-hires/configure --prefix=/home/simonea/ultima7_exult/install-o2 $COMMON \
    --with-optimization=normal --with-debug=symbols
make -j16 && make check

# Sanitisers (configure strips -O from CXXFLAGS, so the level comes from --with-optimization).
# Every ASan process runs with ASLR off and under a timeout (WSL2 6.18 + g++ 9.4 ASan hangs otherwise,
# §6.5). That includes configure's test programs and the tools the data build runs (expack, ...).
# -fno-sanitize-recover=undefined: a UBSan report fails the run (§6.5).
# -Wno-duplicated-branches: configure turns on -Wduplicated-branches, and g++ 9.4 stalls on it in
# UBSan-instrumented code (exult.cc ran for more than 38 min; about 20 s without it). All UBSan checks stay on.
mkdir -p ../build-asan && cd ../build-asan
UBSAN_OPTS=halt_on_error=1:print_stacktrace=1
ASAN_RUN="env ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=$UBSAN_OPTS setarch x86_64 -R timeout 5400"
$ASAN_RUN ../exult-hires/configure $COMMON --with-optimization=light --with-debug=symbols \
    CXXFLAGS="-g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wno-duplicated-branches" \
    LDFLAGS="-fsanitize=address,undefined" \
    TEST_WRAPPER="env ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=$UBSAN_OPTS setarch x86_64 -R timeout 900"
$ASAN_RUN make -j16 && $ASAN_RUN make check

# SDL 3.2 baseline (ARGB only): SDL 3.2.14 built once into deps/prefix-3.2 (cmake + ninja, WP-00).
# PKG_CONFIG_LIBDIR replaces pkg-config's built-in path (deps/prefix/lib/pkgconfig, i.e. 3.4.18), so a
# missing prefix-3.2 fails configure instead of silently building against 3.4.18. The sysroot .pc
# files (libpng, zlib, ogg, vorbis, ...) stay visible. --disable-new-dtags turns the rpath into
# DT_RPATH, which the loader searches before env.sh's LD_LIBRARY_PATH (3.4.18, same soname). With the
# default DT_RUNPATH the binary would run against 3.4.18.
mkdir -p ../build-sdl32 && cd ../build-sdl32
../exult-hires/configure $COMMON --with-optimization=normal \
    LDFLAGS="-Wl,-rpath,$D/prefix-3.2/lib" LIBS="-Wl,--disable-new-dtags" \
    PKG_CONFIG_PATH="$D/prefix-3.2/lib/pkgconfig:$SYSROOT_PC" PKG_CONFIG_LIBDIR="$SYSROOT_PC"
make -j16 && make check

# Reference oracle: upstream + P3 only. Build "all": in a fresh out-of-tree build the top-level
# Makefile has no rules for the subdirectory convenience libraries, so "make exult" stops at once.
cd /home/simonea/ultima7_exult/exult-hires && git worktree add ../exult-upstream 8b6ab6b43
cd ../exult-upstream && git cherry-pick <P3-commit> && autoreconf -v -i
mkdir -p ../build-upstream && cd ../build-upstream && ../exult-upstream/configure $COMMON --with-optimization=normal && make -j16

# Game tests
cd /home/simonea/ultima7_exult/build-o2 && U7_BG_STATIC=/mnt/e/Games/RolePlayingGames/ultima7/static make check-game
```

* `build-linux` (-O0) stays as the debug build.
* `env.sh` is needed for every configure, `config.status --recheck` and `make`, because pkg-config and the autotools exist only in `deps/prefix/bin`. `PKG_CONFIG_PATH` and `PKG_CONFIG_LIBDIR` go in as configure **arguments**, as above. configure records them, so a recheck after a `configure.ac` or `Makefile.am` edit reuses them instead of taking `env.sh`'s 3.4.18 path.
* The lanes share the source tree, because the data build writes `data/*.flx`, `*_flx.h` and similar files into `exult-hires/data`. Build one tree at a time: two builds at once race on those files, and building one lane can make the others recompile about 20 files. WP-00 builds with `-j6` instead of `-j16`, because this machine's RAM is unstable under heavy load. Its scripts `tmp/wp00/configure_all.sh` and `build_all.sh` implement these commands (`docs-hires/impl/WP-00.md`).
* Each new source file is registered in all four build descriptions in the WP that adds it:
  * `Makefile.am` (`EXULTSOURCES`, `Makefile.am:31-127`) and the subdirectory `Makefile.am` files (`ibuf8_scaled.cc` and `world_present.cc` in `libimagewin_la_SOURCES` outside `if BUILD_EXULT`; the `hires_*` files in `libshapes_la_SOURCES` next to `vgafile.cc`; §6.1);
  * `Makefile.common` (and `SHAPES_OBJS`, plus every object list that contains `ibuf8.o`);
  * `msvcstuff/vs2019/Exult.vcxproj` plus `.filters`;
  * `ios/Exult.xcodeproj`. The hi-res code compiles out there through `HAVE_PNG_H`.

  The last two cannot be built here. `check_build_lists.py` in `ci.sh` (§6.6) checks that they list every file.
* The Python tools need `pytest`, which is not yet in `tools-venv`: `~/.local/bin/uv pip install --python /home/simonea/ultima7_exult/tools-venv/bin/python -r tools/hires/requirements.txt` (pinned versions).
* `.gitignore` gets `data/shortcutbar.vga`, `data/shortcutbar_vga.h`, `/expack`, `/ipack` and `__pycache__/`.

### 7.2 Windows (the user's PC): **MSYS2 UCRT64 under `E:\Dati\Ultima7_Upscale`**

**Decision:** native MSYS2 UCRT64, driven from WSL through interop. Rejected: cross-compiling from WSL
with llvm-mingw (the toolchain exists in `tmp/b_perf_gpu/`) and MSVC/vcpkg.

Justification: it is what upstream `ci-windows.yml` uses, and `Makefile.mingw` relies on `MSYSTEM`,
`cygpath` and `pkg-config` (`Makefile.mingw:31-164`). pacman provides SDL3 3.4.x and every dependency
prebuilt, `expack.exe` runs natively, `make install` copies the DLLs via `ntldd`, and no admin rights are
needed. The cross route would build every dependency by hand and needs a native `expack` wrapper
(build.md §5 W2); MSVC needs Visual Studio, and its CI job does not run in forks.

Steps:
1. **Install, no admin.** From WSL (the output argument is single-quoted, because bash would otherwise eat the backslashes; `msys2-base-x86_64-latest.sfx.exe` does not exist on repo.msys2.org):
   ```bash
   cd /mnt/e/Dati/Ultima7_Upscale
   curl -fLO https://repo.msys2.org/distrib/msys2-x86_64-latest.sfx.exe      # 200, about 43 MB
   ./msys2-x86_64-latest.sfx.exe -y '-oE:\Dati\Ultima7_Upscale\'
   ```
   That gives `E:\Dati\Ultima7_Upscale\msys64`. (The GitHub `nightly-x86_64` release asset `msys2-base-x86_64-latest.sfx.exe` is an equivalent mirror.)
2. **Packages, driven from WSL:**
   ```bash
   B=/mnt/e/Dati/Ultima7_Upscale/msys64/usr/bin/bash.exe
   export MSYSTEM=UCRT64 CHERE_INVOKING=1 WSLENV=MSYSTEM/u:CHERE_INVOKING/u
   $B -lc 'pacman -Syuu --noconfirm'; $B -lc 'pacman -Syuu --noconfirm'
   $B -lc 'pacman -S --noconfirm --needed base-devel git zip mingw-w64-ucrt-x86_64-{toolchain,binutils,ntldd,sdl3,libpng,zlib,libogg,libvorbis,fluidsynth,libtimidity,munt-mt32emu}'
   ```
3. **Working copy on NTFS.** MSYS2 cannot build from `\\wsl.localhost`. Run `git clone -b hires /home/simonea/ultima7_exult/exult-hires /mnt/e/Dati/Ultima7_Upscale/src/exult-hires` once, then `git -C /mnt/e/Dati/Ultima7_Upscale/src/exult-hires pull` per iteration.
4. **Build, test, install:**
   ```bash
   $B -lc 'cd /e/Dati/Ultima7_Upscale/src/exult-hires && make -f Makefile.mingw -j16 Exult.exe hires_unit.exe hires_present.exe \
           && ./hires_unit.exe && SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software ./hires_present.exe \
           && make -f Makefile.mingw install U7PATH=E:/Dati/Ultima7_Upscale/ExultHires'
   ```
   The default `OPT_LEVEL` is `-O2` (`Makefile.mingw:190`). **Never** install over the 1.12.1 folder (SDL2/msvcrt DLL clash).
5. **Run.** `E:\Dati\Ultima7_Upscale\ExultHires\Exult.exe -c E:\Dati\Ultima7_Upscale\exult-hires.cfg`. That config has its own saves and gamedat under `E:\Dati\Ultima7_Upscale\ExultHires\`, BG at `E:\Games\RolePlayingGames\ultima7`, `hires_path=E:\Dati\Ultima7_Upscale\packs\bg`, and `render_scale=art`. The user's 1.12.1 config and saves stay untouched.
6. **GPU probe (M0, WP-02).** Copy `tmp/b_perf_gpu/bench_win.exe` and `SDL3.dll` to `E:\Dati\Ultima7_Upscale\probe\` (a local folder; running from `\\wsl.localhost` stalls) and run `bench_win.exe direct3d11|direct3d12|vulkan|gpu 1920 1200 1280 800 200`.
7. **Renderer matrix (WP-15).** Run `Exult.exe --render-test "...,present=1,bench=300,format=argb|index8"` for each `SDL_RENDER_DRIVER=direct3d11|direct3d12|vulkan|opengl` on both user profiles, plus interactive play. A Win32 process sees a WSL variable only through `WSLENV`, so export `SDL_RENDER_DRIVER=direct3d12 WSLENV=$WSLENV:SDL_RENDER_DRIVER/u` before calling `Exult.exe` from WSL. Alternatively, set the variable inside the MSYS2 `bash -lc '…'` string. The results go to `docs-hires/test/windows_matrix.md`.

---

## 8. Art production plan for 6x BG terrain

This follows `upscale-research/00_recommendation.md`. The engine contract is §5. Packs are local only
(EA-derived). NXbrz is CC-BY-NC-SA and xBRZ is GPLv3; both run offline and are never linked into Exult.

### 8.1 Data locations

| What | Where |
|---|---|
| Reference set from the engine | `/home/simonea/ultima7_exult/art_ref/bg/` (`--dump-art`) |
| Work data (ext4: context windows, raw outputs, QA) | `/home/simonea/ultima7_exult/art_work/{ctx,raw,qa}` |
| Packs | canonical on ext4: `/home/simonea/ultima7_exult/packs/<name>/x6/…`; published to Windows by `publish.sh` as `/mnt/e/Dati/Ultima7_Upscale/packs/<name>/` (§5.7). The active pack is `packs/bg` |
| Tools | `exult-hires/tools/hires/u7hires/` (Python package); `tools/hires/third_party/build_xbrz.sh`. The script fetches xBRZ 1.9 and checks its SHA-256 (`b2dff73b…0fc9`); xBRZ is never vendored. It extracts with `python3 -m zipfile -e` because `unzip` is not installed. It then applies a committed one-line patch, `xbrz-1.9-gcc9.patch`: line 419 `[&] -> bool` becomes `[&]() -> bool`, because a lambda with a trailing return type and no parameter list is C++23 (P1102) and g++ 9.4 rejects it. Finally it builds `libxbrz19.so` (about 2.6 s) and records the patched-source SHA-256 in every sidecar |

### 8.2 Phase A: autonomous, CPU only (here)

| Step | Content | Output | Gate |
|---|---|---|---|
| A0 Inputs | `exult --dump-art art_ref/bg`; `u7art.py` CRC parity cross-check | reference set | dump deterministic; parity 3,885/3,885 |
| A1 Context | Per used terrain (2,105): the engine's 1x flat layer (`terrain/<t1>.png`, real fill, no black holes under RLE) plus a 16-32 px apron from its most frequent real neighbours (`terrain_map.bin`). Per-pixel instance map (shape, frame, world position). Macro sheets for the 77 8×4 torus shapes. Self-wrap only for the ~765 unused frames without a macro sheet. | `art_work/ctx` | windows reproduce the engine renders exactly |
| A2 Route 3 | xBRZ 1.9 at native 6x on a uniquified palette (duplicates nudged 1-3 LSB, inverted by lookup) → **local snap** (OKLab-nearest among the 3×3 parent neighbourhood's indices, ramp-constrained, within 0x01-0xDF) → cycling mask upscaled label-safe (xBRZ + snap on indices keeps E0-E7/FE glints) → per-(shape, frame) **mode consensus** over all instances → **in-tile cycling restriction**: intersect the upscaled cycling mask with the in-tile, edge-clamped 3×3 dilation of the tile's own 1x cycling pixels, per range (exactly the P4 neighbourhood, §5.5), and snap every other cycling pixel to the nearest static index 0x01-0xDF of its parent's ramp → no 0xFF. Material-boundary hybrid variant for grass, dirt and sand, against the "worm" artefact. Without the restriction, context windows pull glints across tile borders: the reviewer measured 25 of 142 cycling frames with 143 pixels outside the parent range, 5 frames that had no cycling at 1x gaining some, and one P4 reject (116,14). | `art_work/raw/r3*`, candidate tiles | P4 = 0 non-compliant pixels on every tile |
| A3 Pack | `mkpack.py` first runs the **engine-equivalent checks** (the `hirescheck` port of §5.5, sharing fixtures with the engine). Generated art goes into `x6/flats.bundle` (entries independent; §5.7), with a JSON sidecar per key `{key, src_crc, route, params, instances, disagreement, qa}`; loose strict groups are used only for curated sets. `pack.txt` with `palette_crc32` copied from `ref.txt`; write on ext4, then `publish.sh` | `packs/bg-r3-{none,nn3}/x6` | `hirescheck` 0 errors |
| A4 Edge decision | In-engine A/B (`--render-test mode=plain` on the 8 regions at 6x, downscaled to 1280x800, 640x400 and the 3440x1440 ACF geometry) plus C1/C2 for `none` vs `nn3` | `docs-hires/art/edge_decision.md` | decided by the metrics (C1 ≤ 1.2 × region baseline, lower C2 tail wins); contact sheets kept for the user to override later |
| A5 Freeze | Copy the winner to `packs/bg` (ext4), then `publish.sh bg` | **`bg-x6-r3 v1`: 3,885/3,885 flats** | QA report; previews |

Phase A takes about 15 minutes per pass single-threaded on this CPU, and a few minutes with 16
processes. The reviewer measured xBRZ plus local snap at 5.6-6.3 s per 768² source window (6×6 chunks).
Scaled to the 160-192² per-terrain windows of A1, that is roughly 13 min for all 2,105 used terrains. Local snap must be
vectorised (about 9 s per 768² in naive NumPy). The pass is fully deterministic: two runs give the same
pack hash (pytest).

### 8.3 QA metrics (gates, implemented in `u7hires/qa.py`)

| # | Metric | Gate |
|---|---|---|
| A1-A3 | format; static palette compliance (0x01-0xDF, 0x00 only where the source uses black, no 0xFF); **cycling compliance = engine P4** (the same function, in-tile clamped neighbourhood; §5.5) with 0 non-compliant pixels; every source cycling pixel keeps ≥ 1 cycling pixel in its block | hard fail (a dropped sparkle is a warning) |
| B1 | 6×6 block majority == source index | **aggregate ≥ 97 %** over the pack, and a **per-tile floor ≥ 85 %** (hard). Tiles between 85 % and 97 % are **flagged** for F1 review, not failed. The QA report carries the per-tile B1 histogram. Calibration: a reviewer measured route 3 at a mean of 98.7 % over 1,416 consensus frames, but 20.2 % of those frames were below 97 % (minimum 85.9 %), including the #3 most used flat, (10,2), at 90.6 %. A per-tile 97 % gate would fail a fifth of the pack, and revision 1 had no repair step for it. Revisit the floor after the first full pass, and add a deterministic edge-band repair only if F1 review rejects flagged tiles |
| B2 | box-mean round trip | per route; flag the bottom 5 % |
| B3 | block-mean OKLab ΔE, mean / p99 | p99 ≤ 0.10 |
| B4 | dominant ramp of the block == ramp of the source | ≥ 95 %; hard fail on a ramp change |
| C1 | seam gradient ratio on world renders built from the final tiles | ≤ 1.2 × the same route's region-level render |
| C2 | edge-strip ΔE for every adjacent pair that occurs on the map (about 61-64k distinct pairs) | repair or regenerate the worst 1 % |
| D1 | instance disagreement | flag above 2 × the route median |
| E1 | registration (generative routes) | ≤ 0.25 source px |
| F1 | human review: contact sheets per family (1x NN, baseline, candidate), plus in-engine previews | accept or override per frame |

The engine itself enforces only §5.5. QA gates block **promotion** of art, never loading.

### 8.4 Phase B: AI models (GPU)

| Step | Where | Content | Gate |
|---|---|---|---|
| B1 Route 2 | WSL CUDA (`tmp/factcheck/venv_cu130`, torch 2.14.1+cu130 sees the 5070 Ti) | 4x-NXbrz via spandrel on A1 windows → Lanczos 1.5x → local snap → consensus → back-projection colour lock; 8x-Arzenal as a look alternative (box 8→6). Minutes for all windows. | same gates; per-family comparison with route 3 |
| B2 Per-terrain | WSL CPU/CUDA, after WP-17 | Top 200 terrains by usage (about 81 % of BG map chunks, about 28 MB) plus transition-rich ones (coast, roads, towns). The whole 768² chunk result from the context window, edge band on the chunk border, keyed by T1 from the dump; skip `duplicate_of` keys whose fills differ. | chunk-border C2 not worse than per-tile; O4a still green |
| B3 Route 1 | Windows ComfyUI portable cu130 in `E:\Dati\Ultima7_Upscale\ComfyUI_windows_portable`, driven from WSL over HTTP | First session on 5 shapes (water 19, grass 147, dirt 149, a floor, sand/shore 10): SDXL 1.0 + xinsir Tile on the route-3 base at 12x, denoise 0.3-0.5, CN 0.6-0.9, fixed seed per class, box 2:1, back-projection, quantize, consensus; **measure VRAM and s/canvas**; challengers SeedVR2-3B and Z-Image-Turbo + Tile. Then the curated families: water/shore, grass, dirt/sand, roads, floors. | E1; D1 outliers reviewed; human acceptance per family |
| B4 Hosted pilot (optional) | OpenRouter REST, own key, cap $25 | 20 windows × a few models (seedream-5-0-flash, flux.2-klein-4b, gemini-3.1-flash-image) for look references only; never bulk | needs user approval (§10 Q4) |
| B5 Freeze | WSL | `curated` = per-family winners promoted over `bg-x6-r3`; the raw outputs archived with model SHA-256 and seed | strict check clean; previews at 1920x1200, 1280x800 and 3440x1440 ACF approved |

Long term: distil the curated look into a native-6x SPAN/RealPLKSR model (traiNNer-redux, CC0
PBRify-SPAN base) so it applies deterministically to all 3,885 frames, and later to SI.

---

## 9. Work packages

Estimates are engineer-days for one developer with AI assistance. They were re-baselined after the
judges' review (A's WP-04, WP-07 and WP-10 were 1.5-2× optimistic) and again after the adversarial
review (§12: about +3.5 engine, +0.5 tooling and +0.5 art days). **[D]** marks a package on the
**first visible demo** path: playing BG at S=6 (NN, pixel-identical to point ×6) under WSLg, at about
day 15. The override oracles (identity and marker packs) pass at about day 26 (WP-09). The first
visible **hi-res terrain** with real art (the WP-13 route-3 pack) follows at about day 32 when one
developer works sequentially.

| ID | Name | Depends | Scope (main files) | Acceptance | Days |
|---|---|---|---|---|---|
| **M0** | | | | | |
| WP-00 [D] | Fork and build matrix | – | `hires` and `upstream-fixes` branches; `build-o2`, `build-asan` (with `TEST_WRAPPER`/`EXULT_WRAPPER` = `env ASAN_OPTIONS=… UBSAN_OPTIONS=halt_on_error=1… setarch x86_64 -R timeout …`, UBSan fatal), `build-upstream` worktree built with `make -j16` (+P3 once WP-03 lands); SDL 3.2.14 into `deps/prefix-3.2` and `build-sdl32`; record reference buildmap SHA-256 lists | §6.7 WP-00 | 1 |
| WP-01 [D] | Test infrastructure | WP-00 | vendor doctest; `tests/` tree; `Makefile.am`/`configure.ac` (`TEST_WRAPPER`)/`Makefile.common` targets linking the convenience libraries (§6.1); `rle_writer.h`; **record `ibuf_golden.txt` on unmodified ibuf8.cc**; `test.cfg.in`; pinned `requirements.txt` + pytest bootstrap; `check_build_lists.py`; `ci.sh` skeleton with timeouts | `make check` green on upstream code (§6.7) | 1.75 |
| WP-02 | Windows GPU probe | – | run `bench_win.exe` on the 5070 Ti (§7.2 step 6); JSON report | INDEX8 exact on D3D11/12/Vulkan | 0.25 |
| WP-03 [D: first three items] | Prerequisite commits | WP-01 | **P3** zero-fill (`chunkter.cc:259`); **main-buffer wiring**: `create_surface` under an RAII `ibuf` guard, `main_ibuf` in `free_surface` and `~Image_window` (`imagewin.cc:518-578, 811-842`); **P11** palette alpha 255 (`iwin8.cc:102-107, 154-159`); `Import_png8` failure leak (`pngio.cc:80-84`, 107, 137-138); `create_buffer_1x` + `SDL_SurfaceOwner` assert; `find_flat_source` + `paint_flats` refactor; `write_minimap` 1x local buffer; `Figure_queue_size`; P1/P2/P5 commits with tests on `upstream-fixes` only | `test_flat_source`; buildmap == build-upstream | 1.75 |
| **M1a: scaled world (NN)** | | | | | |
| WP-04 [D] | Scaled `Image_buffer8` | WP-01, WP-03 | `pixel_scale`, ctors, hooks + `ibuf8_scaled.cc` (17 primitives), clipped `copy`, heap RLE row buffer with run clamping, mixed-scale get/put and storage-extent `blit`, `put_phys` (dest + source clip), `create_another`, `Write_tracker` (`mark_all` = reset + assign), raw-reader audit, `mini_screenshot` via `get_pixel8` | `test_ibuf_scaled` (O1 + `tracker_complete` + S>1-only cases); golden unchanged; ASan clean | 4.75 |
| WP-05 [D] | Scale policy and present path | WP-04 | `world_scale.h` (policy, per-frame filter ladder with `pixelart_ok` and k ≤ 6, `tracked_to_phys`); `world_present.{h,cc}` (ARGB with LUT alpha 0xFF, explicit blend and scale modes, lazy `world_rgb`/halving targets, upload clamp, event watch for resets and device loss); `create_world_scaled_surfaces` with a release-own-only failure path and the latch; unconditional `pixel_scale`/tracker wiring + I12 assert; the `show()` hook and `show_world_scaled` (clear first; scene and pushed semantics); guard-band early-outs; screenshot hook; `rotatecolours`; config keys and `--render-scale` | §6.7 WP-05 | 5 |
| WP-06 [D] | World integration | WP-05 | `get_rendered_flats(scale)`; `paint_chunk_flats` → `blit` + clip skip; BuildGameMap forces off; resize toast; perf scopes per pass | **first visible demo**; buildmap golden unchanged | 0.75 |
| WP-07 | `--render-test` and goldens | WP-06 | harness (§4.2) with `window=`, `game=`, `resize=`, `pushed_resize=`; `passes=flats`, present read-back, bench + `perf_baseline.json`, region list, `buildmap_golden.sh`, `render_regions.sh`, `resize_cycle.sh`, `regen_goldens.sh` | **M1a gate**: O2, O6, O7, present (1:1 and non-1:1), offsets, S cycle and pushed resize under ASan, determinism; paint p95 ≤ 2 ms at -O2 | 3.5 |
| **M1b: tile overrides, dev loop, phase-A art** | | | | | |
| WP-08 | Hi-res store | WP-01 | `hires_png` (expected dims, IHDR check before allocation, user limits, POD-only `setjmp` frame), `hires_rules` (shared P4), `hires_bundle`, `hires_store` (`Tile_view`, sized `terrain()`, `bad_alloc` boundary); `hires_glue` (config, `<HIRES>` tag in `modmgr.cc`, palette 0 with `Get_color8` clamp, bound-checked provider, invalidation at `shapeid.cc:150/420`); build registration | `test_hires_*` (incl. bundle), `test_editor_fixtures` | 4 |
| WP-09 | Per-tile composition | WP-07, WP-08, WP-11 | `Hires::flat` in `paint_flats`, generation checks, reduction path; `mkpack_identity.py` (identity and marker from the templates) | O4a (tiles), O4b, toggle → **M1b engine gate** | 1.5 |
| WP-10 | Dev loop | WP-09, WP-12 | `hires_dev.cc`, 3 `keys.cc` rows, `defaultkeys.txt` (BG and SI), `.reload` poll at `exult.cc:1518`, inspector; per-side reload check with `publish.sh` | §6.7 WP-10 | 1.5 |
| WP-11 | `--dump-art` (flats + terrain) | WP-03, WP-08 | `dump_art.cc` (§4.2): palettes, flats, NN templates, terrain renders, T1 keys, tables | determinism; CRC and T1 parity | 2 |
| WP-12 | Pack tooling (Python) | WP-11 | `u7hires/{ref,pack,check,quantize}.py`, `hirescheck.py` mirroring §5.5 exactly (P4 included) plus P2/E1, `--restamp`, shared rule fixtures; `mkpack --bundle` writer; `publish.sh` (ext4 → `E:`, `.reload` on both sides); `build_xbrz.sh` with the g++ 9 patch; `u7art.py` (its 10-byte v2 header fix was done in WP-00) kept as cross-check | §6.7 WP-12 | 2.5 (tooling) |
| WP-13 | Art phase A (route 3) | WP-12 | §8.2 A1-A5 with the in-tile cycling restriction; full BG pack as a bundle; QA report with the B1 histogram; edge decision | §8.3 gates (recalibrated B1); engine loads 3,885 bundle entries, 0 rejects | 3.5 (art) |
| **M1c: Windows, performance, per-terrain** | | | | | |
| WP-14 | INDEX8 and tracked uploads | WP-07 | INDEX8 path under `SDL_VERSION_ATLEAST(3,4,0)` + runtime fallback; RGB resolve (NEAREST, blend NONE); tracker-driven upload through `tracked_to_phys` in `show_world_scaled` (WP-05 uploads the whole buffer) | §6.7 WP-14; the offsets and S-cycle oracles re-run with tracked uploads | 2 |
| WP-15 | Windows build and measurements | WP-09, WP-14 | MSYS2 setup (§7.2); vcxproj/filters/xcode registration; unit and present exes; renderer matrix; **decision rules** below; the user's real editor fixtures | §6.7 WP-15 | 2.5 |
| WP-16 | Performance pass | WP-07 | `memset` runs in scaled RLE; row-batched translucency; `fast_paths` test | perf gates; O1 green | 1.5 |
| WP-17 | Per-terrain overrides | WP-09, WP-11 | T1 key cache in `Chunk_terrain`; `Store::terrain` decode into the cache; reduction; `mkpack_identity.py --terrain` | O4a (terrain), precedence | 2 |
| WP-18 | Upstream PR series and docs | WP-15 | PRs: tests infra; P3; main-buffer wiring (`main_ibuf` in `create_surface`/`free_surface`/destructor); P11 palette alpha; `Import_png8` leak; `pixel_scale` (no-op at 1, with O1); present path (off by default); flat store. Separately P1, P2, P5. Docs: `docs/hires.md` (user guide, §1.4 list, configs), `docs/hires_modding.md` (Aseprite/GIMP palette setup with `pal0.gpl`, naming, rules, live loop) | each PR builds and passes `make check` alone | 1.5 |
| **Phase B art (parallel, after M1b)** | | | | | |
| WP-19 | Route 2 (NXbrz, WSL CUDA) | WP-13 | §8.4 B1 | gates; family comparison | 2 |
| WP-20 | Per-terrain art (top 200) | WP-17, WP-19 | §8.4 B2 | chunk-border C2 | 3 |
| WP-21 | Route 1 (ComfyUI) and curation | WP-13, WP-15 | §8.4 B3, B5 | per-family acceptance | 5-10 |
| **M2 / M3** | | | | | |
| WP-30 | Sprites (M2) | M1 | §3.6 | O4a/O4b for RLE, translucent and reflected frames; CRC gate; dynamic render-test | 12-15 |
| WP-40 | UI (M3) | WP-30 | §3.7 | hit-testing unchanged; UI overrides | 12-15 |

Totals:
* M0 + M1a: about **18.75 engine days** (demo at about day 15).
* M1b: **9 engine days**, plus 2.5 tooling and 3.5 art.
* M1c: **9.5 engine days**.
* **M1 overall: about 37 engine days, plus 2.5 tooling and 3.5 art for Phase A**, about ±25 %. Proposal A's scope re-baselined accounts for about 27 of them. The judge-requested additions (halving chain, INDEX8 and tracker, dump, dev loop, per-terrain) add about 7. The adversarial-review fixes add about 3.5: main-buffer ownership and fail-soft, explicit present state, the bundle, input hardening, the ASan and SDL 3.2 build lanes, and the new oracles.

**WP-15 decision rules (written down before measuring):**
1. **Performance.** On the 860x300 profile at S=6 (-O2, Windows, the best of D3D11 and D3D12), measure p95 CPU time per frame while walking with lerp on. If it is ≤ 6.7 ms, keep `max_world_mpx = 10` (S=6). Otherwise set the default to 5, which gives S=3 on that profile with reduced or x3 art. The view is never changed.
2. **Load time.** Measure the full flat pack (3,885 tiles) as a bundle and as loose files, from `E:` on Windows and from the ext4 copy in WSL. The bundle already exists (M1b), because the review measured 17.6 s for loose files over drvfs. If the bundle load is above 1 s anywhere, preload at the end of `Shape_manager::load`. If the Windows loose-file load is above 1 s, `publish.sh` stops publishing loose copies of bundled tiles.
3. **Format.** If INDEX8 read-back is exact on a renderer, `present_format=auto` uses it there. Otherwise the code falls back to ARGB for that renderer (logged).

---

## 10. Risks, mitigations and open questions

### 10.1 Risks

| # | Risk | Sev. | Mitigation |
|---|---|---|---|
| 1 | Logical/physical unit mix-ups cause out-of-bounds writes (guard band, `copy`, raw readers, mixed-scale buffers) | Critical | Guard-band functions are disabled at S>1; `copy` is clipped at S>1; mixed-scale semantics are defined (storage-extent blit); raw readers audited; `tracked_to_phys` with upload clamp; RLE runs clamped; O1 with canaries; ASan; O6; I12 |
| 2 | A texture over the renderer limit or excessive memory (Studio zoom, Fill x1) | High | Divisor-only policy with texture and pixel caps; fail-soft to S=1 |
| 3 | Present cost on the 860x300 profile (9.29 Mpx) | High | INDEX8 (4× less upload, free palette ticks); write-tracked uploads; WP-15 rule 1 drops S, never the view |
| 4 | Upstream rework of `screen_texture`, `UpdateRect` and layers (34 `imagewin.cc` commits in 2026) | Med | S>1 never touches that code; presenter in its own file; one-line hooks; data-free present test catches breakage on rebase |
| 5 | AI art breaks index semantics (cycling, 0xFF, ramps) | High | Engine rules P0/P4 and F2; validator P2; quantizer constraints; QA A1-A3 |
| 6 | Per-tile seams (about 80 % of tile edges) | High (visual) | Context + consensus; edge contract A/B; per-terrain overrides (WP-17, WP-20); C2 over all pairs |
| 7 | Stale art under mods | Med | Optional CRC guard (G1); `pack.txt` palette check; per-game roots; `<PATCH>/hires` precedence; mandatory CRC gate in M2 |
| 8 | Editors corrupt PNGs silently (palette reorder, RGB, dropped chunks) | Med | F1/F2 with named causes; tRNS tolerated; guard optional; `pal0.gpl`/`.act` in the dump; editor fixture tests; modding guide |
| 9 | Pack load hitch: 3,885 loose PNGs take 17.6 s from WSL over drvfs (measured), 0.25-0.31 s natively on Windows | High → Low after mitigation | Bundle in M1b (one read); WSL engines read an ext4 copy, never loose files on `/mnt/e`; `publish.sh` mirrors to `E:`; WP-15 rule 2 measures what remains |
| 10 | INDEX8/LINEAR behaviour differs per backend | Low | INDEX8 sampled only with NEAREST, then an RGB resolve; ARGB fallback; probe in WP-02 |
| 11 | Translucency and xform semantics at S | Med | Applied per physical pixel; O1 with real-shaped xform tables |
| 12 | Python and engine disagree on fill or keys | Low | The engine dump is the single source; parity tests |
| 13 | Estimates overrun (single developer) | Med | Demo-first ordering; M1c items can each slip independently; Phase B is not on the engine path |
| 14 | 1x divergence pressure if upstream merges P1/P2 | Low | T1 and per-tile keys are fill-independent; `make regen-goldens` once; pinned tests updated deliberately |
| 15 | Stale main-buffer state across S changes or a pushed target (heap overflow on S=6 → S=1; nulled layer bits) | Critical | Unconditional `pixel_scale`/tracker wiring; all main-buffer writes through `main_ibuf`; RAII guard; I12 assert; S-cycle and pushed-resize oracles under ASan |
| 16 | GPU state defaults (blend BLEND, scale LINEAR, garbage palette alpha) silently darken or blacken the world | High | P11; explicit blend and scale modes on every presenter texture; LUT alpha 0xFF; alpha-0 cases in `hires_present` |
| 17 | Render device reset or loss during a modal loop goes unnoticed | Med | Event watch with atomic flags; consumed in `show_world_scaled`; device loss → window rebuild or S=1 |
| 18 | ASan runs hang at random under WSL2 ASLR, so CI flakes or stalls | Med | `setarch x86_64 -R` for every ASan process; `timeout` on every CI step |
| 19 | The SDL < 3.4 branches or the vcxproj/Xcode lists rot unnoticed | Med | `build-sdl32` lane in `ci.sh`; `check_build_lists.py` lint |
| 20 | Route-3 art fails gates that were set without measurement (B1 per tile, cycling) | Med | B1 recalibrated from the measured distribution; P4 identical everywhere; in-tile cycling restriction; bundle entries independent |

### 10.2 Open questions for the user (with recommended defaults)

| # | Question | Recommended default |
|---|---|---|
| Q1 | If the 860x300 fullscreen view cannot hold 60 fps at S=6, is dropping to S=3 on that profile acceptable, keeping the view? | **Yes**, with automatic S=3; the 573x200 preset is documented as an option, not a requirement |
| Q2 | May Claude install MSYS2 (about 2-3 GB) under `E:\Dati\Ultima7_Upscale\msys64` and keep the packs under `E:\Dati\Ultima7_Upscale\packs`? | **Yes** |
| Q3 | Submit P1, P2, P5, P11 (palette alpha), the main-buffer wiring fix and the infrastructure PRs upstream under your GitHub account, or keep them local? | Claude prepares the branches; **you decide and submit** |
| Q4 | Upload small U7 terrain crops to hosted models (OpenRouter) for a private look pilot, capped at $25? | **No** unless you approve; Phase B uses local models only |
| Q5 | Look direction for the curated families: strictly faithful (structure-locked, tiny detail gain) or more painterly? | **Faithful** (route 1 at low denoise); decided per family from contact sheets |
| Q6 | Is a non-commercial model (4x-NXbrz, CC-BY-NC-SA) acceptable for a private pack? | **Yes**, local use only, never redistributed |
| Q7 | Keep the canonical packs on WSL ext4 (`/home/simonea/ultima7_exult/packs`) and mirror them to `E:\Dati\Ultima7_Upscale\packs` with `publish.sh`, with generated art as one `flats.bundle` per pack? (A WSL engine reading loose PNGs from `E:` takes 17.6 s to load.) | **Yes** |
| Q8 | Route-3 art: accept tiles whose block agreement with the original (B1) is between 85 % and 97 %, flagged for your review on contact sheets, instead of blocking them? (About a fifth of the tiles fall in that band.) | **Yes**; the floor is revisited after the first full pass |

---

## 11. Decision log

| # | Decision | Chosen over | Why |
|---|---|---|---|
| D-01 | **Proposal A as the base** | B (GPU-first), C (modder-first) | Highest average score (A 7.8, C 6.7, B 5.5). Smallest risk surface; S=1 is upstream code plus P3; one-line hooks; the earliest playable milestone. B's present ideas and C's art and modder ideas were grafted in where the judges asked. |
| D-02 | **1x stays byte-identical to upstream + P3.** P1 (`chunkter.cc:104` `> 0`, a genuine off-by-one) and P2 (12/0 fallback, 119-126) live on `upstream-fixes`, one commit each with tests, submitted upstream and merged into `hires` only after upstream accepts them. P5 likewise. **P3 (zero-fill) is mandatory in the fork** (determinism). | B: apply P1/P2/P5 at the fork base | The unmodified upstream binary (+P3) is a free golden oracle (O0). No art key depends on the fill (per-tile `(shape, frame)`, per-terrain T1) and the hi-res fill uses the engine's own `find_flat_source`, so P1/P2 never re-key art; if upstream merges them, the fork rebases and regenerates goldens once. P1 changes the 1x picture in 779 of 2,105 used BG terrains, a visible change the fork should not make alone. |
| D-03 | S belongs to the render target; logical API, physical storage; the main `ib8` is mutated in place; no dual write | a global S; a separate hi-res surface; dual write | Map D1-D3: about 600 call sites assume game px; paint has side effects; the in-process A/B harness needs per-target S. |
| D-04 | **Own `World_presenter` and world texture**; upstream's `screen_texture`, `screen_texture_a` and `UpdateRect` are not created or used at S>1 | A: reuse that pair "unchanged" | Upstream reworked that pair in Aug-Sep 2026 (f32e11377, e57e68864, ddf20ac8d). Decoupling keeps rebases local and makes the present path testable without the engine. It does **not** save memory: upstream's pair is display-sized (about 2 × 20.4 MB on the 3440x1440 scale-4 profile), while INDEX8 `world_texture` plus `world_rgb` take about 46.5 MB at 5160x1800. Revision 1's "saves 2 × 37 MB" was wrong. `world_rgb` is created only when needed. |
| D-05 | ARGB+LUT baseline (SDL 3.2 API) **plus INDEX8 in M1c** under `SDL_VERSION_ATLEAST(3,4,0)` with a runtime fallback; INDEX8 is always resolved NEAREST before filtering | B: require SDL ≥ 3.4; A: INDEX8 post-M1 | Upstream CI builds 3.2.14; B measured 4-10× lower CPU cost and exact read-back; the resolve pass removes the unverified palette-LINEAR backend risk. |
| D-06 | **Write-tracked uploads** inside the scaled helpers | A: upload the requested rect (always the full window) | `Game_window::show()` always requests the full window (`gamewin.h:747-755`). Tracking in the few physical-write helpers is cheap, cannot miss a write, and is fuzz-checked (`tracker_complete`). |
| D-07 | Policy `art`: the largest divisor of S_art within caps; divisor-only stepping; output pixels for HiDPI | map D5 `auto` (smallest S ≥ need); A's decrement 6→5→4 | The user's "render high, downscale at the end"; never lands on scales where ×6 art cannot apply. `auto` stays available as an option. |
| D-08 | Filter ladder NEAREST / PIXELART-LINEAR / LINEAR / **halving chain in M1** | A: halving post-M1; B: GPU area shader tier | r < 0.5 is reachable (small windows) and LINEAR aliases without mipmaps; NEAREST measured unacceptable for downscale. The area shader needs `SDL_GPURenderState`, committed shader blobs and a DXC toolchain that does not run on Ubuntu 20.04. |
| D-09 | Terrain precedence per-terrain (T1) → per-tile → NN; per-terrain in M1c | A: no per-terrain; map/B: FNV of the 1x render | Per-tile-only art leaves about 80 % of edges as potential seams, worst for diffusion art. T1 does not depend on P1/P2, unlike a hash of the fill-dependent render. |
| D-10 | Loader checks F2 (PLTE on used indices), P0, P4, optional G1 guard; **tRNS is a warning**; a missing guard is accepted | C: strict tEXt guard and tRNS reject; A: no palette check in the engine | Catches editor palette remaps and stale art without breaking real Aseprite/GIMP saves, which drop tEXt and may add tRNS. Raw-index reading makes tRNS harmless. |
| D-11 | Own libpng reader (`hires_png`) with RAII; `Import_png8` leak fixed upstream-style | reuse `Import_png8` | Needs tEXt, exact PLTE and an IHDR size check before allocation. It avoids `Import_png8`'s leak on longjmp: the handler at `pngio.cc:80-84` does not free `palette` (107) or `pixels` (137-138). `Import_png8` also opens paths without tag expansion. |
| D-12 | Eager synchronous loading. **The single-file flats bundle is built in M1b** (generated art), loose PNGs override it; a WSL engine reads an ext4 copy | B: async worker decode; revision 1: bundle only as a WP-15 contingency | Determinism and simplicity; async adds pop-in and threads to a single-threaded engine. The measured drvfs cost (17.6 s for 3,885 loose files from WSL, against 0.03 s on ext4 and about 0.3 s natively on Windows) already triggers revision 1's own bundle rule, so the bundle is no longer optional. |
| D-13 | Groups are directories (strict, `.off` disables); bundle entries are independent (D-29); no manifest, sets or families in M1 | C: `hires.txt` sets, priorities, atomic families | Zero-config selection matching "per tile / per group"; can grow later without breaking the layout. |
| D-14 | S_eff < S_art: `x<S>` folder, else engine class-preserving mode reduction | A: drop art; B: nearest-to-mean index reduction | Keeps art visible at S=3/2 when budgets force it; deterministic, index-safe, `reduce(NN6) == NN3`. |
| D-15 | Minimal dev loop (toggle, reload, inspect, `.reload` trigger) | C: plus template export, overlay, recorder, sets; A: toggle only | The user's AI review loop needs reload without a restart; templates come from `--dump-art`; C's extras are deferred until real art exists. |
| D-16 | `--dump-art` (flats + terrain) is the source of art inputs; `u7art.py` is a cross-check | A: fix `u7art.py`'s fill emulation | Removes the Python re-implementation of `paint_tile` (risk of keys and context that never match). |
| D-17 | No `assert(scale==1)` in the scalers | map W11 | It would fire in `scale_layer_color` (`imagewin.cc:1738-1781`), which runs the scalers on a patched current target. |
| D-18 | Scene mode at S>1 draws no world (clear + layers); a pushed non-scene target re-presents the last world | A: draw the stale world; B: fall through to the legacy path (null textures) | Upstream draws either nothing (when the scene and main sizes differ) or the main buffer, which `Scene_view::begin_frame` has cleared to black and the opaque `Fill` scene layer covers (when they match, e.g. a 320x200 scene on a 320x200 view). A clear matches both, keeps intro and endgame identical, and never touches absent textures. Revision 1's "`fullrect` stays empty" mechanism was only half the story. |
| D-19 | Deferred: fine sub-pixel scrolling, earthquake as a present offset, frame pacing, UI-only repaint, VideoOptions entry, area shader | B's extras | Not needed for M1 correctness; each touches many sites or adds toolchain risk; possible M1.5 work. |
| D-20 | Mixed-scale `get/put/blit` sample or replicate; physical copies only between equal scales | undefined (A) | `create_buffer` inherits the current target's scale; push/pop between create and use would otherwise overrun the heap. |
| D-21 | M2 sprites: a side table keyed by `(vga file id, shape, frame)` + store generation, filled at pack load; the physical view carries the tracker and `clip × S` | map D8 identity stamp + LRU first; revision 1: keyed by `Shape_frame*` | No I/O in `get_shape` (frame-0 sweep), a local diff; D8 remains the upgrade path for complete packs. A pointer key suffers ABA: `Shape::reset/set_frame/del_frame` and transient owners free and reallocate frames without invalidation. |
| D-22 | Windows: MSYS2 UCRT64 on `E:` driven from WSL | llvm-mingw cross-compile; MSVC/vcpkg | Matches upstream CI and `Makefile.mingw`; prebuilt SDL 3.4 and dependencies; native `expack`; no admin rights. |
| D-23 | doctest linking the **libtool convenience libraries** (`libshapes`, `libimagewin`, `libu7file`) as `ipack` does, with explicit object lists in `Makefile.common`; a data-free present test; hash-list goldens with `make regen-goldens` | revision 1: wrapper TUs that `#include` the sources; committing PNG goldens | A convenience archive contributes only the members a program references (`tools/ipack` links `libimagewin.la` today without the engine), so revision 1's reason for the wrapper TUs ("cannot link without the engine") was wrong. Its wrapper plan also could not link: `Shape_frame(pixels,…)` lives in `vgafile.cc`, which needs `libu7file`. EA pixels must stay out of the repo; explicit regeneration keeps goldens honest. |
| D-24 | **`main_ibuf` owns every main-buffer field write** (`create_surface` under an RAII guard, `free_surface`, destructor); `pixel_scale` and tracker set unconditionally; I12 asserted | revision 1: save/restore of `ibuf` inside `create_surface` only, S>1-only wiring | `resized()` and `toggle_fullscreen()` call `free_surface()` outside `create_surface`, which nulled a pushed buffer's bits. A switch from S=6 to S=1 left `pixel_scale = 6` on a 1x surface, a heap overflow on the first `fill8` (review blocker). |
| D-25 | Fail-soft releases only what the hi-res setup created and **latches** the failure; it never calls `free_surface()` | revision 1: an unspecified "free" | `free_surface()` destroys the renderer that the upstream fallback needs. Without a latch, the point-scaler retry re-enters S>1 and ends at the fatal throw. |
| D-26 | **P11** palette alpha 255 (upstreamable, output-neutral); presenter textures with explicit blend NONE and explicit scale modes; LUT alpha 0xFF; RGB-only palette compare | relying on SDL defaults | SDL defaults ARGB textures to BLEND and all textures to LINEAR, and `colors2[].a` is indeterminate. A probe showed the INDEX8 resolve turning every pixel black on the user's default 860x300 profile. |
| D-27 | Filter chosen **per frame** from a fresh L (window target only), with a skip on an empty L, k ≤ 6, lazily sized targets, PIXELART only on shader renderers, and a clear before every draw | revision 1: chosen at surface creation; PIXELART whenever SDL ≥ 3.4 | L changes asynchronously and reads 0x0 under a texture target (an endless halving loop); software and D3D9 turn PIXELART into non-integer NEAREST; the backbuffer is undefined after a present. |
| D-28 | Render resets and device loss through `SDL_AddEventWatch` flags, consumed in `show_world_scaled` | revision 1: cases in `Handle_event` | About 14 loops besides the main loop drain SDL events (modal gumps, menus, intro); upstream already uses an event watch for device events for this reason. |
| D-29 | One P4 definition (in-tile, edge-clamped neighbourhood) shared by engine, validator, QA and generator; route 3 restricts cycling to it; generated art in independent bundle entries; B1 gate = aggregate ≥ 97 % + per-tile floor 85 %, flags below 97 % | revision 1: three cycling definitions; B1 ≥ 97 % per tile; strict per-shape groups for generated art | Measured on route 3: 20.2 % of frames below 97 % per tile, 25 cycling frames outside the parent range, and one P4 reject that a strict group would have turned into a whole-shape drop. |
| D-30 | ASan with ASLR off (`setarch x86_64 -R`) and timeouts everywhere; a local `build-sdl32` lane; a build-list lint for vcxproj/Xcode | revision 1: plain ASan runs; the optional fork CI as the only SDL 3.2 check; the MSYS2 build as the vcxproj check | ASan hung in 25-50 % of runs here; nothing local compiled the SDL < 3.4 branches; `Makefile.mingw` never reads the vcxproj. |

---

## 12. Review log

Revision 2 processes all 40 findings of the adversarial review (four reviewers: `code_claims` = CC,
`memory_safety` = MS, `present_sdl` = PS, `tests_build` = TB). The lead architect checked each
finding against the code at `8b6ab6b43` or the SDL 3.4.18 sources, or re-ran the reviewer's probe:
* the ASan hang reproduced in 6 of 12 runs, and in 0 of 12 under `setarch -R`;
* the drvfs read of 3,885 flats took 17.6 s again;
* `import pytest` fails in `tools-venv`;
* palette 0 index 255 is (250, 64, 1), and `Get_color8` clamps it.

The review scripts are in `tmp/review_tests_build/` and `tmp/present_review/`.

**Result: 40 fixed, 0 rejected.** Eight findings duplicate another reviewer's and are fixed by the same
change (marked "dup"). Two sub-points were not adopted, each with its reason in the table.

| ID | Sev. | Finding (short) | Disposition | Where |
|---|---|---|---|---|
| CC-1 | major | `free_surface()` nulls `ibuf->bits` on the pushed target; `resized()`/`toggle_fullscreen()` call it outside `create_surface`, so the save/restore did not close the resize-while-pushed hole | **Fixed.** All main-buffer writes go through `main_ibuf` (`create_surface` under an RAII guard placed before its own `free_surface()`, `free_surface`, destructor); automated `pushed_resize` oracle | §3.2.3, I12, D-24, §4.2, §6.4 |
| CC-2 | minor | `Handle_event` sees only main-loop events; about 14 other loops drop render resets; `DEVICE_LOST` and layer textures ignored | **Fixed.** `SDL_AddEventWatch` with atomic flags in `World_presenter`; consumed in `show_world_scaled`; device reset also frees layer textures; device lost → window rebuild / S=1 | §3.2.5, D-28 |
| CC-3 | minor | Step 7 "free" via `free_surface()` destroys the renderer, so the fallback fails and the retry re-enters S>1 → fatal throw | **Fixed.** Release only own resources, never `free_surface()`; `world_scaled_failed` latch until a config change | §3.2.1, §3.2.3, I11, D-25 |
| CC-4 | minor | Scene-mode rationale wrong: with a 320x200 scene on a 320x200 view, upstream draws the (black) main buffer | **Fixed.** Rationale reworded (black, covered main buffer or nothing; a clear matches both); case added to the manual checklist | §3.2.4, D-18, §6.5 |
| CC-5 | minor | `hires_unit` cannot link: `Shape_frame(pixels,…)` lives in `vgafile.cc`, which needs `libu7file` | **Fixed.** Tests link `libshapes`/`libimagewin`/`libu7file` (autotools) or explicit object lists with `FILE_OBJS` (`Makefile.common`); a test-only RLE writer for wide and malformed scans | §6.1, D-23 |
| CC-6 | minor | D-23's reason ("libimagewin cannot link without the engine") is refuted by `ipack` | **Fixed.** Wrapper TUs dropped; convenience libraries linked as `ipack` does; D-23 rewritten with the real reasons | §6.1, D-23 |
| CC-7 | minor | "2 × 37 MB" saving is wrong; upstream's textures are display-sized (about 20.4 MB each) | **Fixed.** Numbers corrected; the reason is rebase isolation; the S>1 textures cost more, so `world_rgb` and the halving targets are lazy | §3.2.3, D-04 |
| CC-8 | minor | `Import_png8` leak anchors point at unrelated lines | **Fixed.** Re-anchored to `pngio.cc:80-84` (handler), 107 and 137-138 (allocations); the fix frees and nulls the reference parameters | §3.5, D-11, WP-03 |
| MS-1 | **blocker** | `pixel_scale` (and tracker, `guardband_paint_active`) not reset on S>1 → S=1, a heap overflow on the first `fill8` | **Fixed.** Unconditional `pixel_scale`, tracker and `mark_all` on every path, including fail-soft; `guardband_paint_active = false`; I12 debug assert; ASan S-cycle oracle `resize=force6:off:force3:force6`; manual 6→1→6. *Not adopted:* resetting `game_width/height` from `saved_*`, because `resized()` assigns the new game size just before `create_surface`, and restoring `saved_*` would undo it | §3.2.3, I12, D-24, §4.2, §6.4, §6.5 |
| MS-2 | major | Tracker → physical upload frame unspecified; negative logical rects; `mark_all` could union a stale larger rect after a shrink | **Fixed.** Logical tracker coordinates documented; pure `tracked_to_phys` (`(x+off)·S`, clamped) and the source-pointer formula; `upload` clamps again (debug assert); `mark_all` = reset + assign, called in `create_surface`; unit, present and game oracles with `offset_x ≠ 0` and a shrink under ASan | §3.1, §3.2.4, §6.2-6.4, WP-14 |
| MS-3 | minor | (dup CC-3) step 7 "free" ambiguity; `presenter.destroy()` order relative to `SDL_DestroyRenderer` | **Fixed** with CC-3 and PS-6: release own resources only; `destroy()` before `free_layer_textures()`/`SDL_DestroyRenderer`, nulls all pointers; `create()` asserts the current renderer | §3.2.3 |
| MS-4 | minor | (dup CC-1) the fix was incomplete: `free_surface`, destructor `delete ibuf`, a throw skipping the restore | **Fixed** with CC-1 (RAII guard, `main_ibuf` in `free_surface` and destructor) | §3.2.3, D-24 |
| MS-5 | minor | RLE row buffer capacity unspecified; 15-bit scans; malformed runs | **Fixed.** Reusable heap row buffer grown to `scanlen`; every run clamped to the remaining scan while parsing advances exactly as upstream; fuzz with ≥ 4096-px scans and malformed runs under ASan | §3.1, §6.2 |
| MS-6 | minor | Store hands out raw pointers with implied sizes; an S mismatch overruns | **Fixed.** `Tile_view{px, side}` checked against `8·S` (assert + NN fallback); `terrain()` takes the destination w/h and verifies the post-reduction size; `put_phys` clips against the source size too | §3.1, §3.4, §3.5 |
| MS-7 | minor | Loader input checks: provider shape bound, F2 index ≥ PLTE size, IHDR size before allocation and user limits, `setjmp` locals | **Fixed.** All four: bound-checked provider (`get_num_shapes()`), extended F2, F3 on IHDR before allocation plus `png_set_user_limits`/`chunk_malloc_max`, POD-only `setjmp` frame with pre-constructed owner; `bad_alloc` caught at the store boundary | §3.5, §5.2, §5.5, §6.2 |
| MS-8 | minor | M2 physical view bypasses tracker and clip; side table keyed by `Shape_frame*` is ABA-prone | **Fixed** (M2 outline). The view carries the tracker (floor/ceil) and `clip × S`; the key is `(vga id, shape, frame)` + generation | §3.6, D-21 |
| MS-9 | minor | `blit`/`put` ignore source offsets (OOB read for views with `offset_y > 0`); the `SDL_SurfaceOwner` assumption; audit gaps | **Fixed.** `blit` defined over the source storage extent; `create_buffer_1x` for the `SDL_SurfaceOwner` sources and FLI, plus an assert; `Newfile_gump` and `Notebook_gump` added to the audit (both are correct as they are) | §3.1 |
| PS-1 | major | Palette alpha indeterminate; ARGB textures default to BLEND and all textures to LINEAR; INDEX8 resolve goes black; the 1 KB memcmp reads garbage | **Fixed.** P11 (alpha 255, upstreamable, output-neutral, proven by O0); explicit `BLENDMODE_NONE` and scale modes on every presenter texture; LUT alpha 0xFF; RGB-only compare; alpha-0 present cases at r = 0.667 and 0.25; non-1:1 game present oracle | §3.2.4, I10, D-26, §6.3, §6.4, WP-03 |
| PS-2 | minor | Filter and L go stale after async size changes; L = 0x0 under a texture target → endless halving loop | **Fixed.** Ladder per `draw()`; L queried with the window target only; skip if empty; k ≤ 6; lazy (re)sizing of targets; resize-after-create test | §3.2.4, D-27, §6.2, §6.3 |
| PS-3 | minor | Halving chain vs a single 4x4 box exceeds ±1 | **Fixed.** Reference = cascaded per-pass 2x2 boxes (±1); direct LINEAR kept as a negative control | §6.3 |
| PS-4 | minor | PIXELART is NEAREST on software and D3D9 | **Fixed.** `pixelart_ok` gated on the renderer name (shader renderers only), else LINEAR; PIXELART verified in the WP-15 matrix only | §3.2.4, §3.2.7, D-27, WP-15 |
| PS-5 | minor | No backbuffer clear in the main/pushed branches; letterbox bars undefined | **Fixed.** Window target + `SDL_RenderClear` at the start of every branch and in `present_world_frame`; letterbox read-back test | §3.2.4, §3.2.6, §6.3 |
| PS-6 | minor | (dup CC-1, MS-3) destroy order; `free_surface` and destructor act on the current target | **Fixed** with CC-1 and MS-3 | §3.2.3 |
| PS-7 | minor | (dup CC-2) render resets lost in modal loops | **Fixed** with CC-2; Windows manual check (alt-tab and mode change in a modal gump) | §3.2.5, §6.5 |
| PS-8 | minor | (dup CC-7) memory rationale wrong | **Fixed** with CC-7 | D-04 |
| PS-9 | minor | (dup CC-4) scene-mode rationale | **Fixed** with CC-4 | D-18 |
| TB-1 | major | ASan hangs at random under WSL2 ASLR; no timeouts | **Fixed.** `setarch x86_64 -R` + `timeout` for every ASan process (`TEST_WRAPPER`, `EXULT_WRAPPER`); `ci.sh` steps under `timeout` | §6.1, §6.5, §6.6, §7.1, D-30 |
| TB-2 | major | Loose packs over drvfs cost 17.6 s from WSL; shared-folder plan and §5.7/§6.4 inconsistent | **Fixed.** Bundle built in M1b; WSL engines read an ext4 copy; `publish.sh` mirrors to `E:`; config, risk 9 and WP-10/WP-15 acceptance restated per side | §1.1, §4.1, §5.7, §3.8, §9, §10, D-12 |
| TB-3 | major | B1 ≥ 97 % per tile fails about 20 % of route-3 frames; no repair step | **Fixed.** Gate = aggregate ≥ 97 % + per-tile floor 85 %, with tiles below 97 % flagged; histogram in the report; open question Q8. *Deferred:* the deterministic edge-band repair pass is added only if F1 review rejects flagged tiles | §8.3, D-29, Q8 |
| TB-4 | major | Three cycling definitions; context windows leak glints across tiles; strict per-shape groups turn one P4 reject into a whole-shape drop | **Fixed.** One P4 (in-tile, clamped) shared by engine, validator, QA A3 and generator; route 3 restricts cycling to it; generated art in a bundle whose entries are independent | §5.5, §5.7, §8.2, §8.3, D-29 |
| TB-5 | minor | (dup CC-5) link failure | **Fixed** with CC-5 | §6.1 |
| TB-6 | minor | `make exult` fails in a fresh out-of-tree build | **Fixed.** `make -j16` (all) for `build-upstream` | §7.1, WP-00 |
| TB-7 | minor | xBRZ 1.9 does not compile with g++ 9.4; `unzip` missing | **Fixed.** `build_xbrz.sh`: SHA check, `python3 -m zipfile -e`, committed one-line patch, patched-source SHA in the sidecars | §8.1, WP-12 |
| TB-8 | minor | (dup PS-3) halving tolerance | **Fixed** with PS-3 | §6.3 |
| TB-9 | minor | Palette 0 index 255 overflows `v·255/63`; tool and engine CRCs could differ | **Fixed.** Conversion defined as the engine's `Get_color8` clamp; BG vector `0xc9c2c0e7` in `hash_vectors.txt`; `mkpack` copies the CRC from `ref.txt` | §3.5, §5.4, §6.2 |
| TB-10 | minor | MSYS2 does not verify the vcxproj; Xcode is unbuildable here | **Fixed.** `check_build_lists.py` lint in `ci.sh`; the claim about MSYS2 verification removed | §6.6, §7.1, D-30 |
| TB-11 | minor | Nothing local compiles the SDL < 3.4 branches | **Fixed.** SDL 3.2.14 in `deps/prefix-3.2`; `build-sdl32` lane with a full build and `make check` | §3.2.7, §6.6, §7.1, WP-00 |
| TB-12 | minor | Out-of-range `copy` rects cannot share the S=1 op stream | **Fixed.** In-range rects only in the shared and golden streams; out-of-range clipping tested on S>1 buffers alone, with canaries | §3.1, §6.2 |
| TB-13 | minor | Windows steps: wrong sfx URL, backslashes eaten by bash, `SDL_RENDER_DRIVER` not forwarded | **Fixed.** Exact URL (`msys2-x86_64-latest.sfx.exe`); quoted `-o` argument; `WSLENV=…:SDL_RENDER_DRIVER/u` or set inside `bash -lc` | §7.2 |
| TB-14 | minor | pytest is not installed in `tools-venv` | **Fixed.** Pinned `requirements.txt` installed with `uv` in WP-01 and in the `ci.sh` bootstrap | §6.1, §7.1, WP-01 |

**Effect on the plan:** about +3.5 engine days (WP-00 +0.25, WP-01 +0.25, WP-03 +0.25, WP-04 +0.25,
WP-05 +1, WP-07 +0.5, WP-08 +1), +0.5 tooling (WP-12) and +0.5 art (WP-13). M1 is now about 37 engine
days. The demo moves from about day 12 to about day 15, and the first real hi-res terrain from about
day 27 to about day 32.

### 12.1 WP-00 corrections (revision 2.1)

WP-00 built the four lanes; its result was then reviewed and verified. The corrections below are
folded into the sections in the last column. The details and measurements are in
`docs-hires/impl/WP-00.md`.

| ID | Sev. | Finding (short) | Disposition | Where |
|---|---|---|---|---|
| W0-1 | major | §7.1's `build-asan` flags never finish `exult.cc` with g++ 9.4 (stopped after 38 min). The cause is configure's `-Wduplicated-branches` on UBSan-instrumented code: with `-Wno-duplicated-branches` the file compiles in about 20 s. configure and make also ran ASan programs (test programs, `expack`) without `setarch -R` and a timeout | **Fixed.** `-Wno-duplicated-branches`, with every UBSan check kept. WP-00's first workaround, `-fno-sanitize=null,alignment,vptr`, is dropped. configure, make and make check run under `env ASAN_OPTIONS=detect_leaks=0 … setarch x86_64 -R timeout …` | §7.1 |
| W0-2 | major | UBSan reports and continues, so a run with undefined behaviour exits 0; the wrappers pinned no sanitizer options | **Fixed.** `-fno-sanitize-recover=undefined`. `TEST_WRAPPER` (leaks on) and `EXULT_WRAPPER` (leaks off) set `ASAN_OPTIONS` and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. The game scripts fail on any sanitizer report in their log | §6.1, §6.5, §7.1, §9 |
| W0-3 | major | §7.1's `build-sdl32` line fails ("must have Ogg/Vorbis"): `PKG_CONFIG_PATH` with only `prefix-3.2` hides the sysroot `.pc` files. Without `PKG_CONFIG_LIBDIR`, a missing `prefix-3.2` silently gives 3.4.18 through pkg-config's built-in path. The default DT_RUNPATH binds 3.4.18 at run time when `env.sh` is sourced | **Fixed.** `PKG_CONFIG_PATH` = `prefix-3.2` + sysroot, `PKG_CONFIG_LIBDIR` = sysroot, `LIBS=-Wl,--disable-new-dtags` (DT_RPATH); `ci.sh` asserts 3.2.14 | §6.6, §7.1 |
| W0-4 | minor | Buildmap hash lists depend on the harness (sandbox path length and config text; for ASan also the environment), because the dependency paint order follows heap addresses | **Fixed** (test rule). O0 compares runs of one harness and regenerates the upstream list when the harness changes. ASan runs are checked for "no sanitizer report" and by in-process NN oracles, never by hash across processes | §6.4 |
| W0-5 | minor | `env.sh` was described as optional for a configure recheck, and lanes built at the same time race on the shared source tree | **Fixed.** `env.sh` is required for every step; one tree at a time | §7.1 |
| W0-6 | minor | `u7art.py` expected a 19-byte v2 `u7chunks` header; the engine's is 10 bytes (`gamemap.cc:85-96`) | **Fixed** in WP-00 (one line). BG's `u7chunks` is v1, so no BG output changes | §9 (WP-12) |
| W0-7 | minor | W0-4's "same config text" could not hold between a hi-res lane and `build-upstream`: WP-00's harness wrote the lane's build directory into the config (`data_path`) and into argv[0], and it passed the caller's whole environment through, which SDL3 copies onto the heap | **Fixed** (test rule, WP-00 round 3). The harness links `exult` and `data` into the sandbox and runs `exult` under `env -i` with a fixed environment; WP-07's `buildmap_golden.sh` does the same. The `build-upstream` lists, regenerated with it, did not change | §6.4 |

---

## 13. User decisions (2026-10-04)

The user answered "ok, procedi" to §10.2 with the recommended defaults, plus one clarification on Q1:

* **Q1 (clarified).** "6x" means 6x the **original game resolution** (320x200 → 1920x1200), i.e. S multiplies
  game pixels, never screen pixels. Larger views (extended/fullscreen modes, e.g. the user's 3440x1440
  profile with an 860x300 game view → 5160x1800 internal) are considered acceptable on the user's
  hardware (RTX 5070 Ti, modern CPU). Consequences:
  * policy `art` keeps **S = 6 on every profile** whose world texture fits the renderer's texture limit;
  * the default `max_world_mpx` becomes **40** (covers 860x300 and larger extended views at S=6); it is a
    safety cap only, not a performance knob;
  * the WP-15 performance rule becomes **measure and report only** (no automatic drop to S=3); if the
    measurement is bad we discuss it with the user first.
* **Q2** yes (MSYS2 under `E:\Dati\Ultima7_Upscale\msys64`, packs under `E:\Dati\Ultima7_Upscale\packs`).
* **Q3** Claude prepares upstream branches; the user decides and submits.
* **Q4** no hosted-model uploads; Phase B uses local models only (WSL CUDA works).
* **Q5** faithful look.
* **Q6** non-commercial NXbrz acceptable for a private, never-redistributed pack.
* **Q7** yes (canonical packs on ext4, mirrored to `E:` with `publish.sh`, generated art as a bundle).
* **Q8** yes (route-3 tiles with B1 between 85 % and 97 % accepted and flagged for review).

Commits go to local branches only (`hires`, `upstream-fixes`); nothing is pushed.
