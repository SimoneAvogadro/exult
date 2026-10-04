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
	if [ -z "${EXULT_WRAPPER:-}" ] && grep -q __asan_init "$GAME_BUILD/exult"; then
		EXULT_WRAPPER=$game_asan_wrapper
		echo "note: $GAME_BUILD/exult is an ASan build, EXULT_WRAPPER=$EXULT_WRAPPER" >&2
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
	mkdir -p "$GAME_SANDBOX"/{game,patch,mods,source,saves,gamedat,hires,home} || exit 2
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

# Deletes the sandbox of a passing run, unless KEEP_SANDBOX=1.
game_cleanup() {
	if [ "${KEEP_SANDBOX:-0}" = 1 ]; then
		echo "sandbox kept: $GAME_SANDBOX"
	else
		rm -rf "$GAME_SANDBOX"
	fi
}
