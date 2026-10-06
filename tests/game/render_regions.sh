#!/bin/bash
# tests/game/render_regions.sh - the in-process oracles of --render-test on the regions of
# regions.txt (DESIGN.md section 6.4): at S = 2, 3 and 6 with overrides off, the S render is the
# nearest-neighbour upscale of the scale-1 render (O2), 64 random sub-rect repaints keep it so
# and change neither render (O6), and the mini screenshot equals the reference's (O7; not the
# light-source count, which --render-test cannot see without a main actor and the ireg objects).
# Then the window cases on Britain: the present read-back at 1:1 (exact) and at 1280x800 (LINEAR,
# within 1 of the software renderer's scaling), ARGB and INDEX8 in one process (equal within 1;
# INDEX8 must be the texture format on SDL >= 3.4, its pass is skipped before), and a 320x200 game
# area in a 355x200 full area (offset_x 17) with and without the read-back; and passes=flats on
# the coast.
# Every run is made twice and the digests (timings aside) must be equal (determinism, I10); an
# ASan build runs each once (its heap order follows the environment, DESIGN.md section 6.4).
# Skipped (77) without U7_BG_STATIC.
# Usage: [U7_BG_STATIC=...] [EXULT_WRAPPER=...] [REGIONS="name ..."] render_regions.sh [build-dir]
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

game_require_data
game_require_build "${1:-}"

start=$(date +%s)

regions_file="$game_tests_srcdir/game/regions.txt"
while read -r name tx ty w h lift; do
	case "$name" in
	'' | '#'*) continue ;;
	esac
	if [ -n "${REGIONS:-}" ] && ! [[ " $REGIONS " == *" $name "* ]]; then
		continue
	fi
	game_check_render "$name" "tx=$tx,ty=$ty,w=$w,h=$h,lift=$lift,scales=2:3:6,repaint=64,seed=1"
done < "$regions_file"

if [ -z "${REGIONS:-}" ]; then
	britain="tx=800,ty=1330,lift=16,seed=1"
	game_check_render present-1to1 "$britain,w=320,h=200,scales=6,present=1,format=both"
	game_check_render present-1280x800 "$britain,w=320,h=200,scales=6,present=1,format=both,window=1280x800"
	game_check_render offsets "$britain,w=355,h=200,game=320x200,scales=2:3:6,repaint=16"
	game_check_render offsets-present "$britain,w=355,h=200,game=320x200,scales=6,present=1,format=both"
	game_check_render coast-flats "tx=1040,ty=1560,w=640,h=400,lift=16,scales=2:6,passes=flats,repaint=16,seed=1"
fi

echo "render_regions: $game_pass passed, $game_fail failed in $(($(date +%s) - start)) s ($GAME_BUILD)"
[ $game_fail -eq 0 ]
