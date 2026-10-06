/*
 *  render_test.cc - Headless region renders and the oracles of the hi-res
 *  render scale (--render-test).
 *
 *  Copyright (C) 2026  The Exult Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 *  The harness renders a frozen map region the way --buildmap does (gamma 1,
 *  palette 0, no NPCs, the time queue never runs), once into a scale-1
 *  reference buffer and once at each hi-res scale S, and checks that the
 *  scaled storage is the nearest-neighbour upscale of the reference (O2). The
 *  S render goes into a pushed Image_buffer8(w, h, S), or into the window's
 *  own buffer at world scale S when the run needs the real window: present
 *  read-back, a game area inside a larger full area, the S cycle and the
 *  pushed resize. Everything stays in one process, so heap order does not
 *  matter; the digests are reproducible for one binary and one harness.
 *
 *  Keys of the spec ("k=v,k=v"):
 *    tx, ty        top-left tile of the region (required)
 *    w, h          size of the region in game px (the full area; 320x200)
 *    lift          skip_above as in --buildmap: 16, 10, 5 (16)
 *    scales        S values, ':'-separated (2:3:6)
 *    mode          nn (oracles) | plain (images only) (nn)
 *    overrides     no | yes: hi-res overrides from the configured roots (no)
 *    expect        nn | identity | marker:<idx> (nn), the oracle of each S
 *                  render: nn = NN of the reference (O2); identity = the
 *                  same with overrides that must cover every flat source of
 *                  the view (O4a); marker = every logical px of a cell with
 *                  an override has <idx> at its top-left sub-px and the
 *                  reference elsewhere, exact with passes=flats, "differs
 *                  from NN" with passes=all (O4b); <idx> is 0..223
 *    coverage      full | partial (full): with expect=identity|marker, the
 *                  pack must cover every flat cell of the view (full), or
 *                  some but not all of them (partial: overridden and NN
 *                  cells in the same flats caches)
 *    toggle        1: after each S render, overrides off (must give NN of
 *                  the reference, I8) and on again (must give the first S
 *                  render back), each repainting every flats cache of the
 *                  view once, then a repaint that renders none, in this
 *                  process (0)
 *    passes        all | flats (all)
 *    repaint       N random sub-rect repaints per scale (O6) (0)
 *    present       1: read the window back (one scale, S > 1) (0)
 *    format        argb | index8 | both, with present=1 (configuration)
 *    filter        auto | nearest | linear | pixelart, with present=1
 *    window        WxH window, with present=1 (w*S x h*S)
 *    game          WxH game area inside the full area w x h (w x h)
 *    resize        policies to cycle through after the scales, ':'-separated:
 *                  off, art, auto, forceN
 *    pushed_resize 1: resize and toggle fullscreen with a layer pushed (0)
 *    edit          1: a terrain edit at the last scale, then O2 again (0)
 *    bench         N timed renders per scale (0), which must render no flats
 *                  cache at S > 1; with overrides=yes also N cold renders
 *                  (every flats cache of the view painted again with its
 *                  overrides) and N timed paint_flats of every flats cache
 *                  of the view (the render_flats p95 per cache)
 *    seed          srand() after init_files (1)
 *    images        1: also write the images of a passing nn run (0)
 *    out           output directory, must exist (required)
 *  Outputs in out: digest.json; ref_1x.png and hi_<S>.png (indexed) in plain
 *  mode, with images=1 and on a failure, plus diff_<S>.png (mismatching game
 *  px) and present_*.png on a failure.
 */

#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif

#include "render_test.h"

#include "Audio.h"
#include "Configuration.h"
#include "chunks.h"
#include "chunkter.h"
#include "exult_constants.h"
#include "game.h"
#include "gamemap.h"
#include "gamerend.h"
#include "gamewin.h"
#include "hires_glue.h"
#include "ibuf8.h"
#include "ignore_unused_variable_warning.h"
#include "iwin8.h"
#include "palette.h"
#include "shapeid.h"
#include "vgafile.h"

#ifdef HAVE_PNG_H
#	include "pngio.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef __GNUC__
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wold-style-cast"
#	pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#	if !defined(__llvm__) && !defined(__clang__)
#		pragma GCC diagnostic ignored "-Wuseless-cast"
#	endif
#endif    // __GNUC__
#include <SDL3/SDL.h>
#ifdef __GNUC__
#	pragma GCC diagnostic pop
#endif    // __GNUC__

using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::vector;

namespace {
	struct Params {
		int            tx   = -1;
		int            ty   = -1;
		int            w    = 320;
		int            h    = 200;
		int            lift = 16;
		vector<int>    scales{2, 3, 6};
		bool           plain     = false;
		bool           overrides = false;
		int            marker    = -1;       // expect=marker:<idx>; -1: none.
		bool           identity  = false;    // expect=identity.
		bool           toggle    = false;
		bool           partial   = false;    // coverage=partial.
		bool           flats     = false;
		int            repaint   = 0;
		bool           present   = false;
		string         format;    // argb, index8, both; empty: the configuration.
		string         filter;
		int            win_w  = 0;
		int            win_h  = 0;
		int            game_w = 0;
		int            game_h = 0;
		vector<string> resize;    // render_scale policies.
		bool           pushed_resize = false;
		bool           edit          = false;
		int            bench         = 0;
		unsigned       seed          = 1;
		bool           images        = false;
		string         out;
	};

	// Deterministic PRNG for the repaint rects (std::rand is the game's).
	struct Split_mix {
		uint64_t state;

		uint64_t next() {
			uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
			z          = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
			z          = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
			return z ^ (z >> 31);
		}

		int below(int n) {
			return n > 0 ? static_cast<int>(next() % static_cast<uint64_t>(n)) : 0;
		}
	};

	struct Fnv {
		uint64_t hash = 0xcbf29ce484222325ULL;

		void add(const unsigned char* data, size_t len) {
			for (size_t i = 0; i < len; i++) {
				hash ^= data[i];
				hash *= 0x100000001b3ULL;
			}
		}

		string hex() const {
			char text[20];
			snprintf(text, sizeof(text), "%016" PRIx64, hash);
			return text;
		}
	};

	bool parse_int(const string& text, int lo, int hi, int& value) {
		if (text.empty()) {
			return false;
		}
		char*      end = nullptr;
		const long v   = std::strtol(text.c_str(), &end, 10);
		if (*end != '\0' || v < lo || v > hi) {
			return false;
		}
		value = static_cast<int>(v);
		return true;
	}

	bool parse_size(const string& text, int& w, int& h) {
		const size_t x = text.find('x');
		return x != string::npos && parse_int(text.substr(0, x), 1, 16384, w) && parse_int(text.substr(x + 1), 1, 16384, h);
	}

	bool parse_bool(const string& text, bool& value) {
		if (text == "0" || text == "no") {
			value = false;
			return true;
		}
		if (text == "1" || text == "yes") {
			value = true;
			return true;
		}
		return false;
	}

	vector<string> split(const string& text, char sep) {
		vector<string> parts;
		size_t         start = 0;
		for (;;) {
			const size_t end = text.find(sep, start);
			parts.push_back(text.substr(start, end == string::npos ? string::npos : end - start));
			if (end == string::npos) {
				return parts;
			}
			start = end + 1;
		}
	}

	// "forceN" (':' separates the steps) or a render_scale value.
	bool parse_policy(const string& text, string& policy) {
		int n = 0;
		if (text.compare(0, 5, "force") == 0 && parse_int(text.substr(text[5] == ':' ? 6 : 5), 2, 8, n)) {
			policy = "force:" + std::to_string(n);
			return true;
		}
		if (text == "off" || text == "art" || text == "auto") {
			policy = text;
			return true;
		}
		return false;
	}

	bool parse_spec(const string& spec, Params& p) {
		bool ok    = true;
		auto error = [&ok](const string& msg) {
			cerr << "--render-test: " << msg << endl;
			ok = false;
		};
		for (const string& item : split(spec, ',')) {
			const size_t eq = item.find('=');
			if (eq == string::npos) {
				error("'" + item + "' is not key=value");
				continue;
			}
			const string key = item.substr(0, eq);
			const string val = item.substr(eq + 1);
			if (key == "tx") {
				if (!parse_int(val, 0, c_num_tiles - 1, p.tx)) {
					error("tx must be 0.." + std::to_string(c_num_tiles - 1));
				}
			} else if (key == "ty") {
				if (!parse_int(val, 0, c_num_tiles - 1, p.ty)) {
					error("ty must be 0.." + std::to_string(c_num_tiles - 1));
				}
			} else if (key == "w") {
				if (!parse_int(val, 16, 4096, p.w)) {
					error("w must be 16..4096");
				}
			} else if (key == "h") {
				if (!parse_int(val, 16, 4096, p.h)) {
					error("h must be 16..4096");
				}
			} else if (key == "lift") {
				if (!parse_int(val, 1, 31, p.lift)) {
					error("lift must be 1..31");
				}
			} else if (key == "scales") {
				p.scales.clear();
				for (const string& s : split(val, ':')) {
					int n = 0;
					if (parse_int(s, 1, 8, n)) {
						p.scales.push_back(n);
					} else {
						error("scales must be values 1..8 separated by ':'");
					}
				}
			} else if (key == "mode") {
				if (val == "nn" || val == "plain") {
					p.plain = val == "plain";
				} else {
					error("mode must be nn or plain");
				}
			} else if (key == "overrides") {
				if (!parse_bool(val, p.overrides)) {
					error("overrides must be yes or no");
				}
			} else if (key == "expect") {
				p.identity = val == "identity";
				p.marker   = -1;
				if (val.compare(0, 7, "marker:") == 0) {
					// 0xE0..0xFE cycle: such a marker breaks rule P4, so the
					// store would reject every tile (mkpack_identity.py agrees).
					if (!parse_int(val.substr(7), 0, 0xDF, p.marker)) {
						error("expect=marker:<idx> takes a decimal index 0..223 (not a cycling index 224..254)");
					}
				} else if (val != "nn" && val != "identity") {
					error("expect must be nn, identity or marker:<idx>");
				}
			} else if (key == "toggle") {
				if (!parse_bool(val, p.toggle)) {
					error("toggle must be 0 or 1");
				}
			} else if (key == "coverage") {
				if (val != "full" && val != "partial") {
					error("coverage must be full or partial");
				}
				p.partial = val == "partial";
			} else if (key == "inspect") {
				error("inspect needs the hi-res store, which this build does not have");
			} else if (key == "passes") {
				if (val == "all" || val == "flats") {
					p.flats = val == "flats";
				} else {
					error("passes must be all or flats");
				}
			} else if (key == "repaint") {
				if (!parse_int(val, 0, 100000, p.repaint)) {
					error("repaint must be 0..100000");
				}
			} else if (key == "present") {
				if (!parse_bool(val, p.present)) {
					error("present must be 0 or 1");
				}
			} else if (key == "format") {
				if (val == "argb" || val == "index8" || val == "both") {
					p.format = val;
				} else {
					error("format must be argb, index8 or both");
				}
			} else if (key == "filter") {
				if (val == "auto" || val == "nearest" || val == "linear" || val == "pixelart") {
					p.filter = val;
				} else {
					error("filter must be auto, nearest, linear or pixelart");
				}
			} else if (key == "window") {
				if (!parse_size(val, p.win_w, p.win_h)) {
					error("window must be WxH");
				}
			} else if (key == "game") {
				if (!parse_size(val, p.game_w, p.game_h)) {
					error("game must be WxH");
				}
			} else if (key == "resize") {
				for (const string& s : split(val, ':')) {
					string policy;
					if (parse_policy(s, policy)) {
						p.resize.push_back(policy);
					} else {
						error("resize steps must be off, art, auto or forceN (N = 2..8)");
					}
				}
			} else if (key == "pushed_resize") {
				if (!parse_bool(val, p.pushed_resize)) {
					error("pushed_resize must be 0 or 1");
				}
			} else if (key == "edit") {
				if (!parse_bool(val, p.edit)) {
					error("edit must be 0 or 1");
				}
			} else if (key == "bench") {
				if (!parse_int(val, 0, 100000, p.bench)) {
					error("bench must be 0..100000");
				}
			} else if (key == "seed") {
				int seed = 0;
				if (!parse_int(val, 0, 0x7fffffff, seed)) {
					error("seed must be 0..2147483647");
				}
				p.seed = static_cast<unsigned>(seed);
			} else if (key == "images") {
				if (!parse_bool(val, p.images)) {
					error("images must be 0 or 1");
				}
			} else if (key == "out") {
				p.out = val;
			} else {
				error("unknown key '" + key + "'");
			}
		}
		if (p.tx < 0 || p.ty < 0) {
			error("tx and ty are required");
		}
		if (p.out.empty()) {
			error("out is required");
		}
		if (p.scales.empty()) {
			error("scales is empty");
		}
		if (p.game_w > p.w || p.game_h > p.h) {
			error("the game area must fit in w x h");
		}
		if (p.present && (p.scales.size() != 1 || p.scales[0] < 2)) {
			error("present=1 takes exactly one scale, 2..8");
		}
		if (!p.present && (!p.format.empty() || !p.filter.empty() || p.win_w > 0)) {
			error("format, filter and window need present=1");
		}
		if (p.plain
			&& (p.repaint > 0 || p.present || !p.resize.empty() || p.pushed_resize || p.edit || p.toggle || p.identity
				|| p.marker >= 0)) {
			error("mode=plain takes no oracle keys (repaint, present, resize, pushed_resize, edit, toggle, expect)");
		}
		if ((p.identity || p.marker >= 0 || p.toggle) && !p.overrides) {
			error("expect=identity, expect=marker and toggle need overrides=yes");
		}
		if (p.partial && !p.identity && p.marker < 0) {
			error("coverage=partial needs expect=identity or expect=marker");
		}
		if (p.marker >= 0 && (p.present || p.edit)) {
			error("expect=marker takes no present or edit (their oracles are NN)");
		}
		return ok;
	}

	/*
	 *  The storage extent of a buffer: logical [-off_x, w - off_x) x
	 *  [-off_y, h - off_y) at scale S.
	 */
	struct Extent {
		unsigned char* origin = nullptr;    // Physical top-left of the extent.
		int            pitch  = 0;          // Physical bytes per row.
		int            scale  = 1;
		int            w      = 0;    // Logical.
		int            h      = 0;

		unsigned char* row(int py) const {
			return origin + static_cast<std::ptrdiff_t>(py) * pitch;
		}

		int phys_w() const {
			return w * scale;
		}

		int phys_h() const {
			return h * scale;
		}
	};

	Extent extent_of(Image_buffer8& buf, int off_x, int off_y) {
		Extent e;
		e.scale  = buf.get_pixel_scale();
		e.pitch  = static_cast<int>(buf.get_line_width());
		e.w      = static_cast<int>(buf.get_width());
		e.h      = static_cast<int>(buf.get_height());
		e.origin = buf.get_bits() - (static_cast<std::ptrdiff_t>(off_y) * e.pitch + off_x) * e.scale;
		return e;
	}

	string digest(const Extent& e) {
		Fnv fnv;
		for (int y = 0; y < e.phys_h(); y++) {
			fnv.add(e.row(y), static_cast<size_t>(e.phys_w()));
		}
		return fnv.hex();
	}

	vector<unsigned char> snapshot(const Extent& e) {
		vector<unsigned char> copy(static_cast<size_t>(e.phys_w()) * e.phys_h());
		for (int y = 0; y < e.phys_h(); y++) {
			std::memcpy(&copy[static_cast<size_t>(y) * e.phys_w()], e.row(y), static_cast<size_t>(e.phys_w()));
		}
		return copy;
	}

	bool equals(const Extent& e, const vector<unsigned char>& copy) {
		for (int y = 0; y < e.phys_h(); y++) {
			if (std::memcmp(&copy[static_cast<size_t>(y) * e.phys_w()], e.row(y), static_cast<size_t>(e.phys_w())) != 0) {
				return false;
			}
		}
		return true;
	}

	void clear(const Extent& e) {
		for (int y = 0; y < e.phys_h(); y++) {
			std::memset(e.row(y), 0, static_cast<size_t>(e.phys_w()));
		}
	}

	// phys(hi) == NN(ref): every S x S block of hi holds its ref pixel.
	struct Nn_result {
		long long             mismatches = 0;     // Logical pixels.
		int                   first_x    = -1;    // Extent coordinates.
		int                   first_y    = -1;
		vector<unsigned char> mask;    // 1 per mismatching logical pixel.
	};

	Nn_result compare_nn(const Extent& hi, const Extent& ref) {
		Nn_result result;
		const int S = hi.scale;
		if (hi.w != ref.w || hi.h != ref.h || ref.scale != 1) {
			result.mismatches = -1;
			return result;
		}
		vector<unsigned char> expect(static_cast<size_t>(hi.phys_w()));
		for (int y = 0; y < ref.h; y++) {
			const unsigned char* src = ref.row(y);
			for (int x = 0; x < ref.w; x++) {
				std::memset(&expect[static_cast<size_t>(x) * S], src[x], static_cast<size_t>(S));
			}
			for (int k = 0; k < S; k++) {
				const unsigned char* row = hi.row(y * S + k);
				if (std::memcmp(row, expect.data(), expect.size()) == 0) {
					continue;
				}
				if (result.mask.empty()) {
					result.mask.assign(static_cast<size_t>(ref.w) * ref.h, 0);
				}
				for (int x = 0; x < ref.w; x++) {
					if (std::memcmp(
								row + static_cast<std::ptrdiff_t>(x) * S, &expect[static_cast<size_t>(x) * S],
								static_cast<size_t>(S))
						!= 0) {
						unsigned char& m = result.mask[static_cast<size_t>(y) * ref.w + x];
						if (!m) {
							m = 1;
							result.mismatches++;
							if (result.first_x < 0) {
								result.first_x = x;
								result.first_y = y;
							}
						}
					}
				}
			}
		}
		return result;
	}

	// a / b rounded towards minus infinity (b > 0).
	int floor_div(int a, int b) {
		return a >= 0 ? a / b : -((-a + b - 1) / b);
	}

	// The tiles of a region: per tile of the full area, whether a flat is
	// painted there (it has a flat source) and whether that source has an
	// override at the scale asked for.
	struct Cell_grid {
		constexpr static unsigned char has_source   = 1;
		constexpr static unsigned char has_override = 2;

		int                    col0 = 0;    // Tile of extent x 0, relative to the region's tx.
		int                    row0 = 0;
		int                    cols = 0;
		int                    rows = 0;
		vector<unsigned char>  flags;
		int                    sources   = 0;    // Tiles with a flat source.
		int                    overrides = 0;    // Of those, tiles whose source has an override.
		int                    chunks    = 0;    // Distinct chunks under the full area.
		vector<Chunk_terrain*> terrains;         // Their distinct terrains (one flats cache each).

		// The flags of logical px (lx, ly) of the full area (game px, the
		// game area's top-left at (0, 0)).
		unsigned char at(int lx, int ly) const {
			const int c = floor_div(lx, c_tilesize) - col0;
			const int r = floor_div(ly, c_tilesize) - row0;
			return c >= 0 && r >= 0 && c < cols && r < rows ? flags[static_cast<size_t>(r) * cols + c] : 0;
		}
	};

	// The window's current palette as 768 RGB bytes.
	std::array<unsigned char, 768> window_palette(Image_window8* win) {
		std::array<unsigned char, 768> rgb{};
		SDL_Surface*                   surface = win->get_draw_surface();
		const SDL_Palette*             palette = surface != nullptr ? SDL_GetSurfacePalette(surface) : nullptr;
		if (palette != nullptr) {
			for (int i = 0; i < 256 && i < palette->ncolors; i++) {
				rgb[3 * i]     = palette->colors[i].r;
				rgb[3 * i + 1] = palette->colors[i].g;
				rgb[3 * i + 2] = palette->colors[i].b;
			}
		}
		return rgb;
	}

	/*
	 *  The software renderer's LINEAR scaling (SDL_stretch.c scale_mat,
	 *  7-bit fractions, vertical then horizontal), as the reference of a
	 *  present read-back that is not 1:1.
	 */
	struct Stretch_axis {
		int64_t start     = 0;
		int     step      = 0;
		int     left_pad  = 0;
		int     right_pad = 0;
	};

	constexpr int stretch_precision = 7;

	Stretch_axis stretch_axis(int src_n, int dst_n) {
		Stretch_axis a;
		a.step             = static_cast<int>((static_cast<int64_t>(src_n) << 16) / dst_n);
		const int64_t half = 1 << 15;
		const int64_t tmp0 = static_cast<int64_t>(a.step) * (half >> 16);
		const int64_t tmp1 = static_cast<int64_t>(a.step) * (half & 0xFFFF);
		const int64_t x0   = tmp0 + ((tmp1 + 0x8000) >> 16) - half;
		a.start            = x0;
		int64_t sum        = x0;
		for (int i = 0; i < dst_n; i++) {
			if (sum < 0) {
				a.left_pad++;
			} else if (static_cast<int>(static_cast<uint64_t>(sum) >> 16) > src_n - 2) {
				a.right_pad++;
			}
			sum += a.step;
		}
		return a;
	}

	uint32_t interpolate(uint32_t c0, uint32_t c1, int frac0, int frac1) {
		uint32_t out = 0;
		for (int shift = 0; shift < 32; shift += 8) {
			const uint32_t a = (c0 >> shift) & 0xff;
			const uint32_t b = (c1 >> shift) & 0xff;
			out |= (((static_cast<uint32_t>(frac1) * a + static_cast<uint32_t>(frac0) * b) >> stretch_precision) & 0xff) << shift;
		}
		return out;
	}

	vector<uint32_t> stretch_linear(const vector<uint32_t>& src, int src_w, int src_h, int dst_w, int dst_h) {
		vector<uint32_t>   dst(static_cast<size_t>(dst_w) * dst_h);
		const Stretch_axis ah   = stretch_axis(src_h, dst_h);
		const Stretch_axis aw   = stretch_axis(src_w, dst_w);
		const int          one  = 1 << stretch_precision;
		auto               frac = [](int64_t fp) {
            return static_cast<int>((static_cast<uint64_t>(fp) >> (16 - stretch_precision)) & ((1 << stretch_precision) - 1));
		};
		int64_t sum_h = ah.start;
		for (int i = 0; i < dst_h; i++) {
			const bool pad     = i < ah.left_pad || i > dst_h - 1 - ah.right_pad;
			int        index_h = static_cast<int>(static_cast<uint64_t>(sum_h) >> 16);
			int        frac_h0 = frac(sum_h);
			if (pad) {
				index_h = i < ah.left_pad ? 0 : src_h - 1;
				frac_h0 = 0;
			}
			const uint32_t* s0      = &src[static_cast<size_t>(index_h) * src_w];
			const uint32_t* s1      = pad ? s0 : s0 + src_w;
			const int       frac_h1 = one - frac_h0;
			sum_h += ah.step;
			int64_t sum_w = aw.start + static_cast<int64_t>(aw.left_pad) * aw.step;
			for (int j = 0; j < dst_w; j++) {
				int index_w = 0;
				int frac_w  = 0;
				if (j < aw.left_pad) {
					index_w = 0;
					frac_w  = 0;
				} else if (j >= dst_w - aw.right_pad) {
					index_w = src_w - 2;
					frac_w  = one;
				} else {
					index_w = static_cast<int>(static_cast<uint64_t>(sum_w) >> 16);
					frac_w  = frac(sum_w);
					sum_w += aw.step;
				}
				const uint32_t t0                       = interpolate(s0[index_w], s1[index_w], frac_h0, frac_h1);
				const uint32_t t1                       = interpolate(s0[index_w + 1], s1[index_w + 1], frac_h0, frac_h1);
				dst[static_cast<size_t>(i) * dst_w + j] = interpolate(t0, t1, frac_w, one - frac_w);
			}
		}
		return dst;
	}

	/*
	 *  The test run.
	 */
	class Render_test_run {
		Params                            p;
		Game_window*                      gwin      = nullptr;
		Image_window8*                    win       = nullptr;
		bool                              main_mode = false;    // The S render goes into the window's buffer.
		int                               win_w = 0, win_h = 0, win_scale = 1;
		int                               game_w = 0, game_h = 0;
		int                               off_x = 0, off_y = 0;    // Of the game area in the full area.
		vector<std::pair<string, string>> entries;                 // digest.json, in order.
		vector<string>                    failures;
		std::unique_ptr<Image_buffer8>    ref;
		vector<unsigned char>             ref_storage;    // Behind ref in main mode.
		Extent                            ref_extent;
		string                            ref_mini;

		void record(const string& key, const string& value) {
			entries.emplace_back(key, value);
		}

		void record(const string& key, long long value) {
			entries.emplace_back(key, std::to_string(value));
		}

		void record_ms(const string& key, double ms) {
			char text[32];
			snprintf(text, sizeof(text), "%.3f", ms);
			entries.emplace_back(key, text);
		}

		void fail(const string& msg) {
			cerr << "[render-test] FAIL: " << msg << endl;
			failures.push_back(msg);
		}

		string path(const string& name) const {
			return p.out + "/" + name;
		}

		Image_buffer8* main_buffer() const {
			return gwin->get_main_render_target();
		}

		// The extent of the window's buffer, which must be the current target.
		Extent main_extent() const {
			return extent_of(*main_buffer(), off_x, off_y);
		}

		// Paints the whole view into target (null: the window's buffer),
		// cleared first.
		void paint_full(Image_buffer8* target, const Extent& e) {
			clear(e);
			paint(target, -off_x, -off_y, e.w, e.h, true);
		}

		void paint(Image_buffer8* target, int x, int y, int w, int h, bool whole) {
			Image_buffer8* prev = target != nullptr ? gwin->push_render_target(target) : nullptr;
			gwin->render_test_paint(x, y, w, h, p.tx, p.ty, p.lift, whole);
			if (target != nullptr) {
				gwin->pop_render_target(prev);
			}
		}

		string mini_digest(Image_buffer8* target) {
			if (game_w < 3 * 96 || game_h < 3 * 60) {
				return "n/a";    // mini_screenshot reads 288 x 180 game px.
			}
			Image_buffer8* prev = target != nullptr ? gwin->push_render_target(target) : nullptr;
			const auto     mini = win->mini_screenshot();
			if (target != nullptr) {
				gwin->pop_render_target(prev);
			}
			if (!mini) {
				return "none";
			}
			Fnv fnv;
			fnv.add(mini.get(), 96 * 60);
			return fnv.hex();
		}

		bool write_png8(const string& name, const Extent& e) {
#ifdef HAVE_PNG_H
			const auto palette = window_palette(win);
			if (!Export_png8(path(name).c_str(), -1, e.phys_w(), e.phys_h(), e.pitch, 0, 0, e.origin, palette.data(), 256)) {
				fail("cannot write " + path(name));
				return false;
			}
			return true;
#else
			ignore_unused_variable_warning(name, e);
			return false;
#endif
		}

		void write_diff(const string& name, const Nn_result& nn, int w, int h) {
#ifdef HAVE_PNG_H
			if (nn.mask.empty()) {
				return;
			}
			vector<unsigned char>          pixels(nn.mask);
			std::array<unsigned char, 768> palette{};
			palette[3] = 255;    // 1: magenta, a mismatching game px.
			palette[5] = 255;
			Export_png8(path(name).c_str(), -1, w, h, w, 0, 0, pixels.data(), palette.data(), 256);
#else
			ignore_unused_variable_warning(name, nn, w, h);
#endif
		}

		void write_png32(const string& name, const vector<uint32_t>& argb, int w, int h) {
#ifdef HAVE_PNG_H
			vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4);
			for (size_t i = 0; i < argb.size(); i++) {
				rgba[4 * i]     = static_cast<unsigned char>(argb[i] >> 16);
				rgba[4 * i + 1] = static_cast<unsigned char>(argb[i] >> 8);
				rgba[4 * i + 2] = static_cast<unsigned char>(argb[i]);
				rgba[4 * i + 3] = 255;
			}
			Export_png32(path(name).c_str(), w, h, w * 4, 0, 0, rgba.data());
#else
			ignore_unused_variable_warning(name, argb, w, h);
#endif
		}

		// O2 for one render; writes the images on a failure (or images=1).
		void check_nn(const string& tag, const Extent& hi) {
			const Nn_result nn = compare_nn(hi, ref_extent);
			if (nn.mismatches != 0) {
				std::ostringstream msg;
				if (nn.mismatches < 0) {
					msg << tag << ": the S render (" << hi.w << 'x' << hi.h << ") and the reference (" << ref_extent.w << 'x'
						<< ref_extent.h << ") differ in size";
				} else {
					msg << tag << ": " << nn.mismatches << " game px differ from NN of the reference, the first at (" << nn.first_x
						<< ',' << nn.first_y << ") of the full area";
				}
				fail(msg.str());
				write_png8("ref_1x.png", ref_extent);
				write_png8("hi_" + tag + ".png", hi);
				write_diff("diff_" + tag + ".png", nn, ref_extent.w, ref_extent.h);
			} else if (p.images) {
				write_png8("hi_" + tag + ".png", hi);
			}
		}

		// The marker oracle applies to renders at S (overrides need S > 1).
		bool marker_active(int S) const {
			return p.marker >= 0 && S > 1 && Hires::is_enabled();
		}

		// The flat sources of the full area and their overrides at scale S.
		Cell_grid cell_grid(int S) {
			Cell_grid  g;
			const auto wrap = [](int t) {
				return (t % c_num_tiles + c_num_tiles) % c_num_tiles;
			};
			g.col0 = floor_div(-off_x, c_tilesize);
			g.row0 = floor_div(-off_y, c_tilesize);
			g.cols = floor_div(p.w - off_x - 1, c_tilesize) - g.col0 + 1;
			g.rows = floor_div(p.h - off_y - 1, c_tilesize) - g.row0 + 1;
			g.flags.assign(static_cast<size_t>(g.cols) * g.rows, 0);
			std::set<int>            chunks;
			std::set<Chunk_terrain*> terrains;
			for (int r = 0; r < g.rows; r++) {
				for (int c = 0; c < g.cols; c++) {
					const int      tx    = wrap(p.tx + g.col0 + c);
					const int      ty    = wrap(p.ty + g.row0 + r);
					const int      cx    = tx / c_tiles_per_chunk;
					const int      cy    = ty / c_tiles_per_chunk;
					Map_chunk*     chunk = gwin->get_map()->get_chunk(cx, cy);
					Chunk_terrain* terr  = chunk != nullptr ? chunk->get_terrain() : nullptr;
					if (terr == nullptr) {
						continue;
					}
					chunks.insert(cy * c_num_chunks + cx);
					if (terrains.insert(terr).second) {
						g.terrains.push_back(terr);
					}
					const int src = terr->get_flat_source(tx % c_tiles_per_chunk, ty % c_tiles_per_chunk);
					if (src < 0) {
						continue;
					}
					unsigned char& f = g.flags[static_cast<size_t>(r) * g.cols + c];
					f                = Cell_grid::has_source;
					g.sources++;
					const ShapeID id = terr->get_flat(src % c_tiles_per_chunk, src / c_tiles_per_chunk);
					if (Hires::flat(id.get_shapenum(), id.get_framenum() & 31, S).px != nullptr) {
						f |= Cell_grid::has_override;
						g.overrides++;
					}
				}
			}
			g.chunks = static_cast<int>(chunks.size());
			return g;
		}

		// Records the store's report and the override coverage of the view
		// at S; identity and marker need every flat source covered (else the
		// oracle would pass without testing the overrides).
		Cell_grid check_coverage(const string& tag, int S) {
			const Hires::Report* rep = Hires::report(S);
			if (rep != nullptr && rep->failed) {
				record(tag + "_store", "failed: " + rep->failure);
			} else if (rep != nullptr) {
				std::ostringstream text;
				text << rep->loaded << " loaded (bundle " << rep->bundled << "), " << rep->rejected << " rejected, "
					 << rep->unguarded << " unguarded, " << rep->warnings << " warnings";
				record(tag + "_store", text.str());
			} else {
				record(tag + "_store", "none");
			}
			const Cell_grid cells = cell_grid(S);
			record(tag + "_cells",
				   std::to_string(cells.overrides) + " of " + std::to_string(cells.sources) + " flat cells overridden");
			if (!(p.identity || p.marker >= 0)) {
				return cells;
			}
			if (p.partial && (cells.overrides == 0 || cells.overrides >= cells.sources)) {
				fail(tag + ": the overrides cover " + std::to_string(cells.overrides) + " of the " + std::to_string(cells.sources)
					 + " flat cells of the view (coverage=partial needs some but not all)");
			} else if (!p.partial && (cells.sources == 0 || cells.overrides != cells.sources)) {
				fail(tag + ": the overrides cover " + std::to_string(cells.overrides) + " of the " + std::to_string(cells.sources)
					 + " flat cells of the view (the pack must cover them all)");
			}
			return cells;
		}

		// O4b. passes=flats: phys(hi) is NN of the reference, except the
		// top-left sub-px of every logical px of a cell with an override,
		// which is the marker. passes=all: hi differs from NN of the
		// reference (translucent shapes and objects modify markers).
		void check_marker(const string& tag, const Extent& hi, const Cell_grid& cells) {
			if (!p.flats) {
				const Nn_result nn = compare_nn(hi, ref_extent);
				record(tag + "_marker_px", nn.mismatches);
				if (nn.mismatches <= 0) {
					fail(tag + ": with the marker pack the S render does not differ from NN of the reference");
				}
				return;
			}
			const int             S = hi.scale;
			Nn_result             res;
			vector<unsigned char> expect(static_cast<size_t>(hi.phys_w()));
			long long             markers = 0;
			for (int y = 0; y < ref_extent.h && hi.w == ref_extent.w && hi.h == ref_extent.h; y++) {
				const unsigned char* src = ref_extent.row(y);
				for (int k = 0; k < S; k++) {
					for (int x = 0; x < ref_extent.w; x++) {
						std::memset(&expect[static_cast<size_t>(x) * S], src[x], static_cast<size_t>(S));
						if (k == 0 && (cells.at(x - off_x, y - off_y) & Cell_grid::has_override)) {
							expect[static_cast<size_t>(x) * S] = static_cast<unsigned char>(p.marker);
							markers++;
						}
					}
					const unsigned char* row = hi.row(y * S + k);
					if (std::memcmp(row, expect.data(), expect.size()) == 0) {
						continue;
					}
					if (res.mask.empty()) {
						res.mask.assign(static_cast<size_t>(ref_extent.w) * ref_extent.h, 0);
					}
					for (int x = 0; x < ref_extent.w; x++) {
						const size_t at = static_cast<size_t>(x) * S;
						if (std::memcmp(row + at, &expect[at], static_cast<size_t>(S)) != 0) {
							unsigned char& m = res.mask[static_cast<size_t>(y) * ref_extent.w + x];
							if (!m) {
								m = 1;
								res.mismatches++;
								if (res.first_x < 0) {
									res.first_x = x;
									res.first_y = y;
								}
							}
						}
					}
				}
			}
			record(tag + "_markers", markers);
			if (hi.w != ref_extent.w || hi.h != ref_extent.h) {
				fail(tag + ": the S render and the reference differ in size");
			} else if (res.mismatches != 0 || markers == 0) {
				std::ostringstream msg;
				msg << tag << ": " << res.mismatches << " game px differ from the marker prediction (" << markers
					<< " markers), the first at (" << res.first_x << ',' << res.first_y << ") of the full area";
				fail(msg.str());
				write_png8("ref_1x.png", ref_extent);
				write_png8("hi_" + tag + ".png", hi);
				write_diff("diff_" + tag + ".png", res, ref_extent.w, ref_extent.h);
			} else if (p.images) {
				write_png8("hi_" + tag + ".png", hi);
			}
		}

		// The oracle of an S render: NN of the reference (O2, O4a with the
		// identity pack) or the marker prediction (O4b).
		void check_render(const string& tag, const Extent& hi) {
			if (p.overrides && Hires::is_enabled() && hi.scale > 1) {
				const Cell_grid cells = check_coverage(tag, hi.scale);
				if (marker_active(hi.scale)) {
					check_marker(tag, hi, cells);
					return;
				}
			}
			check_nn(tag, hi);
		}

		// The toggle (I8): overrides off must give NN of the reference, and on
		// again the first render; the flats caches notice through
		// Hires::generation().
		void toggle_check(const string& tag, Image_buffer8* target, const Extent& hi) {
			const string first  = digest(hi);
			const int    caches = static_cast<int>(cell_grid(hi.scale).terrains.size());
			uint32       before = Chunk_terrain::get_hires_renders();
			Hires::set_enabled(false);
			paint_full(target, hi);
			check_cache_renders(tag + "_toggle_off", before, caches);
			record(tag + "_toggle_off", digest(hi));
			check_nn(tag + "_toggle_off", hi);
			Hires::set_enabled(true);
			before = Chunk_terrain::get_hires_renders();
			paint_full(target, hi);
			check_cache_renders(tag + "_toggle_on", before, caches);
			const string again = digest(hi);
			record(tag + "_toggle_on", again);
			if (again != first) {
				fail(tag + "_toggle_on: the render with the overrides on again differs from the first one");
				write_png8("hi_" + tag + "_toggle_on.png", hi);
			}
			// Nothing changed since: the caches are current.
			before = Chunk_terrain::get_hires_renders();
			paint_full(target, hi);
			check_cache_renders(tag + "_toggle_warm", before, 0);
		}

		// The flats caches rendered at S > 1 since 'before' must be 'expect'
		// (every cache of the view after a generation change, none when
		// nothing changed).
		void check_cache_renders(const string& tag, uint32 before, long long expect) {
			const uint32    delta   = Chunk_terrain::get_hires_renders() - before;    // Wraps.
			const long long renders = delta;
			record(tag + "_cache_renders", renders);
			if (renders != expect) {
				fail(tag + ": " + std::to_string(renders) + " flats caches rendered, expected " + std::to_string(expect));
			}
		}

		// Invariant I12 for the window's buffer at the expected scale (0: any).
		bool check_i12(const string& tag, int expect_scale) {
			Image_buffer8*     main  = main_buffer();
			const SDL_Surface* draw  = win->get_draw_surface();
			const int          S     = win->get_world_scale();
			const int          guard = 4;    // Image_window::guard_band.
			std::ostringstream msg;
			if (draw == nullptr || main->get_bits() == nullptr) {
				msg << "no draw surface or main buffer bits";
			} else if (
					draw->w != static_cast<int>(main->get_width()) * S + 2 * guard
					|| draw->h != static_cast<int>(main->get_height()) * S + 2 * guard) {
				msg << "draw surface " << draw->w << 'x' << draw->h << " is not " << main->get_width() << 'x' << main->get_height()
					<< " x S=" << S << " + guard band";
			} else if (main->get_pixel_scale() != S) {
				msg << "main buffer pixel scale " << main->get_pixel_scale() << " != world scale " << S;
			} else if ((main->get_tracker() != nullptr) != (S > 1)) {
				msg << "write tracker " << (main->get_tracker() != nullptr ? "set" : "missing") << " at S=" << S;
			} else if (expect_scale > 0 && S != expect_scale) {
				msg << "world scale " << S << ", expected " << expect_scale;
			}
			if (!msg.str().empty()) {
				fail(tag + ": invariant I12: " + msg.str());
				return false;
			}
			return true;
		}

		bool setup_window_geometry() {
			game_w = p.game_w > 0 ? p.game_w : p.w;
			game_h = p.game_h > 0 ? p.game_h : p.h;
			off_x  = (p.w - game_w) / 2;
			off_y  = (p.h - game_h) / 2;
			if (p.present) {
				const int S = p.scales[0];
				win_w       = p.win_w > 0 ? p.win_w : p.w * S;
				win_h       = p.win_h > 0 ? p.win_h : p.h * S;
				win_scale   = win_w / p.w;
				if (win_scale < 1 || win_w != p.w * win_scale || win_h != p.h * win_scale) {
					cerr << "--render-test: the window " << win_w << 'x' << win_h << " must be an integer multiple of " << p.w
						 << 'x' << p.h << endl;
					return false;
				}
			} else {
				win_w     = p.w;
				win_h     = p.h;
				win_scale = 1;
			}
			return true;
		}

		void resize_window() {
			gwin->resized(
					win_w, win_h, false, game_w, game_h, win_scale, Image_window::point, Image_window::Fit, Image_window::point);
		}

		static string scale_policy(int S) {
			return S > 1 ? "force:" + std::to_string(S) : string("off");
		}

		// The S render into the window's buffer, after a policy change.
		void main_step(const string& tag, const string& policy, int expect_scale) {
			Image_window::set_render_scale_override(policy);
			resize_window();
			if (!check_i12(tag, expect_scale)) {
				return;
			}
			render_main(tag);
		}

		void render_main(const string& tag) {
			if (win->get_ib8() != main_buffer()) {
				fail(tag + ": a render target is still pushed");
				return;
			}
			const Extent e = main_extent();
			paint_full(nullptr, e);
			record(tag + "_scale", win->get_world_scale());
			record(tag + "_hi", digest(e));
			check_mini(tag, nullptr);
			if (!p.plain) {
				check_render(tag, e);
			} else if (p.overrides && e.scale > 1) {
				check_coverage(tag, e.scale);
			}
		}

		// O7: the mini screenshot. (Not the light-source count: paint_map
		// counts lights only around a main actor, and --render-test has no
		// main actor nor the ireg objects that carry the lights.)
		void check_mini(const string& tag, Image_buffer8* target) {
			const string mini = mini_digest(target);
			record(tag + "_mini", mini);
			const int S = target != nullptr ? target->get_pixel_scale() : main_buffer()->get_pixel_scale();
			if (mini != ref_mini && !marker_active(S)) {    // The mini screenshot samples the markers.
				fail(tag + ": mini screenshot " + mini + ", the reference has " + ref_mini);
			}
		}

		// O6: random sub-rect repaints of the game area over a spoilt rect,
		// also applied to the reference: the S buffer stays NN of the
		// reference, so it is unchanged where the reference is.
		constexpr static unsigned char repaint_spoil = 0x01;

		void repaints(const string& tag, Image_buffer8* target, const Extent& hi) {
			Split_mix                   rng{p.seed * 0x10001ULL + static_cast<uint64_t>(hi.scale)};
			vector<unsigned char>       before    = snapshot(ref_extent);
			const string                hi_before = digest(hi);
			const vector<unsigned char> hi_snap   = marker_active(hi.scale) ? snapshot(hi) : vector<unsigned char>();
			int                         ref_moved = 0;
			for (int i = 0; i < p.repaint; i++) {
				const int x = rng.below(game_w);
				const int y = rng.below(game_h);
				const int w = 1 + rng.below(game_w - x);
				const int h = 1 + rng.below(game_h - y);
				// Spoil the rect first, so that a missing write shows too.
				(target != nullptr ? target : main_buffer())->fill8(repaint_spoil, w, h, x, y);
				ref->fill8(repaint_spoil, w, h, x, y);
				paint(target, x, y, w, h, false);
				paint(ref.get(), x, y, w, h, false);
				if (!equals(ref_extent, before)) {
					ref_moved++;
					before = snapshot(ref_extent);
				}
				if (marker_active(hi.scale)) {
					// The marker render has no NN reference: it must not change.
					if (!equals(hi, hi_snap)) {
						std::ostringstream msg;
						msg << tag << ": repaint " << i << " of (" << x << ',' << y << ' ' << w << 'x' << h
							<< ") changed the marker render";
						fail(msg.str());
						write_png8("hi_" + tag + "_repaint.png", hi);
						break;
					}
					continue;
				}
				const Nn_result nn = compare_nn(hi, ref_extent);
				if (nn.mismatches != 0) {
					std::ostringstream msg;
					msg << tag << ": repaint " << i << " of (" << x << ',' << y << ' ' << w << 'x' << h << "): " << nn.mismatches
						<< " game px differ from NN of the reference";
					fail(msg.str());
					write_png8("hi_" + tag + "_repaint.png", hi);
					write_diff("diff_" + tag + "_repaint.png", nn, ref_extent.w, ref_extent.h);
					break;
				}
			}
			record(tag + "_repaints", p.repaint);
			// A sub-rect repaint over a spoilt rect restores what the whole
			// view painted: neither the reference nor the S buffer changes
			// (this catches a regression of the scale-independent repaint).
			record(tag + "_repaint_ref_changed", ref_moved);
			const string hi_after = digest(hi);
			record(tag + "_after_repaint", hi_after);
			if (ref_moved > 0) {
				fail(tag + ": " + std::to_string(ref_moved) + " sub-rect repaints changed the scale-1 reference");
			}
			if (hi_after != hi_before) {
				fail(tag + ": the sub-rect repaints changed the S buffer");
				write_png8("hi_" + tag + "_repaint.png", hi);
			}
		}

		void bench(const string& tag, Image_buffer8* target, const Extent& e) {
			if (p.bench <= 0) {
				return;
			}
			using Clock = std::chrono::steady_clock;
			vector<double> paint_ms;
			vector<double> upload_ms;
			vector<double> show_ms;
			paint(target, -off_x, -off_y, e.w, e.h, true);    // Warm the caches.
			const uint32 warm_before = Chunk_terrain::get_hires_renders();
			const bool   present     = p.present && target == nullptr;
			if (present) {
				win->show();
			}
			for (int i = 0; i < p.bench; i++) {
				const auto t0 = Clock::now();
				paint(target, -off_x, -off_y, e.w, e.h, true);
				const auto t1 = Clock::now();
				paint_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
				if (present) {
					win->upload_world();
					const auto t2 = Clock::now();
					win->show();
					const auto t3 = Clock::now();
					upload_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
					show_ms.push_back(std::chrono::duration<double, std::milli>(t3 - t2).count());
				}
			}
			auto report = [&](const string& what, vector<double>& ms) {
				if (ms.empty()) {
					return;
				}
				std::sort(ms.begin(), ms.end());
				const double median = ms[ms.size() / 2];
				const size_t p95i   = std::min(ms.size() - 1, (ms.size() * 95 + 99) / 100 - 1);
				const double p95    = ms[p95i];
				record_ms("bench_" + tag + "_" + what + "_median_ms", median);
				record_ms("bench_" + tag + "_" + what + "_p95_ms", p95);
				char line[160];
				snprintf(
						line, sizeof(line), "[render-test] bench %s %s (%dx%d, %d runs): median %.3f ms, p95 %.3f ms", tag.c_str(),
						what.c_str(), e.w, e.h, p.bench, median, p95);
				cout << line << endl;
			};
			report("paint", paint_ms);
			report("upload", upload_ms);
			report("present", show_ms);
			if (e.scale > 1) {
				// Warm paints reuse every flats cache (the generation check).
				check_cache_renders("bench_" + tag + "_warm", warm_before, 0);
			}
			if (!p.overrides || !Hires::is_enabled() || e.scale < 2) {
				return;
			}
			// Cold paints with per-tile art: a toggle off and on changes
			// Hires::generation(), so every flats cache of the view renders
			// again (the store stays loaded).
			const vector<Chunk_terrain*> terrains = cell_grid(e.scale).terrains;
			const int                    caches   = static_cast<int>(terrains.size());
			vector<double>               cold_ms;
			const uint32                 cold_before = Chunk_terrain::get_hires_renders();
			for (int i = 0; i < p.bench; i++) {
				Hires::set_enabled(false);
				Hires::set_enabled(true);
				const auto t0 = Clock::now();
				paint(target, -off_x, -off_y, e.w, e.h, true);
				cold_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
			}
			check_cache_renders("bench_" + tag + "_cold", cold_before, static_cast<long long>(caches) * p.bench);
			report("cold_paint", cold_ms);
			// render_flats per cache: paint_flats of every cache of the view
			// with its overrides, each call timed (the buffer is reused, as
			// render_flats reuses a cache of the same scale).
			Image_buffer8  flats_buf(c_chunksize, c_chunksize, e.scale);
			vector<double> flats_ms;
			for (int i = 0; i < p.bench; i++) {
				for (Chunk_terrain* terr : terrains) {
					const auto t0 = Clock::now();
					terr->paint_flats(flats_buf, true);
					flats_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
				}
			}
			record("bench_" + tag + "_caches", caches);
			report("render_flats", flats_ms);
		}

		// LUT(NN(ref)) as the window shows it: the world texture the reference
		// implies, at 1:1 or scaled as the software renderer's LINEAR does.
		vector<uint32_t> expected_frame(int S) const {
			const int        tex_w = p.w * S;
			const int        tex_h = p.h * S;
			const auto       pal   = window_palette(win);
			vector<uint32_t> texture(static_cast<size_t>(tex_w) * tex_h);
			for (int y = 0; y < tex_h; y++) {
				const unsigned char* src = ref_extent.row(y / S);
				for (int x = 0; x < tex_w; x++) {
					const unsigned char c = src[x / S];
					texture[static_cast<size_t>(y) * tex_w + x]
							= 0xff000000U | (uint32_t{pal[3 * c]} << 16) | (uint32_t{pal[3 * c + 1]} << 8) | pal[3 * c + 2];
				}
			}
			if (win_w == tex_w && win_h == tex_h) {
				return texture;
			}
			return stretch_linear(texture, tex_w, tex_h, win_w, win_h);
		}

		// One read-back against expected_frame(); returns the pixels (empty on
		// a failure to read).
		vector<uint32_t> read_back(const string& tag, int S) {
			vector<uint32_t> got;
			SDL_Surface*     frame = win->read_back_world();
			if (frame == nullptr) {
				fail(tag + ": no read-back");
				return got;
			}
			if (frame->w != win_w || frame->h != win_h) {
				fail(tag + ": read-back " + std::to_string(frame->w) + "x" + std::to_string(frame->h) + ", window "
					 + std::to_string(win_w) + "x" + std::to_string(win_h));
			} else {
				got.resize(static_cast<size_t>(win_w) * win_h);
				for (int y = 0; y < win_h; y++) {
					std::memcpy(
							&got[static_cast<size_t>(y) * win_w],
							static_cast<const unsigned char*>(frame->pixels) + y * frame->pitch, static_cast<size_t>(win_w) * 4);
				}
			}
			SDL_DestroySurface(frame);
			if (got.empty()) {
				return got;
			}
			const bool             one_to_one = win_w == p.w * S && win_h == p.h * S;
			const vector<uint32_t> expect     = expected_frame(S);
			int                    max_err    = 0;
			long long              over       = 0;
			Fnv                    fnv;
			for (size_t i = 0; i < got.size(); i++) {
				const uint32_t g = got[i] & 0xffffff;
				const uint32_t e = expect[i] & 0xffffff;
				fnv.add(reinterpret_cast<const unsigned char*>(&g), 3);
				for (int shift = 0; shift < 24; shift += 8) {
					const int err = std::abs(static_cast<int>((g >> shift) & 0xff) - static_cast<int>((e >> shift) & 0xff));
					max_err       = std::max(max_err, err);
					if (err > (one_to_one ? 0 : 1)) {
						over++;
					}
				}
			}
			record(tag, fnv.hex());
			record(tag + "_max_error", max_err);
			if (over > 0) {
				fail(tag + ": " + std::to_string(over) + " channel values off by more than " + (one_to_one ? "0" : "1") + " (max "
					 + std::to_string(max_err) + ")");
				write_png32(tag + ".png", got, win_w, win_h);
				write_png32(tag + "_expected.png", expect, win_w, win_h);
			}
			return got;
		}

		static int max_difference(const vector<uint32_t>& a, const vector<uint32_t>& b) {
			int diff = 0;
			for (size_t i = 0; i < a.size() && i < b.size(); i++) {
				for (int shift = 0; shift < 24; shift += 8) {
					diff = std::max(
							diff, std::abs(static_cast<int>((a[i] >> shift) & 0xff) - static_cast<int>((b[i] >> shift) & 0xff)));
				}
			}
			return diff;
		}

		// Present read-back (main mode, S > 1), per format: the full upload
		// after the render, then tracked partial uploads of fills that the
		// reference mirrors (tracked_to_phys, also over the offset band).
		// Exact at 1:1, else within 1 of the software renderer's LINEAR.
		void present_checks() {
			const int S = win->get_world_scale();
			if (2 * win_w < p.w * S || 2 * win_h < p.h * S) {
				fail("present: the read-back oracle covers ratios >= 0.5 only");
				return;
			}
			record("present_window", std::to_string(win_w) + "x" + std::to_string(win_h));
			vector<string> formats;
			if (p.format == "both") {
				formats = {"index8", "argb"};
			} else {
				formats = {p.format.empty() ? string("config") : p.format};
			}
			Split_mix        rng{p.seed * 0x20003ULL};
			vector<uint32_t> last;
			for (size_t f = 0; f < formats.size(); f++) {
				const string& fmt = formats[f];
				if (f > 0) {
					config->set("config/video/hires/present_format", fmt, false);
					win->rebuild_surfaces();    // Keeps the picture and the palette.
					win->take_rebuild_request();
					if (!check_i12("present_" + fmt, S)) {
						return;
					}
				}
				// The format the presenter really uses: INDEX8 falls back to ARGB.
				const string actual = win->world_present_format();
				record("present_" + fmt + "_actual", actual);
				if (fmt == "index8" && actual != "index8") {
#if SDL_VERSION_ATLEAST(3, 4, 0)
					fail("present: index8 requested, the world texture is " + actual);
#else
					cout << "[render-test] present: no INDEX8 before SDL 3.4, the index8 pass is skipped" << endl;
					record("present_index8", "skipped");
#endif
					continue;
				}
				const vector<uint32_t> full = read_back("present_" + fmt, S);
				if (full.empty()) {
					return;
				}
				if (!last.empty()) {
					const int diff = max_difference(full, last);
					record("present_formats_max_difference", diff);
					if (diff > 1) {
						fail("present: the " + formats[f - 1] + " and " + fmt + " read-backs differ by " + std::to_string(diff));
					}
				}
				last = full;
				for (int k = 0; k < 3; k++) {
					const int     x   = rng.below(p.w) - off_x;
					const int     y   = rng.below(p.h) - off_y;
					const int     w   = 1 + rng.below(p.w - off_x - x);
					const int     h   = 1 + rng.below(p.h - off_y - y);
					const uint8_t pix = static_cast<uint8_t>(0x20 + 16 * k + static_cast<int>(f));
					main_buffer()->fill8(pix, w, h, x, y);    // Tracked.
					ref->fill8(pix, w, h, x, y);
					last = read_back("present_" + fmt + "_fill" + std::to_string(k), S);
					if (last.empty()) {
						return;
					}
				}
			}
			screen_to_game_grid(S);
		}

		// screen_to_game on a 16 x 16 grid equals the mapping at S=1.
		void screen_to_game_grid(int S) {
			vector<int> at_s;
			vector<int> at_1;
			for (int pass = 0; pass < 2; pass++) {
				vector<int>& grid = pass == 0 ? at_s : at_1;
				if (pass == 1) {
					Image_window::set_render_scale_override("off");
					resize_window();
					if (!check_i12("present_grid_1x", 1)) {
						return;
					}
				}
				for (int j = 0; j < 16; j++) {
					for (int i = 0; i < 16; i++) {
						int gx = 0;
						int gy = 0;
						win->screen_to_game(i * (win_w - 1) / 15, j * (win_h - 1) / 15, false, gx, gy);
						grid.push_back(gx);
						grid.push_back(gy);
					}
				}
			}
			if (at_s != at_1) {
				fail("present: screen_to_game at S=" + std::to_string(S) + " differs from S=1");
			}
			// Back to S for the steps that follow.
			Image_window::set_render_scale_override(scale_policy(S));
			resize_window();
			check_i12("present_restore", S);
		}

		// A terrain edit at S > 1 (set_flat + commit_edits re-renders the
		// cache at its scale), then the render again against a new reference.
		// The cells are tried from the centre of the region on, until an edit
		// shows in the reference (a cell can be hidden under objects).
		void terrain_edit(const string& tag, Image_buffer8* target, const Extent& hi) {
			const vector<unsigned char> old_ref = snapshot(ref_extent);
			const int                   tiles_w = game_w / c_tilesize;
			const int                   tiles_h = game_h / c_tilesize;
			int                         tried   = 0;
			for (int ring = 0; ring < std::max(tiles_w, tiles_h) && tried < 32; ring++) {
				for (int dy = -ring; dy <= ring && tried < 32; dy++) {
					for (int dx = -ring; dx <= ring && tried < 32; dx++) {
						if (std::max(std::abs(dx), std::abs(dy)) != ring) {
							continue;
						}
						const int vx = tiles_w / 2 + dx;
						const int vy = tiles_h / 2 + dy;
						if (vx < 0 || vy < 0 || vx >= tiles_w || vy >= tiles_h) {
							continue;
						}
						const int      tx    = (p.tx + vx) % c_num_tiles;
						const int      ty    = (p.ty + vy) % c_num_tiles;
						Map_chunk*     chunk = gwin->get_map()->get_chunk(tx / c_tiles_per_chunk, ty / c_tiles_per_chunk);
						Chunk_terrain* terr  = chunk != nullptr ? chunk->get_terrain() : nullptr;
						if (terr == nullptr) {
							continue;
						}
						const int          cell_x = tx % c_tiles_per_chunk;
						const int          cell_y = ty % c_tiles_per_chunk;
						const ShapeID      own    = terr->get_flat(cell_x, cell_y);
						const Shape_frame* fown   = own.get_shape();
						if (fown == nullptr || fown->is_rle()) {
							continue;
						}
						// Another flat of the same terrain.
						ShapeID other;
						bool    found = false;
						for (int k = 0; k < 256 && !found; k++) {
							const ShapeID      b  = terr->get_flat(k % 16, k / 16);
							const Shape_frame* fb = b.get_shape();
							if (fb != nullptr && !fb->is_rle()
								&& (b.get_shapenum() != own.get_shapenum() || b.get_framenum() != own.get_framenum())) {
								other = b;
								found = true;
							}
						}
						if (!found) {
							continue;
						}
						tried++;
						// Every flats cache of the view at S, so that commit_edits
						// re-renders at S.
						paint(target, -off_x, -off_y, hi.w, hi.h, true);
						terr->set_flat(cell_x, cell_y, other);
						terr->commit_edits();
						clear(hi);
						paint(target, -off_x, -off_y, hi.w, hi.h, true);
						clear(ref_extent);
						paint(ref.get(), -off_x, -off_y, ref_extent.w, ref_extent.h, true);
						if (equals(ref_extent, old_ref)) {
							// Hidden: undo, and try the next cell.
							terr->set_flat(cell_x, cell_y, own);
							terr->commit_edits();
							continue;
						}
						std::ostringstream what;
						what << "tile " << tx << ',' << ty << " (" << own.get_shapenum() << ':' << own.get_framenum() << " -> "
							 << other.get_shapenum() << ':' << other.get_framenum() << "), " << tried << " tried";
						record(tag + "_edit", what.str());
						ref_mini = mini_digest(ref.get());
						record(tag + "_edit_ref", digest(ref_extent));
						record(tag + "_edit_hi", digest(hi));
						check_mini(tag + "_edit", target);
						check_render(tag + "_edit", hi);
						return;
					}
				}
			}
			fail(tag + ": no terrain edit near the centre of the region showed (" + std::to_string(tried) + " tried)");
		}

		// A pushed layer survives resized() and the fullscreen toggle, and the
		// window's buffer satisfies I12 afterwards (CC-1, MS-1).
		void pushed_resize() {
			const int S_before = win->get_world_scale();
			const int handle   = win->create_layer("render-test", 64, 32);
			if (handle < 0) {
				fail("pushed_resize: no layer");
				return;
			}
			auto* layer = static_cast<Image_buffer8*>(win->get_layer_ibuf(handle));
			for (int y = 0; y < 32; y++) {
				for (int x = 0; x < 64; x++) {
					layer->put_pixel8(static_cast<unsigned char>(x * 7 + y * 13), x, y);
				}
			}
			const unsigned char* bits  = layer->get_bits();
			const Extent         e     = extent_of(*layer, 0, 0);
			const string         d0    = digest(e);
			auto                 check = [&](const string& step) {
                if (layer->get_bits() != bits || digest(e) != d0) {
                    fail("pushed_resize " + step + ": the pushed layer's bits changed");
                    return false;
                }
                if (win->get_ib8() != layer) {
                    fail("pushed_resize " + step + ": the layer is no longer the render target");
                    return false;
                }
                return true;
			};
			Image_buffer8* prev = gwin->push_render_target(layer);
			resize_window();
			bool ok = check("resized");
			if (ok) {
				win->toggle_fullscreen();
				ok = check("toggle_fullscreen");
				win->toggle_fullscreen();
				ok = ok && check("toggle_fullscreen back");
			}
			gwin->pop_render_target(prev);
			check_i12("pushed_resize popped", 0);
			record("pushed_resize_toggled_scale", win->get_world_scale());
			resize_window();    // toggle_fullscreen changes the window size.
			win->destroy_layer(handle);
			if (check_i12("pushed_resize restored", S_before)) {
				render_main("pushed_resize");
			}
		}

	public:
		explicit Render_test_run(const Params& params) : p(params) {}

		int run(BaseGameInfo* game) {
			if (!setup_window_geometry()) {
				return 2;
			}
			main_mode = p.present || p.game_w > 0 || !p.resize.empty() || p.pushed_resize;
			Image_window8::set_gamma(1, 1, 1);
			config->set("config/video/vsync", 0, false);    // bench: time the present, not the display.
			if (!p.format.empty()) {
				config->set("config/video/hires/present_format", p.format == "both" ? "index8" : p.format, false);
			}
			if (!p.filter.empty()) {
				config->set("config/video/hires/present_filter", p.filter, false);
			}
			Image_window::set_render_scale_override(main_mode ? scale_policy(p.scales[0]) : "off");
			gwin = new Game_window(
					win_w, win_h, false, game_w, game_h, win_scale, Image_window::point, Image_window::Fit, Image_window::point);
			win = gwin->get_win();
			Audio::Init();
			Game::create_game(game);
			gwin->init_files(false);    // init, but don't show plasma
			std::srand(p.seed);         // init_files seeded it from the clock.
			gwin->get_map()->init();
			gwin->set_map(0);
			gwin->get_pal()->set(0);
			gwin->get_render()->test_passes = p.flats ? Game_render::Pass_flats : Game_render::Pass_all;
			Hires::set_enabled(p.overrides);    // Whatever config/video/hires/overrides says.

			if (static_cast<int>(main_buffer()->get_width()) != p.w || static_cast<int>(main_buffer()->get_height()) != p.h
				|| win->get_start_x() != -off_x || win->get_start_y() != -off_y) {
				cerr << "--render-test: the window's full area is " << main_buffer()->get_width() << 'x'
					 << main_buffer()->get_height() << " at " << win->get_start_x() << ',' << win->get_start_y() << ", expected "
					 << p.w << 'x' << p.h << " at " << -off_x << ',' << -off_y << endl;
				return 2;
			}
			record("spec_region", std::to_string(p.tx) + "," + std::to_string(p.ty) + " " + std::to_string(p.w) + "x"
										  + std::to_string(p.h) + " lift " + std::to_string(p.lift));
			record("spec_game", std::to_string(game_w) + "x" + std::to_string(game_h));
			record("spec_passes", p.flats ? "flats" : "all");
			record("spec_target", main_mode ? "window" : "pushed");
			record("spec_overrides", p.overrides ? "yes" : "no");
			string expect = p.identity ? "identity" : "nn";
			if (p.marker >= 0) {
				expect = "marker:" + std::to_string(p.marker);
			}
			record("spec_expect", expect);

			// The scale-1 reference, painted once (and again after an edit).
			using Clock = std::chrono::steady_clock;
			if (main_mode) {
				ref_storage.assign(static_cast<size_t>(p.w) * p.h, 0);
				ref = std::make_unique<Image_buffer8>(ref_storage.data(), p.w, p.w, p.h, off_x, off_y, 1);
			} else {
				ref = std::make_unique<Image_buffer8>(p.w, p.h);
			}
			ref_extent    = extent_of(*ref, off_x, off_y);
			const auto t0 = Clock::now();
			paint_full(ref.get(), ref_extent);
			record_ms("time_ref_ms", std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
			ref_mini = mini_digest(ref.get());
			record("ref_1x", digest(ref_extent));
			record("ref_mini", ref_mini);
			if (p.plain || p.images) {
				write_png8("ref_1x.png", ref_extent);
			}

			string                         last_tag;
			std::unique_ptr<Image_buffer8> last_hi;
			for (size_t i = 0; i < p.scales.size(); i++) {
				const int    S   = p.scales[i];
				const string tag = "s" + std::to_string(S);
				if (main_mode) {
					if (i > 0) {
						Image_window::set_render_scale_override(scale_policy(S));
						resize_window();
					}
					if (!check_i12(tag, S)) {
						continue;
					}
					const auto t1 = Clock::now();
					render_main(tag);
					record_ms("time_" + tag + "_ms", std::chrono::duration<double, std::milli>(Clock::now() - t1).count());
					if (p.plain) {
						write_png8("hi_" + std::to_string(S) + ".png", main_extent());
					}
					if (p.repaint > 0) {
						repaints(tag, nullptr, main_extent());
					}
					if (p.toggle) {
						toggle_check(tag, nullptr, main_extent());
					}
					bench(tag, nullptr, main_extent());
				} else {
					auto         hi = std::make_unique<Image_buffer8>(p.w, p.h, S);
					const Extent e  = extent_of(*hi, 0, 0);
					const auto   t1 = Clock::now();
					paint_full(hi.get(), e);
					record_ms("time_" + tag + "_ms", std::chrono::duration<double, std::milli>(Clock::now() - t1).count());
					record(tag + "_hi", digest(e));
					if (p.plain) {
						write_png8("hi_" + std::to_string(S) + ".png", e);
						if (p.overrides && S > 1) {
							check_coverage(tag, S);
						}
					} else {
						check_mini(tag, hi.get());
						check_render(tag, e);
						if (p.repaint > 0) {
							repaints(tag, hi.get(), e);
						}
						if (p.toggle) {
							toggle_check(tag, hi.get(), e);
						}
					}
					bench(tag, hi.get(), e);
					last_hi  = std::move(hi);
					last_tag = tag;
				}
			}
			if (p.present) {
				present_checks();
			}
			for (size_t i = 0; i < p.resize.size(); i++) {
				const string& policy = p.resize[i];
				const int expect = policy.compare(0, 6, "force:") == 0 ? std::atoi(policy.c_str() + 6) : policy == "off" ? 1 : 0;
				string    tag    = "resize" + std::to_string(i) + "_" + policy;
				tag.erase(std::remove(tag.begin(), tag.end(), ':'), tag.end());
				main_step(tag, policy, expect);
			}
			if (p.edit) {
				if (main_mode) {
					if (win->get_world_scale() < 2) {
						fail("edit: the window's world scale is 1 (the edit step needs S > 1)");
					} else {
						terrain_edit("edit", nullptr, main_extent());
					}
				} else if (last_hi) {
					terrain_edit("edit", last_hi.get(), extent_of(*last_hi, 0, 0));
				}
			}
			if (p.pushed_resize) {
				if (win->get_world_scale() < 2) {
					fail("pushed_resize: the window's world scale is 1 (the step needs S > 1)");
				} else {
					pushed_resize();
				}
			}
			record("result", failures.empty() ? "pass" : "fail");
			std::ofstream out(path("digest.json"));
			out << "{\n";
			for (size_t i = 0; i < entries.size(); i++) {
				out << "  \"" << entries[i].first << "\": \"" << entries[i].second << '"' << (i + 1 < entries.size() ? "," : "")
					<< '\n';
			}
			out << "}\n";
			if (!out.good()) {
				cerr << "--render-test: cannot write " << path("digest.json") << endl;
				return 1;
			}
			cout << "[render-test] " << (failures.empty() ? "PASS" : "FAIL") << " (" << failures.size() << " failures), digest "
				 << path("digest.json") << endl;
			return failures.empty() ? 0 : 1;
		}
	};
}    // namespace

/*
 *  As paint_map_at_tile; with whole_view
 *  false only the chunks of the clip rect are covered, as a dirty repaint
 *  (Game_window::paint) covers them.
 */

void Game_window::render_test_paint(int x, int y, int w, int h, int toptx, int topty, int skip_above, bool whole_view) {
	const int savescrolltx = scrolltx;
	const int savescrollty = scrollty;
	const int saveskip     = skip_lift;
	scrolltx               = toptx;
	scrollty               = topty;
	skip_lift              = skip_above;
	map->read_map_data();    // Gather in all objs., etc.
	win->set_clip(x, y, w, h);
	if (whole_view) {
		render->paint_map(0, 0, get_width(), get_height());
	} else {
		// The rect within the game area, as Game_window::paint passes it.
		const int gx = std::max(x, 0);
		const int gy = std::max(y, 0);
		const int gw = std::min(x + w, get_width()) - gx;
		const int gh = std::min(y + h, get_height()) - gy;
		if (gw > 0 && gh > 0) {
			render->paint_map(gx, gy, gw, gh);
		}
	}
	win->clear_clip();
	scrolltx  = savescrolltx;
	scrollty  = savescrollty;
	skip_lift = saveskip;
}

int Render_test(BaseGameInfo* game, const std::string& spec) {
	Params params;
	if (!parse_spec(spec, params)) {
		return 2;
	}
	Render_test_run test(params);
	const int       result = test.run(game);
	Audio::Destroy();
	return result;
}
