# Gap 1: Terrain override granularity and tile-edge seams

Scope: how the override unit for flat terrain should be chosen. The two candidates are a per-(shapes.vga, shape, frame) 8x8 tile override and a per-`Chunk_terrain` 128x128 override (768x768 at S=6). This report also covers how seams at tile and chunk edges come about, and what the engine and the art pipeline must guarantee so they do not show.

Code base: `/home/simonea/ultima7_exult/exult-hires` at master `8b6ab6b43`, read-only. Game data: BG `/mnt/e/Games/RolePlayingGames/ultima7/static`, SI `/mnt/e/Games/RolePlayingGames/Serpent/static`. All numbers in section 2 come from small Python scripts. They reuse the readers in `tools/hires/u7art.py` and the session scratchpad `gap1_stats*.py`.

---

## 0. TL;DR

1. **Neither granularity solves seams on its own.**
   * Per-tile overrides that are AI-upscaled one at a time seam at almost every tile edge. Only 20.1% of BG flat-to-flat edges join a tile to an identical copy of itself.
   * Per-terrain overrides remove about 94% of the seam-prone edges, because those edges are inside a chunk. The other ~6% sit on chunk borders, and there the neighbour is a *different* terrain 93% of the time. In uniform grass fields that would show as a regular **768-px grid**.
   * Both need the same **edge contract**: the outer band of every override is derived deterministically from the override's own 1x border pixels.
2. **Recommended: a hybrid with one engine hook.** `Chunk_terrain::render_flats` always builds the exact 1x render first, then:
   1. If a **per-terrain override** exists for the *content hash of that 1x render*, use it.
   2. Otherwise compose the cache tile by tile from **per-tile overrides**.
   3. Otherwise **nearest-neighbour expand** the tile.

   Per-tile art is the v1 bulk deliverable, about 8.5 MiB for all of BG. Per-terrain art is an optional, selective upgrade: the top 200 BG terrains cover 81% of map chunks and take roughly 30 MiB.
3. **Do not key per-terrain overrides by terrain number, and do not gate them on `modified`.**
   * Terrain numbers are renumbered by map-editor insert/delete/swap (`gamemap.cc:1271-1468`).
   * `modified` gives false positives: `swap_terrains` sets it on unchanged content (`gamemap.cc:1278-1280`).
   * `modified` also gives false negatives: it is cleared after saving (`gamemap.cc:475`) and is false for any terrain loaded from a mod's `patch/u7chunks` (`gamemap.cc:152-153`, `chunkter.cc:143`).
   * A **hash of the 1x rendered flats (16,384 B)** catches all of these, and also catches changed flat pixels from a patched `shapes.vga`.
4. Fix three engine quirks **before** any art is authored, because each one changes the 1x render and so the hash:
   * The neighbour bound `tiley + y > 0` (`chunkter.cc:104`). Fixing it changes the fill under 2,367 RLE positions in 779 of 2,105 used BG terrains (SI: 3,604 positions in 1,085 terrains).
   * The full-chunk fallback that may pick the void tile 12/0 (`chunkter.cc:119-126`): 112 positions in BG, 194 in SI.
   * The uninitialised cache buffer (`chunkter.cc:259`, `ibuf8.h:37-39`), which leaks garbage where a tile has no shape (BG: flat 48/8 does not exist and is used in 2 terrains).
5. The current `art_original/chunks` and `art_original/superchunks` renders are **not** what the engine shows. They have **black 8x8 holes (index 0) at every RLE terrain position**, where the engine paints a neighbouring flat. Verified on 2,005 of 2,005 sampled positions. 53% of BG map chunks contain such positions, so these images are unsafe as AI context or as a hash source until `u7art.render_chunk` emulates `paint_tile`. The same tool also has a wrong v2 header constant (`tools/hires/u7art.py:33`).

---

## 1. How terrain is stored, shared and cached today

### 1.1 Data model

* **`u7chunks`** is a flat array of terrains. v1 uses 256 tiles x 2 bytes = 512 B per terrain: shape is 10 bits and frame is 5 bits (`chunkter.cc:153-156`). v2 adds a 10-byte header `FF FF FF FF 'e' 'x' 'l' 't' 00 00` and uses 3 bytes per tile: 16-bit shape, 8-bit frame (`gamemap.cc:84-95`, `chunkter.cc:149-152`). BG and SI are both v1: 1,572,864 B / 512 = **3,072 terrains**.
  * The file is opened from `<PATCH>/u7chunks` if it exists, otherwise from static (`gamemap.cc:149-173`).
  * The number of terrains is derived from the file length (`gamemap.cc:184-191`).
* **`u7map`** holds, per map, a `terrain_map[192][192]` of terrain numbers (`gamemap.h:70`). It is read per superchunk as 16x16 little-endian shorts (`gamemap.cc:223-240`). Multi-map games each have their own `terrain_map`, but **`chunk_terrains` is static and shared by all maps** (`gamemap.h:61-65`, `gamemap.cc:79-83`).
* **`Chunk_terrain`** stores `ShapeID shapes[256]` (`chunkter.h:36`), `num_clients` (`:41`), `modified` (`:42`), `rendered_flats` (`:43`) and MRU links (`:46-48`). Terrains are read lazily by `Game_map::get_terrain(tnum)` (`gamemap.h:181-184`) and `read_terrain` (`gamemap.cc:110-128`).
* **Instancing.** `Game_map::get_chunk_objects(cx, cy)` points each `Map_chunk` at `get_terrain(terrain_map[cx][cy])` (`gamemap.cc:361-369`). `Map_chunk::set_terrain` registers itself as a client (`chunks.cc:702-724`). One `Chunk_terrain` therefore serves every map chunk that uses its number. In BG, terrain 0 serves 4,001 map chunks.
* **RLE tiles inside a terrain** (in BG all are shapes 150 and up: 331, 314, 306, 393, ...) are *not* drawn by the terrain. `set_terrain` turns each one into a `Terrain_game_object` or `Animated_object` inside the map chunk (`chunks.cc:726-740`). These objects are painted in render pass 2, `paint_chunk_flat_rles` (`gamerend.cc:234-241`, `:537-549`), or with the non-flat objects.

### 1.2 The flats cache

* `get_rendered_flats()` moves the terrain to the front of a global circular MRU list. It returns the cached `Image_buffer8` or builds one (`chunkter.h:95-101`).
* `render_flats()` allocates a `c_chunksize x c_chunksize` (128x128) buffer (`chunkter.cc:259`). When `queue_size > Figure_queue_size()`, which is hard-coded to **100** (`chunkter.cc:234-242`), it first evicts the LRU tail. It then calls `paint_tile` for all 256 tiles (`chunkter.cc:248-268`).
* **Consumers:**
  * `Game_render::paint_chunk_flats` does `gwin->win->copy8(cflats->get_bits(), c_chunksize, c_chunksize, xoff, yoff)` (`gamerend.cc:520-531`). This is pass 1 of `paint_map` (`gamerend.cc:226-233`).
  * `Game_map::write_minimap` averages the cache's RGB per terrain, over *all* 3,072 terrains (`gamemap.cc:1690-1722`).
* **Invalidation** happens only on `commit_edits()`, which re-renders in place (`chunkter.cc:208-216`), on eviction (`:250-258`, `:274-277`), and on destruction (`:182-186`).
  * There is no "flush all caches" entry point today. One will be needed when S or the override pack changes.
  * Quirk: `commit_edits()` can allocate a buffer without inserting the terrain into the MRU list, because insertion only happens in `get_rendered_flats`. Harmless today; at S=6 such a buffer is 576 KiB that the queue does not count.
* The cache holds **palette indices**, so day/night, lightning and colour cycling never invalidate it (see palette.md section 4). Every override path must therefore produce indexed pixels.
* **Terrain-editor mode** (`skip_lift == 0`) bypasses the cache entirely. `paint_terrain_only` calls `Chunk_terrain::render_all`, which `copy8`s every flat straight into the window (pass 1) and `paint_shape`s RLE tiles (pass 2) (`gamerend.cc:157-183`, `chunkter.cc:284-312`). The per-tile hook must also be applied there. Per-terrain overrides would simply not be shown while editing, which is acceptable.

### 1.3 `paint_tile` and the fill under RLE tiles (`chunkter.cc:86-133`)

* **Flat tile:** `copy8(shape->get_data(), 8, 8, tilex*8, tiley*8)` (`:89-91`).
* **RLE tile:** the code looks for a substitute flat so that no black gap shows. The comment mentions the ice caves.
  1. It scans the 3x3 neighbourhood row-major, from (-1,-1) to (+1,+1), and takes the first non-RLE shape. It skips the "palette cycling void tile" 12/0 (`:102-116`).
  2. If nothing is found, it scans the whole chunk row-major for the first non-RLE shape (`:119-126`). This second scan does **not** skip 12/0.
  3. Whatever it found is copied with `copy8` (`:129-131`).
* **Bug at `:104`:** `tiley + y > 0` should be `>= 0`. Row 0 can never be used as a neighbour source. An RLE tile in row 0 or row 1 can therefore not take its fill from row 0, and the first-hit order changes.
* **Null shapes:** if a tile's `ShapeID` resolves to `nullptr`, `paint_tile` writes nothing. Examples are a frame beyond the shape's count (`Shape::store_frame` returns null, `vgafile.cc:973-983`) or a missing shape. The cache buffer comes from `new unsigned char[w*h]` with no initialisation (`ibuf8.h:37-39`), so those 8x8 cells contain **heap garbage**, or stale pixels after a `commit_edits()` re-render.
* **Frame normalisation:** for flats the engine uses `framenum &= 31` (`vgafile.cc:912-914`). A v2 terrain entry with frame 37 of a flat shape therefore draws frame 5. Per-tile override lookups must use the same normalised frame.
* **Consequence for overrides:** the substitute flat is painted at a position where the map data says "RLE object". Its neighbours there, often a copy of itself to the left or above, are a context that **does not exist in the map data**. It is visible wherever the RLE sprite does not fully cover the 8x8 cell, which is common around bushes and rocks, not only in ice caves. A hi-res macro-texture tile placed next to a copy of itself will seam there (section 3).

### 1.4 Who mutates terrain contents, and the `modified` flag lifecycle

| Mutation | Code | `modified` | Cache refreshed? |
|---|---|---|---|
| Map-editor paint/drop of a flat (terrain mode) | `exult.cc:3452-3466`, paste `exult.cc:3568-3576` → `set_flat` | set (`chunkter.cc:193-200`) | Only on `commit_terrain_edits` → `commit_edits` → `render_flats` (`gamemap.cc:1474-1501`) |
| Moving or deleting an RLE terrain object | `Terrain_game_object::move` / `remove_this` → `set_flat(prev_flat or 12/0)` (`objs.cc:1640-1683`) | set | **No.** `set_flat` does not re-render. The cache stays stale until it is evicted. Terrain objects are not draggable in play (`objs.cc:1092-1094`), but usecode `remove_item` or the map editor can do this. |
| Undo | `abort_edits` (`chunkter.cc:223-229`) | stays set | n/a, because the editor bypasses the cache |
| Swap adjacent terrain numbers | `swap_terrains` (`gamemap.cc:1271-1303`) | set on both, **content unchanged** | no |
| Insert/delete terrain | `insert_terrain` / `delete_terrain` (`gamemap.cc:1313-1468`) | set on all renumbered ones, **content unchanged**; terrain maps of all maps shifted | no |
| Copy-construct | `chunkter.cc:168-176` (no callers found) | set | n/a |
| Save | `write_chunk_terrains` (`gamemap.cc:435-486`) writes `<PATCH>/u7chunks`, then calls `set_modified(false)` (`:475`) | **cleared**, content still differs from static | no |
| Load a mod or patch `u7chunks` | `init_chunks` (`gamemap.cc:152-153`) + ctor (`chunkter.cc:143`) | false | n/a |
| Move chunk contents (cheat) | `cheat.cc:536,554`: `Map_chunk::set_terrain` with another existing terrain, or terrain 0 | untouched | n/a (pointer change only) |

Conclusion: `modified` is neither necessary nor sufficient as an "override still valid" test, and terrain numbers are not stable identities. The only robust identity is **the content**.

---

## 2. Measurements on the real data

### 2.1 Terrain usage

| | BG | SI |
|---|---|---|
| Terrains in `u7chunks` | 3,072 | 3,072 |
| Terrains used on map 0 | **2,105** | **2,791** |
| Distinct contents among used terrains | 2,056 (49 exact duplicates) | 2,775 |
| Top 1 / 10 / 50 / 100 / 200 / 500 / 1000 cover (% of 36,864 map chunks) | 10.9 / 30.3 / 55.8 / 68.4 / **80.8** / 90.7 / 96.4 | – / – / 55.7 / 62.9 / **72.4** / 86.3 / 93.9 |
| Terrains used exactly once | 876 | 1,388 |
| Used terrains that contain RLE tiles | 1,824 (in 19,701 map chunks = 53%) | 2,445 (17,318 map chunks) |
| RLE tile positions in world | 166,573 (1.8% of tiles) | 187,762 |
| Fill changes if `:104` is fixed | 2,367 positions, 779 terrains | 3,604 positions, 1,085 terrains |
| Fill that falls back to the void tile 12/0 | 112 | 194 |
| Tiles with an unresolvable shape | 149 (flat 48/8, 2 terrains) | 0 |
| Distinct flats used | 3,120 | 4,161 |
| Distinct (tile, 4-neighbour) contexts | 247,926 (median 13 per flat, max 4,517) | 343,355 (median 15, max 6,037) |
| Distinct unordered pairs of *different* flats that touch | 64,228 | 98,892 |
| Distinct right-neighbours per flat (BG) | median 4, p90 39, max 265 | – |
| Adjacent map chunks using the *same* terrain number (h / v) | 6.9% / 8.3% | – |

The two most used BG terrains, 0 (4,001 uses) and 2940 (2,521 uses), are almost entirely grass, shape 19.

### 2.2 Edge classification (BG, horizontal and vertical, flat-to-flat edges only: 18.23 M)

| Edge class | Share | Seamless with naive per-tile AI? | Seamless with per-shape macro-texture? | Seamless with per-terrain override? |
|---|---|---|---|---|
| Same frame both sides (self-tiling) | **20.1%** | only if each tile is made wrap-around | conflicts with the torus for most frames | yes, inside the chunk |
| Same shape, consistent with the shape's best-fit WxH frame torus (e.g. shape 19: 8x4 frames, right = f+1 in its row of 8, down = f+8) | **48.2%** | no | **yes** | yes, inside the chunk |
| Same shape, irregular arrangement | **14.4%** | no | no | yes, inside the chunk |
| Different shapes (material transition) | **17.3%** | no | no | yes, inside the chunk |
| … of all the above, edges on a chunk border | **6.2%** of all flat-to-flat edges; **6.4%** of edges where the tiles differ. 48% of those are same-shape, i.e. *inside* a uniform material | – | – | **no** (neighbour varies) |

**Shape 19 (grass, 32 frames).** Inside a row of 8 frames the right neighbour is 100% deterministic (0→1→2→3, 4→5→6→7). The row wraps 7→0 74% of the time and 3→4 74% of the time. Alternative arrangements make up the rest; terrain 2940, for example, stacks the frames as 4x4 blocks 0-3/4-7/8-11/12-15. Grass is therefore a 64x32 px periodic macro-texture with some deliberate variation. This confirms `upscale-research/diffusion.md:17` and shows its limit: about 21% of grass edges break the torus.

Only **50 distinct flats** ever appear surrounded on all four sides by copies of themselves. They account for 14% of flat tile instances, all of them large uniform areas such as terrain 883, which is 256 x 48/4.

At 1x the original art is almost continuous across tile edges. The mean |ΔRGB| between the two pixel columns meeting at an edge is 30.5, against 25.0 for adjacent pixels inside a tile. The original artists painted across tile borders, so tile-by-tile upscaling is what creates the discontinuities.

### 2.3 Size and time

* **Per-tile set at S=6:** 48x48 = 2,304 B per frame. All 3,885 BG flat frames take ≈ 8.5 MiB raw; the 3,120 used ones take ≈ 6.9 MiB. They can be preloaded.
* **Per-terrain override at S=6:** 768x768 = 576 KiB raw, as indexed PNG:
  * ≈ 8 KiB if nearest-neighbour (pointless);
  * ≈ 140 KiB for a proxy with real detail (Lanczos x6, noise σ=6, Floyd-Steinberg quantised to indices 0x00-0xDF). Real AI detail will probably be larger.
  * Totals at that size: all of BG ≈ 290 MiB, all of SI ≈ 380 MiB, top-200 BG ≈ 27 MiB.
* **Decode:** a 768x768 indexed PNG takes ≈ 1.2 ms (libpng via PIL on this machine).
* **Composing a cache from per-tile overrides:** 256 copies of 48x48, ≈ 0.6 MB of memcpy, well under 1 ms.
* **Cache memory at S=6:** 100 x 576 KiB = 56 MiB, with the existing queue limit (also noted in shapes.md section 6 and ibuf.md section 5.5).
* **Runtime granularities that are clearly too big:**
  * per map chunk (36,864 x 576 KiB ≈ 20 GiB raw, and it breaks terrain sharing and the cache);
  * per superchunk (12,288² ≈ 151 Mpx each, 144 of them).
  * Superchunks are useful **only as offline context** for the generator.

---

## 3. Where seams come from and what each unit can guarantee

1. **Per-tile overrides upscaled in isolation.** Every edge except self-edges (about 80%) joins two images generated without knowledge of each other. Detail that the AI invents, such as a grass blade or stone grain, ends at x = 48k. The palette report's world-aligned dithering advice (palette.md section 7.6, lines 233-236) fixes **dither** seams only, not content seams.
2. **Per-tile overrides from a toroidal per-shape macro-texture.** Upscale the shape's 8x4 frame atlas with wrap-around (diffusion.md:17-22), then slice it. This makes the 48.2% torus edges seamless by construction. It does **not** cover:
   * self-edges of torus frames (the macro-tile next to a copy of itself), and the fill-under-RLE contexts from section 1.3, which are also mostly self-adjacent;
   * the 14.4% irregular same-shape edges;
   * the 17.3% material transitions.
3. **Per-tile overrides derived from context upscales.** Upscale chunks or windows with real neighbours, then pick or vote one rendition per (shape, frame) across its median 13 contexts (api_models.md:211-215). Interior detail becomes more plausible. Each tile still has one image that must meet *every* neighbour it has (median 4, p90 39 distinct right-neighbours), so edges still need a rule.
4. **Per-terrain overrides.** All 15 inner tile rows and columns are seamless if the terrain is upscaled as one image, or as a crop of a larger context window. Each terrain is used with many different neighbours: terrain 0 has 4,001 placements, and only 7-8% of chunk adjacencies repeat the same terrain. The 128-px chunk border therefore cannot be content-matched. Without an edge rule this shows as a **grid every 768 hi-res px**. It is most visible in grass and water fields, because 48% of differing chunk-border edges are same-material.
5. **Mixing the two.** A chunk drawn from a per-terrain override next to a chunk composed from per-tile overrides must also meet cleanly.

**Edge contract.** This is the one rule that makes every combination tile.

> The outer band (B hi-res px, e.g. B = S/2 = 3) of every override, per-tile and per-terrain alike, is a deterministic function D of only that override's own 1x border pixels. The interior blends into the band (ordered-dither crossfade in index space, world-aligned threshold map).

* Because the original art is nearly continuous across 1x edges (section 2.2), two bands produced by the same D meet with the original's own 1x step, and no new discontinuity appears.
* D can be pure nearest-neighbour index replication. This is the simplest choice and trivially verifiable, but slightly blocky along edges.
* D can also be a 1-D smooth interpolation along the edge, computed in RGB and quantised with the same world-aligned ordered dither the interior uses. Both sides then use the same thresholds.
* Hosted-model research already proposes this as a "seam guard band" (api_models.md:206). The point here is that it must be **one shared rule across both granularities**, and that it is what makes them interoperable.
* The contract is checked offline:
  * a test asserts that the band matches D(1x border) for every override;
  * a seam QA renders every distinct adjacent pair that occurs in the map (64k pairs in BG) and scores the joint.

---

## 4. Evaluation of the granularity options

| Criterion | A. Per-tile (file, shape, frame) | B. Per-terrain (content-hashed) | C. Per map chunk (map, cx, cy) | D. Superchunk at runtime |
|---|---|---|---|---|
| Seams without edge contract | ~80% of edges | ~6% of edges (chunk borders, grid-like) | 0% within the superchunk | 0% within it |
| Seams with edge contract | original-level everywhere | original-level everywhere | – | – |
| Interior quality | limited: one image per tile for all its contexts | best: whole-chunk context | best | best |
| Storage, S=6 | ≈ 8.5 MiB (BG) | 140 KiB+ per terrain; top-200 ≈ 30 MiB, all ≈ 0.3-0.4 GiB | ≈ 20 GiB raw | ≈ 22 GB raw |
| Works with the existing cache and sharing | yes, inside `render_flats` | yes, it *is* a cache prefill | no: needs a per-map-chunk cache | no |
| Survives map-editor edits | automatically (key is the tile) | yes: hash mismatch falls back to A | no | no |
| Survives terrain renumbering and mod `u7chunks` | yes | yes (content hash) | no | no |
| Survives mod-patched flat pixels in `shapes.vga` (`vgafile.cc:866-885`) | only with a source check (store the 64-B 1x tile or its hash) | yes (hash covers pixels) | – | – |
| Fill under RLE tiles | uses the substitute's override (needs the effective `ShapeID`) | baked in; the artist can paint a proper under-layer | – | – |
| Also used outside the cache | yes: `render_all`, `Shape_frame::paint` for flats (`vgafile.cc:525-534`) | no | – | – |
| Engine complexity | low | low (plus hash and a lookup) | high | high |

**Verdict.** Implement **A and B together** in the engine, with precedence B, then A, then nearest-neighbour. Deliver A first, generated by a context-aware pipeline (macro-texture torus where it fits, context voting otherwise), with the edge contract. Use B selectively:
* for the most-seen terrains: top 100 = 68% and top 200 = 81% of BG map chunks;
* for terrains rich in transitions (coastlines, roads, town floors), where one image per tile cannot look good;
* where the area under RLE objects deserves real art.

C and D are rejected as runtime units. Superchunks and larger windows remain the right **offline context** for generation.

---

## 5. Recommended engine design (terrain part)

### 5.1 `render_flats` pipeline

```
Image_buffer8* Chunk_terrain::render_flats():
  ensure src1x  (Image_buffer8 128x128, ZERO-FILLED)          // new, 16 KiB per cached terrain
  ensure rendered_flats (128*S x 128*S; S==1 -> alias src1x)  // existing member, scaled
  for each tile: eff[t] = paint_tile_1x(tx, ty)               // returns ShapeID actually painted
                                                               // (flat itself or fill substitute)
  if S == 1: return src1x
  key = fnv1a64(src1x.bits, 16384)                            // same algorithm in tools/hires
  if hires.terrain(key) and (optional) its stored 1x == src1x:
        decode into rendered_flats; return
  for each tile t with eff[t] valid:
        if (ov = hires.flat(SF_SHAPES_VGA, eff[t].shape, eff[t].frame & 31))  and  ov.src64 == tile 1x pixels:
              copy 8S x 8S
        else: nearest-neighbour expand the 8x8 from src1x
  tiles with no shape: NN-expand the zero-filled 1x cell (deterministic)
```

* **Keep `src1x` alongside the hi-res cache.** It costs 1.6 MiB for 100 terrains. It is the hash source, and it is the correct input for `write_minimap` (`gamemap.cc:1705-1722`), which should not average or decode 3,072 hi-res buffers. It also allows an "S=1 is pixel-identical to legacy" regression test.
* **Key.** A 64-bit FNV-1a over the 16,384 index bytes is trivial to implement identically in C++ and Python. Optionally store the 1x source in the override sidecar and compare exactly, so hash collisions cannot cause errors. Put the key in the file name, e.g. `<mod-or-patch>/hires/x6/terrain/<16 hex>.png`. Duplicate terrain contents (49 used in BG) then share one file automatically.
* **No reliance on `modified` or `tnum`** (section 1.4). Map-editor edits reach `render_flats` through `commit_edits` (`chunkter.cc:208-216`), produce a new hash and fall back to the per-tile path automatically. That is exactly the "drop when modified" behaviour, without its false positives and false negatives.
* **Stale caches** after `Terrain_game_object::move` / `remove_this` (`objs.cc:1640-1683`) already exist at 1x. Optionally call `free_rendered_flats()` from `set_flat` so that the next paint re-renders and re-keys.
* **Flushing.** Add `static Chunk_terrain::flush_all_rendered()`. It walks `render_queue` and calls `free_rendered_flats()` (also on terrains outside the queue via `chunk_terrains`). Call it when S changes, when the hi-res pack is (re)loaded, and in `Game_map::clear_chunks`.
* **Queue limit.** Keep `Figure_queue_size()` (`chunkter.cc:234-242`) but make it S-aware, or derive it from the view size as the commented-out code suggests. 100 x 576 KiB = 56 MiB is acceptable on desktop and too much on Android.
* **Per-terrain overrides are complete 768x768 indexed images** of the flat layer only: no RLE objects, with the under-RLE area painted. They follow the palette rules (cycling indices preserved, no 255) in palette.md sections 7.2-7.6.
* **Smooth scrolling and sub-tile offsets** do not change this. The cache is blitted at game-pixel offsets multiplied by S (`gamerend.cc:228-231`, `:529`), so every tile and chunk border stays on an S-aligned hi-res boundary.

### 5.2 Optional engine-side seam helpers (not v1)

* **Bleed-margin tiles.** A per-tile override could carry a few extra pixels per side (e.g. 56x56). When composing the cache, those margins are crossfaded into the neighbour with world-aligned ordered dither. This works only *inside* a terrain, because a terrain does not know the chunks around it. Chunk borders still depend on the edge contract.
* **Paint-time chunk-border crossfade.** This interacts with dirty rects and clip rects in `paint_map`, and it would run every frame. Rejected.
* **Macro-texture-aware fill under RLE.** Pick the torus-consistent frame, e.g. 19/(f+1) to the right of 19/f, instead of a copy. This needs per-shape torus metadata in the pack. Deferred; per-terrain overrides cover the visible cases.

---

## 6. Art-pipeline implications (tools/hires)

1. **`u7art.render_chunk` must emulate `paint_tile` exactly**, including the fill rules after the engine fixes in section 7. Today it writes zeros under RLE tiles (`tools/hires/u7art.py:199-207`), and the superchunk export reuses it (`:296-310`). Verified: every one of 2,005 sampled RLE positions in `art_original/chunks` is an all-index-0 8x8 block. Black holes in AI context images produce dark halos in the upscale, and a hash computed from such renders will never match the engine's.
2. **Fix `V2_CHUNK_HDR`** (`tools/hires/u7art.py:33`). It is a 26-byte "exlevel" string; the engine uses the 10-byte `FF FF FF FF "exlt" 00 00` (`gamemap.cc:84-95`). Also read `<PATCH>/u7chunks` and patched `shapes.vga` the way the engine does, so mods are covered.
3. **Context renders are flat-only**, with the fill and without RLE objects. Otherwise bushes and rocks bleed into the flat layer as "ghosts".
4. **Generation order:**
   * per-shape toroidal macro-texture for torus shapes (shape 19 and 147-149 fit 8x4 at 74-82%; 64 and 30 at 99%);
   * context windows (chunk plus margin) for transitions;
   * vote or medoid per (shape, frame);
   * apply the edge band D;
   * quantise to indices with the world-aligned threshold map;
   * keep the original indices of cycling pixels.
5. **Emit metadata with each override:**
   * per-tile: the 64-byte 1x source;
   * per-terrain: the FNV-1a key and optionally the 16 KiB 1x source;
   * optional per-shape torus WxH.
6. **Seam QA** is a test step, not a visual check. For each of the 64k distinct touching pairs, and for each chunk-border pair that occurs, assemble the hi-res joint and measure the edge-gradient energy against the interior. The baseline is the same metric on the nearest-neighbour joint.

---

## 7. Engine fixes to land before any art is hashed

| Fix | Where | Effect on output | Impact |
|---|---|---|---|
| `tiley + y > 0` → `>= 0` | `chunkter.cc:104` | different fill under some RLE tiles | BG 2,367 positions / 779 terrains (37% of used); SI 3,604 / 1,085 |
| Skip void 12/0 in the full-chunk fallback too (consistency) | `chunkter.cc:119-126` | different fill in rare cases | BG 112, SI 194 positions |
| Zero-fill the cache before painting | `chunkter.cc:259` (`fill8(0)`), `ibuf8.h:37-39` | deterministic pixels where a shape is missing | BG 149 tiles (48/8) in 2 terrains |
| Return the effective `ShapeID` from `paint_tile` | `chunkter.cc:86-133` | none (refactor) | needed by the per-tile hi-res pass |
| Optional: re-render or free the cache in `set_flat` | `chunkter.cc:193-200` | removes a stale cache after terrain-object moves | rare in play |

Each fix changes the 1x output, which matters for golden-image tests and for hashes. Land them as separate, test-covered commits *before* art generation. The bound fix changes 37% of the used BG terrains' renders. Making the fixes conditional is possible but not worth it, because the differences are under RLE objects.

---

## 8. Tests the design implies

* **S=1 identity:** after the fixes, the new `render_flats` produces bytes identical to the reference implementation for all 3,072 terrains, BG and SI. Store golden hashes.
* **Cross-language key:** Python and C++ FNV-1a over the emulated and engine 1x renders agree for all used terrains.
* **Fill unit tests:** row-0 neighbour, all-RLE neighbourhood leading to the full-chunk scan, void skipping, missing shape giving zero-fill.
* **Invalidation:**
  * `set_flat` + `commit_edits` drops a per-terrain override;
  * `swap_terrains`, `insert_terrain` and `delete_terrain` keep it bound to the right content;
  * a patched `u7chunks` and a patched flat frame drop the relevant overrides.
* **Precedence and fallback:** terrain override, then per-tile override, then nearest-neighbour. A per-tile override whose 64-byte source does not match is ignored.
* **Edge contract validator** over the whole pack, plus a seam-QA metric on a sample of real map joints.
* **Cache:** `flush_all_rendered` frees everything; the queue limit holds at S=6; `commit_edits` outside the queue does not leak.

---

## 9. Open questions

* B (band width) and the choice of D: nearest-neighbour or smooth-along-edge with dither. Needs a visual A/B at S=6 on grass, water and roads.
* Should per-terrain overrides be allowed to be *partial*, e.g. a per-tile mask "fall back to A here"? Probably not for v1.
* Which terrains get B overrides: by usage rank, by transition density, or by hand (towns)?
* Should RLE terrain objects (shapes 150 and up, e.g. 331, 314, 306) get per-terrain-placement art? No: they stay per-frame RLE overrides (shapes.md), painted after the flats.
* Multi-map mods share `chunk_terrains` across maps (`gamemap.h:61`). Content-hash keys handle this, but usage statistics should include all maps.
