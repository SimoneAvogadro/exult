#!/bin/bash
# tests/game/override_regions.sh - the per-tile override oracles of --render-test (DESIGN.md
# section 6.4), with packs that tools/hires/mkpack_identity.py makes from the game's own flats in
# a scratch directory (removed at the end unless a failing case keeps its sandbox; they are
# derived from the game's art):
#   * O4a: the identity pack (every tile NN_S of its 1x flat, x2, x3 and x6 folders of loose PNGs)
#     on every region of regions.txt at S = 2, 3, 6: the render with overrides on equals NN of the
#     scale-1 render, and the pack must cover every flat cell of the view; also through the
#     window's buffer with the present read-back, the S cycle and a terrain edit; and once with
#     mode=plain and present=1 (no oracle: the window shown and timed, a short bench and walk);
#   * O4a, reduction: an identity pack with an x6 bundle only, so S = 2 and 3 come from the
#     store's reduction (section 5.6) and S = 6 from the bundle;
#   * O4b: the marker pack (identity with index 1 at the top-left sub-pixel of every S x S block)
#     with passes=flats on every region: the render equals the predicted image exactly; with
#     passes=all it differs from NN; repaints leave it unchanged;
#   * O4b, per-tile fallback: a marker pack of the tiles whose shape + frame is even only
#     (coverage=partial), so the same flats caches mix overridden and NN cells, predicted exactly;
#   * toggle (I8): overrides on, off (NN of the reference, through Hires::generation() and the
#     flats caches), on again (the first render), in one process, in pushed buffers and in the
#     window's buffer with a game area offset in the full area; each toggle must render every flats
#     cache of the view once, and a repaint after it none.
# Every run is made twice with equal digests (once in an ASan build), and the log must show the
# store loading every flat with no reject.
# Skipped (77) without U7_BG_STATIC or tools/hires/mkpack_identity.py.
# Usage: [U7_BG_STATIC=...] [EXULT_WRAPPER=...] [REGIONS="name ..."] override_regions.sh [build-dir]
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

game_require_data
game_require_build "${1:-}"

start=$(date +%s)

mkpack="$game_tests_srcdir/../tools/hires/mkpack_identity.py"
if ! [ -f "$mkpack" ]; then
	echo "SKIP: $mkpack not found" >&2
	exit 77
fi
game_make_packs
packs=$GAME_PACKS
python3 "$mkpack" "$U7_BG_STATIC" "$packs/identity" || exit 2
python3 "$mkpack" "$U7_BG_STATIC" "$packs/identity6" --scales 6 --bundle || exit 2
python3 "$mkpack" "$U7_BG_STATIC" "$packs/marker" --kind marker --marker 1 || exit 2
python3 "$mkpack" "$U7_BG_STATIC" "$packs/marker-even" --kind marker --marker 1 --scales 2,3,6 --subset even || exit 2
all_flats=$(find "$packs/identity/x6/flats" -name '*.png' | wc -l)
even_flats=$(find "$packs/marker-even/x6/flats" -name '*.png' | wc -l)
flats=$all_flats

# The store must have loaded every flat of the pack ($flats) at each scale, with no reject (the
# log has one summary line per scale it loaded).
check_store_log() {
	local log=$GAME_SANDBOX/run.log
	if ! grep -q "^\[hires\] x[0-9]*: $flats tiles loaded" "$log" \
		|| grep -E '^\[hires\] x[0-9]*: ' "$log" | grep -vqE "^\[hires\] x[0-9]*: $flats tiles loaded .*, 0 rejected, "; then
		echo "FAIL: the store did not load all $flats flats without a reject:" >&2
		grep '^\[hires\]' "$log" | head -5 >&2
		return 1
	fi
	return 0
}

GAME_RUN_CHECK=check_store_log

regions_file="$game_tests_srcdir/game/regions.txt"
regions() {
	while read -r name tx ty w h lift; do
		case "$name" in
		'' | '#'*) continue ;;
		esac
		if [ -n "${REGIONS:-}" ] && ! [[ " $REGIONS " == *" $name "* ]]; then
			continue
		fi
		echo "$name tx=$tx,ty=$ty,w=$w,h=$h,lift=$lift,seed=1"
	done < "$regions_file"
}

GAME_HIRES_PACK=$packs/identity
while read -r name region; do
	game_check_render "identity-$name" "$region,scales=2:3:6,overrides=yes,expect=identity,repaint=16"
done < <(regions)

GAME_HIRES_PACK=$packs/marker
while read -r name region; do
	game_check_render "marker-$name" "$region,scales=2:3:6,overrides=yes,expect=marker:1,passes=flats,repaint=16"
done < <(regions)

if [ -z "${REGIONS:-}" ]; then
	britain="tx=800,ty=1330,lift=16,seed=1"
	coast="tx=1040,ty=1560,w=640,h=400,lift=16,seed=1"
	GAME_HIRES_PACK=$packs/identity
	game_check_render identity-present "$britain,w=320,h=200,scales=6,overrides=yes,expect=identity,present=1,format=both"
	game_check_render identity-plain-walk "$britain,w=320,h=200,scales=6,mode=plain,overrides=yes,present=1,bench=2,walk=2"
	game_check_render identity-resize "$britain,w=355,h=200,game=320x200,scales=6,overrides=yes,expect=identity,resize=off:force3:force2:force6,edit=1"
	GAME_HIRES_PACK=$packs/identity6
	game_check_render identity-reduce-coast "$coast,scales=2:3:6,overrides=yes,expect=identity,repaint=16"
	game_check_render identity-reduce-britain "$britain,w=640,h=400,scales=2:3:6,overrides=yes,expect=identity"
	GAME_HIRES_PACK=$packs/marker
	game_check_render marker-all-coast "$coast,scales=2:6,overrides=yes,expect=marker:1,passes=all,repaint=16"
	game_check_render marker-all-britain "$britain,w=640,h=400,scales=2:6,overrides=yes,expect=marker:1,passes=all"
	game_check_render toggle-britain "$britain,w=640,h=400,scales=2:3:6,overrides=yes,expect=marker:1,passes=all,toggle=1"
	game_check_render toggle-window "$britain,w=355,h=200,game=320x200,scales=2:3:6,overrides=yes,expect=marker:1,passes=flats,toggle=1,repaint=16"
	GAME_HIRES_PACK=$packs/marker-even
	flats=$even_flats
	game_check_render marker-partial-britain "$britain,w=640,h=400,scales=2:3:6,overrides=yes,expect=marker:1,coverage=partial,passes=flats,repaint=16,toggle=1"
	game_check_render marker-partial-coast "$coast,scales=2:6,overrides=yes,expect=marker:1,coverage=partial,passes=flats"
	flats=$all_flats
fi

echo "override_regions: $game_pass passed, $game_fail failed in $(($(date +%s) - start)) s ($GAME_BUILD)"
[ $game_fail -eq 0 ]
