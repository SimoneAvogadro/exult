# M1a: merge of `hires-store` (WP-08) into `hires`

Date: 2026-10-06. Integrator step. Status: **merged**, all lanes green, worktree removed.

## 1. Starting state

The previous attempt of this step had been interrupted (quota) before it changed anything: `hires` was clean at
`e67515a36`, no merge in progress, `hires-store` at `2de194198` with a clean worktree. The merge base was
`2488d8ab5` (WP-00 + WP-01); `hires-store` added 3 commits, `hires` 20 (WP-02 .. WP-07).

## 2. Merge

Merge commit `138e0bd5f` ("Merge branch 'hires-store' into hires", `--no-ff`, with the trailer lines).

Conflicts, all resolved by keeping both sides' entries:

| File | Resolution |
|---|---|
| `Makefile.common` | `HIRES_UNIT_TEST_OBJS`: `test_flat_source.o` (hires) and `test_editor_fixtures.o`, `test_hires_{png,rules,store}.o` (store), in name order. `HIRES_UNIT_OBJS` (both sides' objects) merged automatically |
| `tests/Makefile.am` | `hires_unit_SOURCES`: the same five test files in name order |
| `tests/README` | hires' `test_world_scale.cc` entry followed by the store's `hires_test_util` / `test_hires_*` / `test_editor_fixtures` entries |
| `tools/hires/build_lists_companions.txt` | both pairs kept: `imagewin/imagewin.cc imagewin/world_present.cc` and `shapeid.cc hires_glue.cc` (with its comment) |

Merged automatically, checked: `Makefile.am` (`hires_glue.{cc,h}`), `shapes/Makefile.am`, `gamemgr/modmgr.cc`,
`shapeid.cc`, `msvcstuff/vs2019/Exult.vcxproj` + `.filters` (both well-formed XML; they list the WP-04/05/06 files
and the WP-08 files), `ios/Exult.xcodeproj/project.pbxproj`. `configure.ac` and `exult.cc` had no conflict.

No integration fix was needed: no further commit after the merge.

## 3. Verification (one lane at a time, `make -j6`)

| Step | Result |
|---|---|
| `autoreconf -v -i` | ok |
| `check_build_lists.py --strict` | 0 not allowed, 0 missing companions (3 pairs, all active) |
| `pytest tools/hires/tests` (tools-venv) | 17 passed |
| build-o2: make, `make check` | ok; hires_unit 77 cases / 159627 assertions passed (1 skipped: the golden recorder, `doctest::skip`), hires_present passed |
| build-o2: hires_unit with `U7_BG_STATIC` | 159634 assertions passed (the BG palette CRC case runs) |
| build-o2: `make check-game` (`U7_BG_STATIC=/mnt/e/.../static`) | smoke, buildmap_golden, render_regions, resize_cycle, perf: all PASS (2m33s) |
| build-asan: make and `make -j1 check` under `setarch x86_64 -R` (ASAN_RUN of ci.sh) | ok; TEST_WRAPPER has setarch + timeout; hires_unit 77 cases, hires_present passed |
| build-sdl32 (SDL 3.2.14 from deps/prefix-3.2): make, `make check` | ok; hires_unit 77 cases, hires_present 148 assertions passed |

No failure occurred, so nothing had to be re-run as a suspected hardware fault.

Logs: `/home/simonea/ultima7_exult/tmp/m1a-*.log`.

## 4. Clean-up

`hires-store` is fully merged (`git branch --merged hires`). The worktree
`/home/simonea/ultima7_exult/exult-hires-store` held only ignored generated files and was removed with
`git worktree remove` (no `--force`). The branch `hires-store` itself is kept (not deleted). The out-of-tree build
trees `build-store` and `build-store-asan` still exist; their source tree is gone, so they can be deleted.

## 5. `git log --oneline 8b6ab6b43..hires`

```
138e0bd5f Merge branch 'hires-store' into hires
e67515a36 tests: game-data oracles, buildmap goldens and perf baseline
bc22490bc Render test: --render-test region renders and oracles
a4040c213 Game_window, Image_window: hooks for the render test
9af6fd935 Game_window: show the world scale in the resize toast
0b50d4249 Game_render: paint the flats cache at the render target's scale
329e117ba Chunk_terrain: cache the rendered flats per pixel scale
bd6437ebc Image_window: render the world at S > 1 and present it
cc5789dd5 imagewin: world scale policy and World_presenter
24f798313 tests: check the scaled Image_buffer8 against the scale-1 reference
ee3260538 Image_buffer: read scaled buffers through the primitives
34ed75ab9 Image_buffer: pixel scale, write tracker and scaled Image_buffer8 storage
2de194198 Hires: engine glue for the override store and the <HIRES> path
63f9fd655 Shapes: hi-res override store, with unit tests and fixtures
30829faf2 tools/hires: build-list lint ignores an object's own dependency rule
adce34854 Image_window: add create_buffer_1x() for code that uses the bits
34f50ad1c Image_window8: make the palette opaque
4b5feb577 pngio: free the buffers when libpng fails half-way through a file
845283a59 Image_window: write the main buffer's fields through main_ibuf
002d47182 Chunk_terrain: size the flats cache from the game area
cbe6d9692 Game_map: paint the minimap's terrains into a local buffer
c43b0df65 Chunk_terrain: find_flat_source() and paint_flats()
b1f065bc2 Chunk_terrain: clear the flats cache before painting it
2488d8ab5 tools/hires: pinned requirements, pytest smoke test, build-list lint, ci.sh
7155df413 Tests: doctest unit tests, a scale-1 golden for Image_buffer8, game-data skeleton
2ba5cac93 Hires: design snapshot and u7art.py
c243d2f6e .gitignore: ignore generated shortcutbar files and root expack/ipack
```
