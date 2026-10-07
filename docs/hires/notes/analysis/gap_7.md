# Gap 7: Can the golden-test oracle `render(S) == NN_upscale(render(1))` be checked in the integrated engine?

Scope: `exult-hires` at upstream master `8b6ab6b43`. All `file:line` references are relative to
`/home/simonea/ultima7_exult/exult-hires` unless stated otherwise. This note builds on `build.md` §6
(the headless `--buildmap` harness, already verified byte-identical across runs), `ibuf.md` §5.6
(primitive-level differential oracle) and `present.md` §7.4 (point x6 as a presentation oracle).
It does not repeat them.

---

## 0. Verdict (TL;DR)

1. **The oracle is feasible in the integrated engine, but only on the 8-bit index buffer, from a frozen
   state, with lerping off and with the time queue never dispatched between or during renders.**
   All three conditions already hold on the `--buildmap` path (`exult.cc:2857-2912`), which is why
   it is byte-identical across runs.
2. **The premise "S=1 and S=6 cannot be rendered in one process" is too strong.** A code audit shows
   that every paint-time side effect does one of four things: it schedules *future* work on the
   time queue, changes palette RGB (never indices), adds dirty rects, or updates bookkeeping. None of
   them changes the indices that an immediately following paint writes (§3). Two consecutive renders
   of the same frozen state into two different render targets are valid. Two conditions apply:
   (a) nothing calls `Time_queue::activate` (`exult.cc:1391`) in between, and (b) the render scale
   belongs to the **render target**, not to the process or window. If the design makes S a global
   that can only change by recreating the window (`setup_video` → `create_surface`), separate
   processes are needed. These are also deterministic, provided the seed, data and save are pinned.
3. **The RNG is not the problem.** There is exactly one RNG (`std::rand`, seeded once from
   `SDL_GetTicks` at `gamewin.cc:587-588`). No world paint function calls it. It is used only by
   `handle_event`, constructors and egg hatching (§2.1). Re-seeding right after `init_files` makes
   restored state reproducible, as long as audio is off: the OPL synth calls `std::rand` from the
   audio thread (`audio/midi_drivers/fmopl.cpp:514`).
4. **The wall clock only matters through lerp.** Palette rotation, fades, day/night transitions and
   lightning change `Image_window8::colors` and the SDL palettes (`iwin8.cc:96-174`). They never
   change the index buffer. Translucency xforms and ramp-remap tables are built from palette 0 or
   from static files (`shapeid.cc:336-358`, `palette.cc:581-600`), so they do not depend on the
   palette in effect at the time. Only `paint_lerped` (`gamerend.cc:434-514`, which takes a
   wall-clock factor from `exult.cc:1480-1500`) changes geometry.
5. **Full-map S=6 `--buildmap` is possible but is the wrong tool for CI.** Measured on the existing
   BG output, one 12288² superchunk image is 151 MB as 8 bpp. A numpy NN check takes 0.08 s, while
   PNG encoding takes 1.5 s and 6.2 MB per superchunk (≈0.9 GB and ≥3.7 min for 144 tiles, before
   the S=6 paint cost). Use a **region-limited `--render-test`** entry point (§5) for CI and keep a
   full-map *in-memory* A/B sweep (no PNGs) for nightly runs.
6. **Some primitives must be exempt from, or masked out of, the oracle** (§7). These are hi-res
   override frames, scaler-prefiltered fallbacks, any "thin" overlay drawn in physical pixels
   (outlines, grid, bbox lines), physical sub-pixel lerp offsets, the guard band, and everything
   after the index buffer (presentation, downscale, RGB). For overrides, an **identity-override
   test** (override = NN-upscale of the original frame, so the output must match bit for bit) and a
   **marker-override** test give exact oracles for the override plumbing itself (§6).

---

## 1. What exactly gets compared

- `Image_window::screenshot(dst, paletted=true)` (`imagewin/imagewin.cc:1254-1256`) saves
  `draw_surface`. `paletted_surface` is **always** `draw_surface` (`imagewin.cc:762`), and
  `draw_surface` is an 8-bit surface of `inter/scale + 2*guard_band` (`imagewin.cc:726-737`).
  `SaveIMG_RW` (`imagewin/save_screenshot.cc:314-370`) crops the guard band
  (`save_screenshot.cc:95-99`) and writes PNG colour type 3 with the SDL surface palette as PLTE
  (`save_screenshot.cc:124-136`).
- In current master only the **world** is drawn into that buffer: terrain, objects, sprites and
  effects, the border fill, and editor overlays. Gumps, text effects, conversation, drag and the
  mouse go to layers through `push_render_target` (`gumps/Gump_manager.cc:197-199`, `drag.cc:317-341`,
  `effects.cc:296-300`). A paletted screenshot is therefore a pure world image, which is exactly the
  scope of the oracle.
- **Compare decoded indices, not PNG bytes.** The PLTE chunk carries the *current* RGB palette,
  including rotations (`gamewin.cc:1054-1077` → `iwin8.cc:135-174`), gamma (`iwin8.cc:104-106`) and
  fades. Two renders with identical indices but taken 100 ms apart have different PNG bytes.
  `--buildmap` avoids this only because it sets gamma to 1 (`exult.cc:2880`) and palette 0
  (`exult.cc:2895`) and never runs the main loop.
- Palette-cycling water and lava are just indices 0xE0-0xFF in the buffer. In the BG superchunk
  `u7map4a`, 514 of 4.19 M pixels are in that range. They need no special handling.

---

## 2. Sources of nondeterminism and S-dependence, classified

### 2.1 RNG

| Fact | Ref |
|---|---|
| Only one engine RNG: `std::rand`; no `<random>` engines, no `random()` in engine code | grep over `*.cc/*.h` (excluding tools/mapedit) |
| Seeded once from wall clock | `gamewin.cc:587-588` (`srand(SDL_GetTicks())` in `init_files`) |
| Weather/particles: `rand` only in `set_frame<...,randomize>` / `do_move` / `change_ndrops` (called from `handle_event`), `Lightning_effect::handle_event`, `Storm_effect` ctor, `Cloud::next/set_start_pos`, `Clouds_effect` ctor | `effects.cc:1330-1334, 1355, 1390-1397, 1476, 1515-1517, 1667-1777` |
| **Paint functions of effects are pure readers of effect state** | `Particledrop::paint` `effects.cc:1309-1322`; `Rain_effect::paint` 1442-1456; `Cloud::paint` 1732-1739; `Sprites_effect::paint` 452-460; projectiles 855-863, 998-1003 |
| Animator frame choice uses `rand` / `Game::get_ticks` only in `get_next_frame` (from `handle_event`) | `objs/animate.cc:368-426` |
| Egg hatching uses `rand` (runs at restore via `activate_eggs`) | `objs/egg.cc:1334`, `gamewin.cc:2947` |
| Audio thread consumes `std::rand` (OPL white noise) → racy interleaving with game RNG | `audio/midi_drivers/fmopl.cpp:514` |
| Static noise (`fill_static`) only in BG intro/menus | `imagewin/ibuf8.cc:115-134`, `gamemgr/bggame.cc:781-1221` |

Consequence: a test hook `srand(seed)` placed **after** `init_files` (it reseeds at 587-588) and
before any restore, with audio disabled, makes restored state reproducible. For a frozen-state
render the seed does not even matter, because paint consumes no random numbers.

### 2.2 Wall clock

| Mechanism | Affects indices? | Ref |
|---|---|---|
| `rotatecolours` (palette cycling) | No (RGB only) | `gamewin.cc:1054-1077`, `iwin8.cc:135-174` |
| Day/night/weather `Palette_transition`, `set_light_source` | No | `gameclk.cc:100-160`, `gamerend.cc:394-410` |
| Lightning flash `pal->set(PALETTE_LIGHTNING)` | No | `effects.cc:1497` |
| Fades (`SDL_GetTicks` busy loops) | No | `palette.cc:391-482` |
| **Lerp factor** → `scrolltx_lo/scrollty_lo`, `avposx_ld/avposy_ld` | **Yes** (geometry) | `exult.cc:1436-1503`, `gamerend.cc:434-514`, `gamewin.cc:1278-1307` |
| `FA_TIMESYNCHED` animation frame from `Game::get_ticks` | Only through `handle_event` | `objs/animate.cc:395-402` |
| `Game::get_ticks` itself is a cached value, set from `SDL_GetTicks` in setup/main loop | n/a | `game.h:180-185`, `gamewin.cc:2873` |

There are 133 `SDL_GetTicks` call sites in engine sources. A virtual clock for *simulation*
equivalence across S is therefore out of scope. The oracle must be a **frozen-state** oracle.

### 2.3 Paint-time side effects (audit)

| Side effect | Where | Changes indices of the next paint? |
|---|---|---|
| `Npc_actor::paint`: `dormant=false`, `tqueue->remove/add(now+500)`, `add_nearby_npc` | `actors.cc:5133-5148` | No (future work only) |
| `Animated_*::paint` → `want_animation` → `start_animation` (`tqueue->add(+20)`) | `objs/animate.cc:592-595, 618-621, 655-658`; `animate.h:114-118`; `animate.cc:301-306` | No |
| `Actor::paint_weapon` updates `weapon_rect` (dirty bookkeeping) | `actors.cc:2118-2147` | No |
| `Game_render::paint_map`: `render_seq++`, `painted=true` | `gamerend.cc:198-199` | No (ordering uses equality with current seq) |
| `Particledrop::paint` → `add_dirty` | `effects.cc:1320` | No |
| Full repaint → `clock->set_light_source(...)`, special-light expiry → `clock->set_palette()` | `gamerend.cc:394-410` | No (palette only) |
| `read_map_data` loads superchunks on demand (creates objects, may start usecode scripts via `Usecode_script::start`) | `gamemap.cc:305-340`, `gamemap.cc:849-853` | Only on first touch. Both renders of the same game-px region load the same set, and the scripts are queued, not run |

So within one process, `render_A; render_B` from the same frozen state produces the same indices
for the same scale. That makes cross-scale A/B valid, **given** the two caveats in §4.

### 2.4 Data inputs that must be pinned

- `--buildmap` reads ireg (movable objects) from `<GAMEDAT>` (`gamemap.cc:793-812`, called from
  `get_superchunk_objects` `gamemap.cc:1199-1203`). With an empty gamedat (the case in
  `run/bg-gamedat`, 0 files), only map terrain and **ifix** objects are drawn: no NPCs and no movable
  items. With a player's gamedat present, the output depends on that save. Tests must point
  `gamedat_path` at an empty scratch dir (static mode) or at a pinned, unpacked save (dynamic mode).
- `<PATCH>` (`PATCH_U7MAP`, `PATCH_U7IFIX`, `gamemap.cc:208, 569`), mods, and the shape/xform
  patches (`shapeid.cc:336-338`) change the output. Pin them as `build.md` §6.3 does.
- Config: `smooth_scrolling` (`gamewin.cc:387`), audio enabled (fmopl RNG), and cheat/editor state
  (`gamerend.cc:220-289`), including `bbox_palindex` (default -1, `gamerend.h:34`).

### 2.5 Uninitialised memory

`Chunk_terrain::render_flats` allocates `new Image_buffer8(128,128)` (`objs/chunkter.cc:259`), whose
constructor does `new unsigned char[w*h]` without value-initialising it (`imagewin/ibuf8.h:37-39`).
A tile that is RLE gets a neighbouring flat painted underneath it (`chunkter.cc:92-131`). If **no**
tile in the chunk is flat, the cache keeps heap garbage, and garbage is visible wherever the RLE
tile is transparent. I checked BG's `u7chunks` and `shapes.vga`: 0 of 3072 chunks (2105 used in
`u7map`) are all-RLE, so BG is not affected. Mods and SI are unverified. The hi-res flats cache must
be value-initialised (`new unsigned char[n]()`), and the golden harness should run once under ASan
or valgrind.

### 2.6 What looks risky but is not, for index compares

- Translucency (`Xform_palette`) tables are loaded from `xform.tbl` or built once from **palette
  0** (`shapeid.cc:336-358`). They are S-invariant and time-invariant. A per-pixel table lookup
  commutes with NN upscaling.
- `PT_RampRemap` tables come from `Generate_remap_xformtable` (`palette.cc:697-724`), whose ramps
  are always computed from palette 0 and cached (`palette.cc:581-600`).
- Border fill uses `pal->get_border_index()`, which depends on the palette number (`palette.cc:124`).
  It is only drawn outside `[0,w)×[0,h)` (`gamerend.cc:370-384`), that is, into the guard band, and
  the screenshot crops it.

---

## 3. The existing static path: `BuildGameMap`

`exult.cc:2857-2912`:

1. Sets gamma to 1 (`2880`) and creates `Game_window(2048, 2048, false, 2048, 2048, 1, point, Fit, point)`
   (`2887`). The window is 2048² game px and the scaler is point x1, so
   `ShouldPaintIntoGuardband()` is false (`imagewin/imagewin.h:815-826`) and no guard-band painting
   happens.
2. Runs `Audio::Init` (`2890`), `Game::create_game` (`2891`), `init_files(false)` (`2892`) (this
   seeds `rand` from the clock and queues the clock), `get_map()->init()`, `set_map(mapnum)`, and
   palette 0 (`2893-2895`).
3. For each of 12×12 superchunks: `paint_map_at_tile(0,0,w,h, x*256, y*256, maplift)` (`2899`) →
   `gamerend.cc:50-68` (sets scroll and skip_lift, runs `read_map_data`, `set_clip`, calls
   `render->paint_map(0,0,get_width(),get_height())`). This bypasses `Game_window::paint`, so it
   skips effects, gumps, the light-source palette update and the guard-band logic. Then
   `screenshot(dst,true)` (`2905`).
4. `exit(0)` (`2910`). The main loop, and with it `tqueue->activate` (`exult.cc:1391`),
   `paint_lerped` and `rotatecolours` (`exult.cc:1436-1518`), is never entered.

Why it is deterministic: there are no NPCs (empty gamedat), and the time queue is never dispatched,
so animators never advance and objects are drawn at their stored frames. Paint consumes no RNG, no
lerp is applied, and the palette is fixed. `build.md` §6.3 measured 144 PNGs in 19.5 s, byte-identical
across two runs.

**Costs at S=6 (measured with the real `run/bg-saves/u7map4a.png`, NN-upscaled in numpy):**

| Item | S=1 | S=6 |
|---|---|---|
| Index buffer per superchunk | 4.2 MB | **151 MB** (12288²) |
| PNG size per superchunk | 1.63 MB | **6.2 MB** (row repetition compresses well) |
| PNG encode (zlib 6) | n/a | **1.5 s** per superchunk → ≥3.7 min, ≈0.9 GB for 144 |
| NN check in numpy (all 36 phases) | n/a | 0.08 s |

Another hazard: if the design's S-x world surface is owned by `Image_window`, `Game_window(2048,2048)`
at S=6 also allocates presentation textures of the same size (12288² ARGB ≈ 604 MB, or 151 MB as
INDEX8). That exceeds the GPU texture limit on many drivers (often 8192 or 16384). A test entry point
must not run presentation at S-x size.

---

## 4. One process or two: refined verdict

**Valid in one process (preferred)** when all of these hold:
- The renders run back to back from the same frozen state, with no `tqueue->activate`, no
  `paint_lerped`, and no event processing in between.
- The S is carried by the **target buffer**. The engine already has the mechanism:
  `Game_window::push_render_target/pop_render_target` (`gamewin.cc:541-550`) switch
  `Image_window8::ib8/ibuf` (`imagewin/iwin8.h:63-68`) and `Shape_frame::scrwin`. Meanwhile
  `Game_window::get_width()` stays the window's `game_width` (`gamewin.h:212-218`,
  `imagewin.h:640-646`). The harness can therefore push a 1x `Image_buffer8(w,h)`, paint, pop, then
  push the S-x target, paint, pop, and compare.
- Per-scale caches coexist. Today `Chunk_terrain::rendered_flats` is a single 1x cache with an LRU
  of 100 (`objs/chunkter.cc:234-268`). The hi-res path needs its own cache, keyed by S, rather than
  replacing the 1x one.
- The paint entry used is `paint_map_at_tile` / `Game_render::paint_map`, **or** `Game_window::paint`
  with the point scaler only. `BeginPaintIntoGuardBand` overwrites `ibuf->width/height` from
  `draw_surface` (`imagewin.cc:1198-1199, 1219-1220`). With a non-point scaler and a redirected
  target, it would corrupt the redirected buffer's geometry.

**Separate processes are required** if S is global, for example fixed at window creation, or if
switching S goes through `setup_video`/`resized` (`imagewin.cc:858-876`, `toggle_fullscreen` 1119-1139). Those paths recreate
surfaces and can trigger paints and effects. Cross-process determinism then depends on pinned
data, save and seed, audio off, and no main loop. Static mode already satisfies this. Dynamic mode
(save restore) should be self-checked by rendering twice at S=1 and asserting identical indices.

**Simulation equivalence across S** (does game logic evolve identically at S=1 and S=6?) is not
practical to test end to end, because of the 133 wall-clock sites. It is covered indirectly by the
scalar digest (O7 below), which catches S leaking into logic-visible quantities.

---

## 5. Proposed headless, region-limited render entry point

**CLI** (declared next to `--buildmap` at `exult.cc:289-312`, validated like `exult.cc:393-430`,
and dispatched next to `exult.cc:986-989`; the `setup_video` guard at `exult.cc:825` must also skip
it):

```
exult -c test.cfg --bg [--mod M] --render-test "map=0,tx=…,ty=…,w=512,h=512,lift=16,scale=6,
      seed=1,save=<path|none>,effects=0,overrides=0|1|identity,mode=static|dynamic,
      ab=1,out=<dir>"
```

**Sequence** (`RenderTest`, modelled on `BuildGameMap`):

1. `Image_window8::set_gamma(1,1,1)`. Create `Game_window(w,h,false,w,h,1,point,Fit,point)` in
   **game px**, which keeps presentation small. Audio must be disabled in `test.cfg`
   (`run/exult.cfg` already has `<audio><enabled>no</enabled>`).
2. `Game::create_game`, `init_files(false)`, then **`srand(seed)`** (overriding `gamewin.cc:588`).
3. Static mode: `get_map()->init(); set_map(map); get_pal()->set(0)` with an **empty** gamedat dir.
   Dynamic mode: `restore_gamedat(path)` (`gamedat.cc:175`) + `read()` (`gamewin.cc:1453-1469`),
   which runs `setup_game` and `activate_eggs` (`gamewin.cc:2860-2960`, 2947). Then
   `set_lerping_enabled(0)`, purge effects if `effects=0`
   (`Effects_manager::remove_all_effects`, `effects.cc:184`), and set scrolls with `set_scrolls(tx,ty)`
   (`gamewin.cc:1085`). Note that `read()` leaves the palette **faded out** (`gamewin.cc:2923`), and
   `Palette::set` is then a no-op (`palette.cc:136-138`). Call `fade_in(0)` (or equivalent) before
   writing viewable PNGs. Indices are unaffected either way.
4. Render. Static mode uses `paint_map_at_tile(0,0,w,h,tx,ty,lift)`. Dynamic mode uses
   `Game_window::paint()` with the point scaler, which includes `Effects_manager::paint`, border
   fill and the light pass. Record `paint_map`'s return value (the light-source count).
5. If `ab=1`: render to a 1x target and to the S target via `push_render_target` as in §4, compare
   in process, write `ref_1x.png`, `hi_S.png` and `diff.png` (mismatching game pixels marked), and
   set the exit code to 0 or 1. Otherwise write the S-x index image only.
6. Write the scalar digest (O7) as JSON. Call `exit()` without entering the main loop.

**Golden regions (BG, static mode first):** open terrain with RLE shorelines, water and lava
(cycling indices), dense forest (large RLE plus translucency), a town at `lift=16/10/5`
(roof modes, `exult.cc:2861-2872`), mountains, superchunk and world-wrap borders (tx near 0 and
3071; `Figure_screen_offset` wraps at `gamerend.cc:74-85`). **Dynamic mode** (pinned saves): a
crowded town with NPCs in several schedules, a dungeon (blackness `gamerend.cc:648-700`, needs
`main_actor`), an invisible party member (`paint_invisible`), rain, snow and clouds with
`effects=1`, and a projectile in flight. CI uses synthetic data only (see `build.md`); the BG
regions run locally or nightly.

**Nightly full sweep:** a `BuildGameMap` variant that does the in-memory A/B per superchunk at S=6
and writes only hashes and failure crops. Peak memory is about 155 MB plus caches, and no 0.9 GB of
PNGs is written.

---

## 6. Oracle family (what to assert)

| # | Oracle | Exact? | Notes |
|---|---|---|---|
| O1 | Primitive differential fuzz: same random op sequence on S=1 and S=k buffers, `phys == NN(ref)` | exact | `ibuf.md` §5.6; no game data; CI |
| O2 | Static region: `render_S == NN(render_1)` with overrides off | exact | §5 static mode; any S (test S ∈ {2,3,6}) |
| O3 | Dynamic region (pinned save, effects on/off) | exact | same, plus self-check (two S=1 runs identical) |
| O4a | **Identity override**: generate override PNGs that are the NN-upscale of the original frames (all flats 0..149 plus some RLE, translucent and reflected frames), load as overrides, require output identical to O2/O3 output | exact | tests hotspot mapping, clipping, transparency (255/tRNS), translucency, reflection, flats-cache path |
| O4b | **Marker override**: as O4a, but with one physical pixel per S×S block (or a corner) set to a unique index | exact (predictable diff) | proves overrides are actually used; diff must equal the predicted marker set |
| O5 | Real overrides: `render_S(ovr) == render_S(no ovr)` outside the union of override footprints | exact outside mask | needs a debug hook that records each override paint's game-px rect; recommend clipping overrides to S × original frame bbox so the footprint is well defined |
| O6 | Repaint idempotence at S: full paint, snapshot, `paint(subrect)` for random logical subrects; buffer unchanged | exact | catches physical/logical clip mapping, guard-band and dirty-rect bugs without a 1x reference |
| O7 | S-invariant scalars: `paint_map` light count, palette number and brightness after a full repaint, `get_width/height`, number of superchunks read, nearby-NPC count, tqueue size, `mini_screenshot` bytes | exact | catches S leaking into logic, e.g. the full-repaint test at `gamerend.cc:394` or chunk ranges at `gamerend.cc:206-219` fed with physical px |
| O8 | Presentation and downscale (RGB) | tolerance ±1 | outside the index oracle (`build.md` §6.4 item 4) |

On O7 and `mini_screenshot`: it averages 3×3 game px (`iwin8.cc:181-226`). If the S version averages
the corresponding 3S×3S physical block, the average of an NN-upscaled image is identical, so the
save thumbnail is an exact S-invariant for non-override renders. Today it indexes `ibuf` with
game-size math (`iwin8.cc:190-201`) and will need adapting.

---

## 7. Exempt primitives and excluded conditions

| Primitive / condition | Where | Why NN fails | Handling |
|---|---|---|---|
| Hi-res override frames | (new) override paint path | by definition | O4a/O4b exact; O5 masked |
| Fallback pre-scaling of non-override frames with xBR/hq | (new, optional) | filter ≠ NN | oracle runs with fallback = NN; pre-scale tested by golden hashes only |
| Status and selection outlines if drawn 1 physical px thin | `Shape_frame::paint_rle_outline` `shapes/vgafile.cc:640-699`; `Actor::paint` outlines `actors.cc:2094-2111`; selected objects `gamerend.cc:271-279` | the outline would be thinner than S | default NN (no exemption); if "thin" is chosen, exclude via test state (no hit, charm, poison or other flags) or a mask from outline rects |
| Editor grid, chunk outline, chunk label text | `gamerend.cc:91-125, 140-151, 243-251, 281-289` | 1-px `fill8`/`fill_translucent8`, `paint_text` | editor-only; run tests with `cheat.in_map_editor()` false |
| Bbox debug lines (`draw_line8`) | `shapes/shapeinf.cc:546-605`, `gamerend.cc:607-617`, `imagewin/ibuf8.cc:239` | a physical Bresenham line is not the NN of a 1x line | `bbox_palindex = -1` in tests; or require `draw_line8` to rasterise in game px and fill S×S blocks |
| Physical sub-pixel lerp offsets | `gamerend.cc:434-514`, `gamewin.cc:1301-1306` | offset not a multiple of S | tests run with lerp off (or `factor = 0x10000` → offset 0). Optional translation oracle for interior pixels: `render_S(off=qS+r)` = `NN(render_1(q))` shifted by r, excluding party and barge, which use `avposx_ld` |
| Guard band (painting and border fill) | `imagewin.cc:1142-1224`, `gamerend.cc:338, 370-384` | partial-pixel band if defined in physical px | always crop to `[0,S·w)×[0,S·h)`; the crop must use the S-x guard size |
| Presentation, downscale, gamma, RGB | `imagewin.cc` show path | not an index image | O8 |
| `fill_static` | `ibuf8.cc:115-134` | RNG, intro only | not world; excluded |
| Fonts drawn into the main buffer, if overridden | `gamerend.cc:102` (editor only) | override | as overrides |

Everything else in the world path must satisfy the oracle **without** exemption: `copy8` of flats
(`gamerend.cc:529`), `paint_rle`, `paint_rle_translucent`, `paint_rle_transformed`, `paint_invisible`,
`fill8` (blackness, `gamerend.cc:664-700`), `fill_translucent8`, sprites, clouds, rain/snow/sparkles,
projectiles, and terrain-only mode (`gamerend.cc:157-183`).

---

## 8. Design constraints the oracle imposes (recommendations)

1. **Make S a property of the render target or view**, not of the process or window. This enables
   in-process A/B through the existing `push_render_target`, and it is consistent with the
   "mutate `ib8` in place" constraint from `ibuf.md` §1.
2. Keep **all logic-visible geometry in game px**: `get_width/height`, `paint(x,y,w,h)` arguments,
   dirty rects, `paint_map` chunk ranges, effect state (`effects.cc:1422-1429` use
   `get_game_width`; `Cloud` ctor `effects.cc:1651-1653`).
3. Default every overlay primitive to NN (S×S blocks). Make "thin" variants an explicit option that
   is off in tests.
4. Make lerp offsets game-px by default. A physical-precision smooth scroll is an opt-in that is
   excluded from O2 and O3.
5. Value-initialise new hi-res caches (§2.5). Key caches by S so 1x and S-x coexist.
6. Clip override images to S × the original frame bbox, which makes O5 masks exact. Expose a debug
   hook that records override paints.
7. Provide a runtime switch to disable overrides and to force NN fallback (needed by O2–O5).

---

## 9. Latent upstream issues found while auditing

- `Sprites_effect::paint` subtracts `get_scrolltx_lo()` from the **y** coordinate
  (`effects.cc:459`; it should be `get_scrollty_lo()`). Sprites jitter vertically while lerping.
  This is harmless for the oracle with lerp off, but worth fixing in the fork.
- `Chunk_terrain::render_flats` uses an uninitialised buffer (§2.5).
- `paint_tile` neighbour search uses `tiley + y > 0`, so it never looks at row 0 neighbours
  (`chunkter.cc:104`). This is deterministic and cosmetic.
- `BeginPaintIntoGuardBand` assumes `ibuf` is the window buffer (`imagewin.cc:1168-1201`), which is
  unsafe with a redirected render target and a non-point scaler.

---

## 10. Touchpoints (summary)

| Where | What | Change |
|---|---|---|
| `exult.cc:289-312, 393-430, 825, 986-989` | CLI declare/validate/dispatch | add `--render-test` spec; skip `setup_video`; dispatch `RenderTest` |
| `exult.cc:2857-2912` | `BuildGameMap` | template for `RenderTest`; nightly S sweep variant with in-memory A/B |
| `gamewin.cc:587-588` | `srand(SDL_GetTicks())` | test hook to reseed after `init_files` |
| `gamewin.cc:541-550`, `imagewin/iwin8.h:63-68` | render-target redirection | carry S on the target; used by A/B |
| `gamerend.cc:50-68, 192-291, 328-414` | paint entries | S-agnostic arguments in game px; return light count for O7 |
| `gamerend.cc:434-514`, `exult.cc:1436-1503` | lerp | keep game-px offsets by default; off in tests |
| `objs/chunkter.cc:248-268` | flats cache | separate value-initialised S-x cache keyed by S |
| `imagewin/imagewin.cc:1254-1271`, `save_screenshot.cc:95-99, 314-370` | paletted screenshot | dump the S-x index surface with S-x guard crop |
| `imagewin/imagewin.cc:1142-1224` | guard band | define the band in physical px; safe with redirected targets |
| `imagewin/iwin8.cc:181-226` | `mini_screenshot` | sample 3S×3S blocks (O7 invariant) |
| `shapes/vgafile.cc:640-699`, `shapes/shapeinf.cc:546-605`, `gamerend.cc:91-151` | outlines, bbox, grid | NN by default; thin optional |
| `effects.cc:452-460` | sprite y lerp bug | fix `get_scrollty_lo` |

---

## 11. Risks and open questions

- **Dynamic-mode determinism is inferred, not yet measured.** Restore runs usecode and eggs, and
  `read()` runs fades and `clear_screen(true)`. Some path may consult `SDL_GetTicks` to decide
  visible state. Mitigation: the first test in each dynamic suite renders twice at S=1 and asserts
  identity.
- **Superchunk load order can affect render order.** Object dependencies are computed as
  superchunks load. A region test and `--buildmap` may load superchunks in different orders, so
  compare like with like (same entry, same region), never region crops against `u7map??.png`.
- If the design puts S on the window only, in-process A/B is lost and every check costs two engine
  start-ups.
- Do SI and mods contain all-RLE chunks (uninitialised cache)? Run once under ASan or valgrind.
- Should "thin" outlines and physical lerp be offered at all? Each one is an oracle exemption.
- Is `Game_window::paint` (dynamic mode) needed in CI, or is `paint_map_at_tile` plus effects
  painted explicitly enough? The latter avoids the guard band and the light-pass palette writes.
