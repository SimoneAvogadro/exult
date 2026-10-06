#!/bin/bash
# tests/game/perf.sh - the world paint at S=6 (DESIGN.md section 6.4, Perf): --render-test bench=200
# renders Britain into the window's buffer 200 times (warm flats caches) and reports the median and
# p95 of the paint, the tracked upload and the present (software renderer here: the upload and
# present numbers are for comparison between runs only). Gates:
#   * paint p95 of the 320x200 view at S=6 <= PERF_GATE_MS (2.0 ms);
#   * every paint p95 at most 20 % (and 0.05 ms, the timer noise) above this host's line in
#     perf_baseline.json, when there is one.
# A failing gate is measured once more before it counts (timing noise). Only -O2 (or -O3) builds
# without sanitizers are measured; others are skipped (77), as is a run without U7_BG_STATIC.
# Usage: [U7_BG_STATIC=...] perf.sh [build-dir]
#        perf.sh --record [build-dir]    write this host's line into perf_baseline.json
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

record=0
if [ "${1:-}" = --record ]; then
	record=1
	shift
fi

game_require_data
game_require_build "${1:-}"
if [ "$GAME_ASAN" = 1 ] || ! grep -qE '^OPT_LEVEL = *-O[23]' "$GAME_BUILD/Makefile" 2>/dev/null; then
	echo "SKIP: $GAME_BUILD is not an -O2 build without sanitizers" >&2
	exit 77
fi

baseline="$game_tests_srcdir/game/perf_baseline.json"
host=$(uname -n)
gate=${PERF_GATE_MS:-2.0}
# name|spec; the digest keys are bench_s6_<what>_{median,p95}_ms.
cases="view320x200|tx=800,ty=1330,w=320,h=200,lift=16,seed=1,scales=6,present=1,bench=200
view860x300|tx=780,ty=1320,w=860,h=300,lift=16,seed=1,scales=6,present=1,bench=200"

value() {    # value <digest> <key>
	sed -n "s/^ *\"$2\": \"\([0-9.]*\)\".*/\1/p" "$1"
}

baseline_value() {    # baseline_value <case> <key>
	[ -f "$baseline" ] || return 0
	sed -n "s/^ *\"$host\/$1\/$2\": *\([0-9.]*\).*/\1/p" "$baseline"
}

results=""
fail=0
while IFS='|' read -r name spec; do
	ok=0
	for attempt in 1 2; do
		case_results=""
		if ! game_render_test "perf-$name" "$spec"; then
			fail=1
			break
		fi
		digest="$GAME_SANDBOX/out/digest.json"
		line="$name:"
		bad=""
		for what in paint upload present; do
			med=$(value "$digest" "bench_s6_${what}_median_ms")
			p95=$(value "$digest" "bench_s6_${what}_p95_ms")
			line="$line $what median $med p95 $p95 ms;"
			case_results="$case_results$name/${what}_p95_ms $p95
"
			if [ "$what" = paint ]; then
				if [ "$name" = view320x200 ] && awk -v v="$p95" -v g="$gate" 'BEGIN { exit !(v > g) }'; then
					bad="$bad paint p95 $p95 ms > $gate ms;"
				fi
				base=$(baseline_value "$name" paint_p95_ms)
				if [ -n "$base" ] && awk -v v="$p95" -v b="$base" 'BEGIN { exit !(v > 1.2 * b && v > b + 0.05) }'; then
					bad="$bad paint p95 $p95 ms > 1.2 x baseline $base ms;"
				fi
			fi
		done
		echo "perf $line (attempt $attempt, $host)"
		game_cleanup > /dev/null
		if [ -z "$bad" ]; then
			ok=1
			break
		fi
		echo "perf: $name:$bad" >&2
	done
	results="$results${case_results:-}"
	if [ $ok = 0 ]; then
		echo "FAIL: perf $name" >&2
		fail=1
	fi
done <<< "$cases"

if [ $record = 1 ] && [ $fail = 0 ]; then
	# One line per host, case and value; other hosts' lines are kept.
	tmpfile=$(mktemp "$baseline.XXXXXX") || exit 2
	{
		echo "{"
		{
			[ -f "$baseline" ] && grep -E '^ *"[^"]+/[^"]+/[^"]+":' "$baseline" | grep -v "^ *\"$host/" | sed 's/,$//'
			echo "$results" | grep -E 'paint_p95_ms' | while read -r key val; do
				echo "  \"$host/$key\": $val"
			done
		} | LC_ALL=C sort | sed '$!s/$/,/'
		echo "}"
	} > "$tmpfile" && mv "$tmpfile" "$baseline"
	echo "recorded: $baseline"
fi
exit $fail
