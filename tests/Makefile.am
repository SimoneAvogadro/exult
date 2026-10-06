# Tests of the hi-res render path (DESIGN.md section 6).
#
# "make check" builds and runs:
#   hires_unit     SDL-free doctest unit tests (unit/);
#   hires_present  data-free present tests with the offscreen video driver and
#                  the software renderer (present/); exit code 77 = skipped.
# They link the libtool convenience libraries, as tools/ipack does: a program
# takes only the archive members it references, so the engine is not pulled in.
#
# "make check-game" runs the game-data scripts in game/. Each one exits with 77
# (skipped) unless U7_BG_STATIC names the "static" directory of a Black Gate
# installation. EXULT_WRAPPER (environment) is prefixed to every exult call; for
# an ASan exult it defaults to ASLR off and leak checks off (game/lib.sh). The
# top-level check-game builds "all" first, so in a sanitizer build tree run it
# like every make there, with ASLR off and leak checks off (tests/README).

AUTOMAKE_OPTIONS = subdir-objects

AM_CPPFLAGS = -I$(srcdir)/unit -I$(top_srcdir) -I$(top_srcdir)/headers \
	-I$(top_srcdir)/files -I$(top_srcdir)/imagewin -I$(top_srcdir)/shapes \
	$(SDL_CFLAGS) $(PNG_CFLAGS) $(INCDIRS) $(WINDOWING_SYSTEM) \
	$(DEBUG_LEVEL) $(OPT_LEVEL) $(WARNINGS) $(CPPFLAGS) \
	-DHIRES_TEST_DATA_DIR=\"$(abs_srcdir)/data\"

if HAVE_SDL
check_PROGRAMS = hires_unit hires_present
endif

hires_unit_SOURCES = \
	unit/main.cc \
	unit/hires_test_util.cc \
	unit/hires_test_util.h \
	unit/ibuf_ops.cc \
	unit/ibuf_ops.h \
	unit/rle_writer.h \
	unit/test_editor_fixtures.cc \
	unit/test_flat_source.cc \
	unit/test_hires_png.cc \
	unit/test_hires_rules.cc \
	unit/test_hires_store.cc \
	unit/test_ibuf_golden.cc \
	unit/test_ibuf_scaled.cc \
	unit/test_pngio.cc \
	unit/test_rle_writer.cc \
	unit/test_support.h \
	unit/test_world_scale.cc

# In dependency order: Shape_frame (vgafile.cc) and the hi-res store need
# Flex, U7exists and friends from libu7file; the PNG reader needs libpng.
hires_unit_LDADD = \
	$(top_builddir)/shapes/libshapes.la \
	$(top_builddir)/imagewin/libimagewin.la \
	$(top_builddir)/files/libu7file.la \
	$(PNG_LIBS) $(ZLIB_LIBS) $(SYSLIBS)

hires_present_SOURCES = \
	present/test_present.cc

hires_present_LDADD = \
	$(top_builddir)/imagewin/libimagewin.la \
	$(SDL_LIBS) $(SYSLIBS)

TESTS = $(check_PROGRAMS)

AM_TESTS_ENVIRONMENT = SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software SDL_AUDIO_DRIVER=dummy; \
	export SDL_VIDEO_DRIVER SDL_RENDER_DRIVER SDL_AUDIO_DRIVER;

# Empty by default. build-asan sets it at configure time to run every test
# under "setarch x86_64 -R timeout ..." with pinned sanitizer options
# (DESIGN.md sections 6.1 and 6.5).
LOG_COMPILER = $(TEST_WRAPPER)

# Game-data tests (DESIGN.md section 6.4), run one after another.
GAME_TESTS = \
	game/smoke.sh \
	game/buildmap_golden.sh \
	game/render_regions.sh \
	game/override_regions.sh \
	game/dump_art.sh \
	game/resize_cycle.sh \
	game/dev_loop.sh \
	game/perf.sh

check-game:
	@pass=0; skip=0; fail=0; \
	for t in $(GAME_TESTS); do \
		EXULT_BUILD_DIR='$(abs_top_builddir)' HIRES_TESTS_SRCDIR='$(abs_srcdir)' \
			bash '$(srcdir)'/$$t; rc=$$?; \
		case $$rc in \
		0) echo "PASS: $$t"; pass=`expr $$pass + 1`;; \
		77) echo "SKIP: $$t"; skip=`expr $$skip + 1`;; \
		*) echo "FAIL: $$t (exit code $$rc)"; fail=`expr $$fail + 1`;; \
		esac; \
	done; \
	echo "check-game: $$pass passed, $$skip skipped, $$fail failed"; \
	test $$fail -eq 0

# Records game/golden/*.sha256 with the reference build (upstream + P3):
#   make regen-goldens U7_BG_STATIC=... UPSTREAM_BUILD=/path/to/build-upstream
# Commit the new lists with the cause in the message.
regen-goldens:
	@test -n '$(UPSTREAM_BUILD)' || { echo "regen-goldens: set UPSTREAM_BUILD to the build of upstream + P3" >&2; exit 2; }
	HIRES_TESTS_SRCDIR='$(abs_srcdir)' bash '$(srcdir)'/game/regen_goldens.sh '$(UPSTREAM_BUILD)'

.PHONY: check-game regen-goldens

EXTRA_DIST = \
	README \
	doctest.h \
	data/hires \
	data/ibuf_golden.txt \
	game/lib.sh \
	game/test.cfg.in \
	game/regions.txt \
	game/dump_art_check.py \
	game/regen_goldens.sh \
	game/perf_baseline.json \
	game/golden/buildmap0.sha256 \
	game/golden/buildmap1.sha256 \
	game/golden/buildmap2.sha256 \
	game/golden/inspect.json \
	$(GAME_TESTS)

CLEANFILES = *~
