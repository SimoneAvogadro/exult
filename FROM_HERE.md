# FROM_HERE: handover for the next agent

Status date: 2026-10-07. Owner: Simone Avogadro (GitHub `SimoneAvogadro`). The owner's language is Italian:
answer in Italian, write code, commits and docs in English.

## 1. What this fork is

A fork of Exult, the Ultima VII engine, with two goals:

1. **High-resolution world rendering.** The game world renders internally at an integer **render scale S**.
   The target is S=6, that is 6x the *original* game resolution: a 320x200 game view becomes 1920x1200.
   - Game logic, hit testing and mouse picking stay in original game pixels.
   - If the window is smaller than the render, the image is downscaled at present time.
   - Larger "extended" views also run at S=6; the user accepts that. On the user's fullscreen profile
     (3440x1440, game view 860x300) the internal size is 5160x1800.
2. **Hi-res art overrides.** Mods can replace shape frames selectively with 6x art: one tile, a group, or a
   whole 16x16-tile terrain chunk.
   - The art is palette-indexed against palette 0, so day/night, colour cycling and translucency keep
     working.
   - Terrain flats (shapes 0..149 of `shapes.vga`, 8x8 px) come first. RLE sprites are milestone M2; UI,
     gumps, fonts and faces are M3.

The authoritative spec is **`docs/hires/DESIGN.md`**:
- §2: invariants;
- §5: override file format and rules;
- §9: work packages (WP);
- §12: adversarial review log;
- §13: user decisions.

Background and per-WP notes are in **`docs/hires/notes/`**, copied from the owner's machine:
- `analysis/`: code analysis of upstream master; start with `analysis/00_architecture_map.md`;
- `design/`: the three original proposals and DESIGN rev 2;
- `impl/WP-*.md`: per-WP implementation notes with exact commands and deviations; read the WP you touch;
- `upscale-research/`: AI and algorithmic upscaler research; `00_recommendation.md` is the summary;
- `art/phase_a_report.md` and `art/phase_b_pilot.md`: art results;
- `test/windows_matrix.md`: Windows performance measurements;
- `workflows/*.js`: the orchestration scripts used so far, for reference only. Their paths are the owner's.

## 2. Branches (remote `fork` = github.com/SimoneAvogadro/exult)

| Branch | Base | Content |
|---|---|---|
| `hires` | upstream `8b6ab6b43` | **Main feature branch.** The engine work, tests and docs. This file lives here. |
| `hires-art` | upstream `8b6ab6b43` | Python art tooling `tools/hires/u7hires/` (pipeline, QA, validator, packer, routes 1/2/3, voting). **Merged into `hires` on 2026-10-07** (no conflicts); work on `hires` from now on. pytest: build xBRZ with `tools/hires/third_party/build_xbrz.sh <dir>` and set `U7_XBRZ_LIB=<dir>/libxbrz19.so`, otherwise the xBRZ tests skip. |
| `upstream-fixes` | upstream `8b6ab6b43` | Upstreamable bug fixes that change 1x output (P1, P2, P5 plus tests). They are deliberately kept **out** of `hires`, so `hires` stays byte-identical to upstream at S=1. The owner decides whether to submit them to exult/exult. |

`master` on the fork is upstream plus one owner commit. It is not used by this work.

## 3. Done (all on `hires` unless stated)

| WP | Content |
|---|---|
| WP-00 | Build matrix: -O2, ASan/UBSan, SDL 3.2.14 lane, upstream reference build; buildmap goldens |
| WP-01 | Test infrastructure: vendored doctest, `make check` (`tests/unit`, `tests/present`), `tests/game` (game-data oracles), `tools/hires/ci.sh`, pytest smoke |
| WP-02 | Windows GPU probe: INDEX8 textures are pixel-exact on D3D11, D3D12, Vulkan and OpenGL (RTX 5070 Ti). Notes only |
| WP-03 | Prerequisites: P3 zero-fill, `find_flat_source`/`paint_flats`, minimap 1x buffer, flats cache size, main-buffer ownership, pngio leak, opaque palette, `create_buffer_1x` |
| WP-04 | Scaled `Image_buffer8` (`pixel_scale`, logical API with physical storage, `ibuf8_scaled.cc`, write tracker) with a fuzz oracle against nearest-neighbour |
| WP-05 | Scale policy (`world_scale.h`) and `World_presenter`. Includes INDEX8 and tracked uploads, so **WP-14 is effectively done** |
| WP-06 | World integration: flats cache per scale, `blit`, resize toast |
| WP-07 | `--render-test` headless region renderer and oracles; `tests/game` scripts; perf baseline |
| WP-08 | Override store (`shapes/hires_png`, `hires_rules`, `hires_store`, bundle reader), `hires_glue`, the `<HIRES>` path tag |
| WP-09 | Per-tile hi-res flats in `Chunk_terrain::paint_flats`; identity and marker oracles (`tools/hires/mkpack_identity.py`) |
| WP-11 | `--dump-art` reference set (flats, templates, terrain T1 keys, maps) |
| WP-10 | Developer loop: Ctrl-Alt-O toggle, Ctrl-Alt-R reload, Ctrl-Alt-I inspect, `.reload` trigger file |
| WP-15 | Windows build with MSYS2 UCRT64 on the owner's PC, plus measurements. Code and review fixes complete (commit "WIP WP-15"); `make check` re-run in the cloud on 2026-10-07. Open: WP-15b, the interactive Windows checks of `notes/impl/WP-15.md` §6 |
| §13.1 | **User decision 2026-10-07: always 6x.** `render_scale` defaults to `art` (Android/iOS keep `off`); precedence terrain → tile → NN, NN without smoothing |
| WP-17 | Per-terrain overrides in the engine (`Chunk_terrain::get_t1_key`, `paint_hires_terrain`), inspector, `--render-test marked=terrain`, `mkpack_identity.py --terrain`, unit and pytest cases. Verified end to end on the synthetic world (`make check-world`); the BG cases of `override_regions.sh` are not run yet; see `notes/impl/WP-17.md` |
| BG in the cloud | With the private repos of §4b, a cloud session ran `make check-game`: all pass after the `terrain-reduce-britain` fix (notes/impl/WP-17.md §2c). pytest with `U7_BG_STATIC`, `U7_XBRZ_LIB` and `U7_ART_ORIGINAL=<hires-assets>/art_original`: all pass but the numpy version pin of that venv |
| mkterrain | `tools/hires/mkterrain.py`: whole-terrain overrides from route-1 windows; terrains 1825/1826 of the pilot verified in the engine |
| Test world | `tests/data/hires/world` (generator `make_world.py`) and `tests/world/world_tests.sh` (`make check-world`, also a `ci.sh` step): 25 cases, data-free. `dev_loop.sh`'s `golden/inspect.json` was updated by hand for WP-17 (its fake terrain file is now rejected, F1, when painted), as the world test confirmed; re-run `dev_loop.sh` on BG to confirm |

On the owner's PC, outside the repo:
- the Windows build is installed in `E:\Dati\Ultima7_Upscale\ExultHires`;
- the launcher is `Play-ExultHires.bat`; the config is `exult-hires.cfg` (`render_scale=art`, `dev=yes`, pack `packs\bg`);
- measured on the 860x300 view at S=6 with real art: p95 frame time **2.7 ms on D3D11**.

**Art (local only, never committed: the art is derived from EA data).** All 3,885 Black Gate flats exist at 6x.
- Packs:
  - `packs/bg`: the default, route-3 hybrid xBRZ; passes every QA gate;
  - `bg-r3`: plain xBRZ;
  - `bg-r2`: NXbrz model.
- Where they are: `/home/simonea/ultima7_exult/packs` on the WSL machine, mirrored to `E:\Dati\Ultima7_Upscale\packs`.
- Limitation: the algorithmic routes add no detail inside grass, dirt and sand.
- Diffusion pilot (SDXL + xinsir Tile ControlNet, `tools/hires/u7hires/route1.py`):
  - it adds believable detail, but only on **whole terrains**, not as per-tile flats, which seam;
  - so the production path is per-terrain overrides (WP-17) plus border blending.

## 4. What is left, in order

1. ~~Run the WP-17 game-data oracles~~: done in the cloud with the asset repos (all pass). Still worth one run on
   the owner's WSL lanes (ASan, SDL 3.2) and an interactive BG session (headless, BG stops at character creation).
2. **WP-15b** on the owner's PC: the interactive Windows items of `notes/impl/WP-15.md` §6.
3. **WP-16, performance pass** (DESIGN §9): `memset` runs in the scaled RLE painter, row-batched translucency, a `fast_paths` test. The current numbers already pass, so this is optional polish. With per-terrain art, consider a decoded-terrain cache in the store.
4. **Wrap-up docs:** the user guide `docs/hires.md` and `docs/hires_modding.md`; then rerun the full test matrix.
5. **Art phase B2** (owner's machine with GPU): generate 5-10 per-terrain overrides with route 1, blend the borders, A/B them in the engine; then a production run of about 600-1,100 terrains (3-6 GPU hours).
6. **M2, sprites** (DESIGN §3.6) and **M3, UI** (§3.7).

## 4b. Private asset repositories (game data and hi-res art)

Two **private** repositories hold what this public fork must never contain. Never copy their content into this
repo, into commit messages, or into anything public.

| Repo | Content |
|---|---|
| `git@github.com:SimoneAvogadro/u7assets.git` | Original Ultima VII files: `blackgate/` (BG + FoV, `static/` is what Exult reads) and `serpentisle/` |
| `git@github.com:SimoneAvogadro/ultima7-high-res-tiles.git` | 6x packs (`packs/bg` is the active one), `art_original/`, `art_ref/` (`--dump-art`), `work/` (QA, diffusion pilot, voted candidates, context windows) |

Cloud or new-machine setup, next to the `exult` checkout:

```bash
git clone git@github.com:SimoneAvogadro/u7assets.git
git clone git@github.com:SimoneAvogadro/ultima7-high-res-tiles.git hires-assets
export U7_BG_STATIC=$PWD/u7assets/blackgate/static            # enables make check-game
# exult.cfg: blackgate path = …/u7assets/blackgate ; hires_path = …/hires-assets/packs/bg ;
#            savegame/gamedat/patch/mods = scratch dirs OUTSIDE both repos (Exult writes there)
```

With these, the game-data oracles and the art tooling (`tools/hires/u7hires`, merged from `hires-art`) also work in
the cloud. GPU routes (route 1 diffusion, route 2 NXbrz) need a CUDA GPU. Model weights are not stored: route 1
downloads SDXL base, xinsir/controlnet-tile-sdxl-1.0 and madebyollin/sdxl-vae-fp16-fix from HuggingFace, and
the SR models are listed with SHA-256 in `tools/hires/u7hires`.

**Keep them in sync:** commit new packs or reports to `ultima7-high-res-tiles`, and pull before working.
On the owner's WSL machine the clones are `/home/simonea/ultima7_exult/u7assets` and
`/home/simonea/ultima7_exult/hires-assets`. The old paths `packs/<name>`, `art_original`, `art_ref` and
`art_work/<dir>` are symlinks into `hires-assets`. The Windows build reads `E:\Dati\Ultima7_Upscale\packs`, a
mirror made with `publish.sh`.

## 5. How to build and test (generic Linux, e.g. a cloud VM)

- **Dependencies:** a C++17 compiler, autotools, `autoconf-archive`, pkg-config, SDL3 ≥ 3.2 (3.4 enables INDEX8 and PIXELART), libpng, zlib, ogg and vorbis.
  - Ubuntu 24.04 has no SDL3 package: build SDL 3.4 from source (`cmake -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF` is enough for `make check`, which uses the offscreen driver).
  - The owner's WSL had no sudo, so everything was built in user space. That is not needed on a normal VM.

```bash
autoreconf -v -i
mkdir ../build && cd ../build
../exult/configure --disable-exult-studio --disable-gimp-plugin --disable-aseprite-plugin \
    --disable-shp-thumbnailer --with-optimization=normal --with-debug=symbols
make -j"$(nproc)" && make check      # data-free unit + presenter tests (SDL offscreen/software)
```

- **Synthetic-world tests:** `make check-world` (about 40 s) runs the render oracles, `--dump-art`, WP-17, the sample pack, the dev loop and the inspector on `tests/data/hires/world`, a tiny DEVEL game with original procedural art and a 6x sample pack (committed; no EA data). **They run anywhere.**
- **Game-data tests:** `make check-game` with `U7_BG_STATIC=<Black Gate static dir>`. The original Ultima VII files are copyrighted and **not in this repo**: clone the private `u7assets` repo (§4b), which makes the BG checks (buildmap goldens vs upstream, BG regions, perf) possible in the cloud too. Without them the scripts exit 77 (skip).
  - The Windows build and its measurements still need the owner's PC (MSYS2 on `E:`); GPU art routes need a CUDA GPU.
- **ASan:** build with `--with-optimization=light` and `CXXFLAGS="-fsanitize=address,undefined -fno-sanitize=null,alignment,vptr"`.
  - With g++ 9.4, the three excluded UBSan checks make `exult.cc` compile forever.
  - Under WSL2, run ASan processes as `setarch x86_64 -R` (ASLR hang).
- **Build lists:** register every new source file in `Makefile.am`, `Makefile.common` and `msvcstuff/vs2019/*.vcxproj(+filters)`. `tools/hires/check_build_lists.py --strict` lints this.

## 6. Rules and gotchas

- **S=1 must stay byte-identical to upstream (+P3).** S>1 code goes in new files or functions, with one-line hooks in existing ones. Never change 1x output on `hires`; put that on `upstream-fixes`.
- **Never commit EA-derived pixels.** That covers packs, renders and dumps. Commit hash lists only.
- **Paint order depends on heap address order.** `Game_object::dependencies` is a `std::set` of pointers (`objs/objs.h:93`). Builds with a different allocator, such as ASan, produce slightly different buildmaps, so each lane has its own reference.
- **The owner's PC had unstable RAM:** bit flips under load and a 0x1A bugcheck. The overclock is now disabled, but stability is not yet proven.
  - On that machine keep loads moderate.
  - Re-run non-reproducible failures before debugging them.
  - Produce art with the redundancy/vote tools (`tools/hires/README.md`).
- **Commits:** messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Push to the `fork` remote only, never to exult/exult.
