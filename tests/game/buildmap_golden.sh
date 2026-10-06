#!/bin/bash
# tests/game/buildmap_golden.sh - oracle O0 (DESIGN.md section 6.4): "exult --buildmap 0|1|2" (the
# 144 superchunk maps at each roof level) with render_scale=art in the config must give the SHA-256
# lists in golden/buildmap{0,1,2}.sha256, which regen_goldens.sh records with build-upstream
# (upstream 8b6ab6b43 + P3 + P11) in this same harness. That proves the scale-1 path byte-identical
# to upstream (invariant I2) and that --buildmap forces S=1.
# The maps depend on heap address order, so the lists are comparable only between runs of this
# script (lib.sh: same sandbox path length, config text and environment). An ASan build is
# skipped: its allocator changes the order (its hashes are never compared, section 6.4).
# Skipped (77) without U7_BG_STATIC.
# Usage: [U7_BG_STATIC=...] [BUILDMAP_LEVELS="0 1 2"] buildmap_golden.sh [build-dir]
#        buildmap_golden.sh --record <dir> [build-dir]    write the lists to <dir> instead
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

record_dir=""
if [ "${1:-}" = --record ]; then
	record_dir=${2:?"--record needs a directory"}
	shift 2
fi

game_require_data
game_require_build "${1:-}"
if [ "$GAME_ASAN" = 1 ]; then
	echo "SKIP: $GAME_BUILD/exult is an ASan build; its buildmap hashes depend on the allocator" >&2
	exit 77
fi

golden_dir="$game_tests_srcdir/game/golden"
fail=0
start=$(date +%s)
for level in ${BUILDMAP_LEVELS:-0 1 2}; do
	game_make_sandbox buildmap art
	game_run_exult --bg --buildmap "$level"
	rc=$?
	count=$(find "$GAME_SANDBOX/saves" -maxdepth 1 -name 'u7map??.png' | wc -l)
	if [ $rc -ne 0 ] || [ "$count" -ne 144 ]; then
		echo "FAIL: exult --buildmap $level: exit code $rc, $count maps (log: $GAME_SANDBOX/run.log)" >&2
		fail=1
		continue
	fi
	if ! game_check_log "$GAME_SANDBOX/run.log" || ! game_check_hires_log "$GAME_SANDBOX/run.log"; then
		fail=1
		continue
	fi
	list=$(cd "$GAME_SANDBOX/saves" && sha256sum u7map??.png | LC_ALL=C sort -k2)
	if [ -n "$record_dir" ]; then
		echo "$list" > "$record_dir/buildmap$level.sha256" || exit 2
		echo "recorded: buildmap $level -> $record_dir/buildmap$level.sha256"
	else
		golden="$golden_dir/buildmap$level.sha256"
		if [ ! -f "$golden" ]; then
			echo "FAIL: no golden list $golden (make regen-goldens)" >&2
			fail=1
			continue
		fi
		if [ "$list" != "$(cat "$golden")" ]; then
			changed=$(diff <(echo "$list") "$golden" | grep -c '^<')
			echo "FAIL: buildmap $level: $changed of 144 maps differ from $golden (sandbox kept: $GAME_SANDBOX)" >&2
			diff <(echo "$list") "$golden" | grep '^<' | head -5 >&2
			fail=1
			continue
		fi
		echo "ok: buildmap $level: 144 maps equal $golden"
	fi
	game_cleanup > /dev/null
done
echo "buildmap_golden: levels ${BUILDMAP_LEVELS:-0 1 2} in $(($(date +%s) - start)) s ($GAME_BUILD)"
exit $fail
