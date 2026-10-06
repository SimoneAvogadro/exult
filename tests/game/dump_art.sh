#!/bin/bash
# tests/game/dump_art.sh - the reference set of --dump-art (DESIGN.md sections 4.2 and 6.4, "Dump"):
#   * two runs give identical trees (determinism, I10; one run in an ASan build);
#   * dump_art_check.py: the tree is complete, the flat CRCs and palette_crc32 equal the values that
#     tools/hires/mkpack_identity.py computes from the game's files, the tables match u7chunks and
#     u7map, and the tile kinds, the fill, the T1 keys, the terrain layers and the usage columns equal
#     its own ports of the engine's rules (T1 and fill parity);
#   * the templates are an identity pack: the store loads every flat with no reject, and O4a
#     (expect=identity) passes at S = 2 and 3 (reduced from x6) and 6;
#   * a second dump into the same directory gives the same tree; a dump that holds a file it did not
#     write, or a changed one (manifest.txt), a non-empty directory that holds no dump, and a
#     directory inside the static directory, are refused (exit code 2) untouched.
# The dumps are derived from the game's art: they live in a scratch directory that is removed at the
# end (unless KEEP_SANDBOX=1 or a case fails).
# Skipped (77) without U7_BG_STATIC.
# Usage: [U7_BG_STATIC=...] [EXULT_WRAPPER=...] dump_art.sh [build-dir]
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

game_require_data
game_require_build "${1:-}"

start=$(date +%s)
check="$game_tests_srcdir/game/dump_art_check.py"
game_make_packs
dumps=$GAME_PACKS
fail=0

# dump <name> <dir> <expected exit code>: --dump-art into dir in a new sandbox.
dump() {
	game_make_sandbox "dump-$1"
	game_run_exult --bg --dump-art "$2"
	local rc=$?
	if [ $rc -ne "$3" ]; then
		echo "FAIL: dump-$1: exit code $rc, expected $3 (log: $GAME_SANDBOX/run.log)" >&2
		grep -E '^--dump-art|Sanitizer|runtime error' "$GAME_SANDBOX/run.log" | head -5 >&2
		GAME_KEEP_PACKS=1
		return 1
	fi
	if ! game_check_log "$GAME_SANDBOX/run.log"; then
		GAME_KEEP_PACKS=1
		return 1
	fi
	echo "ok: dump-$1 (exit code $rc): $(grep -E '^--dump-art' "$GAME_SANDBOX/run.log" | head -1)"
	game_cleanup > /dev/null
	return 0
}

dump first "$dumps/first" 0 || fail=1
if [ $fail -eq 0 ]; then
	python3 "$check" "$U7_BG_STATIC" "$dumps/first" || fail=1
fi
if [ $fail -eq 0 ] && [ "$GAME_ASAN" = 0 ]; then
	dump second "$dumps/second" 0 || fail=1
	if [ $fail -eq 0 ] && ! diff -r "$dumps/first" "$dumps/second" > "$dumps/diff.txt"; then
		echo "FAIL: two dumps differ (determinism):" >&2
		head -5 "$dumps/diff.txt" >&2
		GAME_KEEP_PACKS=1
		fail=1
	fi
	# Again into the first one: the earlier dump is replaced.
	if [ $fail -eq 0 ]; then
		dump again "$dumps/first" 0 || fail=1
		if [ $fail -eq 0 ] && ! diff -r "$dumps/first" "$dumps/second" > /dev/null; then
			echo "FAIL: a dump into an earlier dump differs from a fresh one" >&2
			GAME_KEEP_PACKS=1
			fail=1
		fi
	fi
	# No work is lost: a template added to, or changed in, an earlier dump makes a new dump into it
	# refuse, and the directory stays as it was.
	if [ $fail -eq 0 ]; then
		tmpl=$(find "$dumps/second/templates/x6/flats" -name '*.png' | sort | head -1)
		added=$(dirname "$tmpl")/mine.png
		echo modder > "$added"
		dump refuse-added "$dumps/second" 2 || fail=1
		[ -f "$added" ] || fail=1
		rm -f "$added"
		cp "$tmpl" "$dumps/template.png"
		printf x >> "$tmpl"
		dump refuse-changed "$dumps/second" 2 || fail=1
		[ "$(tail -c 1 "$tmpl")" = x ] || fail=1
		cp "$dumps/template.png" "$tmpl"
		if ! diff -r "$dumps/first" "$dumps/second" > /dev/null; then
			fail=1
		fi
		if [ $fail -ne 0 ]; then
			echo "FAIL: a refused dump into an earlier dump changed it" >&2
			GAME_KEEP_PACKS=1
		fi
	fi
fi

# Refused: a non-empty directory without a dump, and one inside the static directory (a scratch
# static directory of links to the game's files, given with a trailing slash).
mkdir -p "$dumps/other" "$dumps/static"
echo keep > "$dumps/other/keep.txt"
dump refuse-other "$dumps/other" 2 || fail=1
if [ "$(ls -A "$dumps/other")" != keep.txt ]; then
	echo "FAIL: the refused directory changed" >&2
	fail=1
fi
for f in "$U7_BG_STATIC"/*; do
	ln -s "$f" "$dumps/static/"
done
real_static=$U7_BG_STATIC
U7_BG_STATIC="$dumps/static/"
dump refuse-static "$dumps/static/out" 2 || fail=1
U7_BG_STATIC=$real_static
if [ -e "$dumps/static/out" ]; then
	echo "FAIL: --dump-art created a directory inside the static directory" >&2
	fail=1
fi

# The templates as the pack root: every flat loads, and O4a holds.
if [ $fail -eq 0 ]; then
	flats=$(find "$dumps/first/templates/x6/flats" -name '*.png' | wc -l)
	check_store_log() {
		local log=$GAME_SANDBOX/run.log
		if ! grep -q "^\[hires\] x[0-9]*: $flats tiles loaded" "$log" \
			|| grep -E '^\[hires\] x[0-9]*: ' "$log" | grep -vqE "^\[hires\] x[0-9]*: $flats tiles loaded .*, 0 rejected, "; then
			echo "FAIL: the store did not load all $flats templates without a reject:" >&2
			grep '^\[hires\]' "$log" | head -5 >&2
			return 1
		fi
		return 0
	}
	GAME_RUN_CHECK=check_store_log
	GAME_HIRES_PACK=$dumps/first/templates
	game_check_render templates-britain "tx=800,ty=1330,w=640,h=400,lift=16,seed=1,scales=2:3:6,overrides=yes,expect=identity" \
		|| fail=1
	game_check_render templates-coast "tx=1040,ty=1560,w=640,h=400,lift=16,seed=1,scales=6,overrides=yes,expect=identity" \
		|| fail=1
fi

if [ $fail -ne 0 ]; then
	GAME_KEEP_PACKS=1
fi
echo "dump_art: $([ $fail -eq 0 ] && echo PASS || echo FAIL) in $(($(date +%s) - start)) s ($GAME_BUILD)"
[ $fail -eq 0 ]
