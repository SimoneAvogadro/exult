#!/bin/bash
# tests/world/world_tests.sh - the hi-res render oracles on the synthetic test world
# (tests/data/hires/world: original art made by make_world.py, no EA data), so that they run
# anywhere, CI and cloud machines included, without the Ultima VII files. The world loads as the
# DEVEL game "hirestest" (world.cfg.in); every pack below is made in a scratch directory, except
# the committed sample pack.
#   * the committed world is up to date (make_world.py --check);
#   * NN (O2): regions at S = 2, 3, 6 with repaints, across the map's wrap, with a game area offset,
#     through the window's present read-back (1:1 and 1280x800), the S cycle with a terrain edit,
#     the pushed resize, a short bench and walk;
#   * O4a / O4b with identity and marker packs of the world's flats (mkpack_identity.py): loose
#     x2/x3/x6, an x6 bundle reduced to S = 2 and 3, marker with passes=flats and all, half the
#     tiles (coverage=partial), the toggle;
#   * --dump-art: dump_art_check.py (CRCs, tables, kinds, fill, T1, layers) and determinism;
#   * WP-17: identity terrains from the dump (with identity tiles, and x6 terrains alone, reduced),
#     the precedence oracle (identity tiles + marker terrains, marked=terrain);
#   * the sample pack (real 6x art): the store loads its 16 tiles and 1 terrain with no reject;
#     the inspector names TERRAIN (pond), TILE (grass, water, road group), NN (sand: no file;
#     cobble: disabled group; a rock RLE tile filled from cobble); the S = 6 render equals the
#     recorded digest (world_golden.txt); tools/hires/hirescheck.py agrees (when numpy and Pillow
#     import; WORLD_PYTHON names another python); the window path with a bench and walk;
#   * the developer loop (dev=1, keys=1) and a terrain file that is not a PNG (rejected on decode,
#     F1, the tiles paint).
# Every run is made twice with equal digests (once in an ASan build).
# Usage: [EXULT_WRAPPER=...] [WORLD_RECORD=1] [WORLD_PYTHON=...] world_tests.sh [build-dir]
#   WORLD_RECORD=1 writes the sample digests to world_golden.txt instead of comparing.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../game/lib.sh"

game_require_build "${1:-}"
start=$(date +%s)

world="$game_tests_srcdir/data/hires/world"
mkpack="$game_tests_srcdir/../tools/hires/mkpack_identity.py"
dump_check="$game_tests_srcdir/game/dump_art_check.py"
golden="$game_tests_srcdir/world/world_golden.txt"

python3 "$world/make_world.py" --check || exit 1

game_make_packs
packs=$GAME_PACKS
python3 "$world/make_world.py" --assemble "$packs/static" || exit 2
GAME_STATIC=$packs/static
GAME_CFG_IN=$game_tests_srcdir/world/world.cfg.in
GAME_FLAGS="--game hirestest"
static=$GAME_STATIC
flats=$(sed -n 's/^flats=//p' "$world/world.txt")
pond_key=$(sed -n 's/^terrain 2 pond t1=\([0-9a-f]*\).*/\1/p' "$world/world.txt")

# The store loaded $flats tiles at every scale it loaded, with no reject.
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

centre="tx=24,ty=24,w=320,h=200,lift=16,seed=1"
wrap="tx=3060,ty=3064,w=320,h=200,lift=16,seed=1"
all6="tx=0,ty=0,w=384,h=256,lift=16,seed=1"    # chunks (0..2, 0..1): the six terrains

# NN (O2) and the window paths.
game_check_render nn-centre "$centre,scales=2:3:6,repaint=16"
game_check_render nn-wrap "$wrap,scales=2:6,repaint=8"
game_check_render nn-all "$all6,scales=2:3:6"
game_check_render nn-offset "tx=40,ty=20,w=355,h=200,game=320x200,scales=2:6,repaint=8"
game_check_render nn-present "$centre,scales=6,present=1,format=both"
game_check_render nn-present-scaled "$centre,scales=6,present=1,window=1280x800"
game_check_render nn-resize "tx=40,ty=20,w=355,h=200,game=320x200,scales=6,resize=off:force3:force2:force6,edit=1"
game_check_render nn-pushed-resize "$centre,scales=6,pushed_resize=1"
game_check_render nn-walk "tx=40,ty=20,w=355,h=200,game=320x200,scales=6,present=1,bench=2,walk=3"

# O4a / O4b with packs made from the world's flats.
python3 "$mkpack" "$static" "$packs/identity" || exit 2
python3 "$mkpack" "$static" "$packs/identity6" --scales 6 --bundle || exit 2
python3 "$mkpack" "$static" "$packs/marker" --kind marker --marker 1 || exit 2
python3 "$mkpack" "$static" "$packs/marker-even" --kind marker --marker 1 --subset even || exit 2
GAME_RUN_CHECK=check_store_log
GAME_HIRES_PACK=$packs/identity
game_check_render identity "$all6,scales=2:3:6,overrides=yes,expect=identity,repaint=16"
game_check_render identity-present "$centre,scales=6,overrides=yes,expect=identity,present=1,format=both"
GAME_HIRES_PACK=$packs/identity6
game_check_render identity-reduce "$all6,scales=2:3:6,overrides=yes,expect=identity"
GAME_HIRES_PACK=$packs/marker
game_check_render marker "$all6,scales=2:3:6,overrides=yes,expect=marker:1,passes=flats,repaint=16"
game_check_render marker-all "$all6,scales=2:6,overrides=yes,expect=marker:1,passes=all"
game_check_render toggle "tx=40,ty=20,w=355,h=200,game=320x200,scales=2:3:6,overrides=yes,expect=marker:1,passes=flats,toggle=1,repaint=8"
GAME_RUN_CHECK=
GAME_HIRES_PACK=$packs/marker-even
game_check_render marker-partial "$all6,scales=2:6,overrides=yes,expect=marker:1,coverage=partial,passes=flats,toggle=1"

# --dump-art: the reference set, checked against the world's files, twice the same.
dump_art() {
	game_make_sandbox "dump-$1"
	# shellcheck disable=SC2086
	if ! game_run_exult $GAME_FLAGS --dump-art "$2" || ! game_check_log "$GAME_SANDBOX/run.log"; then
		echo "FAIL: --dump-art $1 (log: $GAME_SANDBOX/run.log)" >&2
		game_keep_pack
		game_fail=$((game_fail + 1))
		return 1
	fi
	game_cleanup > /dev/null
	return 0
}
if dump_art first "$packs/dump" && dump_art second "$packs/dump2"; then
	if ! python3 "$dump_check" "$static" "$packs/dump"; then
		echo "FAIL: dump_art_check.py" >&2
		game_fail=$((game_fail + 1))
	elif ! diff -r "$packs/dump" "$packs/dump2" > /dev/null; then
		echo "FAIL: two dumps differ (determinism)" >&2
		game_fail=$((game_fail + 1))
	else
		echo "ok: dump-art (checked, deterministic)"
		game_pass=$((game_pass + 1))
	fi
fi

# WP-17: per-terrain overrides from the dump's layers.
python3 "$mkpack" "$static" "$packs/terrain" --terrain "$packs/dump" || exit 2
python3 "$mkpack" "$static" "$packs/terrain6" --scales 6 --terrain "$packs/dump" --no-flats || exit 2
python3 "$mkpack" "$static" "$packs/terrain-marker" --terrain "$packs/dump" --terrain-kind marker --marker 1 || exit 2
GAME_HIRES_PACK=$packs/terrain
game_check_render terrain-identity "$all6,scales=2:3:6,overrides=yes,expect=identity,repaint=8"
GAME_HIRES_PACK=$packs/terrain6
game_check_render terrain-reduce "$all6,scales=2:3:6,overrides=yes,expect=identity"
GAME_HIRES_PACK=$packs/terrain-marker
game_check_render terrain-precedence "$all6,scales=2:3:6,overrides=yes,expect=marker:1,marked=terrain,passes=flats,repaint=8"

# The committed sample pack. Tiles of $all6: pond centre (40, 8), meadow (8, 8), beach sand (8, 20)
# and water (8, 28), plaza cobble (24, 24) and a rock (20, 20), road (40, 24).
sample_inspect="inspect=40:8,inspect=8:8,inspect=8:20,inspect=8:28,inspect=24:24,inspect=20:20,inspect=40:24"
expect_inspect() {    # <key> <expected result prefix>
	local got
	got=$(sed -n "s/^  \"inspect_$1_s6\": \"\(.*\)\",\{0,1\}$/\1/p" "$GAME_SANDBOX/out/digest.json")
	case "$got" in
	"$2"*) return 0 ;;
	esac
	echo "FAIL: inspect $1 at S=6 is '$got', expected '$2...'" >&2
	return 1
}
check_sample() {
	local log=$GAME_SANDBOX/run.log d=$GAME_SANDBOX/out/digest.json ok=0 line="" got key
	if ! grep -qE '^\[hires\] x6: 16 tiles loaded \(bundle 0\), 0 rejected, 0 groups skipped, 0 unguarded, 0 warnings, 1 terrains;' "$log"; then
		echo "FAIL: sample pack store line:" >&2
		grep '^\[hires\]' "$log" | head -3 >&2
		ok=1
	fi
	expect_inspect 40_8 "TERRAIN <HIRES>/x6/terrain/$pond_key.png" || ok=1
	expect_inspect 8_8 "TILE <HIRES>/x6/flats/0001_" || ok=1
	expect_inspect 8_20 "NN (no override)" || ok=1
	expect_inspect 8_28 "TILE <HIRES>/x6/flats/0004_" || ok=1
	expect_inspect 24_24 "NN (no override)" || ok=1
	expect_inspect 20_20 "NN (no override)" || ok=1
	expect_inspect 40_24 "TILE <HIRES>/x6/flats/road/0005_" || ok=1
	for key in ref_1x s6_hi; do
		got=$(sed -n "s/^  \"$key\": \"\([0-9a-f]*\)\".*/\1/p" "$d")
		if [ "${WORLD_RECORD:-0}" = 1 ]; then
			line="$line$key $got"$'\n'
		elif ! grep -qx "$key $got" "$golden"; then
			echo "FAIL: sample $key digest $got is not the recorded one ($golden):" >&2
			grep "^$key " "$golden" >&2
			ok=1
		fi
	done
	if [ "${WORLD_RECORD:-0}" = 1 ]; then
		printf '# The sample pack render of world_tests.sh ("sample"): FNV-1a-64 digests of digest.json.\n%s' "$line" > "$golden"
		echo "recorded $golden"
	fi
	return $ok
}
GAME_HIRES_PACK=$world/pack
GAME_RUN_CHECK=check_sample
game_check_render sample "$all6,scales=6,mode=plain,overrides=yes,$sample_inspect"
GAME_RUN_CHECK=
# The offline validator (tools/hires/hirescheck.py, needs numpy and Pillow) agrees with the store.
hirescheck="$game_tests_srcdir/../tools/hires/hirescheck.py"
python=${WORLD_PYTHON:-python3}
if "$python" -c 'import numpy, PIL' 2> /dev/null; then
	if out=$("$python" "$hirescheck" --static "$static" "$world/pack" 2>&1) \
		&& grep -q "tiles 16 loaded 16 rejected 0 groups_skipped 0 unguarded 0 warnings 0" <<< "$out"; then
		echo "ok: hirescheck on the sample pack (16 loaded, 0 rejected)"
		game_pass=$((game_pass + 1))
	else
		echo "FAIL: hirescheck on the sample pack:" >&2
		echo "$out" | head -10 >&2
		game_fail=$((game_fail + 1))
	fi
else
	echo "skip: hirescheck (no numpy/Pillow for $python; set WORLD_PYTHON to a venv's python)"
fi
game_check_render sample-window "tx=40,ty=20,w=355,h=200,game=320x200,scales=6,mode=plain,overrides=yes,present=1,bench=2,walk=4"

# The developer loop: a marker pack whose flats.next holds the identity flats (dev_loop.sh).
python3 "$mkpack" "$static" "$packs/dev" --kind marker --marker 1 --scales 2,6 || exit 2
python3 "$mkpack" "$static" "$packs/dev-next" --scales 2,6 || exit 2
mv "$packs/dev-next/x2/flats" "$packs/dev/x2/flats.next" || exit 2
mv "$packs/dev-next/x6/flats" "$packs/dev/x6/flats.next" || exit 2
GAME_HIRES_PACK=$packs/dev
game_check_render dev-pushed "$centre,scales=2:6,overrides=yes,expect=marker:1,passes=flats,dev=1"
game_check_render dev-window "tx=40,ty=20,w=355,h=200,game=320x200,scales=6,overrides=yes,expect=marker:1,passes=flats,dev=1,keys=1"

# A terrain file that is not a PNG, for the pond's key: rejected (F1) when the pond is painted,
# the tiles paint instead (NN oracle with identity tiles), and the inspector says so.
python3 "$mkpack" "$static" "$packs/bad-terrain" --scales 6 || exit 2
mkdir -p "$packs/bad-terrain/x6/terrain" || exit 2
printf 'not a png\n' > "$packs/bad-terrain/x6/terrain/$pond_key.png" || exit 2
check_bad_terrain() {
	local j=$GAME_SANDBOX/out/inspect.json
	if ! grep -q '"terrain_override": {"result": "NN", "reason": "rejected: F1 unreadable PNG: not a PNG (bad signature)"' "$j"; then
		echo "FAIL: the inspector does not report the F1 reject of the terrain file:" >&2
		grep terrain_override "$j" | head -2 >&2
		return 1
	fi
	expect_inspect 40_8 "TILE <HIRES>/x6/flats/0004/0004_"
}
GAME_HIRES_PACK=$packs/bad-terrain
GAME_RUN_CHECK=check_bad_terrain
game_check_render bad-terrain "$all6,scales=6,overrides=yes,expect=identity,inspect=40:8"
GAME_RUN_CHECK=

echo "world_tests: $game_pass passed, $game_fail failed in $(($(date +%s) - start)) s ($GAME_BUILD)"
[ $game_fail -eq 0 ]
