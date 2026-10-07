#!/bin/bash
# tests/windows/win_rt.sh - one --render-test of a Windows Exult.exe (Makefile.mingw install),
# started from WSL through interop (docs-hires/test/windows_matrix.md, "Method").
# Usage: EXULT_WIN_DIR=... U7_BG_STATIC_WIN=... win_rt.sh <name> <spec> [hires_path]
#   EXULT_WIN_DIR     Windows path of the install directory (Exult.exe and data\), for example
#                     'E:\Exult\ExultHires'. Each run gets the sandbox <EXULT_WIN_DIR>\rt\<name>
#                     (made anew: own game, patch, mods, source, saves, gamedat, hires and out
#                     directories and a scratch exult.cfg), so no play configuration is touched.
#   U7_BG_STATIC_WIN  Windows path of a Black Gate "static" directory (read only).
#   hires_path        Windows path of the <HIRES> root (default: the sandbox's empty hires).
#   SDL_RENDER_DRIVER, SDL_GPU_DRIVER, SDL_VIDEO_DRIVER are forwarded when set (plain WSLENV
#   entries: the /u flag does not forward on every Windows 10 build); RT_TIMEOUT (300 s).
# -p keeps Exult's home in the working directory (else %LOCALAPPDATA%\Exult); data_path is named
# because an interop process gets a Linux argv[0]. Audio off, gamma 1, render_scale off (the spec
# sets the scale). Prints the exit code and wall time, the renderer actually used, the store's
# summary and the bench, time and present lines of digest.json; the log is <sandbox>\run.log.
set -u
if [ $# -lt 2 ] || [ -z "${EXULT_WIN_DIR:-}" ] || [ -z "${U7_BG_STATIC_WIN:-}" ]; then
	echo "usage: EXULT_WIN_DIR=... U7_BG_STATIC_WIN=... $0 <name> <spec> [hires_path]" >&2
	exit 2
fi
name=$1
spec=$2
hires=${3:-}
W=${EXULT_WIN_DIR%\\}
L=$(wslpath -u "$W") || exit 2
if [ ! -f "$L/Exult.exe" ]; then
	echo "ERROR: no Exult.exe in $W" >&2
	exit 2
fi
sb=$L/rt/$name
rm -rf "$sb"
mkdir -p "$sb"/{game,patch,mods,source,saves,gamedat,hires,out} || exit 2
ws="$W\\rt\\$name"
[ -z "$hires" ] && hires="$ws\\hires"
cat > "$sb/exult.cfg" << CFG || exit 2
<config>
  <disk>
    <data_path>$W\\data</data_path>
    <game>
      <blackgate>
        <path>$ws\\game</path>
        <static_path>$U7_BG_STATIC_WIN</static_path>
        <patch>$ws\\patch</patch>
        <mods>$ws\\mods</mods>
        <source>$ws\\source</source>
        <savegame_path>$ws\\saves</savegame_path>
        <gamedat_path>$ws\\gamedat</gamedat_path>
        <hires_path>$hires</hires_path>
      </blackgate>
    </game>
  </disk>
  <audio>
    <enabled>no</enabled>
  </audio>
  <video>
    <gamma>
      <red>1.00</red>
      <green>1.00</green>
      <blue>1.00</blue>
    </gamma>
    <hires>
      <render_scale>off</render_scale>
    </hires>
  </video>
</config>
CFG
cd "$sb" || exit 2
fw=""
for v in SDL_RENDER_DRIVER SDL_GPU_DRIVER SDL_VIDEO_DRIVER; do
	[ -n "${!v:-}" ] && fw="$fw:$v"
done
w=${WSLENV:-}
w=${w%:}
export WSLENV="${w}${fw}"
WSLENV=${WSLENV#:}
t0=$(date +%s.%N)
timeout "${RT_TIMEOUT:-300}" "$L/Exult.exe" -p -c "$ws\\exult.cfg" --bg --render-test "$spec,out=$ws\\out" > run.log 2>&1
rc=$?
t1=$(date +%s.%N)
actual=$(grep -aoE '"[a-z0-9_]*_present_renderer": *"[^"]*"' out/digest.json 2>/dev/null | head -1 | sed 's/.*: *"\(.*\)"/\1/')
echo "rc=$rc wall=$(awk "BEGIN { printf \"%.2f\", $t1 - $t0 }") s renderer=${actual:-?} (requested ${SDL_RENDER_DRIVER:-default}) sandbox=$sb"
grep -aE '^\[hires\] x[0-9]+:|FAIL|^--render-test' run.log | head -5
grep -aE '"(bench|time|present)_|_present_' out/digest.json 2>/dev/null
exit $rc
