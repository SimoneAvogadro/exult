#!/bin/bash
# tools/hires/ci.sh - local CI of the hi-res fork (docs/hires/DESIGN.md, section 6.6).
#
# Steps, each under a timeout, one build tree at a time:
#   1. bootstrap: install the pinned Python packages (requirements.txt) into tools-venv with uv;
#   2. for each lane (default: o2 asan sdl32): configure the tree if it has no config.status
#      (the commands of DESIGN.md section 7.1), make, make check. build-asan runs all three under
#      ASAN_RUN (ASLR off, a timeout, leak checks off for the tools the build runs; the tests get
#      their own options from the TEST_WRAPPER recorded at configure time). build-sdl32 must
#      resolve SDL 3.2.14 from deps/prefix-3.2;
#      Then make check-world (the oracles on the synthetic test world, no game data) in every
#      lane that passed, build-asan under ASAN_RUN;
#   3. the build-list lint (check_build_lists.py --strict);
#   4. pytest tools/hires/tests (data-dependent tests skip themselves without U7_BG_STATIC);
#   5. only with U7_BG_STATIC: make check-game in build-o2, and in build-asan under ASAN_RUN with
#      EXULT_WRAPPER; a lane whose configure, make or lane check failed gets no game step.
# A failing step runs once more. On the development machine (unstable RAM under load) a failure
# that does not repeat is a suspected hardware fault: it is reported as FLAKY, never as a pass.
#
# Usage: tools/hires/ci.sh [--lanes "o2 asan sdl32"] [--jobs N]
# Environment: ROOT (default: the directory that holds the source tree), JOBS (default 6),
#              U7_BG_STATIC (enables step 5), UV (default: uv on PATH, else ~/.local/bin/uv).
# Output: every step's output in $ROOT/tmp/ci-<date>.log, one summary line on stdout.
# Exit status: 0 all steps passed, 1 a step failed, 3 a step failed once and passed on the
# retry (FLAKY), 2 usage error or another ci.sh is running.
set -u

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ROOT=${ROOT:-$(dirname "$SRC")}
JOBS=${JOBS:-6}
LANES="o2 asan sdl32"
while [ $# -gt 0 ]; do
	case $1 in
	--lanes) LANES=$2; shift 2 ;;
	--jobs) JOBS=$2; shift 2 ;;
	-h | --help) sed -n '2,/^set -u/{/^#/p}' "${BASH_SOURCE[0]}"; exit 0 ;;
	*) echo "ci.sh: unknown argument $1" >&2; exit 2 ;;
	esac
done

DEPS=$ROOT/deps
VENV=$ROOT/tools-venv
mkdir -p "$ROOT/tmp" || exit 2
LOG=$ROOT/tmp/ci-$(date +%Y%m%d-%H%M%S).log
[ -e "$LOG" ] && LOG=${LOG%.log}-$$.log    # Two runs in the same second.

# One CI run at a time: the lanes share the source tree (the data build writes into it), and the
# machine must not be loaded with two builds.
exec 9> "$ROOT/tmp/ci.lock"
if ! flock -n 9; then
	echo "ci.sh: another ci.sh is running ($ROOT/tmp/ci.lock)" >&2
	exit 2
fi

# shellcheck source=/dev/null
source "$DEPS/env.sh" || exit 2
UV=${UV:-$(command -v uv || echo "$HOME/.local/bin/uv")}

COMMON="--disable-exult-studio --disable-gimp-plugin --disable-aseprite-plugin --disable-shp-thumbnailer"
SYSROOT_PC=$DEPS/sysroot/usr/lib/x86_64-linux-gnu/pkgconfig:$DEPS/sysroot/usr/share/pkgconfig
PC34=$DEPS/prefix/lib/pkgconfig:$SYSROOT_PC
PC32=$DEPS/prefix-3.2/lib/pkgconfig:$SYSROOT_PC
UBSAN_OPTS=halt_on_error=1:print_stacktrace=1
ASAN_RUN=(env ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=$UBSAN_OPTS setarch x86_64 -R)
TEST_WRAPPER_ASAN="env ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=$UBSAN_OPTS setarch x86_64 -R timeout 900"
EXULT_WRAPPER_ASAN="env ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=$UBSAN_OPTS setarch x86_64 -R timeout 1800"

passed=()
failed=()
flaky=()
skipped=()
built=" "    # The lanes whose configure, make and lane check passed.

log() {
	echo "[$(date '+%F %T')] $*" >> "$LOG"
}

# step NAME COMMAND...: runs the command with its output in the log; once more if it fails.
step() {
	local name=$1
	shift
	local start
	start=$(date +%s)
	log "=== $name: $*"
	"$@" >> "$LOG" 2>&1
	local rc=$?
	if [ $rc -ne 0 ]; then
		log "--- $name failed (exit code $rc), running it once more"
		"$@" >> "$LOG" 2>&1
		local rc2=$?
		if [ $rc2 -eq 0 ]; then
			log "--- $name: FLAKY (failed once, then passed): suspected hardware fault or intermittent bug"
			flaky+=("$name")
			return 0
		fi
		log "--- $name: FAILED again (exit code $rc2) after $(($(date +%s) - start)) s"
		failed+=("$name")
		return 1
	fi
	log "--- $name: ok in $(($(date +%s) - start)) s"
	passed+=("$name")
	return 0
}

in_dir() {
	local dir=$1
	shift
	(cd "$dir" && "$@")
}

bootstrap() {
	timeout 600 "$UV" pip install --python "$VENV/bin/python" -r "$SRC/tools/hires/requirements.txt" &&
		timeout 120 "$VENV/bin/python" -c 'import numpy, PIL, pytest; print("pytest", pytest.__version__)'
}

autoreconf_if_needed() {
	[ -x "$SRC/configure" ] || (cd "$SRC" && timeout 900 autoreconf -v -i)
}

configure_lane() {
	local lane=$1
	local build=$ROOT/build-$lane
	[ -f "$build/config.status" ] && return 0
	mkdir -p "$build" || return 1
	case $lane in
	o2)
		in_dir "$build" timeout 1800 "$SRC/configure" --prefix="$ROOT/install-o2" $COMMON \
			--with-optimization=normal --with-debug=symbols PKG_CONFIG_PATH="$PC34"
		;;
	asan)
		in_dir "$build" "${ASAN_RUN[@]}" timeout 1800 "$SRC/configure" $COMMON \
			--with-optimization=light --with-debug=symbols \
			CXXFLAGS="-g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wno-duplicated-branches" \
			LDFLAGS="-fsanitize=address,undefined" TEST_WRAPPER="$TEST_WRAPPER_ASAN" PKG_CONFIG_PATH="$PC34"
		;;
	sdl32)
		in_dir "$build" timeout 1800 "$SRC/configure" $COMMON --with-optimization=normal \
			LDFLAGS="-Wl,-rpath,$DEPS/prefix-3.2/lib" LIBS="-Wl,--disable-new-dtags" \
			PKG_CONFIG_PATH="$PC32" PKG_CONFIG_LIBDIR="$SYSROOT_PC"
		;;
	*)
		echo "unknown lane $lane"
		return 1
		;;
	esac
}

# The checks that the lane is the one DESIGN.md section 7.1 describes.
check_lane() {
	local lane=$1
	local build=$ROOT/build-$lane
	case $lane in
	asan)
		timeout 60 grep -q '^TEST_WRAPPER = .*setarch x86_64 -R timeout' "$build/tests/Makefile" || {
			echo "build-asan: make check would run the tests without the setarch/timeout wrapper"
			return 1
		}
		;;
	sdl32)
		local version
		version=$(PKG_CONFIG_PATH="$PC32" PKG_CONFIG_LIBDIR="$SYSROOT_PC" timeout 60 pkg-config --modversion sdl3)
		if [ "$version" != 3.2.14 ] || ! timeout 60 grep -q "^SDL_CFLAGS = .*$DEPS/prefix-3.2/include" "$build/Makefile"; then
			echo "build-sdl32: SDL is '$version', not 3.2.14 from deps/prefix-3.2"
			return 1
		fi
		;;
	esac
	return 0
}

make_lane() {
	local lane=$1
	shift
	if [ "$lane" = asan ]; then
		in_dir "$ROOT/build-$lane" "${ASAN_RUN[@]}" timeout 5400 make -j"$JOBS" "$@"
	else
		in_dir "$ROOT/build-$lane" timeout 3600 make -j"$JOBS" "$@"
	fi
}

# make check; in build-asan with -j1, so that only one sanitizer process runs at a time (the
# tree is already built, so only the test programs are compiled serially).
check_lane_tests() {
	local lane=$1
	if [ "$lane" = asan ]; then
		in_dir "$ROOT/build-$lane" "${ASAN_RUN[@]}" timeout 5400 make -j1 check
	else
		in_dir "$ROOT/build-$lane" timeout 3600 make -j"$JOBS" check
	fi
}

# make check-game. It runs "make all" first, so in build-asan it runs under ASAN_RUN like every
# make there (the tools of the data build are ASan programs); EXULT_WRAPPER sets exult's own options.
game_lane() {
	local lane=$1
	local run=()
	local wrapper=
	if [ "$lane" = asan ]; then
		run=("${ASAN_RUN[@]}")
		wrapper=$EXULT_WRAPPER_ASAN
	fi
	in_dir "$ROOT/build-$lane" "${run[@]}" env HIRES_TEST_TMP="$ROOT/tmp" EXULT_WRAPPER="$wrapper" \
		timeout 7200 make check-game
}

# make check-world: like game_lane, on the synthetic world of tests/data/hires/world.
world_lane() {
	local lane=$1
	local run=()
	local wrapper=
	if [ "$lane" = asan ]; then
		run=("${ASAN_RUN[@]}")
		wrapper=$EXULT_WRAPPER_ASAN
	fi
	in_dir "$ROOT/build-$lane" "${run[@]}" env HIRES_TEST_TMP="$ROOT/tmp" EXULT_WRAPPER="$wrapper" \
		WORLD_PYTHON="$VENV/bin/python" timeout 3600 make check-world
}

run_pytest() {
	in_dir "$SRC" timeout 1800 "$VENV/bin/python" -m pytest -p no:cacheprovider \
		--basetemp="$ROOT/tmp/pytest-ci" tools/hires/tests
}

log "ci.sh: source $SRC, lanes: $LANES, jobs: $JOBS, U7_BG_STATIC=${U7_BG_STATIC:-}"
log "git: $(git -C "$SRC" rev-parse --short HEAD) $(git -C "$SRC" status --porcelain | wc -l) changed paths"

step bootstrap bootstrap
step autoreconf autoreconf_if_needed
for lane in $LANES; do
	step "configure-$lane" configure_lane "$lane" || continue
	step "make-$lane" make_lane "$lane" || continue
	step "lane-$lane" check_lane "$lane" || continue
	built+="$lane "
	step "check-$lane" check_lane_tests "$lane"
	step "world-$lane" world_lane "$lane"
done
step lint in_dir "$SRC" timeout 600 "$VENV/bin/python" tools/hires/check_build_lists.py --strict
step pytest run_pytest
if [ -n "${U7_BG_STATIC:-}" ]; then
	for lane in o2 asan; do
		case " $LANES " in
		*" $lane "*) ;;
		*) continue ;;
		esac
		case $built in
		*" $lane "*) step "game-$lane" game_lane "$lane" ;;
		*)
			log "=== game-$lane: skipped, build-$lane did not configure, build or pass its lane check"
			skipped+=("game-$lane")
			;;
		esac
	done
fi

summary="ci.sh: ${#passed[@]} passed, ${#flaky[@]} flaky${flaky[*]:+ (${flaky[*]})}, ${#failed[@]} failed${failed[*]:+ (${failed[*]})}"
summary+="${skipped[*]:+, ${#skipped[@]} skipped (${skipped[*]})}; log $LOG"
log "$summary"
echo "$summary"
if [ ${#failed[@]} -gt 0 ]; then
	exit 1
elif [ ${#flaky[@]} -gt 0 ]; then
	exit 3
fi
exit 0
