# Proposal C: art pipeline and modder-first hi-res design (architect C_art_modder)

Repo: `/home/simonea/ultima7_exult/exult-hires`, master `8b6ab6b43`. All `file:line` anchors refer to that tree and were re-checked for this proposal unless marked "(map)", meaning they come from `docs-hires/analysis/00_architecture_map.md` and its synthesizer verified them.
Inputs: the architecture map (read in full), `gap_1`, `gap_3`, `gap_5`, `gap_6`, `gap_7`, `build.md`, `present.md` and `gap_2` (summaries), and `upscale-research/00_recommendation.md` plus `prior_art.md` §3-4.

---

## 0. Summary

**Core idea.** Treat the hi-res feature as a *content pipeline with an engine at the end*, not as an engine feature that also needs content. Four guarantees make that work:

1. **The engine is the only source of truth.** A new mode, `exult --dump-art`, writes a *reference set* for one configuration. It contains every 1x source the engine actually draws, the canonical CRCs, the terrain keys, the effective palette and the terrain and world tile tables. Every offline tool (Python pipeline, validator, QA, templates) reads only this reference set. No tool re-implements engine rules.
2. **One file per override, self-describing, by convention.** Overrides are raw-index PNGs named by key (`flats/0019_03.png`, `terrain/9a1c0e44b2d6f001.png`). Each PNG carries its guard (the source CRC) in a `tEXt` chunk. To add an override, a modder drops a file into a pack folder. A manifest (`hires.txt`, Exult's own `%%section` format) is optional. It adds metadata, named **sets** and atomic **families**.
3. **A live authoring loop inside the engine.** A dev mode adds hot reload (key or trigger file), an inspector that shows the key under the mouse and why an override is or is not active, template export (Mesen-style "builder"), an overlay that highlights fallbacks, and an on/off A/B toggle. A modder never has to compute a hash or guess a file name.
4. **Exact oracles at every boundary.** NN equivalence (`render_S == NN(render_1)`), identity and marker packs generated from the reference set, an "S=1 equals the upstream binary" oracle, cross-language hash vectors, and a validator whose rules come from the same C++ library the engine loader uses.

The engine architecture follows the map's D1-D10 (S on the render target, logical API with physical storage, one main buffer, a new present path, NN fallback, terrain cache composition, the sprite slot in M2). The deviations are listed in §13. The most important ones:

* The **default render-scale policy is `art`**: render at the art scale (6) whenever the caps allow, and downscale at present. This is the user's stated preference.
* The **per-terrain key is fill-independent**. This lets the fork keep 1x output byte-identical to upstream. The P1/P2 fixes become upstreamable commits that do not block the art.

---

## 1. Goals and non-goals

**Goals**
* G-1: Render the world at S_eff (6 for the target profile), with hi-res terrain flats first. Game logic, picking and dirty rects stay in game px (map I1-I5).
* G-2: Selective overrides at four granularities: single tile, tile family, whole terrain (chunk template), and named sets. Each can be switched on and off without editing art.
* G-3: An authoring loop of edit, save, see it in game in under 2 s, without restarting.
* G-4: Validation that catches every error a modder can realistically make (wrong palette, editor-remapped indices, wrong size, cycling indices in the wrong place, stale art against a modded `shapes.vga`) and names the rule and file.
* G-5: A reproducible production pipeline for the 3,885 BG flats at 6x on what exists today: WSL CPU, WSL CUDA on the RTX 5070 Ti, and later Windows ComfyUI. QA gates are measured, not eyeballed.
* G-6: Tests for all new functionality: SDL-free unit tests, headless golden tests and pipeline tests.

**Non-goals (v1)**
* RGBA or truecolour overrides (map §4.1). Indexed against palette 0 only.
* Per-placement terrain art (per map chunk or per superchunk at runtime; gap_1 §4, rejected).
* Runtime neighbour conditions per pixel (Mesen `tileNearby`). Context is resolved offline into per-terrain overrides.
* Shipping EA-derived art. Packs are built locally from the user's own data (research §5).
* Changing 1x game behaviour in the fork's default build (§3).

---

## 2. The workflow the engine must serve

```
          ┌─────────────────────────── engine (exult) ─────────────────────────────┐
 game data│ --dump-art ──► art_ref/<cfg>/ (1x sources, CRCs, terrain keys, tables) │
          │ --render-test ─► indexed region renders at S (goldens, QA previews)    │
          │ play/dev mode ─► hot reload · inspector · template export · A/B        │
          └──────▲──────────────────────────────┬───────────────────────────────────┘
                 │ packs (x6/…/*.png, hires.txt)│ templates, todo, inspector keys
   ┌─────────────┴──────────┐        ┌──────────▼─────────────┐
   │ hirestool (C++, no SDL) │◄──────│ u7hires (Python)       │
   │ check · stamp · synth   │ gate  │ context · routes · QA  │
   │ index · downscale · pack│       │ quantize · consensus   │
   └─────────────────────────┘       │ review · promote       │
                                     └────────────────────────┘
```

**Persona A: a modder fixing one tile.**
1. Run Exult with `<hires><dev>yes</dev>`.
2. Hover over the tile and press **Ctrl-Alt-I**. The toast reads `flat 0016:03 · terrain #2940 key 9a1c0e44b2d6f001 · NN (no override)`, and the key is copied to the clipboard.
3. Press **Ctrl-Alt-E**. Three files are written to `<pack>/x6/_work/`:
   * `flats/0016_03.png`: the NN-6x template with its guard;
   * `terrain/9a1c….png`: the 768² NN template;
   * `terrain/9a1c….ctx.png`: the 3×3-chunk 1x context at 6x.
4. Paint in Aseprite in indexed mode, using the palette file shipped in the reference set. Save into `<pack>/x6/flats/`.
5. Press **Ctrl-Alt-R**, or let the pipeline touch `.reload`. The engine rescans, validates and flushes the terrain cache, then repaints. If validation fails, the toast names the rule (`P4: cycling index 0xE3 outside source mask at (17,40)`) and `hires.log` has the detail.
6. Press **Ctrl-Alt-O** to flip between override and NN.

**Persona B: our AI pipeline.**
1. Run `--dump-art` once per configuration.
2. Run `u7hires context` to build windows and macro sheets from the reference tables.
3. Run `u7hires build --route xbrz|sr|comfy` into a set.
4. Gate with `hirestool check`.
5. Run `u7hires qa`, which reports metrics, contact sheets and `--render-test` previews at 6x and at real display sizes.
6. Review, then `u7hires promote` the accepted frames into the curated set. The running engine reloads by itself.

---

## 3. Decision: 1x behaviour, P1/P2/P3

**Decision: in its default build, the fork keeps every deterministic 1x pixel byte-identical to upstream master.** Fixes that change 1x output are kept as separate, upstreamable commits on a branch `upstream-fixes` and are submitted upstream. The fork merges them only after upstream accepts them.

| Fix | Changes deterministic 1x output? | Fork default | Rationale |
|---|---|---|---|
| P1 `tiley + y > 0` (`objs/chunkter.cc:104`, verified) | yes (BG: 2,367 positions, 779 terrains) | **upstream-fixes branch** | Genuine off-by-one, but cosmetic (under RLE objects) |
| P2 12/0 fallback (`chunkter.cc:119-126`, verified) | yes (112 positions) | **upstream-fixes branch** | Same |
| P5 sprite y-lerp (`effects.cc:459`, verified: subtracts `get_scrolltx_lo()` from y) | only while lerping | **upstream-fixes branch** | Not needed by hi-res |
| P3 zero-fill (`imagewin/ibuf8.h:37-39`, `chunkter.cc:259`) | only replaces heap garbage | **fork, mandatory** | Determinism |
| P4 `paint_tile` returns the effective `ShapeID` | no (refactor) | fork | Needed by per-tile composition |
| P6 `encode_rle` runs, P7 empty frame, P8 `reload_shapes` | no | fork | Safety |

Why this is safe for the art: three design choices remove P1/P2 from the critical path.
1. The **per-tile path calls the same `paint_tile` selection** (P4), so the hi-res fill always matches whatever 1x fill the binary has. The O2 oracle holds with or without P1/P2.
2. The **per-terrain key is fill-independent** (§4.4). Cells that are not the terrain's own flat are hashed as zeros plus a kind bitmap, so a later P1/P2 merge does not re-key a single terrain. Per-terrain art paints the under-RLE area itself.
3. Keeping 1x identical turns **the unmodified upstream binary into an oracle** (O0, §8). The only exception is the cells P3 zero-fills: in BG, the 149 tiles of flat 48/8 in 2 terrains. The dump lists them and O0 masks them.

---

## 4. Architecture

### 4.1 Components

| Component | Location (new unless noted) | SDL? | Used by |
|---|---|---|---|
| Hires core: keys, naming, FNV-1a-64, canonical CRC, terrain key, PNG I/O with guards, mode-filter reduction, rule checks | `shapes/hires/hires_core.{h,cc}`, `hires_png.{h,cc}`, `hires_rules.{h,cc}` | no | engine, hirestool, tests |
| Pack discovery, sets, manifest parsing, store (M1: flats and terrain) | `shapes/hires/hires_pack.{h,cc}`, `hires_store.{h,cc}` | no | engine, hirestool |
| Terrain composition | `objs/chunkter.{h,cc}` (changed) | no | engine |
| Dev features: reload poll, inspector, template, overlay, recorder | `hires_dev.{h,cc}` (top level, needs `Game_window`), `keys.cc`/`keyactions.cc` (changed) | yes | engine |
| CLI modes `--dump-art`, `--render-test`, `--hires-*` | `exult.cc` (changed), `hires_cli.cc` (new) | yes | pipeline, tests |
| `hirestool` | `tools/hirestool.cc` | no | pipeline, CI |
| Python pipeline `u7hires` | `tools/hires/u7hires/` (next to the existing `u7art.py`) | n/a | art production |

The core is SDL-free so that it links into `tests/` and `hirestool` with `-lpng -lz` only, like ipack does (`build.md` §6.2). It is compiled only under `HAVE_PNG_H` (`shapes/pngio.h:27`). Without libpng (Android, iOS), the loader reports "no PNG support" and the engine runs NN only until the M2 companion format exists.

### 4.2 Keys and identities (normative)

```cpp
namespace Hires {
enum class Kind : uint8_t { Frame = 0, Terrain = 1 };
struct Key {                 // frame overrides
    uint8_t  file;           // ShapeFile (shapeid.h:39-51); SF_SHAPES_VGA for flats
    uint16_t shape;          // logical shape (imports keep 2048+)
    uint16_t frame;          // flats: frame & 31 (vgafile.cc:912-914); RLE: requested frame
    uint64_t packed() const { return (uint64_t(file) << 40) | (uint64_t(shape) << 16) | frame; }
};
using Terrain_key = uint64_t;  // FNV-1a-64, see below
uint32_t content_crc(const Shape_frame&);          // gap_6 §7.3 canonical frame CRC32
Terrain_key terrain_key(const Chunk_terrain&, const Image_buffer8& flats1x);
}
```

* **Frame guard.** `content_crc` is CRC32 (IEEE, zlib `crc32`) over the canonical frame: `u8 kind | i16le xleft,xright,yabove,ybelow | w·h indices (transparent → 255) | ceil(w·h/8) mask bytes`. For flats that is `0 | 8,-1,8,-1 | 64 bytes | FF×8`. The same function runs in `--dump-art`, in `hirestool` and in the runtime loader. A Python port is checked against shared vectors in `tests/data/hash_vectors.txt`.
* **Terrain key, version T1 (fill-independent).**
  * Byte stream: `"U7TK" | 0x01 | own[32] | pix[16384]`.
  * `own` is a 256-bit map. Bit *t* is set when tile *t* resolves to a non-RLE frame, i.e. `get_shape()` (`objs/chunkter.h:85-86`) is non-null and `!is_rle()`.
  * `pix` is the 128×128 1x raster. Own-flat cells hold their 64 pixels; every other cell (RLE, unresolvable) holds zeros.
  * Key = FNV-1a-64 (offset `0xcbf29ce484222325`, prime `0x100000001b3`).
  * The key is unchanged by any change to the fill heuristic. It changes on any edit of a flat's pixels, any map-editor edit of the terrain, and any change from flat to RLE or back. Terrain numbers and `modified` are never used (gap_1 §1.4).

### 4.3 Packs, sets and the store

**Search chain.** The first root that has a key wins, per key:
1. `--hires-pack <dir>` (repeatable, CLI).
2. `<PATCH>/hires` (mod-specific). `<PATCH>` is replaced by the mod's patch dir (`gamemgr/modmgr.cc:70-74`, verified).
3. `<PREFIX_HIRES>`: a new tag registered next to `<…_PATCH>` (`gamemgr/modmgr.cc:606-610`, verified pattern). Config key `config/disk/game/<g>/hires_path`, default `$game_path/hires`.
4. `<DATA>/hires/<game>` (Exult-bundled; empty today).

**Inside a root.** The scale folder is chosen first: `x<S_eff>` if present, otherwise `x<S_art>` reduced by the integer factor `S_art/S_eff` (§4.5).
* The **base set** is the files directly under the scale folder.
* **Named sets** live in `sets/<name>/` with the same sub-layout.
* Priority comes from the `sets` section of `hires.txt`, overridable by config. A leading `-` disables a set.
* Families (`%%section families`) group keys. A family listed in `%%section atomic` activates only if **every** member has a valid override (map §4.5 strict semantics, applied to terrain families such as "water + shore").

**Store (M1 subset of map H1).**

```cpp
class Hires::Store {
public:
    int  scale() const;                 // S_eff
    uint32_t generation() const;        // bumped by rescan, scale change, reload_shapes
    // Returns 8S×8S indices or nullptr; validates on first use; caches state per key.
    const uint8_t* flat(int shape, int frame, const Shape_frame& src1x);
    // Decodes a complete (128S)² override into dst (physical rows); false if none/invalid.
    bool terrain(Terrain_key k, Image_buffer8& dst);
    Rescan_result rescan();             // diff by (path,size,mtime); drops changed entries
    const Entry_info* explain(Key) const;          // for the inspector
    const Entry_info* explain(Terrain_key) const;
};
struct Entry_info { enum State {Unknown, None, Ok, Rejected, Inactive_family, Guard_mismatch} st;
                    std::string path, set, rule, detail; };
```

* **Indexing.** Discovery uses `std::filesystem` (already used, `gamedat.cc:50`) over the chain at startup and on rescan, and builds `key → File_ref{path,set,size,mtime}`. Nothing is decoded at that point.
* **Flats.** Decode lazily on first use. A flat decoded at 6x is 2,304 B; all of BG is about 9 MB (gap_3 §5.4), so flats stay resident.
* **Terrain overrides.** Decode straight into the terrain cache buffer (1.2 ms per 768² PNG, gap_1 §2.3). The cache is the only copy.
* **States.** `explain()` keeps every rejection reason for the inspector and `hires.log`.
* **Palette check.** At pack open, the CRC of the effective palette 0 (static + `<PATCH>`, `palette.cc:141` (map)) is compared with `hires.txt:palette_crc`. A mismatch disables the whole root with one clear message.

### 4.4 Terrain path (M1)

Changes, consistent with map G11-G16:

* **`Chunk_terrain::paint_tile`** (`objs/chunkter.cc:86-133`, verified) returns the `ShapeID` it actually painted, or an invalid ID (P4). The neighbour search is unchanged (§3).
* **`chunkter.h:36-101`** gains:
  * `Image_buffer8* flats1x`, value-initialised (P3);
  * `uint8 hires_scale`, `uint32 hires_gen`, `uint8 compose_src[256]` (0 = per-tile override, 1 = NN, 2 = terrain override; used by the inspector and the overlay);
  * a static `flush_all_rendered()`;
  * a static byte counter.
* **`get_rendered_flats()`** (`chunkter.h:95-101`): re-render when `hires_scale != S_eff || hires_gen != store.generation()`.
* **`render_flats()`** (`chunkter.cc:248-268`, verified), new body:

```
evict tail while bytes > budget && queue_size > working_set        // G12, byte budget
paint 256 tiles into flats1x (zero-filled), eff[t] = paint_tile(t)
if S_eff == 1 or overrides off:  rendered_flats = flats1x (alias); return
alloc rendered_flats(128,128, scale=S_eff) zero-filled if needed  // B1/B7: logical 128², physical (128S)²
k = Hires::terrain_key(*this, *flats1x)
if store.terrain(k, *rendered_flats): compose_src[*] = TERRAIN
else for t in 0..255:
    f = eff[t].valid ? store.flat(eff[t].shape, eff[t].frame & 31, *eff[t].get_shape()) : nullptr
    if f: memcpy 8S rows of 8S bytes into the cell; compose_src[t] = TILE
    else: NN-expand the 8×8 cell of flats1x;          compose_src[t] = NN
if dev overlay on: draw a 1-physical-px border in a marker index around TILE/NN cells
```
* **Precedence:** terrain override → per-tile override (guarded by the frame CRC, gap_1 §8) → NN.
* **`commit_edits()`** (`chunkter.cc:208-216`, verified) re-keys automatically, because the key is recomputed.
* **`gamerend.cc:520-531`** (verified: `copy8(cflats->get_bits(), c_chunksize, c_chunksize, …)`) becomes the scale-aware `win->blit(*cflats, xoff, yoff)` (map B7/G5).
* **`write_minimap`** (`gamemap.cc:1690`) reads `flats1x` and never builds S× caches (G15).
* **`Game_map::clear_chunks`** (`gamemap.cc:259-270`) deletes the terrains (the destructor unlinks them, `chunkter.cc:182-186`). It must also reset the byte counter.
* **`Game_window::resized`** (`gamewin.cc:916-937`) and a store rescan call `flush_all_rendered()`.
* **Terrain-editor `render_all`** (`chunkter.cc:284-313`, verified) and the flat branch of `Shape_frame::paint` (`shapes/vgafile.cc:525-534`, verified) stay NN in M1. `Shape_frame` has no identity until M2 (map H4). `render_all` can use `store.flat()` directly, because it knows the `ShapeID`.

### 4.5 Render scale policy and present path

The map's D4 present path is adopted unchanged: `world_texture`, the logical display rect, and ARGB+LUT baseline with INDEX8 under `SDL_VERSION_ATLEAST(3,4,0)`. The policy changes (deviation 1):

```
compute_render_scale(display, full, policy, S_art=6, S_cap, max_tex, max_world_mpx):
  art  (default): S = S_art
  auto          : S = snap_up_to_divisor(ceil(max(display/full)), S_art)        // map D5
  N | force:N   : S = N
  then while (full·S > max_tex or full·S Mpx > max_world_mpx or S > S_cap): S = next lower divisor of S_art
```

* **Why `art`.** The user explicitly wants "render high-res, downscale at the end". `art` and `auto` differ only when the presentation ratio is ≤ 3, for example a 960×600 window. On the target windowed profile both give S=6 at 1:1, and on the 3440×1440 Auto ACF profile both give 6 (gap_2 §0).
* **Caps.** `max_world_mpx` defaults to 10 Mpx, which covers the 860×300 ACF view (9.29 Mpx). It protects Studio zoom and Fill ×1 from the 16384 texture crash (map risk 2).
* **Downscale quality is therefore not optional.** With `r = display/(full·S)`:
  * r ≥ 1 integer: NEAREST.
  * r > 1 fractional: PIXELART if SDL ≥ 3.4, else LINEAR.
  * 0.5 ≤ r < 1: LINEAR.
  * r < 0.5: if `1/r` is an integer k ∈ {2,3,6}, a **CPU area filter fused with the palette LUT**, averaging in sRGB 8-bit with round-half-up (1.69 ms for 1920×1200→640×400, present.md §7.5). Otherwise a GPU halving chain followed by LINEAR.
  * The averaging definition is normative because the QA previews (§7.3) reproduce it in NumPy (oracle O8).
* **Art when S_eff < S_art.** A root without an `x<S_eff>` folder uses `x6` reduced by the factor m = 6/S_eff ∈ {2,3}. The reduction is a **class-preserving mode filter** (`hires_rules.cc`):
  * per m×m block, take the most frequent index;
  * if the block's 1x parent pixel is in a cycling range R and the block contains indices in R, choose among those only;
  * break ties by the smallest index.
  * The result is deterministic, index-safe and S-commutative with NN. Packs may ship a hand-made `x3/` instead (VCMI and diablo1-4k pattern).

### 4.6 Developer features (dev mode)

All features are gated by `config/video/hires/dev=yes` or `--hires-dev`. They register as `Action::cheat_keys` entries in the `keys.cc` table (pattern at `keys.cc:108`, verified), with default bindings appended to `data/bg/defaultkeys.txt` and the SI file. The combinations below are unused today (verified against `data/bg/defaultkeys.txt`).

| Action | Key | What it does |
|---|---|---|
| `HIRES_RELOAD` | Ctrl-Alt-R | `store.rescan()`; for each changed key report OK or the rule; flush terrain caches; `set_all_dirty` + `paint`. |
| `HIRES_TOGGLE` | Ctrl-Alt-O | Overrides on/off at the same S (A/B). Flushes caches. |
| `HIRES_INSPECT` | Ctrl-Alt-I | Converts the mouse position with `screen_to_game` (`imagewin.cc:1277-1294`, map) to tile `tx,ty` at lift 0, then gets the chunk terrain, the terrain number (`gamemap.h:108-110`, verified) and the flat. It reports the effective fill ID, `compose_src`, the terrain key and `store.explain()`. Output goes to `effects->center_text` (the same mechanism as the resize toast, `gamewin.cc:930-931`), to `hires.log` and to the clipboard (`SDL_SetClipboardText`). M2 adds the object under the cursor via `find_object`. |
| `HIRES_TEMPLATE` | Ctrl-Alt-E | Writes NN templates with guards for the inspected flat and terrain, plus a 3×3-chunk 1x context, into `<first writable root>/x6/_work/`. |
| `HIRES_OVERLAY` | Ctrl-Alt-V | Cycles the overlay: off → mark NN cells → mark per-tile cells. Implemented in the composition step (§4.4), so it costs nothing when off. |

* **Trigger file.** In dev mode the main loop checks the mtime of `<root>/x<S>/.reload` every 500 ms. The check goes just before `gwin->rotatecolours()` (`exult.cc:1517`, verified). That is one `stat` per root, which is cheap even on `/mnt/e` drvfs. Every pipeline tool that writes into a pack touches `.reload`.
* **Recorder (M2, Mesen HD Pack Builder analogue).** With `dev=record`, every key resolved on the paint path at S>1 is appended with a count and a first-seen location to `hires_seen.txt`. `u7hires todo` ranks missing keys by it. Terrain does not need the recorder, because usage comes from the reference tables.
* **ExultStudio.** `Exult_server::reload_shapes` (`server/server.cc:425-429`, verified) gets P8 and bumps `store.generation()`, so re-guarding happens automatically.

### 4.7 Sprite path (M2, outline)

The map's D8 is adopted: an identity stamp in `Vga_file::get_shape` with no I/O, payload resolution on the paint path, a byte-budget LRU, transposed reflections, strict groups and the CRC gate. Additions for the workflow:
* `--dump-art` extends to all eight `Vga_file`s, fonts and pointers, following gap_6 §7.1. PNG rows carry the canonical `oFFs` and `tRNS`=255 (map §4.8).
* `hires_rules` derives the groups (gap_5 §6.1). The validator and the loader share them, so "frames blocking group X" in the report is exactly what the engine will do.
* `hirestool pack` compiles a set into the sparse companion VGA, opened as a path (map H6). Loose PNGs stay valid in dev. The compiled form is required only where libpng is missing.
* The inspector and template export work on the object under the cursor (shape, frame, group state, CRC).
* UI (M3) uses the same layout: `gumps/`, `faces/`, `sprites/`, `paperdol/` and `fonts/<source>/` folders under the scale folder.

---

## 5. Configuration and CLI

```xml
<config><video><hires>
  <enabled>yes</enabled>
  <render_scale>art</render_scale>      <!-- art | auto | 1..6 | force:N ; force bypasses S_cap only -->
  <art_scale>6</art_scale>
  <max_world_mpx>10</max_world_mpx>
  <overrides>on</overrides>             <!-- on | off | identity | marker (synthesised in memory, tests) -->
  <sets></sets>                          <!-- e.g. "curated,xbrz,-nxbrz"; empty = manifest order -->
  <profile>strict</profile>             <!-- strict | lenient (families/groups, guard-less PNGs) -->
  <dev>no</dev>                          <!-- no | yes | record -->
  <cache_mb>256</cache_mb>               <!-- M2 LRU; terrain cache budget = max(working set, 64 MB) -->
  <downscale>auto</downscale>            <!-- auto | area | linear | chain -->
</hires></video>
<disk><game><blackgate><hires_path>/mnt/e/Dati/Ultima7_Upscale/packs/bg</hires_path></blackgate></game></disk></config>
```

CLI flags are declared next to `--buildmap` (`exult.cc:289-312`, verified) and dispatched next to it (`exult.cc:986-989`, verified):

| Flag | Purpose |
|---|---|
| `--hires-pack <dir>` (repeatable), `--hires-sets a,b`, `--hires off\|on\|identity\|marker`, `--render-scale N`, `--hires-dev` | Session overrides of the keys above |
| `--dump-art <dir> [--dump-what flats,terrain,shapes,gumps,…]` | Writes the reference set (§6.3). Modelled on `BuildGameMap` (`exult.cc:2857-2912`, verified); never enters the main loop |
| `--render-test "map=0,tx=…,ty=…,w=…,h=…,lift=16,scale=6,passes=flats\|terrain\|all,overrides=…,sets=…,ab=1,inspect=tx:ty,seed=1,out=<dir>"` | Map C4 / gap_7 §5, extended with `passes` (flats-only renders make marker predictions exact) and `inspect` (JSON `explain()` for scripted checks). Writes `hi_S.png` (indices), `ref_1x.png`, `diff.png` and `digest.json` |

A `tests/game/test.cfg` template sets `patch`, `mods`, `savegame_path` and `gamedat_path` to scratch dirs. Without that, `game.cc:521` creates `<game_path>/patch` inside the user's install (`build.md` §6.3). The current `run/exult.cfg` does not override `patch`.

---

## 6. File formats and directory layout

### 6.1 Pack layout

```
<root>/                                   e.g. /mnt/e/Dati/Ultima7_Upscale/packs/bg  (= E:\Dati\Ultima7_Upscale\packs\bg)
  x6/
    hires.txt                             manifest (optional for loose dev packs)
    .reload                               dev trigger (touched by tools)
    flats/SSSS_FF.png                     base set: per-tile flats (decimal, ≥4/≥2 digits; regex (\d+)_(\d+))
    terrain/<16 hex>.png                  base set: per-terrain overrides (768×768)
    sets/<name>/flats/…, sets/<name>/terrain/…, (M2) sets/<name>/shapes/…
    shapes/ gumps/ faces/ sprites/ paperdol/ fonts/<src>/   (M2/M3)
    _work/                                ignored by the engine (templates, todo, scratch)
    *.json                                ignored by the engine (pipeline provenance sidecars)
  x3/  (optional hand-reduced art)
```

### 6.2 Authoring PNG (normative; checked by the loader and by `hirestool`)

| ID | Rule |
|---|---|
| F1 | Colour type 3, bit depth 8. RGB/RGBA input is rejected with "quantize first (`u7hires quantize`)". |
| F2 | `PLTE`: every present entry equals the effective palette 0 converted as `v·255/63` (map §4.1), and the length covers the max index used. This catches editors that reorder or optimise the palette, the most common silent corruption. ipack's ×4 export colours are rejected explicitly with a hint (`build.md` §7.2). |
| F3 | Size: flats exactly 8·S×8·S, terrain exactly 128·S×128·S (G1). RLE frames follow canonical extents (M2, G2-G3). |
| F4 | `tRNS`: none for flats and terrain (G6). RLE: only index 255 transparent (map §4.8). No ipack/ES +1 rotation. |
| F5 | `tEXt`: `Exult-Hires`=`1`, `Exult-Key` (e.g. `flats/0019_03`, must match the path), `Exult-Scale`=`6`, `Exult-Src-CRC32` (frames; 8 hex), `Exult-Terrain-Key` (terrain; must match the filename). Optional `Exult-Origin` (route, short). Read with `png_read_info` before IDAT, so indexing a pack does not decode pixels. |
| F6 | A missing `Exult-Src-CRC32` is a warning in `lenient` and an error in `strict`. `hirestool stamp --ref` adds it from the reference set. |

Index-class rules (map §4.2, gap_5 §4) live in `hires_rules` and are applied at load:

| ID | Rule | Severity |
|---|---|---|
| P0 | Flats: 0xFF forbidden | error |
| P4 | A pixel in cycling range R (E0-E7, E8-EF, F0-F3, F4-F7, F8-FB, FC-FE) needs its 1x parent, or a 1-px neighbour, in R | error above 0.5 % of pixels, else warning |
| P4b | Every 1x pixel in R keeps at least one pixel in R in its S×S block | warning (dropped sparkle) |
| P2 | Ramp of the block equals the ramp of the 1x parent (`Palette::get_ramps`) | warning; `hirestool` only (needs ramps) |
| E1 | Edge contract as declared in `hires.txt` (`edge=none\|nn3\|smooth3`) | error when declared; `hirestool` only |

### 6.3 Reference set (`--dump-art` output, one per configuration)

```
art_ref/bg-fov/                          (e.g. /home/simonea/ultima7_exult/art_ref/bg-fov)
  ref.txt          %%section header: game, identity (FORGE), mod, si_present, fonts, engine git rev,
                   palette_crc, png_convention "raw-index,transp=255,no-rotation", terrain_key "T1", crc "C1"
  palette/pal00.act, pal00.gpl           768-byte 8-bit palette + GIMP/Aseprite palette (authoring)
  palette/ramps.txt, classes.txt         ramp id per index; class per index (static/cycle range/xform)
  flats/SSSS_FF.png                      1x raw-index + tEXt Exult-Src-CRC32
  flats.txt        key, crc32, map_uses, uses_cycling, used_on_map, provenance(source spec, from_patch)
  terrain/<key>.png                      1x 128² flat layer with the engine's fill (context for AI)
  terrain.txt      tnum, key, uses (all maps), own_cells, rle_cells, missing_cells, duplicate_of
  terrain_tiles.bin  per terrain: 256 × {own shape u16, frame u8, kind u8} + 256 × effective fill {u16,u8,u8}
  terrain_map.bin    per map: 192×192 u16 (terrain numbers, as Game_map reads them, gamemap.h:70)
  missing_cells.txt  cells zero-filled by P3 (mask for oracle O0)
  (M2) shapes/, gumps/, faces/, sprites/, paperdol/, fonts/<src>/ + manifest rows per gap_6 §7.1
```

The binaries are little-endian with a 16-byte header (`"U7HR"`, version, record size). The Python reader is `u7hires/ref.py`. This replaces `art_original/` as the normative input. `art_original/` stays valid research data, but its chunk renders have black holes under RLE cells (gap_1 §0.5). The fixed `u7art.py` (H16) is kept only as a parity cross-check.

### 6.4 Manifest `hires.txt` (Exult `%%section` format, parsed with `Text_msg_file_reader`, `files/msgfile.h:43-80`, verified)

```
%%section pack
:version/1
:game/BG
:scale/6
:palette_crc/3c9d7a10
:edge/nn3
:title/Black Gate 6x terrain (local build)
%%endsection
%%section sets
:curated
:xbrz
:-nxbrz
%%endsection
%%section families
:water/flats/0016:*;flats/0017:0-7
:grass/flats/0019:*;flats/0147:*
%%endsection
%%section atomic
:water
%%endsection
```

Mesen ideas adopted:
* `<ver>`, game binding and scale go in `pack`.
* First match wins, which here means set order.
* `<background>` becomes the per-terrain overrides.
* The HD Pack Builder becomes the template export plus the recorder.
* Palette-independent "default tile" behaviour comes free with indexed art.

Not adopted in v1:
* Per-pixel runtime conditions.
* `randomBackground`. The terrain cache is shared by all placements of a terrain, so variety can only be seeded per (terrain key, cell). That is a v2 `variants` section.

### 6.5 Provenance sidecar (pipeline only)

`flats/0019_03.json` records: `{key, src_crc, route, model, model_sha256, seed, params, instances, consensus_disagreement, qa:{B1,B3,B4,C2,…}, decision, reviewer}`. The engine ignores it. `art_work/raw/` keeps the unquantised outputs as the reproducibility anchor, because GPU routes are not bit-exact across runs.

---

## 7. Validation and QA tooling

### 7.1 `hirestool` (C++, SDL-free; built like ipack in `Makefile.common` and `tools/Makefile.am`)

| Command | Function |
|---|---|
| `hirestool check --ref art_ref/bg-fov --pack <root> [--set s] [--profile strict] [--report r.json]` | Every F, P, E and G rule. Coverage (missing keys, ranked by `map_uses`). Atomic families. Duplicates across sets. A guard mismatch is reported as an error ("pack built against other data"). Exit status ≠ 0 on errors. The JSON has one record per key: `{status, rules[], metrics}`. |
| `hirestool stamp --ref … <png…>` | Writes the `tEXt` guard and key chunks; normalises `PLTE` to palette 0 when the indices are already raw. Refuses if the size or class rules fail. |
| `hirestool synth identity\|marker --ref … --out <root>/x6/sets/<name>` | NN-6x identity pack (all flats and all used terrains) or marker pack (index `0x01` at sub-pixel (0,0) of every 6×6 block of each flat). These are the fixtures for O4a/O4b. |
| `hirestool downscale --to 3 <png>` | Previews the engine's mode-filter reduction. |
| `hirestool index <root>` | Optional `index.txt` (key, path, size, mtime, crc) for slow filesystems. |
| `hirestool pack` / `unpack` (M2) | Companion VGA. |

The validator uses the reference set instead of game data. CI can therefore run it on synthetic reference sets, and the rules library is the same object code the loader uses (map H11 intent, different packaging).

### 7.2 Edge-contract checker

* `nn3`: the outer 3 px of every flat, and the outer 3 px of the 768² frame of every terrain override, equal the NN replication of that override's own 1x border pixels. Corners take the corner pixel.
* `smooth3`: the band equals `D_smooth`, specified in `hires_rules.cc` and ported to Python with test vectors. D_smooth is 1-D linear interpolation in sRGB along the edge over the 1x border row, NN across, then quantised with the local-snap set of the 1x border pixel and a tile-aligned Bayer-8 threshold.
* `none`: no check; seam metrics only.

### 7.3 Python QA (`u7hires qa`)

The research gates are implemented exactly (research §2.8):
* format A1-A3, consistency B1-B4, seams C1-C2, instance disagreement D1, registration E1;
* contact sheets per family, showing 1x NN, the baseline and the candidate;
* in-engine previews: `--render-test passes=all` on 12 golden regions at 6x, plus the normative area downscale to 1280×800 and 640×400.

C2 runs over every touching pair that occurs on the map: 64,228 distinct BG pairs of different flats (gap_1 §2.1), mined from `terrain_tiles.bin` and `terrain_map.bin`. These gates are quality gates, not engine rules. A failure blocks promotion, not loading.

---

## 8. Test strategy

**Frameworks.**
* C++: **doctest**, vendored single header (MIT, C++11; works with g++ 9.4, mingw and MSVC; `build.md` §6.4), in `tests/unit` and `tests/integration`.
* Python: **pytest** in `tools-venv`.
* Game-data tests: shell drivers under automake `TESTS`, returning exit code 77 (skip) when `U7_BG_STATIC` is unset.

| Layer | Test | Oracle | Data |
|---|---|---|---|
| Unit (SDL-free) | `test_hires_keys`: naming parse/format, packing, frame&31 | exact | synthetic |
| | `test_hashes`: FNV-1a-64, T1 terrain key, C1 frame CRC against `tests/data/hash_vectors.txt` (the same file is used by pytest) | exact, cross-language | synthetic |
| | `test_png_io`: F1-F6 accept/reject, tEXt round trip, PLTE-altered file rejected, ipack-rotated file rejected | exact | generated in test |
| | `test_rules`: P0, P4, P4b, E1 (nn3, smooth3) on crafted tiles | exact | synthetic |
| | `test_mode_reduce`: class preservation; `reduce(NN6(x)) == NN3(x)` | exact | random tiles |
| | `test_store`: precedence (terrain > tile > NN), guard mismatch → NN, atomic family, set order, rescan diff (add/modify/delete/rename in a temp dir), generation bump | exact | temp dirs |
| | `test_compose`: `render_flats` composition on a fake terrain via a `Chunk_terrain` test seam (shapes injected); S ∈ {1,2,3,6} | `== NN` with no overrides | synthetic |
| | `test_render_scale`: `compute_render_scale` table from gap_2 §2-§6 and the `art` policy rows | exact | table |
| | Primitive fuzz O1 (map) | `phys == NN(ref)` | random ops |
| Integration (SDL dummy) | `Image_window8` at S, present path, `screen_to_game` invariance (W12), downscale O8 (±1 against the NumPy reference) | tolerance | synthetic |
| Game (BG data, local and nightly) | **O0**: fork `--buildmap 2` at S=1 equals the upstream-master binary (`build-upstream/`), masking `missing_cells.txt` | byte-exact | BG |
| | **O2**: `--render-test ab=1` on 12 regions at S ∈ {2,3,6}, overrides off | `render_S == NN(render_1)` | BG |
| | **O4a**: synth identity pack (flats + terrain), loaded from files | identical to O2 output | BG |
| | **O4b**: synth marker pack, `passes=flats` | diff equals the predicted marker set | BG |
| | O6 repaint idempotence, O7 S-invariant scalars (map) | exact | BG |
| | Dump determinism: `--dump-art` twice gives identical trees; dump CRCs equal `u7art.py` (fixed) for 3,885 flats | exact | BG |
| | `inspect=` JSON for known tiles (terrain 0 grass cell, an RLE fill cell) | golden JSON | BG |
| | Perf smoke at -O2: world paint ≤ 1.6 ms and present ≤ 2 ms at 320×200 S=6 (map M1); `render_flats` with per-tile art ≤ 1 ms; terrain decode ≤ 2 ms | thresholds | BG |
| Pipeline (pytest) | quantizer never emits ≥ 0xE0 outside the mask; consensus deterministic; two CPU-route runs → identical pack hash; `hirestool check` passes on every route-3 output; edge band functions match the C++ vectors | exact | synthetic + BG (skip if absent) |
| Validator | fixture packs, one violation each → expected rule IDs in the JSON report (golden) | golden | synthetic |

**Goldens.** Synthetic goldens are committed as small indexed PNGs. For real BG data, only **hash lists** are committed (`tests/game/golden_bg.txt`: SHA-256 of the index buffers per region, S and pack), never pixels: EA art stays out of the repo. Goldens are regenerated only through an explicit `make regen-goldens`, and the commit message must name the cause.

**Headless run (here).**

```bash
source /home/simonea/ultima7_exult/deps/env.sh
cd /home/simonea/ultima7_exult/build-linux-o2 && make -j16 && make check          # unit + integration
U7_BG_STATIC=/mnt/e/Games/RolePlayingGames/ultima7/static make check-game          # O0, O2, O4, O6, O7
env -u DISPLAY -u WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
  ./exult -c ../exult-hires/tests/game/test.cfg --bg --render-test "tx=1024,ty=2048,w=320,h=200,scale=6,ab=1,out=/home/simonea/ultima7_exult/tmp/rt"
/home/simonea/ultima7_exult/tools-venv/bin/python -m pytest ../exult-hires/tools/hires/tests
```

---

## 9. Build and CI

* **Linux, here.**
  * Keep the configured autotools tree, but add a second out-of-tree build, `build-linux-o2`, configured with `--with-optimization=normal --with-debug=symbols` (P9). The existing `build-linux` is -O0.
  * Register `shapes/hires/*` and `hires_dev.cc` in `shapes/Makefile.am` and the top-level `Makefile.am`, and add a `tests/` subdir with `check_PROGRAMS` and `AM_TESTS_ENVIRONMENT = SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy` (`build.md` §6.4).
  * Add `shapes/pngio.o` and the hires objects to `Makefile.common` `SHAPES_OBJS`, and add a `hirestool` rule beside ipack (Makefile.common:659-676).
  * An ASan variant (`build-linux-asan`) runs the game tests nightly; it catches risk 1 and uninitialised caches.
* **Local CI script.** `tools/hires/ci.sh` builds `-O2`, then runs `make check`, `make check-game` (if data is present), pytest and `hirestool check` on the current curated pack, and writes a one-page summary. If a GitHub fork is created later, a workflow adds `make check` with SDL 3.2.14, which also proves the ARGB baseline compiles without 3.4 (map C7).
* **Windows.** Follows `build.md` §5 W1:
  1. MSYS2 UCRT64 extracted to `E:\Dati\Ultima7_Upscale\msys64`, driven from WSL via `bash.exe -lc`.
  2. A working copy at `E:\Dati\Ultima7_Upscale\src\exult-hires`, pulled from the WSL repo.
  3. `make -f Makefile.mingw Exult.exe hirestool.exe tests.exe`, installed into `E:\Dati\Ultima7_Upscale\ExultHires`, never over 1.12.1.
  4. Run portable (`-p`) with `hires_path=E:\Dati\Ultima7_Upscale\packs\bg`.
  5. The same pack folder is visible from WSL as `/mnt/e/...`, so WSL tools and the Windows engine share one pack, and hot reload works across the boundary through `.reload`.
  6. Windows-specific checks: D3D11/D3D12 present path, ARGB vs INDEX8, LINEAR and PIXELART downscale (map risk 12), and keyboard bindings.
  7. MSVC and iOS project files get the new sources in WP0.1 so they do not drift (map C6). They are not built locally.

---

## 10. Art production plan: 6x BG terrain flats

**Where things run**
* WSL CPU: Python 3.11 venv; xBRZ 1.9 and MMPX built as in `tmp/algo/`. `tools/hires/third_party/build_xbrz.sh` fetches and SHA-checks the sources and is never vendored (GPLv3, offline only).
* WSL CUDA: torch cu130 on the RTX 5070 Ti, already working (research §3.2).
* Windows: ComfyUI portable cu130 (research §3.3).
* Storage:
  * reference set and work data on ext4: `~/ultima7_exult/art_ref`, `~/ultima7_exult/art_work/{ctx,raw,qa}`;
  * packs on E: so Windows can see them.
* Licences: NXbrz is CC-BY-NC-SA, xBRZ is GPLv3 (offline only), SDXL is OpenRAIL++. Packs are for local use only.

**Pipeline stages** (research §2, bound to the engine contract)
1. **Reference.** Run `--dump-art` and use only its outputs.
2. **Context.**
   * Real-map windows: each used terrain's 1x flat layer with the engine fill, plus a 16-32 px apron from its most frequent neighbours. The neighbours come from `terrain_map.bin` across all maps.
   * Per-pixel instance maps of `(shape, frame, world pos)`.
   * Macro sheets for the 77 8×4 torus shapes.
   * Self-wrap only for frames never used on the map with no torus.
3. **Upscale routes into sets.**
   * Route 3 (`xbrz`): xBRZ 6x on the uniquified palette.
   * Route 2 (`nxbrz`): 4x-NXbrz → Lanczos 1.5x.
   * Route 1 (`sdxl`): xBRZ base → 12x → SDXL + xinsir Tile → box 2:1.
4. **Cut and consensus** per `(shape, frame)`: mode for index routes, OKLab medoid for RGB routes.
5. **Colour lock and quantize.** Back-projection for RGB routes. The cycling mask is upscaled label-safe first (xBRZ+snap on indices, keeping E0-E7/FE). Static pixels use local snap over the 3×3 parent neighbourhood, ramp-constrained, within 0x01-0xDF. Dither is ordered and tile-aligned, or none.
6. **Edge band** per the pack's declared contract (§7.2), applied last.
7. **Write** PNG + tEXt guard + JSON sidecar via `u7hires pack`. Touch `.reload`.
8. **Gate** with `hirestool check --profile strict`, then QA (§7.3), then review.

**Production phases**

| Phase | Output | Runs on | Effort | Exit gate |
|---|---|---|---|---|
| A0 Fixtures | `sets/identity`, `sets/marker` (via `hirestool synth`) | WSL | hours | O4a/O4b green |
| A1 Baseline | `sets/xbrz-{none,nn3,smooth3}`: all 3,885 flats | WSL CPU (minutes per run) | 3 d | strict check 0 errors; A1-A3 pass; B1 ≥ 97 %/tile; B3 p99 ≤ 0.10 |
| A2 Edge decision | decision record: which contract (or none) becomes the pack's `edge` | engine A/B on 12 regions at 6x and downscaled + C1/C2 | 1 d | C1 ≤ 1.2 × the region-level baseline; reviewer sign-off |
| A3 SR route | `sets/nxbrz` | WSL CUDA (minutes) | 2 d | same gates; per-family comparison with A1 |
| A4 Per-terrain | `terrain/` for the top-200 terrains (80.8 % of BG map chunks; ≈ 27-30 MiB) plus transition-rich ones (coasts, roads, towns), generated from context windows and cropped to 768² | WSL CPU/CUDA | 3 d | chunk-border C2 not worse than the per-tile composition; O4a still green for the rest |
| A5 Diffusion | `sets/sdxl-<family>` for water/shore, grass, dirt, floors, roads; first GPU session measures VRAM and s/canvas (research §3.3) | Windows ComfyUI | 5-10 d (curation-bound) | E1 registration ≤ 0.25 source px; D1 outliers reviewed; human acceptance per family |
| A6 Freeze | `curated` set: per-family winners, promoted with sidecars; `bg-x6 v1` = all 3,885 flats (coverage 100 %) + top-200 terrains | WSL | 2 d | strict check clean; golden hash list for the 12 regions recorded; previews at 1920×1200, 1280×800, 3440×1440 ACF approved |

Families come from ramp classes plus clustering of the adjacency graph (gap_5 T3), then curated by hand (`tools/hires/data/bg_families.txt`). Text-like tiles and the water and shoreline families are budgeted for manual work first (research §2.8, prior art).

---

## 11. Milestones and work packages

M1 = terrain end to end (engine + tooling + pipeline baseline). M2 = sprites. M3 = UI. Estimates are engineer-days for one developer with AI assistance. WPs in the map's engine core (W*, B*) are listed so that the plan is complete; they follow the map.

| ID | Name | Depends | Scope | Acceptance / tests | Days |
|---|---|---|---|---|---|
| WP0.1 | Build & test scaffolding | – | `build-linux-o2`; doctest; `tests/` + `make check`; pytest; `ci.sh`; register sources in all four build descriptions | `make check` and pytest green; -O2 binary | 2 |
| WP0.2 | Safety/determinism commits + upstream-fixes branch | 0.1 | P3, P4, P6, P7, P8 in the fork; P1, P2, P5 on `upstream-fixes` with tests | O0 harness passes (with mask); fill-selection unit tests pin current behaviour | 2 |
| WP0.3 | Upstream oracle O0 | 0.1 | `build-upstream/` + compare script | byte-identical 144 superchunks | 0.5 |
| WP1.1 | Hires core lib | 0.1 | keys, FNV, T1 key, C1 CRC, PNG read/write with tEXt, F/P rules, mode reduction | unit tests, cross-language vectors | 4 |
| WP1.2 | `--dump-art` (flats + terrain + tables) | 1.1, 0.2 | reference set §6.3 | deterministic twice; CRC parity with fixed `u7art.py`; Python T1 keys equal C++ | 3 |
| WP1.3 | `u7art.py` fixes + `u7hires.ref` | 1.2 | H16 fixes, readers | parity tests | 1.5 |
| WP1.4 | `hirestool` | 1.1 | check / stamp / synth / index / downscale | fixture packs → expected rule IDs; synth identity validates clean | 3 |
| WP2.1 | Image_buffer8 scale + NN primitives | 0.1 | map B1-B9 | O1 fuzz at S ∈ {2,3,6} | 6 |
| WP2.2 | Present path + `art` policy + downscalers | 2.1 | map W1-W16, §4.5 | render-scale table tests; O8; W12 invariance; windowed S=6 1:1 | 6 |
| WP2.3 | Terrain path + store M1 | 1.1, 2.1 | §4.3-§4.4, G11-G16 | `test_store`, `test_compose`; O2, O4a/O4b on BG | 4 |
| WP2.4 | Config/CLI + `--render-test` | 2.2, 2.3 | §5 | `inspect=` golden JSON; A/B digest | 3 |
| WP2.5 | Game oracle suite | 2.4, 1.4 | O2, O4a, O4b, O6, O7, perf smoke, hash goldens | all green at -O2; ASan clean | 3 |
| WP3.1 | Hot reload | 2.3 | trigger poll, key, rescan diff, targeted flush, diagnostics | `test_store` rescan cases; manual: edit → visible < 2 s on `/mnt/e` | 2 |
| WP3.2 | Inspector, template export, overlay, A/B | 3.1, 2.4 | §4.6 actions + default keys | `inspect` golden; templates pass `hirestool check` once painted identically | 2.5 |
| WP3.3 | Modder guide | 3.2 | `docs/hires_modding.md` (palette setup in Aseprite and GIMP, naming, rules, live loop) | a dry run by following the guide only | 1.5 |
| WP4.1 | Context assembly | 1.3 | windows + apron, instance maps, macro sheets | pytest on synthetic maps; windows reproduce the engine's 1x terrain renders exactly | 2 |
| WP4.2 | Route 3 + quantizer + consensus + edge variants | 4.1, 1.4 | phases A0-A1 | strict check 0 errors; gates A1-B4 | 3 |
| WP4.3 | QA harness + review | 4.2, 2.4 | §7.3 metrics, sheets, render-test previews, decisions file | metrics reproduce the research baselines on its windows A/B | 3 |
| WP4.4 | Edge decision (A2) | 4.3, 3.1 | in-engine A/B + metrics | decision record committed | 1 |
| WP4.5 | Route 2 (A3) | 4.3 | WSL CUDA | gates | 2 |
| WP4.6 | Per-terrain top-200 (A4) | 4.4 | terrain overrides | chunk-border C2; O4a for the rest | 3 |
| WP4.7 | Windows build + ComfyUI first session | 2.2 | MSYS2 build, install, D3D checks, ComfyUI + 5-shape session | Exult.exe runs the pack at S=6; VRAM and time measured | 3 |
| WP4.8 | Route 1 + curation + freeze (A5-A6) | 4.5, 4.6, 4.7 | `bg-x6 v1` | §10 A6 gate | 7-12 |
| M2 | Sprites | M1 | map D8 + §4.7: identity stamp, LRU, groups, dump of all files, companion VGA, recorder | map M2 exit criteria + validator group reports | ~20 |
| M3 | UI | M2 | map D9 | map M3 exit criteria | ~15 |

**M1 total ≈ 75-80 days**: about 45 engine, 15 tooling and 20 art, with the art partly in parallel from WP1.3 on. The critical path is 0.1 → 1.1 → 2.1 → 2.3 → 2.4 → 2.5. The art track starts at WP1.2 and does not wait for the S>1 renderer: it can preview with `--render-test` as soon as WP2.3 lands.

---

## 12. Risks

| # | Risk | Sev. | Mitigation |
|---|---|---|---|
| 1 | Editors silently change palette or indices (palette optimisation, RGB export, ipack rotation) | High | F1/F2 hard reject with the cause named; `.gpl`/`.act` palette files in the reference set; guide; `stamp` normalises only when indices are provably raw |
| 2 | AI art breaks index semantics (cycling, ramps) | High | Label-safe cycling path, local-snap quantizer, P4/P2 rules in the loader and validator |
| 3 | Seams: per-tile grid or chunk-border grid at 768 px | High | Context + consensus; edge contract chosen by measured A/B (A2); per-terrain art for top terrains; C2 over all 64k pairs |
| 4 | A fill-independent key misses a fill-dependent case (the art relies on engine fill pixels) | Low | Per-terrain art is complete by definition (F3); context renders still use the engine fill; the inspector shows `compose_src` |
| 5 | Logical/physical unit bugs in the core (map risk 1) | Critical | Map W8/B4 first; ASan nightly; O1, O6 |
| 6 | Hot reload races (editor saving mid-write, rename-on-save, drvfs mtime granularity) | Med | Re-validate on the next trigger; reject truncated PNGs (libpng error → "Rejected: read error", retried on the next reload); the trigger is the single sync point |
| 7 | GPU routes are not reproducible; model drift; NC licences | Med | Raw outputs archived with model SHA-256 and seed; packs local only; route 3 as a deterministic fallback for every frame |
| 8 | Curation effort exceeds the estimate (diffusion) | Med | Baseline pack complete before A5; A5 per family, so it can stop at any family |
| 9 | `art` policy costs performance at large views (9.3 Mpx at ACF 860×300) | Med | `max_world_mpx` cap; `auto` policy; INDEX8 upload on SDL ≥ 3.4; dirty-rect uploads |
| 10 | Upstream never accepts P1/P2 | Low | The fork keeps the upstream 1x pixels; the art is unaffected (fill-independent key) |
| 11 | Strict guards hide art under mods (CRC mismatch) and confuse modders | Med | The inspector and `hires.log` give the exact reason; `lenient` profile; per-mod packs under `<PATCH>/hires` |
| 12 | Four build descriptions drift; no libpng on Android/iOS | Low | WP0.1 registers everywhere; NN-only fallback; companion VGA in M2 |
| 13 | Windows present path differences (D3D LINEAR, INDEX8) | Med | ARGB baseline; WP4.7 explicit checks |

---

## 13. Deviations from the map (D1-D10 and §3)

1. **D5, default policy.** `render_scale=art` (S = S_art whenever the caps allow) is the default, per the user's explicit preference. D5's `auto` (minimal S snapped up) is kept as an option. As a consequence, the r < 0.5 downscaler (area or halving chain) is required in M1, not optional.
2. **D7 and §4.7, terrain key.** It is a **fill-independent T1 hash** (own-flat pixels + an own-cell bitmap), not FNV of the 1x render "after P1-P4". This decouples art identity from the fill heuristic.
3. **§3.1 P1/P2 (and P5) "land first".** They live on an `upstream-fixes` branch and are not merged into the fork's default until upstream accepts them. The fork's 1x output stays byte-identical to upstream (new oracle O0). P3 and P4 land as the map says.
4. **H11/H15, validator shape.** The validator consumes the `--dump-art` reference set (`hirestool check --ref`) instead of parsing game data itself. It shares the rules library with the loader, as the map intends.
5. **D8, shipping format for M1.** For terrain, loose raw-index PNGs are an accepted shipping format on desktop. The companion VGA becomes mandatory only in M2 and for platforms without libpng.
6. **H12, PNG loader.** It adds `tEXt` guard and key chunks and a full PLTE-equality rule (F2), so each override is self-describing without a JSON sidecar. Sidecars stay pipeline-only.
7. **H14, search chain.** Added: `--hires-pack` roots, named sets with priority and disable, atomic families, and per-scale folders with a normative class-preserving mode-filter reduction for S_eff < S_art. The map snaps S to divisors of 6 but does not define the art reduction.
8. **D10, tests.** Adds O0 (upstream binary), `--render-test` parameters `passes=` and `inspect=`, shared cross-language hash vectors, validator golden reports, and a pytest suite for the pipeline.
9. **Q5, edge contract.** It is not fixed to NN for v1. Three variants are produced and the contract is chosen by in-engine A/B plus metrics (A2). The pack declares its contract and the validator enforces it.
10. **New engine scope not in the map:** dev mode (hot reload, inspector, template export, overlay, A/B toggle, recorder). It is about 4.5 days and is what makes the authoring loop usable.

---

## 14. Open questions

* Should `max_world_mpx` default lower (for example 6 Mpx) on laptops, with `auto` suggested in the toast when a frame exceeds budget?
* Reference configuration for the base pack: BG+FoV without SI for M1 (map Q10). Should the dump and pack include the SI-enabled BG set before M2?
* `smooth3` exact definition: is a tile-aligned Bayer-8 dither in the band acceptable on water, or should cycling bands always be NN?
* v2 variants (per terrain key and cell): worth it for grass repetition, or is per-terrain art enough?
* Should `--dump-art` also export all 13 palettes and the xform tables for future RGB-route previews of night and fog grading?
