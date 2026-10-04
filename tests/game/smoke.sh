#!/bin/bash
# tests/game/smoke.sh - checks the game-data harness: exult renders "--buildmap 0" (the 144
# superchunk maps) headless from a sandbox (lib.sh), exits with 0 and prints no sanitizer report.
# Skipped (77) without U7_BG_STATIC.
# Usage: [U7_BG_STATIC=...] [EXULT_WRAPPER=...] smoke.sh [build-dir]   (default: EXULT_BUILD_DIR)
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

game_require_data
game_require_build "${1:-}"
game_make_sandbox smoke

start=$(date +%s)
game_run_exult --bg --buildmap 0
rc=$?
count=$(find "$GAME_SANDBOX/saves" -maxdepth 1 -name 'u7map??.png' | wc -l)
if [ $rc -ne 0 ] || [ "$count" -ne 144 ]; then
	echo "FAIL: exult --buildmap 0: exit code $rc, $count maps (log: $GAME_SANDBOX/run.log)" >&2
	exit 1
fi
game_check_log "$GAME_SANDBOX/run.log" || exit 1
echo "smoke: 144 maps in $(($(date +%s) - start)) s ($GAME_BUILD)"
game_cleanup
