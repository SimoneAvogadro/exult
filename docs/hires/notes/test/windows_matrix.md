# Windows renderer matrix and measurements (WP-15)

Date: 2026-10-06. Spec: DESIGN.md §7.2 step 7, §9 (WP-15 row and decision rules, amended by §13: the
performance rule is measure-and-report only), §6.7 (WP-15). Implementation notes: `docs-hires/impl/WP-15.md`.

## Host and build

| Item | Value |
|---|---|
| PC | `SevenJars`: AMD Ryzen 7 9700X (8 cores, 16 threads), 64 GB, NVIDIA GeForce RTX 5070 Ti (driver 32.0.16.1714), Windows 10 Pro 22H2 (10.0.19045) |
| Toolchain | MSYS2 UCRT64 in `E:\Dati\Ultima7_Upscale\msys64`: g++ 16.2.0, SDL3 3.4.18, libpng 1.6.59 (pacman, 2026-10-06) |
| Source | `E:\Dati\Ultima7_Upscale\src\exult-hires`, branch `hires` at `11daacd16` plus the uncommitted WP-15 diff (`render_test.cc`, `hires_glue.cc`, `tests/README`) |
| Build | `make -f Makefile.mingw -j8 Exult.exe hires_unit.exe hires_present.exe` (default `-O2`), installed with `make -f Makefile.mingw install U7PATH=E:/Dati/Ultima7_Upscale/ExultHires` |
| Default renderer | SDL picks `direct3d11` when `SDL_RENDER_DRIVER` is unset (log line `world texture ... on renderer 'direct3d11'`) |
| Session state | **The workstation was locked** for all runs (`LogonUI.exe` in console session 1). Windows were created and presented normally, but the Windows clipboard was denied to every process (`OpenClipboard` error 5), and the timings were taken without an interactive desktop. Re-measure when the desktop is in use if the numbers matter at the margin. |

## Tests

| Test | Result |
|---|---|
| `hires_unit.exe` | 77 passed, 1 skipped (the user's own editor exports, `tests/data/hires/editor/real`, not yet provided); with `U7_BG_STATIC=E:/Games/RolePlayingGames/ultima7/static` the BG palette-0 CRC case passes too |
| `hires_present.exe` (offscreen, software) | 12 of 12 test cases, 276 assertions |
| `make -f Makefile.mingw check-hires` | passes (both programs) |

## Method

`Exult.exe --render-test` from WSL through interop (`tests/windows/win_rt.sh` in the repo; the rounds
below ran with its scratch predecessor `tmp/wp15/win_rt.sh`, same configuration and cases): each run gets a
scratch configuration and directories under `<EXULT_WIN_DIR>\rt\<case>`, here
`E:\Dati\Ultima7_Upscale\ExultHires\rt\<case>` (own patch, mods, saves, gamedat; audio off; gamma 1), `-p`
(portable home), and `SDL_RENDER_DRIVER` forwarded through `WSLENV`. vsync is 0 in every run, so present
times are CPU submission times, not display waits. `tests/windows/run_matrix.sh` (then
`tmp/wp15/run_matrix.sh`) ran all 50 cases twice (rounds r1 and r2), `tests/windows/summarize.py` makes the
table rows. Every case exited 0 in both rounds. The renderer and format of each row were read from the logs
(`world texture ... INDEX8 on renderer '<name>'`; `summarize.py` over r1 and r2: all 100 rows name the
requested renderer, and every `index8` and `art-index8` row an INDEX8 texture); since the review fix, `present=1` also records them in
`digest.json` (`s6_present_renderer`, `s6_present_format`, also in `mode=plain`), `win_rt.sh` prints the
renderer actually used, and a run fails when a single `SDL_RENDER_DRIVER` is not the renderer in use.

To repeat (from WSL, Exult installed with `make -f Makefile.mingw install U7PATH=...`):

```bash
export EXULT_WIN_DIR='E:\Dati\Ultima7_Upscale\ExultHires' U7_BG_STATIC_WIN='E:\Games\RolePlayingGames\ultima7\static'
tests/windows/run_matrix.sh tmp/matrix/r3 'E:\Dati\Ultima7_Upscale\packs\bg'
python3 tests/windows/summarize.py tmp/matrix/r3 | column -t -s $'\t'
```

Profiles (both at S=6):
* **320**: `tx=800,ty=1330,w=320,h=200`: world texture 1920x1200 in a 1920x1200 window (1:1, NEAREST). This is
  the windowed play profile of `exult-hires.cfg`.
* **860**: `tx=780,ty=1320,w=860,h=360,game=860x300,window=3440x1440`: the user's fullscreen profile (3440x1440,
  scale 4: a full area of 860x360 around the 860x300 game area), world texture 5160x2160, shown at r = 0.667
  (LINEAR, letterboxed). Run as a 3440x1440 window, not fullscreen.

Cases per renderer and profile:
* `index8-auto`, `argb-auto` (`auto` names the filter, chosen by the ladder): `present=1,format=…,bench=300,walk=300`: the read-back oracle (exact at 1:1,
  ±1 against the software-LINEAR reference otherwise, after the render and after three tracked partial
  uploads), `screen_to_game` grid, 300 static frames (paint, tracked upload, present) and 300 walk frames;
* `both-auto`: `format=both`: ARGB and INDEX8 in one process, read-backs must agree within 1;
* `index8-pixelart`: `filter=pixelart`, read-back oracle, `bench=100`;
* `art-index8`: the real pack `E:\Dati\Ultima7_Upscale\packs\bg` (3,885 loose x6 flats, route-3 hybrid),
  `mode=plain,overrides=yes,present=1,format=index8,bench=300,walk=300`: the real present path with art,
  timed, without the read-back oracle (the art is not NN of the reference). Also the store load time.

**walk=N** (new render-test key, WP-15) paints N frames, each one tile further east, then uploads (tracked)
and presents each. It is the proxy for decision rule 1's "walking with lerp on": every frame is a full
repaint of the view, as a lerped scroll frame is, and the flats caches of the chunks that come into view
render on the way (27 caches at 320x200, 53 at 860x300 over 300 tiles), with their overrides in the art
cases. A tile per frame enters new chunks several times as often as real walking does, so the p95 is
conservative. Game logic (NPCs, usecode, audio) is not in it: it measures the render cost that S adds.
### Correctness (both rounds)

| Renderer | Profile | ARGB 1:1 or LINEAR | INDEX8 | `format=both` ARGB vs INDEX8 | PIXELART (filter chosen, max error) |
|---|---|---|---|---|---|
| direct3d11 | 320 | pass, max error 0/0 (nearest) | pass, max error 0/0 | pass, max error 0/0, difference 0/0 | pixelart, pass, max error 0/0 |
| direct3d11 | 860 | pass, max error 0/0 (linear) | pass, max error 0/0 | pass, max error 0/0, difference 0/0 | pixelart, pass, max error 0/0 |
| direct3d12 | 320 | pass, max error 0/0 (nearest) | pass, max error 0/0 | pass, max error 0/0, difference 0/0 | pixelart, pass, max error 0/0 |
| direct3d12 | 860 | pass, max error 0/0 (linear) | pass, max error 0/0 | pass, max error 0/0, difference 0/0 | pixelart, pass, max error 0/0 |
| vulkan | 320 | pass, max error 0/0 (nearest) | pass, max error 0/0 | pass, max error 0/0, difference 0/0 | pixelart, pass, max error 0/0 |
| vulkan | 860 | pass, max error 0/0 (linear) | pass, max error 0/0 | pass, max error 0/0, difference 0/0 | pixelart, pass, max error 0/0 |
| opengl | 320 | pass, max error 0/0 (nearest) | pass, max error 0/0 | pass, max error 0/0, difference 0/0 | pixelart, pass, max error 0/0 |
| opengl | 860 | pass, max error 0/0 (linear) | pass, max error 0/0 | pass, max error 0/0, difference 0/0 | pixelart, pass, max error 0/0 |
| software | 320 | pass, max error 0/0 (nearest) | pass, max error 0/0 | pass, max error 0/0, difference 0/0 | linear, pass, max error 0/0 |
| software | 860 | pass, max error 0/0 (linear) | pass, max error 0/0 | pass, max error 0/0, difference 0/0 | linear, pass, max error 0/0 |

### NN frame cost, p95 in ms (round 1 / round 2)

| Renderer | Profile | Format | paint | upload | present | walk frame median | **walk frame p95** |
|---|---|---|---|---|---|---|---|
| direct3d11 | 320 | index8 | 0.272 / 0.221 | 0.204 / 0.205 | 0.131 / 0.092 | 0.407 / 0.413 | **0.597 / 0.684** |
| direct3d11 | 320 | argb | 0.261 / 0.249 | 0.542 / 0.528 | 0.109 / 0.127 | 0.725 / 0.712 | **0.980 / 0.986** |
| direct3d11 | 860 | index8 | 1.482 / 1.724 | 0.764 / 0.878 | 0.142 / 0.164 | 1.810 / 1.934 | **2.557 / 2.714** |
| direct3d11 | 860 | argb | 1.837 / 1.779 | 2.322 / 2.492 | 0.169 / 0.193 | 3.392 / 3.440 | **4.216 / 4.347** |
| direct3d12 | 320 | index8 | 0.299 / 0.260 | 1.165 / 1.114 | 0.730 / 0.543 | 1.717 / 1.508 | **2.124 / 2.007** |
| direct3d12 | 320 | argb | 0.322 / 0.329 | 2.812 / 3.826 | 0.277 / 0.264 | 2.849 / 3.530 | **3.453 / 4.329** |
| direct3d12 | 860 | index8 | 1.859 / 2.031 | 3.257 / 4.349 | 1.252 / 1.464 | 5.288 / 6.250 | **6.207 / 7.764** |
| direct3d12 | 860 | argb | 2.020 / 2.237 | 10.645 / 15.793 | 0.403 / 0.432 | 11.615 / 14.813 | **13.524 / 18.158** |
| vulkan | 320 | index8 | 0.314 / 0.341 | 8.384 / 9.927 | 0.440 / 0.473 | 1.490 / 1.734 | **1.984 / 2.299** |
| vulkan | 320 | argb | 0.444 / 0.393 | 11.408 / 8.004 | 0.277 / 0.243 | 3.840 / 5.764 | **6.188 / 6.686** |
| vulkan | 860 | index8 | 2.139 / 2.022 | 46.331 / 21.192 | 4.828 / 4.318 | 8.154 / 7.948 | **10.515 / 10.079** |
| vulkan | 860 | argb | 2.509 / 2.561 | 18.784 / 15.793 | 0.356 / 0.368 | 14.915 / 15.355 | **17.260 / 17.509** |
| opengl | 320 | index8 | 0.340 / 0.274 | 0.180 / 0.216 | 0.025 / 0.069 | 0.330 / 0.328 | **0.567 / 0.537** |
| opengl | 320 | argb | 0.389 / 0.397 | 2.007 / 2.108 | 0.045 / 0.050 | 2.052 / 2.153 | **2.438 / 2.572** |
| opengl | 860 | index8 | 1.528 / 1.492 | 0.718 / 0.696 | 0.118 / 0.123 | 1.727 / 1.823 | **2.275 / 2.550** |
| opengl | 860 | argb | 2.623 / 2.487 | 10.402 / 10.166 | 0.096 / 0.101 | 11.426 / 11.114 | **12.700 / 13.721** |
| software | 320 | index8 | 0.298 / 0.339 | 0.048 / 0.076 | 1.391 / 1.446 | 1.281 / 1.305 | **1.719 / 1.820** |
| software | 320 | argb | 0.468 / 0.418 | 0.575 / 0.566 | 1.583 / 4.867 | 1.615 / 1.709 | **2.138 / 2.518** |
| software | 860 | index8 | 2.374 / 2.625 | 0.656 / 0.682 | 15.440 / 16.363 | 17.394 / 17.288 | **19.253 / 19.442** |
| software | 860 | argb | 2.571 / 2.962 | 3.086 / 3.333 | 12.317 / 14.613 | 16.417 / 16.927 | **17.889 / 19.340** |

### Frame cost with the real art (`packs\bg`, loose, INDEX8), ms (round 1 / round 2)

| Renderer | Profile | walk frame median | **walk frame p95** | walk caches rendered | cold paint p95 | render_flats p95 per cache | store load |
|---|---|---|---|---|---|---|---|
| direct3d11 | 320 | 0.414 / 0.415 | **0.653 / 0.652** | 27 | 0.441 / 0.526 | 0.043 / 0.042 | 626.2 / 653.6 |
| direct3d11 | 860 | 1.826 / 1.833 | **2.561 / 2.779** | 53 | 3.116 / 3.175 | 0.041 / 0.055 | 631.8 / 671.6 |
| direct3d12 | 320 | 1.870 / 1.626 | **2.337 / 2.226** | 27 | 0.432 / 0.476 | 0.040 / 0.053 | 639.8 / 683.1 |
| direct3d12 | 860 | 6.241 / 6.487 | **7.489 / 7.676** | 53 | 3.442 / 3.474 | 0.059 / 0.059 | 625.7 / 675.1 |
| vulkan | 320 | 1.650 / 1.565 | **2.763 / 3.105** | 27 | 0.457 / 0.500 | 0.038 / 0.052 | 627.0 / 672.1 |
| vulkan | 860 | 8.159 / 8.014 | **10.164 / 10.223** | 53 | 3.954 / 3.413 | 0.061 / 0.060 | 633.5 / 663.6 |
| opengl | 320 | 0.350 / 0.326 | **0.580 / 0.552** | 27 | 0.593 / 0.510 | 0.040 / 0.042 | 638.9 / 664.6 |
| opengl | 860 | 1.737 / 1.866 | **2.410 / 2.584** | 53 | 2.921 / 3.323 | 0.042 / 0.045 | 638.3 / 672.8 |
| software | 320 | 1.283 / 1.289 | **1.687 / 1.728** | 27 | 0.455 / 0.569 | 0.040 / 0.047 | 1519.1 / 690.5 |
| software | 860 | 16.579 / 17.106 | **18.113 / 20.658** | 53 | 3.454 / 3.709 | 0.061 / 0.053 | 626.2 / 666.9 |

The Vulkan "upload" p95 of the static bench (up to 46-50 ms at 860) comes from a few slow frames at the start
of the run: its median is 2.6 ms, and the walk, which runs after it, has an upload p95 of 3.3 ms. WP-02 saw the
same spread (`vulkan` INDEX8 full upload 0.88-4.81 ms).

### Bundle pack, 860 profile (`E:\Dati\Ultima7_Upscale\bench\bg-bundle`, `x6\flats.bundle` built from `packs\bg`, same 3,885 tiles), two runs

| Renderer | store load | first S=6 render (incl. load) | walk frame median | **walk frame p95** | cold paint p95 |
|---|---|---|---|---|---|
| direct3d11 | 28.1 / 27.2 ms | 46.1 / 46.4 ms | 1.868 / 1.833 | **2.659 / 2.699** | 2.961 / 3.196 |
| direct3d12 | 29.4 / 29.1 ms | 48.6 / 47.3 ms | 6.846 / 6.569 | **8.865 / 8.393** | 3.442 / 3.447 |

## Store load time (decision rule 2)

`[hires] x6: … ms` (Store load inside the first S>1 paint: scan, decode, F1-F3, P0, P4, G1 against the live
1x flats). `tmp/wp15/make_bundle.py` wrote the bundle copy (same tiles and guards; the canonical packs are
unchanged). It is not in the repo: it is a ten-line call of `u7hires.pack.write_bundle` (branch `hires-art`,
not merged) over `<pack>/x6/flats/**/SSSS_FF.png`, and belongs with the art tools when they merge.

| Side | Pack location | Game data (`static`) | Loose (3,885 PNG) | Bundle |
|---|---|---|---|---|
| Windows (native) | `E:` | `E:` | **618-691 ms** warm (23 runs); **1,519 ms** on the first read after install | **27-29 ms** (7 runs) |
| WSL (build-o2) | ext4 | `/mnt/e` (drvfs) | 1,553-1,592 ms | 1,390-1,414 ms |
| WSL (build-o2) | ext4 | ext4 copy (`tmp/wp15/static-ext4`) | 122 ms | 25.5 ms |

The WSL figure is not the bundle's: with `static` on drvfs, fetching the 3,885 source flats from
`shapes.vga` for G1/P4 costs about 1.4 s (`time_ref_ms`, the 1x reference paint, is 620-650 ms from drvfs
and 8 ms from ext4, for the same reason). Natively on Windows the same work takes about 25 ms; there the
loose-file cost is the 3,885 opens and PNG decodes.

## Decision rules (§9, amended by §13: measure and report)

1. **Performance (measure and report only).** 860 profile, S=6, -O2, Windows, best of D3D11 and D3D12, p95
   frame time while scrolling (walk proxy, real art): **D3D11 2.56-2.78 ms** with loose files and
   **2.66-2.70 ms** with the bundle; D3D12 7.49-7.68 ms (8.39-8.87 with the bundle). Best of the two:
   **2.7 ms ≤ 6.7 ms**. S=6 holds on the user's fullscreen profile with headroom; nothing to discuss with the
   user. `max_world_mpx` stays 40 (§13; the 860 profile needs 11.1 Mpx). D3D11 is also SDL's default here.
   D3D12 and Vulkan cost 3-4x more per frame at 5160x2160 (texture upload), OpenGL is as fast as D3D11.
2. **Load time.**
   * *Bundle above 1 s anywhere?* Windows: no (27-29 ms). WSL: 1.39-1.41 s, but only with the game's
     `static` directory on drvfs, and the time is the source-flat fetch, not the bundle (25.5 ms with an ext4
     `static`). Read literally, the rule triggers ("preload at the end of `Shape_manager::load`"). That would
     move the same 1.4 s from the first S>1 frame to startup in a WSL development setup and gain nothing on
     Windows. **Recommendation:** do not preload; for WSL engines, keep a copy of `static` on ext4 (as the
     packs already are) or accept the one-time hitch. Not implemented (measure and report).
   * *Windows loose load above 1 s?* Warm 0.62-0.69 s: no. First read after install 1.52 s: yes. The rule's
     consequence ("`publish.sh` stops publishing loose copies of bundled tiles") belongs to the art track
     (`publish.sh` is on `hires-art`, not merged). **Recommendation:** publish `packs/bg` with its
     `flats.bundle` to `E:` (load 27 ms) and no loose copies of bundled tiles; today `E:\…\packs\bg` holds loose
     files only. Not done here.
3. **Format.** INDEX8 read-back is exact (max error 0) on `direct3d11`, `direct3d12`, `vulkan`, `opengl` and
   `software`, at 1:1 and through the LINEAR resolve at r = 0.667, and ARGB == INDEX8 (difference 0)
   everywhere. `present_format=auto` therefore uses INDEX8 on every Windows renderer, and it does as built:
   a run without `format=` (configuration `auto`) reports `index8` with an exact read-back on all five
   renderers. No per-renderer ARGB fallback is needed. INDEX8 is also the cheaper format on
   every renderer (for example D3D11 at 860: 2.6-2.7 ms p95 against 4.2-4.3 ms for ARGB).

## PIXELART

`filter=pixelart` selects PIXELART on the four shader renderers and LINEAR on `software`
(`renderer_pixelart_ok`, §3.2.4). Its read-back is exact at 1:1 and, at r = 0.667, equal to the software-LINEAR
reference (max error 0): when minifying, SDL's PIXELART sampling gives the bilinear result. The visual check of
PIXELART for magnification (r > 1, e.g. a window larger than the world texture) needs an interactive session.

## Not covered here (need the user at the desktop)

* Interactive play on Windows (walk with lerp on and off, gumps, combat, save/load), and the §6.5 checklist.
* Alt-tab and a display-mode change while a conversation or modal gump is open, on D3D11 and D3D12 (render
  reset outside the main loop). `hires_present` covers the reset events with the software renderer only.
* The clipboard part of the dev keys (`keys=1`: Ctrl-Alt-I puts the inspector text on the clipboard): the locked
  workstation denies the clipboard to every process (an SDL probe and PowerShell's `Set-Clipboard` fail the same
  way). The other `keys=1` assertions passed up to that point.
* The user's own Aseprite and GIMP exports for `tests/data/hires/editor/real/` (§6.2).

## Developer loop on Windows (WP-10's Windows side)

With the scratch packs of `dev_loop.sh` mirrored to `E:\Dati\Ultima7_Upscale\bench\{dev,inspect}`:
`dev=1` (pushed, S = 2 and 6; window, S = 6) passed 2 + 5 runs: the engine reading `E:` natively sees a
`.reload` touch and reloads within 390-405 ms (swap) and 0-43 ms (swap back). One earlier window run (keys on)
missed the swap-back reload; it did not recur in the next 10 window runs and is recorded, not acted on. The
inspector golden (`golden/inspect.json`) matches on Windows after the WP-15 fix of `root_relative` (it
printed absolute `E:/…` paths before). The publish.sh round trip from WSL (§6.7 WP-10) waits for `hires-art`.
