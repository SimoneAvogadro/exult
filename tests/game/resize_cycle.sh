#!/bin/bash
# tests/game/resize_cycle.sh - the S transitions of the window's own buffer (invariant I12,
# DESIGN.md sections 6.4 and 12.3):
#   * the S cycle force6 -> off -> force3 -> force6 through Game_window::resized with a 320x200 game
#     area in a 355x200 full area; after each step I12 holds and the render is the NN upscale of the
#     scale-1 reference; then a terrain edit at S=6 (set_flat + commit_edits re-renders the flats
#     cache at S) is rendered and compared again;
#   * the same terrain edit in pushed targets at S = 3 and 6;
#   * pushed_resize: resized() and two fullscreen toggles while a layer is the render target leave
#     the layer's bits and pixels alone, and the window's buffer satisfies I12 afterwards.
# Every run fails on a "[hires] mixed-scale" log line (a flats cache of another scale reached a
# target) and on any sanitizer report. Run it in the ASan build too (make check-game there, or
# EXULT_WRAPPER as in lib.sh). Non-ASan builds run each case twice and compare the digests.
# Skipped (77) without U7_BG_STATIC.
# Usage: [U7_BG_STATIC=...] [EXULT_WRAPPER=...] resize_cycle.sh [build-dir]
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

game_require_data
game_require_build "${1:-}"

start=$(date +%s)

britain="tx=800,ty=1330,lift=16,seed=1"
game_check_render s-cycle "$britain,w=355,h=200,game=320x200,scales=6,resize=force6:off:force3:force6,edit=1,repaint=8"
game_check_render edit-pushed "tx=1700,ty=820,w=320,h=200,lift=16,seed=1,scales=3:6,edit=1"
game_check_render pushed-resize "$britain,w=320,h=200,scales=6,pushed_resize=1"

echo "resize_cycle: $game_pass passed, $game_fail failed in $(($(date +%s) - start)) s ($GAME_BUILD)"
[ $game_fail -eq 0 ]
