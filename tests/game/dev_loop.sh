#!/bin/bash
# tests/game/dev_loop.sh - the hi-res developer loop (DESIGN.md sections 3.8, 5.7, 6.7 WP-10) through
# --render-test, with packs that tools/hires/mkpack_identity.py makes from the game's own flats in a
# scratch directory (removed at the end unless a failing case keeps its sandbox; they are derived
# from the game's art):
#   * dev=1: the toggle and reload actions of the keys (hires_dev.cc), then the .reload poll: no
#     reload without a change, a reload within 2 s of a touch after the pack's x<S>/flats (marker)
#     and x<S>/flats.next (identity) are swapped, the identity render (NN), the swap back, the
#     marker render again, and no reload with dev mode off; in pushed buffers at S = 2 (the x2
#     trigger) and 6 (the x6 = art scale trigger) and in the window's buffer with a game area
#     offset in the full area;
#   * keys=1 (window case): Ctrl-Alt-O, R and I as SDL key events through the game's key bindings
#     (keys.cc, defaultkeys.txt): nothing with cheats off; nothing with dev mode off; toggle,
#     reload and the inspector's text on the clipboard with cheats and dev mode on;
#   * inspect: the inspector (explain_at) for four known tiles of Britain at S = 2 and 6 with an
#     identity pack of the even tiles, one unreadable PNG and one terrain file: a flat tile (TILE),
#     an RLE tile filled from a neighbour (TILE of the source), a tile without an override (NN), an
#     RLE tile whose source is rejected (NN, F1), the terrain file (not a PNG: rejected, F1, when
#     its terrain is painted, so the tiles paint; WP-17); compared
#     with golden/inspect.json (no pixels: shape, frame and terrain numbers, T1 keys, pack paths);
#     again with hires_path ending in "//." (as get_system_path leaves a trailing separator on
#     Windows: "\."), which must give the same <HIRES>/... paths.
# Every run is made twice with equal digests (once in an ASan build).
# Skipped (77) without U7_BG_STATIC or tools/hires/mkpack_identity.py.
# Usage: [U7_BG_STATIC=...] [EXULT_WRAPPER=...] [INSPECT_RECORD=1] dev_loop.sh [build-dir]
#   INSPECT_RECORD=1 writes the inspector's output to golden/inspect.json instead of comparing.
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
# dev: the marker pack (x2 and x6: a reduced marker would vanish), with the identity flats as
# flats.next (swapped in by the test; at S = 2 through x2/.reload, at S = 6 through x6/.reload).
python3 "$mkpack" "$U7_BG_STATIC" "$packs/dev" --kind marker --marker 1 --scales 2,6 || exit 2
python3 "$mkpack" "$U7_BG_STATIC" "$packs/identity" --scales 2,6 || exit 2
mv "$packs/identity/x2/flats" "$packs/dev/x2/flats.next" || exit 2
mv "$packs/identity/x6/flats" "$packs/dev/x6/flats.next" || exit 2
# inspect: the even tiles, an unreadable 0021_02 (odd: not in the pack otherwise) and a terrain
# file for the T1 key of terrain 471 (not a PNG: rejected when that terrain is painted, WP-17).
python3 "$mkpack" "$U7_BG_STATIC" "$packs/inspect" --scales 6 --subset even || exit 2
printf 'not a png\n' > "$packs/inspect/x6/flats/0021_02.png" || exit 2
mkdir -p "$packs/inspect/x6/terrain" || exit 2
printf 'not decoded\n' > "$packs/inspect/x6/terrain/005dde822ea6a3c8.png" || exit 2

britain="tx=800,ty=1330,lift=16,seed=1"

GAME_HIRES_PACK=$packs/dev
game_check_render dev-pushed "$britain,scales=2:6,overrides=yes,expect=marker:1,passes=flats,dev=1"
game_check_render dev-window "$britain,w=355,h=200,game=320x200,scales=6,overrides=yes,expect=marker:1,passes=flats,dev=1,keys=1"

golden="$game_tests_srcdir/game/golden/inspect.json"
check_inspect() {
	local got=$GAME_SANDBOX/out/inspect.json
	if [ "${INSPECT_RECORD:-0}" = 1 ]; then
		cp "$got" "$golden" || return 1
		echo "recorded $golden"
		return 0
	fi
	if ! diff -u "$golden" "$got" >&2; then
		echo "FAIL: $got differs from $golden" >&2
		return 1
	fi
	return 0
}
GAME_HIRES_PACK=$packs/inspect
GAME_RUN_CHECK=check_inspect
inspect_spec="$britain,scales=2:6,overrides=yes,inspect=810:1340,inspect=815:1336,inspect=850:1360,inspect=800:1331"
game_check_render inspect "$inspect_spec"
if [ "${INSPECT_RECORD:-0}" != 1 ]; then
	GAME_HIRES_SUFFIX=//.
	game_check_render inspect-separators "$inspect_spec"
	GAME_HIRES_SUFFIX=
fi
GAME_RUN_CHECK=

echo "dev_loop: $game_pass passed, $game_fail failed in $(($(date +%s) - start)) s ($GAME_BUILD)"
[ $game_fail -eq 0 ]
