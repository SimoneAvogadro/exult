#!/bin/bash
# tests/game/perf.sh - the world paint at S=6 (DESIGN.md section 6.4, Perf): --render-test bench=200
# renders Britain into the window's buffer 200 times (warm flats caches) and reports the median and
# p95 of the paint, the tracked upload and the present (software renderer here: the upload and
# present numbers are for comparison between runs only). Gates:
#   * paint p95 of the 320x200 view at S=6 <= PERF_GATE_MS (2.0 ms);
#   * every paint p95 at most 20 % (and 0.05 ms, the timer noise) above this host's line in
#     perf_baseline.json, when there is one;
#   * cold render_flats with per-tile art (an identity x6 pack from tools/hires/mkpack_identity.py,
#     in a scratch directory): the p95 of one flats cache's paint_flats with its overrides, over
#     every cache of the 320x200 view at S=6 timed 50 times, <= PERF_FLATS_GATE_MS (1.0 ms); the
#     digest key is bench_s6_render_flats_p95_ms. The render test also fails a warm paint that
#     renders a flats cache, and a cold paint (an overrides toggle changes Hires::generation())
#     that does not render every cache of the view.
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

# Cold render_flats with per-tile art.
flats_gate=${PERF_FLATS_GATE_MS:-1.0}
mkpack="$game_tests_srcdir/../tools/hires/mkpack_identity.py"
game_make_packs
packs=$GAME_PACKS
if ! [ -f "$mkpack" ]; then
	echo "note: perf flats320x200 skipped: $mkpack not found" >&2
elif ! python3 "$mkpack" "$U7_BG_STATIC" "$packs/identity" --scales 6 > /dev/null; then
	echo "FAIL: perf flats: mkpack_identity.py" >&2
	fail=1
else
	GAME_HIRES_PACK=$packs/identity
	ok=0
	for attempt in 1 2; do
		if ! game_render_test perf-flats "tx=800,ty=1330,w=320,h=200,lift=16,seed=1,scales=6,overrides=yes,expect=identity,bench=50"; then
			echo "(sandbox kept: $GAME_SANDBOX)" >&2
			game_keep_pack
			break
		fi
		per_chunk=$(value "$GAME_SANDBOX/out/digest.json" bench_s6_render_flats_p95_ms)
		caches=$(sed -n 's/^ *"bench_s6_caches": "\([0-9]*\)".*/\1/p' "$GAME_SANDBOX/out/digest.json")
		cold=$(value "$GAME_SANDBOX/out/digest.json" bench_s6_cold_paint_p95_ms)
		echo "perf flats320x200: cold paint p95 $cold ms, render_flats with overrides p95 $per_chunk ms per cache ($caches caches) (attempt $attempt, $host)"
		game_cleanup > /dev/null
		if [ -n "$per_chunk" ] && awk -v v="$per_chunk" -v g="$flats_gate" 'BEGIN { exit !(v <= g) }'; then
			ok=1
			break
		fi
		echo "perf: flats320x200: render_flats p95 ${per_chunk:-?} ms > $flats_gate ms" >&2
	done
	unset GAME_HIRES_PACK
	if [ $ok = 0 ]; then
		echo "FAIL: perf flats320x200" >&2
		fail=1
	fi
fi

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
