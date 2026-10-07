# Build system, CI, tests and tools: analysis for the hi-res (S=6) fork

Scope: build and test infrastructure of `exult-hires` (upstream master `8b6ab6b43`, "Fix snapshots-android.yml", 2026-09-15), the CI matrix, the environment constraints (WSL2 Ubuntu 20.04, g++ 9.4, no sudo) and the tools that can extract and re-import U7 art. All `file:line` references are relative to `/home/simonea/ultima7_exult/exult-hires` unless they say otherwise.

Everything in the "verified" boxes was actually run during this analysis. It was done in a throw-away clone in the session scratchpad (`/tmp/claude-1000/.../scratchpad/exult-src`), and the repo itself was not modified.

---

## 0. Key findings

1. **Master builds with g++ 9.4 and SDL3 3.2.14 built from source, in user space, without autotools or pkg-config.** A small wrapper makefile that includes the upstream `Makefile.common` (Appendix A) built the full `exult` binary in about 57 s with `-j16`. It produced no warnings at `-Wall -Wextra`. `exult --version` ran.
2. **Exult can render headless and deterministically.** `exult --bg --buildmap 2` ran with `SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy` and with `DISPLAY`/`WAYLAND_DISPLAY` unset. It wrote 144 indexed 8-bit 2048x2048 PNGs in 19.5 s, and two runs gave **byte-identical** files. This already works as an integration and golden-image harness (exult.cc:2857-2912).
3. **There is effectively no test suite.** The only test-like programs are `files/rwregress.cc` (endian I/O asserts) and `conf/xmain.cc` (`confregress`). `audio/mtest` is a stale one-line SDL1 compile script that points at a file that does not exist. `mapedit/audiotest.cc` is a GTK dialog, not a test. No workflow runs `make check`. The only runtime "test" is the mingw `disttest` target, which runs `Exult.exe --version` (Makefile.mingw:824-827).
4. **The best existing extractor is `ipack -x` with an `all:` script.** It extracted all 14,171 frames of BG `shapes.vga` as indexed PNGs in about 10 s. Its PNG convention has traps that a hi-res pipeline must handle (§7.2):
   - The palette is **rotated by +1**, so that transparency lands on index 0.
   - The hotspot is stored in the `oFFs` chunk.
   - 6-bit palette values are multiplied by 4 with uint8 wrap.
5. **There are four build descriptions to keep in sync**: autotools `*/Makefile.am`, `Makefile.common` (used by `Makefile.mingw`), `msvcstuff/vs2019/*.vcxproj(.filters)` and `ios/Exult.xcodeproj`. `Makefile.common` currently **does not put `shapes/pngio.o` into the engine** (Makefile.common:231-258), although autotools (shapes/Makefile.am:15-19) and MSVC (Exult.vcxproj:546) do. A PNG-based override loader has to fix that.
6. **PNG decoding is not available on every platform.** Android forces `have_png=no` (configure.ac:491-495) and iOS has `HAVE_PNG_H` commented out (ios/include/config.h:215). Any override loader built on `pngio` must compile out cleanly, or the fork must vendor a decoder.
7. **Windows**: the installed Exult 1.12.1 (`/mnt/e/Games/RolePlayingGames/ExultUltimaVII`) is an **SDL2** build (`Exult.exe` imports `SDL2.dll`, which is SDL 2.32.10) linked against msvcrt and MINGW64 DLLs. `SDL3.dll` (3.4.2) sits next to it but is not used by `Exult.exe`. Master needs SDL3. The fork must be built with MSYS2 (UCRT64 is what upstream CI uses) and installed into its **own** folder, never on top of 1.12.1.

---

## 1. Build systems in the tree

### 1.1 Autotools (canonical on Linux, macOS and FreeBSD; also used by Android)

- `configure.ac` requires autoconf ≥ 2.69 (configure.ac:2), C++17 through `AX_CXX_COMPILE_STDCXX([17], [noext], [mandatory])` (configure.ac:153) and libtool (`LT_INIT`, configure.ac:170).
- `m4/` bundles only `gb_enable_lto.m4`. The `AX_*` macros (`AX_CXX_COMPILE_STDCXX`, `AX_CHECK_COMPILE_FLAG`, `AX_COMPARE_VERSION`) need **autoconf-archive**, and the `PKG_*` macros need `pkg.m4` from pkg-config.
- pkg-config is mandatory (configure.ac:468-473).
- The top-level `Makefile.am` builds `exult` from `EXULTSOURCES` (Makefile.am:31-127) plus the libtool convenience libraries listed in `EXULTLIBS` (Makefile.am:129-147). Link libraries come from `EXULTLIBADD` (Makefile.am:149-152): `PNG_LIBS SDL_LIBS ZLIB_LIBS OGG_LIBS MT32EMU_LIBS FLUID_LIBS ALSA_LIBS`.
- `SUBDIRS` is at Makefile.am:11-13. A new `tests` directory goes there, and into `AC_CONFIG_FILES` (configure.ac:1351-1399).
- `gitinfo.h` is generated from `.git` (Makefile.am:331-357). Builds from a tarball without `.git` simply skip it.
- Each subdirectory is a `noinst_LTLIBRARIES` convenience library. Some parts are SDL-free and some are not:
  - `imagewin/Makefile.am`: `libimagewin` = `ibuf8/imagebuf/iwin8`, plus the SDL-dependent `imagewin.cc`, `save_screenshot.cc` and all scalers only when `BUILD_EXULT` (imagewin/Makefile.am:7-52).
  - `shapes/Makefile.am`: `libshapes` always has `pngio.cc` and `vgafile.cc`; everything else only when `BUILD_SHAPES` (shapes/Makefile.am:15-47).
  - `files/Makefile.am`: `libu7file` plus `noinst_PROGRAMS = rwregress` (files/Makefile.am:16-30).
- The warning flags (configure.ac:1263-1306) are filtered through `AX_CHECK_COMPILE_FLAG`, so older compilers silently drop flags they don't know. `--enable-warning-errors` adds `-Werror`.

**configure switches relevant to a minimal build**

| Component | Status | Switch / location |
|---|---|---|
| SDL3 (`sdl3.pc`) | **mandatory**, no version floor in configure | configure.ac:479-485 |
| Ogg/Vorbis/Vorbisfile | **mandatory** when building exult (`audio/OggAudioSample.h:37` includes `<vorbis/vorbisfile.h>` unconditionally) | configure.ac:507-509 |
| pkg-config | **mandatory for configure** | configure.ac:468-473 |
| libpng | optional, detected automatically (`HAVE_PNG_H`); disabled on Android | configure.ac:491-501 |
| zlib | optional (`--disable-zip-support`), defines `HAVE_ZIP_SUPPORT` | configure.ac:673-698 |
| Internal Timidity | default on, no external lib | `--disable-timidity-midi`, configure.ac:551-565 |
| ALSA | autodetected | `--disable-alsa`, configure.ac:568-582 |
| FluidSynth / FluidLite | autodetected | `--enable-fluidsynth=yes\|lite\|no`, configure.ac:585-636 |
| mt32emu | autodetected | `--disable-mt32emu`, configure.ac:639-656 |
| hq2x/3x/4x, xBR scalers | default on | configure.ac:701-750 |
| SDL3_image | only for tools `mockup` and `smooth`; warns if missing | configure.ac:849-867 (`--disable-tools` skips it) |
| Exult Studio (GTK3 ≥ 3.16, ICU, freetype2, libpng) | default **off** | configure.ac:766-823 |
| usecode compiler (bison/flex) | default off | `--enable-compiler`, configure.ac:870-878 |
| GIMP / Aseprite plugin / shp thumbnailer | default off | configure.ac:927-1030 |
| data files (expack) | default on, **needed** | configure.ac:881-889 |

All optional MIDI drivers compile to empty translation units when their macro is not defined (`#ifdef USE_*_MIDI` at the top of each `audio/midi_drivers/*.cpp`). That is why `Makefile.common` can compile all of them unconditionally (Makefile.common:82-102).

**SDL3 minimum version.** configure has no floor. The code only uses APIs from SDL 3.2:
- `SDL_SetRenderLogicalPresentation`, `SDL_SetTextureScaleMode`, `SDL_SCALEMODE_NEAREST`/`LINEAR` (imagewin/imagewin.cc:662-743, 2127-2149);
- the version print at exult.cc:996-1002.

CI pins **SDL release-3.2.14** and **SDL_image release-3.2.4** (ci-linux.yml, "Checkout SDL 3" steps). MSYS2 and the Windows install ship 3.4.x. Treat **3.2.0 as the practical floor and 3.2.14 as the tested baseline**.

### 1.2 `Makefile.common` and `Makefile.mingw` (no autotools)

- `Makefile.common` is a plain GNU-make file of object lists and rules. It is **included only by `Makefile.mingw`** (Makefile.mingw:508). It defines `MAIN_OBJS` … `USECODE_OBJS`, the combined `OBJS` (Makefile.common:291), the data-flex generation through `expack` (Makefile.common:620-656) and the tools (`ipack`, `shp2pcx`, `splitshp`, `ucc`, `ucxt`, `confregress`, …; Makefile.common:623-795).
- It expects the includer to define `SRC`, `EXEC`, `EXEEXT`, `LIBEXT`, `SERVER_OBJS`, `ICON_OBJS`, `WIN32_OBJS`, `CXXFLAGS`, `CPPFLAGS`, `LDFLAGS` and `LIBS`. `Makefile.mingw` does this at lines 398-414 and 510-522.
- **It works on Linux without autotools.** This was verified (Appendix A). Three things are needed:
  1. **Define `CXXFLAGS` before `include Makefile.common`.** Several target-specific rules copy it with `:=` at parse time (Makefile.common:541, 543, 559). If it is not defined yet, `UnixSeqMidiDriver.o` compiles without `-std=c++17` and fails.
  2. **Add an order-only dependency on the generated data headers.** 21 `.cc` files include `data/exult_flx.h`, but `Makefile.common` declares only some of those dependencies, and `Makefile.mingw` adds a few more (Makefile.mingw:572-578). A clean parallel build races. The fix is `$(filter-out $(FILE_OBJS),$(OBJS)): | $(FLEXES) $(BG_PAPERDOLL) $(BG_MR_FACES)`. `FILE_OBJS` must be excluded because `expack` itself is built from them.
  3. **Pass the configuration as `-D` flags.** The sources include `config.h` only `#ifdef HAVE_CONFIG_H` (208 files; for example headers/common_types.h:23 and headers/pent_include.h:27).
- **Drift between `Makefile.common` and the other build files:**
  - `SHAPES_OBJS` (Makefile.common:231-258) lacks `shapes/pngio.o`, `fontgen.o` and `shapewrite.o`. `pngio.o` appears only in `IPACK_OBJS` and `EXULT_THUMB_OBJS` (Makefile.common:671, 734).
  - `objs/objiter.cc` is listed only in objs/Makefile.am:32. It is template code included from a header, so this is harmless.
- `Makefile.mingw` builds the Windows binaries:
  - It derives the toolchain from `$MSYSTEM` (MINGW64/MINGW32/UCRT64/CLANG64/CLANGARM64; Makefile.mingw:31-66).
  - It gets the libraries through `pkg-config` (Makefile.mingw:77-164).
  - It hard-codes the feature defines in `CPPFLAGS`: `-DUSE_EXULTSTUDIO -DHAVE_PNG_H -DUSE_MT32EMU_MIDI -DUSE_HQ*_SCALER -DUSE_XBR_SCALER …` (Makefile.mingw:510-522).
  - It links `LIBS := -lmingw32 $(SDL_LIBS) $(ZIP_LIBS) -lpng $(OGG_LIBS) … -lDbghelp` (Makefile.mingw:401).
  - `install` strips `Exult.exe`, copies the DLLs it depends on using `ntldd` (`copy_dlls_for_exe`, Makefile.mingw:504-506) and copies `data/exult*.flx` (Makefile.mingw:599-606).

### 1.3 Other build descriptions

- **MSVC**: `msvcstuff/vs2019/Exult.sln` uses vcpkg manifests:
  - dependencies: libpng, sdl3[vulkan], zlib, libogg, libvorbis, libmt32emu, fluidsynth, dirent (msvcstuff/vs2019/vcpkg.json);
  - defines in `msvc_include.h:60-74`, including `HAVE_PNG_H`;
  - sources listed one by one in `Exult.vcxproj` (220 `ClCompile` entries; `shapes\pngio.cc` at line 546).
- **iOS**: `ios/Exult.xcodeproj/project.pbxproj` lists the sources; `ios/include/config.h` holds the defines (no PNG).
- **Android**: Gradle and CMake wrap **autotools**. `android/app/src/main/cpp/dependencies/exult/CMakeLists.txt.in:10-11` runs `autoreconf` and `configure --enable-libexult … --disable-tools --disable-data`. It is not reusable as a native CMake build.

**Where a new hi-res source file must be registered**: the subdirectory `Makefile.am`, `Makefile.common` (and `Makefile.mingw` if it is Windows- or Studio-specific), `msvcstuff/vs2019/Exult.vcxproj` plus `.filters`, and `ios/Exult.xcodeproj` if iOS is kept. Android picks it up from `Makefile.am`.

### 1.4 Generated data

`exult` needs `data/exult.flx`, `exult_bg.flx` and `exult_si.flx` at runtime, and the matching `data/*_flx.h` headers at compile time. Both are produced by `tools/expack -i data/*.in` (data/Makefile.am:287-337; Makefile.common:623-656).

When cross-compiling, configure requires a native `expack` and `head2data` in `PATH` (configure.ac:1312-1335).

`.gitignore` does not cover `data/shortcutbar.vga`, `data/shortcutbar_vga.h` or the top-level `expack`/`ipack` that `Makefile.common` produces. They show up as untracked after a build, and the fork should add them.

---

## 2. Compiler and environment facts

- **g++ 9.4 is sufficient.**
  - `std::filesystem` is used in gamedat.cc:50 and cheat.cc:50. It lives in libstdc++ since GCC 9.1, so no `-lstdc++fs` is needed.
  - Only the integer form of `std::from_chars` is used (files/msgfile.cc:197, 233; shapes/font_map.cc:51, 80).
  - The full build in Appendix A compiled with **zero warnings**.
  - Upstream CI uses newer gcc and clang (`ubuntu-latest`). The fork should keep a g++ 9 build green, or document gcc ≥ 9 as the minimum.
- **Local tools as of this analysis.**
  - `cmake 4.4.4` and `ninja 1.13.2` are in `~/.local/bin`. They were installed with `uv tool install` at 13:27, probably by a parallel task. CMake 4.x is fine for SDL 3.2, which needs ≥ 3.16.
  - There is no `m4`, `pkg-config`, `bison`, `flex` or clang.
  - apt indexes are populated, so `apt-get download` works without root.
  - The **runtime** `.so` files for png16, z, ogg, vorbis(file), X11, Xext, pulse, wayland and xkbcommon are installed. Only the `-dev` headers are missing, except `libx11-dev` and `x11proto-dev`, which are present.
- **WSLg is available**: `DISPLAY=:0`, `WAYLAND_DISPLAY=wayland-0` and `/mnt/wslg/PulseServer`. Interactive runs under WSL can show a window.
- **Windows interop works.** `cmd.exe /c ver` runs (it warns about the UNC working directory, which is harmless). On the Windows side there are `C:\Python312`, Git and `nvidia-smi.exe`. MSYS2 is **not** installed (`C:\msys64` is absent), and `E:\Dati\Ultima7_Upscale` does not exist yet.
- **A parallel workspace** at `/home/simonea/ultima7_exult/deps` (`env.sh`, `debs/`, `sysroot/`, `prefix/bin/autoreconf` …) already took the autotools route:
  - It used `apt-get download` + `dpkg -x` of autoconf 2.69, automake 1.16.1, libtool 2.4.6, autoconf-archive, m4, pkg-config and the `-dev` libraries.
  - It ran `autoreconf` **inside the repo**, which created untracked `Makefile.in`, `configure`, `aclocal.m4` and `autom4te.cache`.
  - It configured out of tree in `../build-linux` with `--disable-exult-studio --disable-gimp-plugin --disable-aseprite-plugin --disable-shp-thumbnailer`.

  Both routes are therefore available.

---

## 3. CI (`.github/workflows`)

| Workflow | Platform | How it builds | Tests |
|---|---|---|---|
| `ci-linux.yml` | ubuntu-latest × {gcc, clang} | apt (various -dev libs, including legacy `libsdl2-dev`), **SDL3 3.2.14 and SDL_image 3.2.4 built from source with cmake and installed with sudo**, `autoreconf -v -i`, `./configure --with-debug=extreme --enable-exult-studio … --enable-mods --enable-aseprite-plugin`, `make -j 2` | none |
| `ci-windows.yml` | MSYS2 {UCRT64, clang64, clangarm64} on windows-2022 and windows-11-arm | `msys2/setup-msys2` with `mingw-w64-*-{toolchain,sdl3,fluidsynth,libtimidity,libogg,libvorbis,munt-mt32emu,libpng,zlib,sdl3-image,gtk3,…}`, then `make -f Makefile.mingw … Exult.exe mods exult_studio.exe tools … dist`, a second make with the default target, and Inno Setup installers | none (`disttest` exists but is not invoked) |
| `ci-msvc.yml` | windows-2022 MSBuild + vcpkg | **only runs when `repository_owner` is exult, wench or dominusexult**, so it will not run in a fork | none |
| `ci-macos`, `ci-freebsd`, `ci-ios`, `ci-android`, `ci-omnios`, `snapshots*`, `codeql`, `coverity-scan` | various | autotools or own projects | none |
| `format-check.yml` | clang-format 19, `continue-on-error: true` (informational only) | – | – |

Notes:
- `ci-linux.yml` passes `--enable-usecode-container --enable-nonreadied-objects`. Neither exists in configure.ac, so configure ignores them.
- `snapshots-windows.yml` builds mingw32 / clang64 / clangarm64.
- No workflow runs `make check` or any binary. A fork-level CI would add a `make check` step with `SDL_VIDEO_DRIVER=dummy`.

---

## 4. Plan (1): a working Linux build in user space without sudo

### 4.A Route A, verified: Makefile.common wrapper, no autotools and no pkg-config

```bash
D=$HOME/ultima7_exult/deps2         # any prefix
mkdir -p $D/debs $D/root && cd $D/debs
apt-get download libpng-dev zlib1g-dev libogg-dev libvorbis-dev libxext-dev libpulse-dev
for f in *.deb; do dpkg -x "$f" $D/root; done
# Debian -dev .so links are RELATIVE -> dangle inside the extracted tree; repoint them:
cd $D/root/usr/lib/x86_64-linux-gnu
for l in *.so; do t=$(readlink "$l"); case "$t" in /*) ;; *) [ -e "$t" ] || ln -sfn /usr/lib/x86_64-linux-gnu/$t "$l";; esac; done
ln -sfn libpng16.so libpng.so          # chain link; fix after the loop
# SDL3 (cmake/ninja from `uv tool install cmake ninja`)
cd $D && curl -LO https://github.com/libsdl-org/SDL/releases/download/release-3.2.14/SDL3-3.2.14.tar.gz
tar xzf SDL3-3.2.14.tar.gz
cmake -S SDL3-3.2.14 -B build-sdl -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$D/prefix -DCMAKE_PREFIX_PATH=$D/root/usr \
  -DCMAKE_C_FLAGS="-I$D/root/usr/include" \
  -DSDL_WAYLAND=OFF -DSDL_X11=ON -DSDL_ALSA=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF \
  -DSDL_SNDIO=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build build-sdl -j16 && cmake --install build-sdl      # ~5 s with 16 cores
# Exult
cd ~/ultima7_exult/exult-hires
make -f /path/to/Makefile.linux DEPS=$D -j16 exult ipack        # ~57 s
```

Results observed:
- SDL3 configured with **X11 on** (XShape, Xsync and Xdbe on; Xrandr, Xinput, Xcursor and Xfixes off unless their `-dev` debs are extracted), the dummy and offscreen video drivers, GLES2 and Vulkan/GPU renderers, and the dummy and disk audio drivers.
- Without `pkg-config`, SDL's **PulseAudio backend was turned OFF**, so runs are silent. To get sound under WSLg, extract `pkg-config` from its deb (`dpkg -x`), set `PKG_CONFIG_PATH` to the extracted `.pc` files (with `prefix=` rewritten), and reconfigure SDL. Desktop OpenGL was also off (no GL headers). That is irrelevant to Exult, which renders through the SDL renderer.
- `ldd exult`: `libSDL3.so.0` from the prefix (rpath), the rest from the system.
- `exult --version` works, and so does `exult -c test.cfg --bg --buildmap 2` headless (§6.3).

### 4.B Route B: autotools from extracted debs

This is what `../deps/env.sh` does: extract m4, autoconf, automake, libtool, autoconf-archive and pkg-config, then export `PATH`, `ACLOCAL_PATH`, `PKG_CONFIG_PATH`, `CPATH`, `LIBRARY_PATH` and `LD_LIBRARY_PATH` into the sysroot. Then:

```bash
autoreconf -v -i
../exult-hires/configure --disable-exult-studio --disable-alsa --disable-fluidsynth --disable-mt32emu \
  [--with-debug=symbols --with-optimization=normal]
make -j16
```

Use this route whenever `Makefile.am`, `configure.ac` or `make check` integration is touched. Upstream CI uses autotools, so changes must keep working there.

If the extracted autotools ever misbehave because of hard-coded `/usr/share` paths, a robust fallback is to build GNU m4, autoconf, automake and libtool from their source tarballs with `--prefix=$HOME/.local` (each is a plain `./configure && make install` that needs only cc and perl).

**Recommendation:** use Route A for the fast edit-compile-test loop, and Route B to validate the autotools files before each commit. Add the wrapper to the fork as `Makefile.linux`, or better as `tests/`-aware rules, so the user does not depend on the scratchpad.

---

## 5. Plan (2): producing a Windows build of the fork

**W1, recommended: MSYS2 UCRT64 under `E:\Dati\Ultima7_Upscale`.** This matches upstream `ci-windows.yml` and `README.windows`, and needs no admin rights:

1. Download `msys2-base-x86_64-latest.sfx.exe` and run `msys2-base-x86_64-latest.sfx.exe -y -oE:\Dati\Ultima7_Upscale\`. This gives `E:\Dati\Ultima7_Upscale\msys64`.
2. From WSL, call the MSYS2 bash through interop:
   ```bash
   B=/mnt/e/Dati/Ultima7_Upscale/msys64/usr/bin/bash.exe
   export MSYSTEM=UCRT64 CHERE_INVOKING=1 WSLENV=MSYSTEM/u:CHERE_INVOKING/u
   $B -lc 'pacman -Syuu --noconfirm'      # twice
   $B -lc 'pacman -S --noconfirm --needed base-devel git zip \
     mingw-w64-ucrt-x86_64-{toolchain,binutils,ntldd,sdl3,fluidsynth,libtimidity,libogg,libvorbis,munt-mt32emu,libpng,zlib,sdl3-image}'
   ```
3. Keep a working copy on the Windows filesystem. MSYS2 cannot build from `\\wsl.localhost` UNC paths, and builds there are slow. Either `git clone /home/simonea/ultima7_exult/exult-hires /mnt/e/Dati/Ultima7_Upscale/src/exult-hires` and `git pull` from it, or push to a bare repo on E:.
4. Build and install into a **separate** folder:
   `$B -lc 'cd /e/Dati/Ultima7_Upscale/src/exult-hires && make -f Makefile.mingw -j16 Exult.exe && make -f Makefile.mingw install U7PATH=E:/Dati/Ultima7_Upscale/ExultHires'`.
   The `install` target copies the DLLs that `ntldd` finds (Makefile.mingw:504-506, 599-606).
5. Run it with `-p` (portable mode: config and saves next to the exe; exult.cc:313-326) or `-c <cfg>`. Otherwise it shares `%LOCALAPPDATA%`-based config and saves with 1.12.1 (files/utils.cc:604-628). Point `<static_path>` at `E:\Games\RolePlayingGames\ultima7\static`.
6. Smoke test: `make -f Makefile.mingw disttest` (Makefile.mingw:824-827), or `Exult.exe --version` from WSL.

**Do not reuse the 1.12.1 folder.** Its `Exult.exe` is SDL2- and msvcrt-based. Its `libstdc++-6.dll` and `libgcc_s_seh-1.dll` would clash with a UCRT64 build, and its `SDL3.dll` (3.4.2) is an msvcrt build.

Alternatives:
- **W2**: cross-compile in WSL with an llvm-mingw tarball and SDL's official `SDL3-devel-3.x-mingw.tar.gz`, with ogg, vorbis, png and zlib built by cmake. This needs a cross wrapper, because `Makefile.mingw` depends on `MSYSTEM`, `cygpath` and `pkg-config`. It also needs a **native** `expack`; WSL could run `expack.exe` through interop, but that is fragile. More work, no benefit.
- **W3**: MSVC with vcpkg (`msvcstuff/vs2019`). This needs Visual Studio on Windows, and the CI job is disabled in forks.

---

## 6. Plan (3): a headless unit and integration test suite

### 6.1 What exists

- `files/rwregress.cc`: assert-based endian reader test (`noinst_PROGRAMS`, files/Makefile.am:20).
- `conf/xmain.cc`: `confregress` (conf/Makefile.am:5-21, Makefile.common:776-779).
- `head2data.test` (Makefile.common:793-794) only runs the generator.
- `audio/mtest` is a stale one-liner (`g++ test.cc … -lSDL`).
- No framework, no `check_PROGRAMS`/`TESTS`, nothing in CI.

### 6.2 What can be linked without SDL

These sources compile and link without SDL headers or libraries:

- `files/*` (except the `sdlrwops*` files);
- `imagewin/ibuf8.cc` and `imagewin/imagebuf.cc` (`Image_buffer8(w,h)` owns its pixels; ibuf8.h:36-39);
- `shapes/vgafile.cc` (`Vga_file`, `Shape_frame` constructors from pixels, vgafile.cc:355-390);
- `shapes/pngio.cc` (`Import_png8`/`Export_png8`, pngio.cc:45-258).

`palette.h`, `vgafile.h` and `imagebuf.h` do not include SDL. `ipack` and the thumbnailer prove the set links with only `-lpng -lz` (Makefile.common:659-676). This is the right base for **pure CPU unit tests** of the hi-res code: scaled blits, the override registry, the PNG loader and the downscaler.

### 6.3 What already works headless with the full engine (verified)

```bash
env -u DISPLAY -u WAYLAND_DISPLAY HOME=$T/home SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
  ./exult -c $T/test.cfg --bg --buildmap 2
```

`test.cfg` sets these keys under `config/disk/game/blackgate`, so the user's game folder is never written to:
- `path` = scratch dir;
- `static_path` = `/mnt/e/Games/RolePlayingGames/ultima7/static`;
- `patch`, `mods`, `savegame_path`, `gamedat_path` = scratch dirs;
- `config/disk/data_path` = `<repo>/data`.

The keys are read at gamemgr/modmgr.cc:396-405, 602-617 and 1118-1130, and exult.cc:556-558. Without the `patch` override, `game.cc:521` would `mkdir` `<game_path>/patch` in the user's install.

Result: 144 PNGs `u7map00..8f.png`, each 2048x2048, 8-bit palette (colour type 3), 109 MB, 19.5 s. A second run was byte-identical.

How it works: `BuildGameMap` builds a `Game_window(2048, 2048, …, scale 1, point)` and calls `paint_map_at_tile` for each superchunk. It then calls `Image_window::screenshot(dst, true)`, which writes the **8-bit draw surface** (imagewin/imagewin.cc:1254-1256 → `SaveIMG_RW` in imagewin/save_screenshot.cc). The goldens are therefore **palette-index-exact**.

### 6.4 Proposed layout

```
tests/
  doctest.h                    # vendored single header (MIT; C++11; g++ 9, mingw and MSVC OK)
  unit/                        # SDL-free, CI-safe, synthetic fixtures only
    test_scaled_blit.cc        # Image_buffer8 at S: copy8/paint_rle/... at S
    test_override_registry.cc  # (vga,shape,frame) -> override lookup and fallback
    test_png_override.cc       # Import_png8 conventions (tRNS, rotation, oFFs)
    test_downscale.cc          # S-x buffer -> window size (box/area filter) vs reference
    test_coords.cc             # game<->hi-res<->window mapping, hit-test invariants
  integration/                 # SDL dummy driver, still synthetic data
    test_image_window.cc       # Image_window8 at S, set_palette, screenshot(paletted)
  game/                        # needs real BG data; skipped (exit 77) if U7_BG_STATIC unset
    run_render_regions.sh      # exult --render-test ... (new CLI) or --buildmap
  golden/                      # small indexed PNGs (synthetic only; never commit U7 art)
```

**Golden strategy.**
1. Compare the **8-bit index buffer** byte for byte, as a hash or as an indexed PNG. Everything before presentation is palette-indexed and deterministic (§6.3).
2. **Invariant test**: with no overrides, `render_at(S)` must equal `nearest_upscale(render_at(1), S)`. This holds on synthetic fixtures in CI, and locally on real BG regions. It catches any drawing path that was not scaled (lines, fills, text, translucency tables) without storing large goldens.
3. **Override test**: build a tiny `Vga_file`/`Shape` in memory (or write one with `Flex_writer`, as ipack does at ipack.cc:660-686), generate override PNGs in the test with `Export_png8`, paint, and check that the override pixels appear at the S-scaled hotspot. Also check that frames without an override still go through the NN path.
4. **Presentation and downscale** produce RGB(A), so compare those with a tolerance of ±1.
5. **Hit-testing**: after `screen_to_game` (imagewin/imagewin.cc:1277) at S and with fit/downscale, a click at window pixel p must map to the same game pixel as at S=1.

**Real-data runs and memory.** A `--buildmap` at S=6 would create 12288x12288 surfaces: 151 MB at 8 bpp, plus PNG encoding of each of the 144 tiles. Add a small dedicated option instead, for example `--render-test x,y,w,h,scale,out.png`, declared next to the existing `parameters.declare` calls (exult.cc:289-312) and implemented like `BuildGameMap`.

**Build integration.**
- Autotools: `tests/Makefile.am` with `check_PROGRAMS`/`TESTS`, `AM_TESTS_ENVIRONMENT = SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy`, and `LDADD = shapes/libshapes.la imagewin/libimagewin.la files/libu7file.la $(PNG_LIBS) $(ZLIB_LIBS) $(SDL_LIBS)`. Add the directory to `SUBDIRS` and `AC_CONFIG_FILES`.
- `Makefile.common`: a `check:` target whose test objects link `IPACK_OBJS` minus `tools/ipack.o`, plus the new hi-res objects.
- CI: add `make check` to ci-linux.yml after Build, and a test `.exe` run in ci-windows.yml.

---

## 7. Plan (4): tools to extract and re-import art

### 7.1 Inventory

| Tool | What it does | Usefulness |
|---|---|---|
| **`ipack`** (tools/ipack.cc, tools/ipack.txt) | Script-driven: `-x` extracts `.vga`/flex shapes to indexed PNG; `-c`/`-u` creates or patches archives from PNG; `all:` extracts every shape and frame | **Best extractor.** Needs libpng and zlib only (Makefile.common:659-676; autotools needs `HAVE_PNG` and `HAVE_SDL`, tools/Makefile.am:16-23) |
| `expack` (tools/expack.cc) | Flex pack, unpack, list (`-c/-x/-l/-i manifest`) | Packs arbitrary blobs, for example PNG overrides, into a Flex |
| `shp2pcx` | single `.shp` → PCX (needs SDL3 headers only for `SDL_endian.h`, tools/shp2pcx.cc:25-50) | Legacy; PCX |
| `splitshp` | split or join a single `.shp` into frames | `.shp` only |
| Aseprite plugin (`tools/aseprite_plugin/exult_shp.cc`) | CLI `exult_shp import <shp> <png> [pal]` / `export …`; Lua UI; "Convert to U7 Palette" masks indices 224-255 | Single `.shp`; good for artists |
| GIMP plugin (`tools/gimp_plugin/u7shp.cc`) | load/save `.shp` in GIMP 2 or 3 | Needs GIMP dev libs |
| Exult Studio (`mapedit/shapelst.cc`) | export frame (Export_png8 with rotation, :713); import with **nearest-colour remap** to the game palette (`Find_closest_color`/`Convert_indexed_image`, :1001-1052) | GTK; reference for remapping logic |
| `rip`, `mockup`, `smooth`, `wuc`, `ucxt`, `ucc` | usecode splitter, map mockup from an indexed image, BMP smoother, usecode tools | Not relevant |

### 7.2 Verified extraction command and the PNG conventions it produces

```bash
mkdir -p art_original/shapes
cat > art_original/shapes.ipk <<'EOF'
archive /mnt/e/Games/RolePlayingGames/ultima7/static/shapes.vga
palette /mnt/e/Games/RolePlayingGames/ultima7/static/palettes.flx
all: art_original/shapes/s
EOF
./ipack -x art_original/shapes.ipk     # -> s0000_00.png ... s1023_NN.png  (14,171 files, ~56 MB, ~10 s)
```

Script rules:
- `palette` must come **before** `all:`, because `Read_script` returns as soon as it sees `all:` (ipack.cc:264-277).
- The output directory must already exist.
- File names are `<base>%04d_` + `%02d.png` (ipack.cc:116, 450).
- The same script works for `faces.vga`, `gumps.vga`, `sprites.vga` and `fonts.vga`. `mainshp.flx` is a Flex whose entries are shapes.
- Only palette **0** is used (ipack.cc:748).

PNG format, observed on the output files:
- 8-bit, colour type 3, 256-entry `PLTE`.
- **Palette rotated by +1**: `Export_png8(..., transp=255, ..., transp_to_0=true)` (ipack.cc:464; pngio.cc:216-235). PNG index = (U7 index + 1) mod 256. U7 transparent 255 becomes PNG 0, and `tRNS = [0]`. PNG `PLTE[1]` is U7 colour 0.
- RGB = 6-bit value × 4 **with uint8 wrap** (ipack.cc:752-754). Palette entry 255 in BG has out-of-range bytes and comes out as (232,0,4). 63 maps to 252, not 255, so the PNG colours are slightly darker than in-game, where gamma tables use `max_val` 63 (palette.cc:58, 279).
- `oFFs` chunk: flats get (0,0); RLE frames get (-xright, -ybelow) (ipack.cc:457-462). Example: `s0150_00.png` is 28x37 with `oFFs` (-4,-3).
- Flats are 8x8 (painted with `xleft=yabove=8`, vgafile.cc:444, 525-533).
- **`Import_png8(…, 255, …)` undoes the rotation**, but only if the PNG still has a `tRNS` entry that is fully transparent. That index is dropped from the palette and higher indices shift down (pngio.cc:127-165). If an editor or AI step drops `tRNS` or reorders the palette, the indices come back off by one. A strict validator is required.

### 7.3 Recommendations for the hi-res pipeline

- **Extraction for AI work:** keep `ipack` as the reference extractor. Also add a small Python tool (uv + Pillow) that writes **un-rotated, raw-index PNGs** plus a JSON sidecar per frame `{vga, shape, frame, w, h, xleft, yabove, xright, ybelow, flat}`, and RGB previews for the AI models. This avoids the off-by-one trap and records what is needed for the hotspot maths.
- **Quantising AI output (RGB) back to the U7 palette:**
  - Use nearest colour against palette 0 (as Studio does, shapelst.cc:1001-1052), but **exclude 0xE0-0xFF**, the cycling and special indices (the Aseprite plugin does this too).
  - Where the 1x source used cycling indices (water, lava), carry the index mask over by NN-upscaling the source index map, so that palette animation keeps working.
- **Hotspot maths for S-scaled frames:** `w' = S·w`, `h' = S·h`, `xleft' = S·xleft`, `yabove' = S·yabove`, `xright' = S·xright + S - 1`, `ybelow' = S·ybelow + S - 1`. Flats are consistent with this: `xleft=8, xright=-1` becomes `48, -1`.
- **Where hi-res frames can live:**
  - (a) **Loose PNGs**, for example `<PATCH>/hires/6x/shapes/SSSS_FF.png`. Best for iteration and per-frame selection; decode with `Import_png8`.
  - (b) A **parallel `.vga` of RLE frames** made with `ipack -c` (non-flat spec). Hi-res flats **cannot** be stored as flat frames: `Shape_frame` asserts 8x8 for non-RLE (vgafile.cc:363, 384), and flat versus RLE is detected from the entry length (vgafile.cc:413-449). RLE frames support arbitrary sizes with int16 offsets and could reuse every `paint_rle*` routine in the S-scaled buffer.
  - (c) A **Flex of PNG blobs** made with `expack`. This needs an in-memory PNG reader; `pngio` only reads a `FILE*`.

  Use (a) for development and (b) or (c) for distribution.
- `ipack -u` remains the tool for patching **1x** shapes.

---

## 8. Touchpoints and risks (summary)

See the structured summary for the per-file list. The main risks:
- the four build descriptions drift apart;
- the PNG index rotation and missing `tRNS`;
- no PNG on Android and iOS;
- memory and time for S=6 full-map renders;
- the MSVC CI is disabled in forks;
- DLL clashes with the 1.12.1 install;
- in-tree `autoreconf` leaves untracked generated files;
- the g++ 9 versus upstream compiler gap.

---

## Appendix A: verified `Makefile.linux` wrapper (Route A)

Run it from the repo root: `make -f Makefile.linux DEPS=<prefix> -j16 exult ipack`.

```make
# Linux build via Makefile.common without autotools/pkg-config.
DEPS ?= $(HOME)/opt/exult-deps        # SDL3 in $(DEPS)/prefix, extracted -dev debs in $(DEPS)/root
SRC := .
CC := gcc
CXX := g++
EXEC := exult
EXEEXT :=
LIBEXT := .so
SERVER_OBJS := server/objserial.o server/servemsg.o server/server.o
ICON_OBJS :=
WIN32_OBJS :=
OPT_LEVEL ?= -O2
# CXXFLAGS must be set BEFORE including Makefile.common (target-specific := copies it)
CXXFLAGS := -MMD -std=c++17 -pthread $(OPT_LEVEL) -g -Wall -Wextra
include Makefile.common
CPPFLAGS := -DVERSION=\"$(VERSION)\" -DEXULT_DATADIR=\"$(CURDIR)/data\" -DXWIN \
  -DHAVE_PNG_H -DHAVE_ZIP_SUPPORT -DUSE_FMOPL_MIDI -DUSE_TIMIDITY_MIDI \
  -DUSE_HQ2X_SCALER -DUSE_HQ3X_SCALER -DUSE_HQ4X_SCALER -DUSE_XBR_SCALER \
  -DHAVE_SYS_TYPES_H=1 -DHAVE_SYS_TIME_H=1 -DHAVE_SYS_SOCKET_H=1 -DHAVE_NETDB_H=1 -DHAVE_UNISTD_H=1 -DHAVE_GETOPT_LONG \
  -I. -Iaudio -Iaudio/midi_drivers -Iconf -Idata -Ifiles -Ifiles/zip -Igamemgr -Igumps -Iheaders \
  -Iimagewin -Iobjs -Ipathfinder -Iserver -Ishapes -Ishapes/shapeinf -Itools -Iusecode \
  -isystem $(DEPS)/prefix/include -isystem $(DEPS)/root/usr/include
LDFLAGS := -pthread -L$(DEPS)/prefix/lib -L$(DEPS)/root/usr/lib/x86_64-linux-gnu -Wl,-rpath,$(DEPS)/prefix/lib
LIBS := -lSDL3 -lpng16 -lz -lvorbisfile -lvorbis -logg -ldl
-include $(OBJS:%.o=%.d)
playscene.o shapeid.o shapes/font_map.o: data/exult_flx.h
# Generated data headers must exist before any engine object compiles (expack needs FILE_OBJS first).
$(filter-out $(FILE_OBJS),$(OBJS)): | $(FLEXES) $(BG_PAPERDOLL) $(BG_MR_FACES)
```

Verified output: `Exult version 1.13.1git … Compile-time options: USE_TIMIDITY_MIDI, USE_FMOPL_MIDI, HAVE_ZIP_SUPPORT … Compiler: GCC 9.4.0`.
