# Gap 6: art extraction must use the engine's effective sources, not static `shapes.vga` alone

Scope: how Exult master (`exult-hires` @ `8b6ab6b43`) builds the set of shape frames it actually draws, compared with what
`art_original/` contains today, and what an extractor plus CRC generator must do to match the engine. All `file:line`
references are relative to `/home/simonea/ultima7_exult/exult-hires` unless they say otherwise. I measured the game data
read-only at `/mnt/e/Games/RolePlayingGames/ultima7/static` (BG with FoV) and `/mnt/e/Games/RolePlayingGames/Serpent/static` (SI).

Related analyses: `shapes.md` (loader, reflection, hook design), `palette.md` (index semantics), `build.md` (tools).
This document does not repeat them. It only adds the "effective source" view.

---

## 0. TL;DR

1. `art_original/` was produced by `tools/hires/u7art.py`. It parses **one raw file**, `<static>/shapes.vga`, plus
   `palettes.flx`, `u7chunks` and `u7map` from the static dir (`tools/hires/u7art.py:182-189`; `manifest.json` `"source":
   "/mnt/e/.../ultima7/static"`). It covers shapes 0..1023 only: 3,885 flat frames (shapes 0..149) and 10,286 RLE frames
   (150..1023). It records no provenance, no frame fingerprint, no patch or mod layering, no imports, and no other shape file.
2. The engine draws from **eight `Vga_file`s plus fonts, pointers and menu shapes**. Each is a *stack* of sources
   (static, then Exult data flex, then `<PATCH>`), searched back to front and **whole shape at a time**
   (`shapes/vgafile.cc:866-885`). Some files also have an **import table** that maps a *logical* shape number to a shape in
   a different game's file (`shapes/vgafile.cc:1239-1268`, `shapes/vgafile.h:357-377`).
3. In BG, three things come from SI and are drawn with the BG palette under BG logical numbers:
   * skins: SI `shapes.vga` 1024..1035 become BG shapes **2048..2059** (`data/bg/shape_files.txt:59-70`, numbers assigned
     from `c_max_shapes`=2048 by `shapes/miscinf.cc:157,169-174`);
   * paperdoll "blue" slot gumps: SI `gumps.vga` 54/55 become BG gumps **2048/2049** (`data/bg/shape_files.txt:33-34`);
   * the whole paperdoll file: SI `paperdol.vga` (160 shapes), overlaid by `exult_bg.flx[3]` (`bg_paperdol.vga`: 14 shapes
     replaced, 39 added: 170..208), then `<PATCH>/paperdol.vga`.

   BG also always gets multiracial face **shape 5000** (6 frames) from `exult_bg.flx[4]` in `faces` (`shapeid.cc:257-265`).
4. **What the engine draws depends on configuration.** The current `run/exult.cfg` defines only `blackgate`. So
   `<SERPENT_STATIC>` falls back to `"."` (`gamemgr/modmgr.cc:1238-1239`), and the engine reports *"Support for SI
   Paperdolls in BG is disabled"* and *"Support for SI Multiracial Avatars is disabled"* (`run/buildmap.log:48-51`). The
   installed BG mods (Keyring, Ultima6v1.2, islefaq) replace or add 70, 263 and 1 shapes through their own `shapes.vga`, and
   ship their own `avatar_data.txt`, `faces.vga` and so on. The effective art set is therefore a function of
   (game, mod, SI present?, font setting).
5. **For the first-priority target (flat terrain, shapes 0..149) `art_original` is currently correct.** No patch dir exists,
   and none of the installed mods replaces a flat shape (measured: 0 of 150 in each). Everything else (skins, gumps,
   paperdolls, faces, sprites, fonts, Exult UI shapes) is missing, and nothing in the manifest would let the engine detect a
   mismatch later.
6. Recommendation: build the extractor **inside the engine**, as a `--dump-art <dir>` CLI mode modelled on `--buildmap`
   (`exult.cc:986-989, 2857-2912`). It walks `Shape_manager` after `init_files()` through `Vga_file::get_shape()`. It writes
   raw-index PNGs, one manifest per configuration with provenance, and a **canonical decoded-frame CRC32**. The runtime override
   loader computes the same CRC with the same C++ helper. A re-implementation in Python would have to copy several engine quirks
   (§6) and would drift.

---

## 1. What `art_original/` is today

| Item | Value | Evidence |
|---|---|---|
| Producer | `tools/hires/u7art.py` (untracked file in the fork) | `git status`: `?? tools/hires/` |
| Inputs | `<static>/palettes.flx`, `shapes.vga`, `u7chunks`, `u7map`, all from one dir | `u7art.py:182-189` |
| Flat frames | 3,885 (shapes 0..149), 3,120 used on the map, 369 with indices >= 0xE0 | `manifest.json` `summary` |
| RLE frames | 10,286 (shapes 150..1023), none use opaque index 255 | `manifest.json`, `u7art.py:264-270` |
| PNG convention | raw engine indices, PLTE = palette 0, RLE transparent = 255 via tRNS, **no** ipack rotation | `u7art.py:222-226, 257, 264` |
| Naming | `shapes/{flat,rle}/SSSS_FF.png` (2-digit frame) | `u7art.py:255` |
| Per-frame metadata | shape, frame, extents (RLE), `map_uses` (flats), reserved-index flags | `u7art.py:258-270` |
| Missing | provenance (which source won), imports, `<PATCH>`, mods, CRC, every other shape file | — |

`map_uses` is computed from static `u7chunks`/`u7map`. The engine prefers `<PATCH>/u7chunks` and `<PATCH>/u7map`
(`gamemap.cc:152-171, 208-212`), and the Ultima6 mod ships both. So usage ranking is also config-dependent.

---

## 2. How the engine assembles each shape file

### 2.1 `Vga_file` mechanics that matter for extraction

* **Source stack.** `Vga_file::load(vector<pair<string,int>>)` opens every source with `U7load` (`vgafile.cc:1163-1183`).
  A source is a file path (`second < 0`) or an entry of an Exult flex (`IExultDataSource(name, index)`). `is_patch` is
  set when the path starts with `<PATCH>` (`:1169`). The shape count is the **max** over sources (`:1198-1209`). The
  return value is `false` when `sources[0]` does not exist, even if later sources loaded (`:1195-1197`). The
  paperdoll logic relies on this (§2.4).
* **Back-to-front, whole shape.** `Shape::read` walks the sources from last to first. It takes the first one whose table
  entry for `shapenum` is non-empty (`vgafile.cc:866-885`). Every frame of a shape therefore comes from the same source, and a
  patch shape can change the frame count and the dimensions of every frame. `from_patch` records only a bool (`:881`). The
  *source index* that won is not stored anywhere.
* **Imports.** `import_shapes(source, {(logical, real)…})` (`vgafile.cc:1239-1268`):
  * starts with `reset_imports()` (`:1240`), so a `Vga_file` can have **only one import source**;
  * opens the foreign file into `imported_sources`, and for each pair stores `imported_shape_table[logical] =
    {realshape, slot, src}` (`:1249-1258`);
  * if the source cannot be opened, it still registers each logical number with `pointer_offset = -1` (`:1260-1265`), and
    `get_shape` then returns `nullptr` (`vgafile.h:363-365`).
* **Lookup.** `get_shape(shapenum, framenum)` checks the import table first and then the normal stack
  (`vgafile.h:357-377`). The caller sees only the **logical** number. Reflected RLE frames (`frame|32`) are produced on demand
  by transposition (`vgafile.cc:915-918, 73-126`). Flats are masked with `&31` (`vgafile.cc:443, 912-914`).
* **Enumeration traps:**
  * `get_num_shapes()` returns `shapes.size()` (`vgafile.h:344-346`). It **does not include imported logical numbers**
    (2048+), so a loop `0..get_num_shapes()-1` misses all imports.
  * `get_shape(n, f)` with `n >= shapes.size()` and `n` not imported indexes `shapes[n]` out of range. This is UB, and only
    an `assert(!shapes.empty())` guards it (`vgafile.h:369-374`).
  * `imported_shape_table` is `protected` (`vgafile.h:308`). There is no public way to list the imported logical numbers,
    only `is_shape_imported(n)` and `get_imported_shape_data(n, …)` (`vgafile.h:334-335`).
  * `get_num_frames(n)` forces frame 0 into memory (`vgafile.h:400-408`). For an empty entry it returns 0.

### 2.2 Who owns what (`Shape_manager`)

`Shape_manager` has `Shapes_vga_file shapes` and `Vga_file files[SF_COUNT]` (`shapeid.h:83-85`).
**`files[SF_SHAPES_VGA]` is never loaded.** `shapes.vga` lives in the separate `shapes` member, reached with
`get_shapes()` (`shapeid.h:138-140`), and `cache_shape` special-cases it (`shapeid.cc:562-564`). An extractor that loops over
`get_file(SF_*)` for every `SF_*` gets an empty `Vga_file` for shapes, and `get_shape` would assert.

`ShapeFile` enum: `SF_SHAPES_VGA, SF_GUMPS_VGA, SF_PAPERDOL_VGA, SF_SPRITES_VGA, SF_FACES_VGA, SF_EXULT_FLX, SF_GAME_FLX,
SF_SHORTCUTBAR_VGA, SF_OTHER` (`shapeid.h:39-51`). Fonts are not in it.

### 2.3 Per-file source tables (from `Shape_manager::load`, `shapeid.cc:150-382`)

Order matters: `shapes.reset_imports()` (`:156`) → gumps → paperdolls → BG imports (`:191-233`) → sprites → faces →
exult/game flex → shortcut bar → **`read_shape_info()` → `shapes.init()`** (`:281`, `:116-120`, `shapes/shapevga.cc:972-979`).
`shapes.init()` reloads the `shapes.vga` stack with `resetimports=false`, so the skin imports registered earlier survive.

| ShapeFile | Sources, back-to-front order = last wins | Imports | Notes |
|---|---|---|---|
| shapes (`sman->shapes`) | `<STATIC>/shapes.vga`, `<PATCH>/shapes.vga` if it exists (`shapevga.cc:972-979`) | **BG:** SI `shapes.vga` 1024..1035 → logical 2048..2059 (`shapeid.cc:218-232`) | Logical numbers come from the parsed table, not from 1024+N (§2.5) |
| gumps | `<STATIC>/gumps.vga`, `<PATCH>/gumps.vga`, `resetimports=true` (`shapeid.cc:177`) | **BG, only if the SI paperdoll file was found:** SI `gumps.vga` 54, 55 → 2048, 2049 (`:198-216`) | Used as paperdoll "blue" slot backgrounds (`gumps/Paperdoll_gump.cc:601-602`, `shape_files.txt:44-48`) |
| paperdol | `Shapeinfo_lookup::GetPaperdollSources()` (`shapeid.cc:179`). **BG:** `<SERPENT_STATIC>/paperdol.vga`, `exult_bg.flx[3]` (`bg_paperdol.vga`), `<PATCH>/paperdol.vga`. **SI:** `<STATIC>/paperdol.vga`, `<PATCH>/paperdol.vga` | none | `load()` returns false when SI `paperdol.vga` is missing, which disables paperdolls in BG (`:179-189`) |
| sprites | `<STATIC>/sprites.vga`, `<PATCH>/sprites.vga` (`:235`) | **SIB only:** shape 0 from SI or BG `sprites.vga`, or 28 synthesized 1x1 frames of index 118 (`:236-255`) | `has_trans` forced true (`:567-569`) |
| faces | `<STATIC>/faces.vga`, **BG:** `exult_bg.flx[4]` (`bg_mr_faces.vga`, count 5001, only entry 5000 non-empty), `<PATCH>/faces.vga` (`:257-265`, `gamemgr/bggame.cc:152`) | none | Effective count in BG is 5001. Shape 5000 frames 0..5 = multiracial faces (`data/bg/avatar_data.txt:43-48`) |
| exult_flx | `<DATA or BUNDLE>/exult.flx` (`:267`) | none | Mixed flex: text, palettes and MIDI next to `*_SHP` entries (`data/exult_flx.h:8-40`) |
| game_flx | `exult_bg.flx` / `exult_si.flx` (`:269-271`, `bggame.cc:149`) | none | Mixed flex. Shape entries in BG: `BGMAP_SHP`=0, `INTRO_HAND_SHP`=13 |
| shortcutbar | `exult.flx[31]`, `<PATCH>/shortcutbar.vga` (`:273-279`) | none | |
| fonts | config `gameplay/fonts`: **original (default)** = `exult.flx[29]` + `<PATCH>/fonts_original.vga`; serif = `exult.flx[30]` + `<PATCH>/fonts_serif.vga`; disabled = `<STATIC>/fonts.vga` + `<PATCH>/fonts.vga` (`shapeid.cc:283-304`) | none | Each `Font` copies its entry into a private `Shape_file font_shapes` (`shapes/fontvga.cc:72-86`, `shapes/font.cc:775-795`, `font.h:55`). **With the default config, static `fonts.vga` is not what the game draws.** |

Outside `Shape_manager`:
* `pointers.shp`: `<PATCH>` if it exists, otherwise `<STATIC>` (`mouse.cc:107-111`). The menu instead uses `exult.flx[5]` (`mouse.cc:127`).
* `Game::menushapes` = `MAINSHP_FLX` + `PATCH_MAINSHP` (`game.cc:89`).
* Cheat minimaps `<PATCH>/minimaps.vga` (`cheat.cc:1222-1223`), and save thumbnails (`gamedat.cc:680-759`). These are low priority.

### 2.4 Paperdoll source list in detail

`setup_shape_files()` parses the `shape_files` text (`miscinf.cc:420-439`) through `Read_data_file`. For BG/SI the static copy
is the **embedded resource** `config/shape_files` = `exult_bg.flx[8]` (`miscinf.cc:363-369`, `bggame.cc:157`), not a file in
the static dir. A `<PATCH>/shape_files.txt` is read afterwards (`miscinf.cc:380-414`).

BG `paperdoll_source` = `:si`, `:flx` (`data/bg/shape_files.txt:20-21`). The parser maps `si` →
`<SERPENT_STATIC>/paperdol.vga` and `flx` → `game->get_resource("files/paperdolvga")` = `exult_bg.flx[3]`
(`miscinf.cc:335-345`). `setup_shape_files` then appends `PATCH_PAPERDOL` (`:438`).

Effective BG paperdoll file with SI present (measured):
* SI `paperdol.vga`: count 200, 160 non-empty.
* `bg_paperdol.vga`: count 209, 53 non-empty. **Replaces** 4, 15, 41, 42, 43, 62, 67, 74, 104, 106, 124, 125, 126, 128 (BG
  avatar/companion faces, BG weapons) and **adds** 170..208.
* Result: 209 logical shapes, 199 non-empty. Their pixels come from two different files.

### 2.5 How logical numbers for imports are assigned

`Shape_imports_parser` (`miscinf.cc:151-180`):
* `data.second` = real (foreign) shape, `data.first` = logical. With `%name` the logical number is a running counter that
  starts at `c_max_shapes` = 2048 (`:157`, `exult_constants.h:51`) and increments **in order of appearance** (`:169-174`).
  The digits after `%` are only a label, so `%5` is 2053 only because it is the 6th entry.
* A repeated *real* shape is silently ignored (`:163-167`), including in the patch file.
* The gump and skin sections use **separate parser instances**, so both counters start at 2048 (`:428-430`). Gump 2048 and
  shape 2048 are unrelated.
* Consumers resolve `%N` through `skinvars`/`gumpvars` (`miscinf.cc:257-268, 505-512`; `shapes/shapevga.cc:60-76` for
  paperdoll NPC ids). BG `multiracial_table` uses `%0..%11` for dressed/naked skins and face 5000 frames 0..5
  (`data/bg/avatar_data.txt:43-48`).
* Without SI shapes (`have_si_shapes()==false`), `GetSkinInfoSafe` falls back to the default skin whenever a skin uses an
  imported shape (`miscinf.cc:544-553`, `actors.cc:3373, 4870-4871`). Under the current config, 2048..2059 are therefore
  **unreachable**, so not extracting them is correct for now but wrong once SI is configured.

---

## 3. Configuration dependence (path tags)

* `<SERPENT_STATIC>` and `<ULTIMA7_STATIC>` default to `"."` (`gamemgr/modmgr.cc:1238-1239`). `print_found` replaces them
  with the configured game's static dir. It runs BG then FoV, SI then SS, so **the expansion wins**
  (`modmgr.cc:1240-1243, 1263-1268`). With FoV installed, BG imports come from the FoV static dir. With Silver Seed
  installed, SI imports come from the SS static dir. Those files can differ, and that difference is exactly what a CRC catches.
* Hazard: when SI is not configured, `<SERPENT_STATIC>/shapes.vga` resolves to `./shapes.vga` relative to the process cwd.
  If Exult is started from a directory that contains a `shapes.vga`, `import_shapes` succeeds and `got_si_shapes` becomes
  true. `Shape::read` then finds nothing for 1024..1035 (BG has only 1024 shapes) and returns `nullptr`, so the skins draw as
  invisible. Any extractor must log the **resolved** path of every source.
* Current machine facts:
  * `run/exult.cfg` defines only `blackgate` (`/mnt/e/.../ultima7`).
  * The engine identifies the game as `FORGE` (BG+FoV) and prints `Patch : /mnt/e/.../ultima7/patch` (that directory does
    not exist), then the two "disabled" lines (`run/buildmap.log:24-51`).
  * SI is installed at `/mnt/e/Games/RolePlayingGames/Serpent` but is not configured.
* `<PATCH>` is the game patch dir by default (`modmgr.cc:608-612`). When a mod is active it is the mod's patch dir
  (`modmgr.cc:70-74, 134-140`). `Read_data_file` reads `<PATCH>/shape_files.txt` (`miscinf.cc:380-390`), and
  `setup_avatar_data` reads `<PATCH>/avatar_data.txt`. A mod can therefore change the import lists, the paperdoll sources and
  the skin table. Example: the Keyring mod maps skins to its **own** shapes 1028..1047 instead of imports
  (`mods/Keyring/data/avatar_data.txt`, multiracial_table).
* Installed mods (measured): Keyring `data/shapes.vga`: count 1133, 70 non-empty, 57 of them new (>= 1024).
  Ultima6v1.2 `patch/shapes.vga`: count 1268, 263 non-empty (71 replace originals, 192 new), plus its own
  `u7chunks`/`u7map`/`faces`/`gumps`/`paperdol`/`sprites`/`mainshp`. islefaq: 1 shape, plus `faces.vga` and `u7map`.
  **None replaces a flat (0..149).**
* Engine and ES disagree on BG paperdolls and faces: the engine never reads `<PATCH>/bg_paperdol.vga` or
  `<PATCH>/bg_mr_faces.vga` (`fnames.h:51-52`). Only ES does (`mapedit/shapefile.cc:582-601`). Extraction must follow the
  engine.
* The ES reload path builds **different** stacks than `load()`: faces → `FACES_VGA`+`PATCH_FACES` (drops mr-faces), and
  paperdol → `PAPERDOL`+`PATCH_PAPERDOL` (drops the SI and flx sources) (`shapeid.cc:455-463`). It also indexes
  `shape_cache` by a u7drag kind (see `shapes.md` §3.4). A hi-res attach must re-validate after any reload, and the
  extractor must never use this path.

---

## 4. Palette semantics for imported and Exult-supplied frames

* Imported frames are copied as raw indices from the foreign file and painted through the **current BG palette** and BG
  xforms. Nothing remaps them (`vgafile.h:366-367` returns the foreign `Shape_frame` unchanged).
  The correct reference for quantizing hi-res art is therefore the engine's effective palette:
  `Palette::load(PALETTES_FLX, PATCH_PALETTES, n)` (`shapeid.cc:161`, `palette.cc:141`). The SI palette is the wrong reference.
* Measured: BG and SI `palettes.flx` entries 0..8 and 10..12 are **byte-identical** on this install (BG has no entry 9, SI
  does). So in practice there is no colour shift today, but a mod's `<PATCH>/palettes.flx` can break that. Also, u7art reads
  only static palettes, which ignores `PATCH_PALETTES`.
* Reserved or special indices (>= 0xE0: cycling and translucent) per source, measured as frames that use any index >= 0xE0:
  SI skins 1024..1035: 0/384; SI gumps 54/55: 0/2; SI `paperdol.vga`: 114/458; `bg_paperdol.vga`: 39/167; `bg_mr_faces`: 0/6;
  BG gumps: 3/175; BG faces: 18/296; BG sprites: 46/230; BG `fonts.vga`: 94/1012; BG `shapes.vga`: 1471/14171.
  No in-game source uses index 255 as an opaque pixel, so 255 can stay the transparent key. `endshape.flx` has 6 frames that
  use 255, but it is not world art.
* The PNG export convention must stay **raw indices, no rotation**. `Export_png8(..., transp_to_0=true)` (used by ipack and
  ES) rotates every index by +1 (`shapes/pngio.cc:215-252`). Mixing both conventions in one pipeline shifts colours by one
  index. The manifest should state the convention explicitly.

---

## 5. Measured coverage gap

| Effective source (BG+FoV) | Reachable under current cfg? | Frames | In `art_original`? |
|---|---|---|---|
| shapes 0..149 (flats) | yes | 3,885 | yes (correct) |
| shapes 150..1023 (RLE) | yes | 10,286 | yes (correct, no patch present) |
| shapes 2048..2059 (SI skins) | **no** (needs SI cfg) | 384 | no |
| gumps 0..74 | yes | 175 | no |
| gumps 2048/2049 (SI 54/55) | no (needs SI) | 2 | no |
| paperdol (SI + bg_paperdol) | no (needs SI) | 458 SI + 167 BG (overlaid) | no |
| faces 0..299 + 5000 | yes | 296 + 6 | no |
| sprites | yes | 230 | no |
| fonts (default: `exult.flx[29]`) | yes | ≈1k glyphs | no |
| exult.flx / exult_bg.flx `*_SHP` entries, shortcut bar, pointers, mainshp | yes | — | no |

---

## 6. Engine quirks that a re-implementation would get wrong

1. **`Paperdoll_source_parser` clears the table for every patch line.** `erased_for_patch` is initialised false and is
   never set to true (`miscinf.cc:212-215, 330-332`). A patch `paperdoll_source` section with several lines therefore keeps
   only the **last** one, and `PATCH_PAPERDOL` is appended afterwards (`:438`). A "correct" Python parser would differ from
   the engine.
2. Import logical numbers are positional (`%` counter), and repeated real shapes are deduplicated (§2.5).
3. Only one import source per `Vga_file` is possible (`vgafile.cc:1240`).
4. Paperdoll enablement depends on whether `sources[0]` exists (`vgafile.cc:1195-1197`), not on whether any source loaded.
   The gump import runs only if that succeeded (`shapeid.cc:198`).
5. Whole-shape patch override with a changed frame count (`vgafile.cc:866-885, 909-911`).
6. Imports are invisible to `get_num_shapes()`. `files[SF_SHAPES_VGA]` is empty (§2.1, §2.2).
7. Flats use `frame & 31`, and RLE `frame|32` means a transposed reflection unless the shape has more than 32 frames
   (`vgafile.cc:443, 912-918`; `shapeid.cc:122-131`).
8. `exult.flx` and `exult_bg.flx` mix shapes with text, palettes and MIDI. Decoding every entry as a shape produces garbage.
   Only the `*_SHP`/`*_VGA` indices from `data/exult_flx.h` and `data/exult_bg_flx.h` are shapes. `mainshp.flx` and
   `endshape.flx` are also mixed (u7art-style decoding reports bogus "flats" there).
9. The font source depends on configuration, and fonts bypass `Vga_file::get_shape` (`font.cc:775-795`).
10. ipack cannot produce the effective set. It opens one file with a single `Vga_file` and enumerates `get_num_shapes()`
    (`tools/ipack.cc:95-111, 755-767`), so it sees no patch layering and no imports. "ipack per source plus a manual merge"
    would have to re-implement items 1-6.

---

## 7. Design: an effective-source extractor and a CRC generator

### 7.1 Recommended: in-engine `--dump-art <dir>` mode

Model it on `BuildGameMap` (`exult.cc:2857-2912`). That code already creates a `Game_window`, calls `Game::create_game(game)`
and `gwin->init_files(false)`, which runs the full `Shape_manager::load()` (`gamewin.cc:572-585`) with the real game, mod and
patch paths, and then exits. `--buildmap` is known to work in this WSL setup (`run/buildmap.log`, `run/bg-saves/u7map*.png`).
Argument plumbing exists (`exult.cc:298-312, 401-405, 986-989`). `--bg/--si/--mod` choose the configuration, and
`-c <cfg>` can point at a config that also defines `serpentisle`, so the SI-enabled BG set can be dumped.
`Shape_manager::load()` needs a `Game_window` (`gwin->abort`, `gwin->get_pal()` at `shapeid.cc:181, 353`), and
`setup_avatar_data` needs `Game_window::get_instance()->get_usecode()` (`miscinf.cc:310-311`). A standalone tool linking only
`libshapes`/`libu7file` therefore cannot reuse this code without stubs, which is why the in-engine route is preferred.

Walk:
1. `sman->get_shapes()` for `0..get_num_shapes()-1` (skip `get_num_frames()==0`), **plus** every imported logical number.
2. `sman->get_file(SF_GUMPS_VGA | SF_PAPERDOL_VGA | SF_SPRITES_VGA | SF_FACES_VGA | SF_SHORTCUTBAR_VGA)` the same way.
   Paperdolls only if `can_use_paperdolls()`.
3. `SF_EXULT_FLX` / `SF_GAME_FLX`: only the known shape indices.
4. Fonts: every `sman->get_font(i)` glyph frame, tagged with the font source in use.
5. Later phases: `Mouse::pointers`, `Game::menushapes`.

For each real frame (not the reflections), write:
* a PNG with **raw indices**, PLTE = effective palette 0 (static + `<PATCH>`, 6→8 bit like `Get_color8` at brightness 100,
  `imagewin/iwin8.cc:84-90`), and tRNS only on 255 for RLE frames;
* a manifest row: `{file, shape(logical), frame, rle, xleft,xright,yabove,ybelow, crc32, provenance{source_spec,
  resolved_path, real_shape, imported, from_patch}, uses_reserved, uses_255, map_uses(flats, from the loaded Game_map)}`;
* a manifest header: `{game, game_identity (e.g. FORGE), mod, si_present, fonts_setting, engine_git_rev, palette_crc,
  png_convention:"raw-index,transp=255,no-rotation", scale_target}`.

Write one manifest per configuration, for example `art_original/bg-fov/`, `art_original/bg-fov+si/`, `art_original/bg-fov@Keyring/`.
Frames shared across configurations have the same CRC, so packs can be deduplicated by CRC.

### 7.2 Small API additions this needs (all additive)

* `Vga_file`: a public `std::vector<int> imported_shape_numbers() const` (or an iterator over `imported_shape_table`).
* `Vga_file`: keep the source specs. `U7load` currently drops the `pair<string,int>` (`vgafile.cc:1163-1183`). Add
  `const std::pair<std::string,int>& source_spec(int i)` and record the **winning source index** in `Shape::read`
  (`vgafile.cc:878-882, 893-897`) next to `from_patch`.
* `Font`: a read-only accessor to `font_shapes` (`font.h:55`).
* `Shape_frame`: `uint32 content_crc() const` (§7.3), placed in libshapes so that the dump tool and the runtime loader share it.
* A buffer overload of `crc32` (`files/crc.h` only has `crc32(const char* filename)`), or zlib's `crc32`.

### 7.3 Frame fingerprint (CRC) specification

Hash the **decoded canonical frame**, not the raw RLE stream. ES and ipack re-encode RLE (`Shape_frame::create_rle` /
`encode_rle`), so the same image can have different bytes.
Canonical bytes: `u8 kind (0=flat,1=rle) | i16le xleft,xright,yabove,ybelow | w*h index bytes (row-major, transparent → 255)
| w*h/8 mask bits`. For flats, use the 64 bytes with the extents fixed at `8,-1,8,-1`. CRC32 (IEEE).
The runtime override loader computes `content_crc()` of the frame that `Vga_file::get_shape()` returned for the logical key.
That frame is the imported one for 2048+, and the patch one when a mod replaced the shape. If the CRC differs from the pack's
recorded CRC, the override is rejected and the engine falls back to nearest-neighbour upscaling. This one rule covers mods,
FoV versus BG, SI versus SS imports, and `<PATCH>` edits. Raw-byte CRC (`get_data()`/`get_size()` + extents) would also be
safe, because it fails towards fallback, but it gives false mismatches after any re-encode.

### 7.4 Interim path (until the engine dump exists)

Python `u7art.py` is still valid **for flats 0..149 under the current configuration**. It should immediately gain:
* `"provenance": "<STATIC>/shapes.vga"` and the same `crc32` canonical form (§7.3) per frame, so that a later engine dump can
  cross-check it byte for byte;
* a refusal to run when `<static>/../patch/shapes.vga` exists, or when the manifest is meant for a mod.
Extending the Python tool to mimic §2.3-§2.5 is possible: read `exult.cfg`, `exult_bg.flx[8]` (shape_files),
`exult_bg.flx[3]`/`[4]`, and the SI static dir. It must replicate §6 items 1-5, and it must be validated against the
engine dump's CRC list before anyone relies on it.

### 7.5 Implications for the runtime override store (keying)

* Key = `(ShapeFile, logical shape, frame)` for the `Vga_file`s, `(font_source_id, font#, glyph)` for fonts, and
  `(pointers_source, frame)` for cursors. This matches `Shape_manager::cache_shape` keys (`shapeid.cc:552-576`).
* Imported shapes must be keyed by the logical number (2048+). The `realshape` belongs in provenance only.
* Packs for different configurations can share files by CRC. The loader resolves `key → CRC → image`.
* Overrides for reflected RLE frames are derived by transposing the base override (`shapes.md` §7.4). The extractor does not
  need to emit reflections.

---

## 8. Touchpoints

| Where | What | Change |
|---|---|---|
| `exult.cc:298-312, 986-989, 2857-2912` | CLI modes / `BuildGameMap` | add `--dump-art <dir>` modelled on buildmap |
| `shapeid.cc:150-382` | `Shape_manager::load` | canonical source of truth. The dumper walks the result. The hi-res store attaches here, per file, after imports and after `read_shape_info()` |
| `shapeid.cc:420-468` | `reload_shapes` | different source lists. Re-validate or clear hi-res, and never dump through it |
| `shapes/vgafile.h:298-412` | `Vga_file` | add `imported_shape_numbers()`, source-spec accessor, guard for `n >= shapes.size()` |
| `shapes/vgafile.cc:855-920, 1163-1268` | `Shape::read`, `U7load`, `import_shapes` | record the winning source index and keep the specs |
| `shapes/vgafile.h:47-163` | `Shape_frame` | `content_crc()` |
| `files/crc.{h,cc}` | CRC | buffer overload |
| `shapes/font.h:55`, `shapes/font.cc:775-795` | fonts | accessor for dumping, and a key that includes the font source |
| `shapes/miscinf.cc:151-180, 328-348, 420-484` | import tables, paperdoll sources | read-only use by the dumper (`GetImportedSkins`, `GetImportedGumpShapes`, `GetPaperdollSources`). Optionally fix `erased_for_patch` (changes mod behaviour, so keep it separate) |
| `gamemgr/modmgr.cc:1238-1243` | `<SERPENT_STATIC>` default `"."` | log the resolved paths in the dump header. Optionally make the default an invalid path |
| `tools/hires/u7art.py:247-321` | current extractor | add CRC and provenance, refuse patch/mod configurations, keep raw-index convention |
| `art_original/manifest.json` | output | per-configuration manifests with header and provenance |

---

## 9. Risks

* Overrides authored from `art_original` without a CRC can be silently wrong when a mod or patch changes a shape. The risk
  is low for flats today (no mod touches them) and real for RLE shapes, faces and gumps (Keyring and U6 replace 70+ shapes).
* Turning on SI support changes which art is reachable (skins 2048+, SI paperdolls, gumps 2048/2049). A pack built for the
  no-SI configuration then simply has no overrides for them. That is acceptable, but needs to be known.
* The `"."` fallback for `<SERPENT_STATIC>` can import from an unintended file depending on cwd.
* Font art depends on configuration. An override pack built for `fonts=original` must not apply to `serif` or `disabled`.
  Key fonts by source spec.
* PNG index rotation (ipack/ES `transp_to_0`) versus raw indices (u7art) can shift colours by one index if the two
  conventions are mixed.
* If a Python mirror of the engine's source rules is relied on without the engine CRC cross-check, it will diverge (§6).

## 10. Open questions

* Should the reference configuration for the 6x base pack be **BG+FoV with SI configured** (adds skins and SI paperdolls),
  given that SI is installed on this machine but missing from `exult.cfg`?
* Should the hi-res store accept per-configuration packs, or one CRC-addressed pool plus a per-configuration key map?
* Is a one-line fix of `Paperdoll_source_parser::erased_for_patch` in scope for the fork, given that it changes behaviour
  for mods with multi-line `paperdoll_source` patches?
* Should `--dump-art` also emit the effective palette set (0..12 with `<PATCH>`) and xform tables, so that offline quantizers
  use exactly what the engine uses?
