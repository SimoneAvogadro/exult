# tests/game/lib.sh - helpers shared by the game-data test scripts (sourced, not run).
#
# Environment:
#   U7_BG_STATIC     the "static" directory of a Black Gate installation, read only. When it is
#                    unset, every script exits with 77 (skipped).
#   EXULT_BUILD_DIR  the build tree whose exult and data are tested ("make check-game" sets it).
#   EXULT_WRAPPER    prefixed to every exult call. When it is empty and exult is an ASan build,
#                    game_require_build sets it to $game_asan_wrapper (below, DESIGN.md section 6.5):
#                    ASLR off and LeakSanitizer off. An explicit value is used as it is.
#   HIRES_TEST_TMP   where the sandboxes are created; default: "tmp" next to the build tree.
#   GAME_TIMEOUT     seconds before an exult run is killed (default 1800).
#   GAME_HIRES_PACK  a pack root (absolute path) that new sandboxes use as their <HIRES> root:
#                    the sandbox's hires directory becomes a link to it. Empty: an empty root.
#   KEEP_SANDBOX=1   keep the sandbox of a passing run (it holds EA-derived images: never commit).
#
# The game data stays untouched: the config (test.cfg.in) points every writable path (game,
# patch, mods, source, saves, gamedat, hires, HOME) into the sandbox. Rendered output depends on
# heap address order, so runs are comparable only when they come from the same harness
# (DESIGN.md section 6.4). Therefore nothing of the caller or of the build tree reaches exult:
# mktemp keeps the sandbox path length constant, the build's exult and data are linked into the
# sandbox (argv[0] and data_path name sandbox paths only), the config is written per sandbox, and
# exult runs from the sandbox under env -i with a fixed environment (SDL3 copies every
# environment variable onto the heap). Keep HIRES_TEST_TMP the same for runs that are compared.

game_tests_srcdir=${HIRES_TESTS_SRCDIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}

# The wrapper of an ASan exult (DESIGN.md section 6.5): ASLR off, because the g++ 9.4 runtime hangs
# at random under WSL2's ASLR; LeakSanitizer off, because leaks at exit are not test failures; fatal
# UBSan reports. game_run_exult adds the timeout.
game_asan_wrapper="env ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 setarch $(uname -m) -R"

# Exits with 77 (skipped) when U7_BG_STATIC is unset, with an error when it is not a BG static dir.
game_require_data() {
	if [ -z "${U7_BG_STATIC:-}" ]; then
		echo "SKIP: U7_BG_STATIC is not set" >&2
		exit 77
	fi
	if [ ! -f "$U7_BG_STATIC/initgame.dat" ]; then
		echo "ERROR: U7_BG_STATIC=$U7_BG_STATIC has no initgame.dat" >&2
		exit 2
	fi
}

# Sets GAME_BUILD to the absolute path of the build tree ($1, else EXULT_BUILD_DIR). For an ASan
# exult it sets an empty EXULT_WRAPPER to $game_asan_wrapper, so that a bare "make check-game"
# in an ASan tree does not run exult with ASLR and leak checks on.
game_require_build() {
	local build=${1:-${EXULT_BUILD_DIR:-}}
	if [ -z "$build" ]; then
		echo "ERROR: no build tree (pass it, or set EXULT_BUILD_DIR)" >&2
		exit 2
	fi
	GAME_BUILD=$(cd "$build" && pwd) || exit 2
	if [ ! -x "$GAME_BUILD/exult" ] || [ ! -f "$GAME_BUILD/data/exult.flx" ]; then
		echo "ERROR: no exult or data/exult.flx in $GAME_BUILD" >&2
		exit 2
	fi
	GAME_ASAN=0
	if grep -q __asan_init "$GAME_BUILD/exult"; then
		GAME_ASAN=1
		if [ -z "${EXULT_WRAPPER:-}" ]; then
			EXULT_WRAPPER=$game_asan_wrapper
			echo "note: $GAME_BUILD/exult is an ASan build, EXULT_WRAPPER=$EXULT_WRAPPER" >&2
		fi
	fi
}

# Creates the sandbox test-<name>.XXXXXX and sets GAME_SANDBOX. $2 is the config's
# render_scale (default off).
game_make_sandbox() {
	local name=$1
	local render_scale=${2:-off}
	local tmp=${HIRES_TEST_TMP:-$(dirname "$GAME_BUILD")/tmp}
	mkdir -p "$tmp" || exit 2
	GAME_SANDBOX=$(mktemp -d "$tmp/test-$name.XXXXXX") || exit 2
	mkdir -p "$GAME_SANDBOX"/{game,patch,mods,source,saves,gamedat,home} || exit 2
	if [ -n "${GAME_HIRES_PACK:-}" ]; then
		ln -s "$GAME_HIRES_PACK" "$GAME_SANDBOX/hires" || exit 2
	else
		mkdir "$GAME_SANDBOX/hires" || exit 2
	fi
	ln -s "$GAME_BUILD/exult" "$GAME_SANDBOX/exult" || exit 2
	ln -s "$GAME_BUILD/data" "$GAME_SANDBOX/data" || exit 2
	sed -e "s|@SANDBOX@|$GAME_SANDBOX|g" -e "s|@BG_STATIC@|$U7_BG_STATIC|g" \
		-e "s|@RENDER_SCALE@|$render_scale|g" \
		"$game_tests_srcdir/game/test.cfg.in" > "$GAME_SANDBOX/exult.cfg" || exit 2
}

# Runs the sandbox's exult with the sandbox config and the given arguments, from the sandbox,
# under env -i. The output goes to $GAME_SANDBOX/run.log; returns exult's exit code.
game_run_exult() {
	# EXULT_WRAPPER is split into words on purpose.
	# shellcheck disable=SC2086
	(cd "$GAME_SANDBOX" && exec env -i PATH=/usr/bin:/bin HOME="$GAME_SANDBOX/home" \
		SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
		timeout "${GAME_TIMEOUT:-1800}" ${EXULT_WRAPPER:-} \
		"$GAME_SANDBOX/exult" -c "$GAME_SANDBOX/exult.cfg" "$@") > "$GAME_SANDBOX/run.log" 2>&1
}

# Fails when the log holds a sanitizer report. A UBSan report alone does not change the exit
# code unless the build stops on it, so the exit code is not enough.
game_check_log() {
	if grep -qE 'runtime error:|(ERROR|WARNING|SUMMARY): [A-Za-z]+Sanitizer|[A-Za-z]+Sanitizer:DEADLYSIGNAL' "$1"; then
		echo "FAIL: sanitizer report in $1" >&2
		return 1
	fi
	return 0
}

# Fails when the log shows a flats cache blitted into a target of another scale (a stale-scale
# cache, DESIGN.md section 12.3 W6-3) or a hi-res fall-back to S=1.
game_check_hires_log() {
	if grep -qE '\[hires\] (mixed-scale|the S=[0-9]+ world surfaces could not be created)' "$1"; then
		echo "FAIL: [hires] mixed-scale or fall-back line in $1:" >&2
		grep -E '\[hires\] (mixed-scale|the S=)' "$1" | head -5 >&2
		return 1
	fi
	return 0
}

# Runs "exult --bg --render-test <spec>,out=<sandbox>/out" in a new sandbox named $1 (render_scale
# off in the config: the spec sets the scale) and checks the exit code and the log. Sets
# GAME_SANDBOX; the digest is $GAME_SANDBOX/out/digest.json. Returns 0 on a pass.
game_render_test() {
	local name=$1
	local spec=$2
	game_make_sandbox "$name"
	mkdir -p "$GAME_SANDBOX/out" || exit 2
	game_run_exult --bg --render-test "$spec,out=$GAME_SANDBOX/out"
	local rc=$?
	if [ $rc -ne 0 ]; then
		echo "FAIL: --render-test \"$spec\": exit code $rc (log: $GAME_SANDBOX/run.log)" >&2
		grep -E '^\[render-test\] FAIL|^--render-test:|Sanitizer|runtime error' "$GAME_SANDBOX/run.log" | head -10 >&2
		return 1
	fi
	game_check_log "$GAME_SANDBOX/run.log" || return 1
	game_check_hires_log "$GAME_SANDBOX/run.log" || return 1
	return 0
}

# A sandbox that links to GAME_HIRES_PACK is kept: keep the packs too.
game_keep_pack() {
	if [ -n "${GAME_HIRES_PACK:-}" ]; then
		GAME_KEEP_PACKS=1
	fi
}

# game_make_packs: creates the scratch directory GAME_PACKS for packs made from the game's art
# (GAME_HIRES_PACK points into it). It is removed when the script exits, unless a sandbox that
# links into it was kept (a failing game_check_render sets GAME_KEEP_PACKS=1; scripts that keep
# sandboxes otherwise set it too) or KEEP_SANDBOX=1, so a kept sandbox can be run again.
game_make_packs() {
	local tmp=${HIRES_TEST_TMP:-$(dirname "$GAME_BUILD")/tmp}
	mkdir -p "$tmp" || exit 2
	GAME_PACKS=$(mktemp -d "$tmp/packs.XXXXXX") || exit 2
	GAME_KEEP_PACKS=0
	trap game_remove_packs EXIT
}

game_remove_packs() {
	if [ "${GAME_KEEP_PACKS:-0}" = 1 ] || [ "${KEEP_SANDBOX:-0}" = 1 ]; then
		echo "packs kept: $GAME_PACKS (kept sandboxes link to them; derived from the game's art: never commit)" >&2
	else
		rm -rf "$GAME_PACKS"
	fi
}

# The reproducible part of a digest.json: everything but the timings.
game_digest_stable() {
	grep -vE '"(time|bench)_' "$1"
}

# game_check_render <name> <spec>: game_render_test, made twice (once in an ASan build, whose heap
# order follows the environment) with equal digests apart from the timings (determinism, I10).
# When GAME_RUN_CHECK names a command, it runs after each passing run (GAME_SANDBOX is set) and
# must succeed too. Counts into game_pass and game_fail.
game_pass=0
game_fail=0
game_check_render() {
	local name=$1
	local spec=$2
	local runs=2
	local first=""
	local run digest
	if [ "${GAME_ASAN:-0}" = 1 ]; then
		runs=1
	fi
	for run in $(seq 1 $runs); do
		if ! game_render_test "$name" "$spec" || { [ -n "${GAME_RUN_CHECK:-}" ] && ! $GAME_RUN_CHECK; }; then
			echo "FAIL: $name (run $run): $spec (sandbox kept: $GAME_SANDBOX)" >&2
			game_keep_pack
			game_fail=$((game_fail + 1))
			return 1
		fi
		digest=$(game_digest_stable "$GAME_SANDBOX/out/digest.json")
		if [ "$run" = 1 ]; then
			first=$digest
		elif [ "$digest" != "$first" ]; then
			echo "FAIL: $name: the digests of run 1 and run $run differ (sandbox kept: $GAME_SANDBOX):" >&2
			diff <(echo "$first") <(echo "$digest") | head -10 >&2
			game_keep_pack
			game_fail=$((game_fail + 1))
			return 1
		fi
		game_cleanup > /dev/null
	done
	echo "ok: $name ($runs runs): $spec"
	game_pass=$((game_pass + 1))
	return 0
}

# Deletes the sandbox of a passing run, unless KEEP_SANDBOX=1.
game_cleanup() {
	if [ "${KEEP_SANDBOX:-0}" = 1 ]; then
		echo "sandbox kept: $GAME_SANDBOX"
	else
		rm -rf "$GAME_SANDBOX"
	fi
}
