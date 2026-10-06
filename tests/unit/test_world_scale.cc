/*
 *  test_world_scale.cc - The hi-res scale policy, the present filter ladder
 *  and the tracker-to-texture conversion of world_scale.h (DESIGN.md
 *  sections 3.2.2, 3.2.4 and 6.2, test_world_scale).
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

#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif

#include "doctest.h"
#include "test_support.h"
#include "world_scale.h"

#include <algorithm>
#include <cstdint>

namespace {
	World_scale_in art_in(int full_w, int full_h, double mpx = 40) {
		World_scale_in in;
		in.policy        = World_policy::Art;
		in.full_w        = full_w;
		in.full_h        = full_h;
		in.max_world_mpx = mpx;
		return in;
	}

	World_scale_in auto_in(int full_w, int full_h, int out_w, int out_h, double aspect_y = 1.0, double mpx = 40) {
		World_scale_in in = art_in(full_w, full_h, mpx);
		in.policy         = World_policy::Auto;
		in.out_w          = out_w;
		in.out_h          = out_h;
		in.aspect_y       = aspect_y;
		return in;
	}

	World_filter filter_of(int l_w, int l_h, int tex_w, int tex_h, bool pixelart_ok = true) {
		return choose_world_filter(l_w, l_h, tex_w, tex_h, pixelart_ok).filter;
	}

	bool same(const World_phys_rect& a, const World_phys_rect& b) {
		return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
	}
}    // namespace

TEST_CASE("world scale: render_scale values") {
	World_scale_request req;
	CHECK(parse_world_policy("off", req));
	CHECK(req.policy == World_policy::Off);
	CHECK(parse_world_policy("art", req));
	CHECK(req.policy == World_policy::Art);
	CHECK(parse_world_policy("1", req));
	CHECK(req.policy == World_policy::Off);
	CHECK(parse_world_policy(" Auto ", req));
	CHECK(req.policy == World_policy::Auto);
	for (int n = 2; n <= 8; n++) {
		CHECK(parse_world_policy("force:" + std::to_string(n), req));
		CHECK(req.policy == World_policy::Force);
		CHECK(req.force_n == n);
	}
	CHECK(parse_world_policy("FORCE:6", req));
	CHECK(req.force_n == 6);
	req = World_scale_request{World_policy::Art, 0};
	for (const char* bad : {"", "on", "6", "force", "force:", "force:1", "force:9", "force:10", "force:6x", "art6"}) {
		INFO(bad);
		CHECK_FALSE(parse_world_policy(bad, req));
		CHECK(req.policy == World_policy::Art);    // Unchanged.
	}
	World_filter_override filter = World_filter_override::Auto;
	CHECK(parse_world_filter_override("linear", filter));
	CHECK(filter == World_filter_override::Linear);
	CHECK(parse_world_filter_override("PixelArt", filter));
	CHECK(filter == World_filter_override::Pixelart);
	CHECK_FALSE(parse_world_filter_override("bilinear", filter));
	CHECK(filter == World_filter_override::Pixelart);
}

TEST_CASE("world scale: the configuration table of section 3.2.2") {
	// The table was computed with the provisional budget of 10 Mpx.
	SUBCASE("windowed 1920x1200, 320x200 Fit: S=6 at 1:1, NEAREST") {
		CHECK(compute_world_scale(art_in(320, 200, 10)) == 6);
		CHECK(filter_of(1920, 1200, 320 * 6, 200 * 6) == World_filter::Nearest);
	}
	SUBCASE("fullscreen 3440x1440, scale 4, ACF: 860x300 at S=6 (9.29 Mpx), LINEAR") {
		CHECK(compute_world_scale(art_in(860, 300, 10)) == 6);
		CHECK(filter_of(3440, 1440, 860 * 6, 300 * 6) == World_filter::Linear);    // r = 0.667 x 0.8
	}
	SUBCASE("fullscreen preset scale 6, ACF: 573x200 at S=6, PIXELART or LINEAR") {
		CHECK(compute_world_scale(art_in(573, 200, 10)) == 6);
		CHECK(filter_of(3440, 1440, 573 * 6, 200 * 6, true) == World_filter::Pixelart);
		CHECK(filter_of(3440, 1440, 573 * 6, 200 * 6, false) == World_filter::Linear);
	}
	SUBCASE("window 1280x800, 320x200: S=6, LINEAR") {
		CHECK(compute_world_scale(art_in(320, 200, 10)) == 6);
		CHECK(filter_of(1280, 800, 1920, 1200) == World_filter::Linear);
	}
	SUBCASE("window 640x400, 320x200: S=6, one halving + LINEAR") {
		const World_filter_choice choice = choose_world_filter(640, 400, 1920, 1200, true);
		CHECK(choice.filter == World_filter::Halving);
		CHECK(choice.halvings == 1);
	}
	SUBCASE("fullscreen scale 2, ACF: 1720x600 at S=3 (6 would be 37 Mpx)") {
		CHECK(compute_world_scale(art_in(1720, 600, 10)) == 3);
		CHECK(filter_of(3440, 1440, 1720 * 3, 600 * 3) == World_filter::Linear);
	}
	SUBCASE("studio zoom x1 on 1920x1200: S=2, LINEAR exact 2:1") {
		CHECK(compute_world_scale(art_in(1920, 1200, 10)) == 2);
		CHECK(filter_of(1920, 1200, 3840, 2400) == World_filter::Linear);
	}
	SUBCASE("fullscreen scale 1, Fill, 3440x1440: S=1") {
		CHECK(compute_world_scale(art_in(3440, 1440, 10)) == 1);
	}
}

TEST_CASE("world scale: the default budget of 40 Mpx (section 13)") {
	CHECK(compute_world_scale(art_in(320, 200)) == 6);
	CHECK(compute_world_scale(art_in(860, 300)) == 6);      // 9.3 Mpx
	CHECK(compute_world_scale(art_in(1720, 600)) == 6);     // 37.2 Mpx
	CHECK(compute_world_scale(art_in(1920, 1200)) == 3);    // 6 would be 82.9 Mpx
	CHECK(compute_world_scale(art_in(3440, 1440)) == 2);    // 3 would be 44.6 Mpx
	CHECK(compute_world_scale(art_in(2048, 2048)) == 3);    // --buildmap (forced off in the engine)
	CHECK(World_scale_in().max_world_mpx == 40);
}

TEST_CASE("world scale: texture limit and budget edges") {
	SUBCASE("the default texture limit is 16384") {
		CHECK(compute_world_scale(art_in(2730, 100, 1000)) == 6);    // 16380
		CHECK(compute_world_scale(art_in(2731, 100, 1000)) == 3);    // 16386 > 16384
		CHECK(compute_world_scale(art_in(100, 2731, 1000)) == 3);    // either axis
	}
	SUBCASE("a renderer's own limit") {
		World_scale_in in = art_in(320, 200);
		in.max_tex        = 1920;
		CHECK(compute_world_scale(in) == 6);
		in.max_tex = 1919;
		CHECK(compute_world_scale(in) == 3);
		in.max_tex = 639;
		CHECK(compute_world_scale(in) == 1);
	}
	SUBCASE("the budget is inclusive") {
		// 320 x 200 x 36 = 2,304,000 px.
		CHECK(compute_world_scale(art_in(320, 200, 2.304)) == 6);
		CHECK(compute_world_scale(art_in(320, 200, 2.3039)) == 3);
		CHECK(compute_world_scale(art_in(320, 200, 0.576)) == 3);
		CHECK(compute_world_scale(art_in(320, 200, 0.5759)) == 2);
		CHECK(compute_world_scale(art_in(320, 200, 0.256)) == 2);
		CHECK(compute_world_scale(art_in(320, 200, 0.2559)) == 1);
	}
	SUBCASE("off and empty areas") {
		World_scale_in in = art_in(320, 200);
		in.policy         = World_policy::Off;
		CHECK(compute_world_scale(in) == 1);
		CHECK(compute_world_scale(art_in(0, 200)) == 1);
		CHECK(compute_world_scale(art_in(320, -1)) == 1);
	}
	SUBCASE("another S_art") {
		World_scale_in in = art_in(320, 200);
		in.s_art          = 4;
		CHECK(compute_world_scale(in) == 4);
		in.max_world_mpx = 1;    // 4 is 1.02 Mpx: 2
		CHECK(compute_world_scale(in) == 2);
	}
}

TEST_CASE("world scale: force:N") {
	World_scale_in in = art_in(320, 200, 0.001);    // The budget does not apply.
	in.policy         = World_policy::Force;
	for (int n = 2; n <= 8; n++) {
		in.force_n = n;
		CHECK(compute_world_scale(in) == n);
	}
	// Only the texture limit, stepping down by 1 (also to non-divisors).
	in.force_n = 6;
	in.max_tex = 1700;
	CHECK(compute_world_scale(in) == 5);
	in.max_tex = 1000;
	CHECK(compute_world_scale(in) == 3);
	in.max_tex = 300;
	CHECK(compute_world_scale(in) == 1);
}

TEST_CASE("world scale: auto") {
	// The smallest divisor of 6 covering the output, then the caps.
	CHECK(compute_world_scale(auto_in(320, 200, 1920, 1200)) == 6);
	CHECK(compute_world_scale(auto_in(320, 200, 1280, 800)) == 6);    // p = 4 -> 6
	CHECK(compute_world_scale(auto_in(320, 200, 960, 600)) == 3);
	CHECK(compute_world_scale(auto_in(320, 200, 700, 400)) == 3);    // p = 2.19 -> 3
	CHECK(compute_world_scale(auto_in(320, 200, 640, 400)) == 2);
	CHECK(compute_world_scale(auto_in(320, 200, 641, 400)) == 3);
	CHECK(compute_world_scale(auto_in(320, 200, 320, 200)) == 1);
	CHECK(compute_world_scale(auto_in(320, 200, 200, 100)) == 1);
	CHECK(compute_world_scale(auto_in(320, 200, 4000, 2500)) == 6);    // Beyond S_art: S_art
	// Aspect-correct modes divide the height by 1.2.
	CHECK(compute_world_scale(auto_in(860, 300, 3440, 1440, 1.2)) == 6);    // p = 4
	CHECK(compute_world_scale(auto_in(320, 200, 640, 480, 1.2)) == 2);      // p = 2
	CHECK(compute_world_scale(auto_in(320, 200, 640, 480, 1.0)) == 3);      // p = 2.4
	// The caps step down through divisors only.
	CHECK(compute_world_scale(auto_in(320, 200, 1920, 1200, 1.0, 1.0)) == 3);
	CHECK(compute_world_scale(auto_in(320, 200, 1920, 1200, 1.0, 0.4)) == 2);
}

TEST_CASE("world scale: art and auto never land on a non-divisor") {
	hires_test::Rng rng(0x5ca1e);
	for (int i = 0; i < 20000; i++) {
		World_scale_in in;
		in.policy        = rng.chance(50) ? World_policy::Art : World_policy::Auto;
		in.full_w        = 1 + rng.range(0, 3999);
		in.full_h        = 1 + rng.range(0, 2999);
		in.out_w         = rng.range(0, 7999);
		in.out_h         = rng.range(0, 4999);
		in.aspect_y      = rng.chance(50) ? 1.2 : 1.0;
		in.max_tex       = rng.chance(50) ? 0 : 512 + rng.range(0, 15999);
		in.max_world_mpx = 0.01 + double(rng.range(0, 7999)) / 100;
		const int S      = compute_world_scale(in);
		INFO(i);
		CHECK((S == 1 || S == 2 || S == 3 || S == 6));
		if (S > 1) {
			const int64_t max_tex = in.max_tex > 0 ? in.max_tex : world_default_max_texture;
			CHECK(int64_t(in.full_w) * S <= max_tex);
			CHECK(int64_t(in.full_h) * S <= max_tex);
			CHECK(double(in.full_w) * in.full_h * S * S <= in.max_world_mpx * 1e6);
		}
		if (in.policy == World_policy::Art) {
			// The largest divisor within the caps: no divisor above S fits them.
			const int64_t max_tex = in.max_tex > 0 ? in.max_tex : world_default_max_texture;
			for (const int d : {2, 3, 6}) {
				if (d > S) {
					CHECK_FALSE(
							(int64_t(in.full_w) * d <= max_tex && int64_t(in.full_h) * d <= max_tex
							 && double(in.full_w) * in.full_h * d * d <= in.max_world_mpx * 1e6));
				}
			}
		}
	}
}

TEST_CASE("world filter: the ladder") {
	constexpr int tw = 1920;
	constexpr int th = 1200;
	SUBCASE("equal integer ratios are exact") {
		CHECK(filter_of(1920, 1200, tw, th) == World_filter::Nearest);
		CHECK(filter_of(3840, 2400, tw, th) == World_filter::Nearest);
		CHECK(filter_of(5760, 3600, tw, th) == World_filter::Nearest);
	}
	SUBCASE("mixed axes") {
		CHECK(filter_of(3840, 1200, tw, th) == World_filter::Pixelart);    // 2 x 1: min 1, not equal
		CHECK(filter_of(3840, 1200, tw, th, false) == World_filter::Linear);
		CHECK(filter_of(3840, 960, tw, th) == World_filter::Linear);     // 2 x 0.8
		CHECK(filter_of(3840, 500, tw, th) == World_filter::Halving);    // 2 x 0.42
	}
	SUBCASE("fractional upscales: ACF 1.2 and HiDPI 1.25") {
		CHECK(filter_of(1920, 1440, tw, th) == World_filter::Pixelart);
		CHECK(filter_of(2400, 1500, tw, th) == World_filter::Pixelart);
		CHECK(filter_of(2400, 1500, tw, th, false) == World_filter::Linear);
	}
	SUBCASE("downscales") {
		CHECK(filter_of(1919, 1200, tw, th) == World_filter::Linear);
		CHECK(filter_of(960, 600, tw, th) == World_filter::Linear);    // Exactly 0.5: a 2x2 box
		const World_filter_choice below = choose_world_filter(959, 600, tw, th, true);
		CHECK(below.filter == World_filter::Halving);
		CHECK(below.halvings == 1);
		const World_filter_choice quarter = choose_world_filter(480, 300, tw, th, true);
		CHECK(quarter.filter == World_filter::Halving);    // 0.25: one halving, then LINEAR at 0.5
		CHECK(quarter.halvings == 1);
		const World_filter_choice eighth = choose_world_filter(239, 150, tw, th, true);
		CHECK(eighth.filter == World_filter::Halving);
		CHECK(eighth.halvings == 3);
	}
	SUBCASE("the halving depth is bounded") {
		const World_filter_choice tiny = choose_world_filter(1, 1, 16384, 16384, true);
		CHECK(tiny.filter == World_filter::Halving);
		CHECK(tiny.halvings == world_max_halvings);
		CHECK(world_max_halvings == 6);
	}
	SUBCASE("an empty letterbox rect skips the world") {
		CHECK(filter_of(0, 0, tw, th) == World_filter::Skip);
		CHECK(filter_of(1920, 0, tw, th) == World_filter::Skip);
		CHECK(filter_of(0, 1200, tw, th) == World_filter::Skip);
		CHECK(filter_of(-5, 1200, tw, th) == World_filter::Skip);
		CHECK(filter_of(1920, 1200, 0, th) == World_filter::Skip);
	}
	SUBCASE("present_filter forces one direct pass") {
		CHECK(choose_world_filter(480, 300, tw, th, true, World_filter_override::Nearest).filter == World_filter::Nearest);
		CHECK(choose_world_filter(480, 300, tw, th, true, World_filter_override::Linear).filter == World_filter::Linear);
		CHECK(choose_world_filter(1920, 1200, tw, th, true, World_filter_override::Linear).filter == World_filter::Linear);
		CHECK(choose_world_filter(2400, 1500, tw, th, true, World_filter_override::Pixelart).filter == World_filter::Pixelart);
		CHECK(choose_world_filter(2400, 1500, tw, th, false, World_filter_override::Pixelart).filter == World_filter::Linear);
		CHECK(choose_world_filter(0, 0, tw, th, true, World_filter_override::Nearest).filter == World_filter::Skip);
	}
	SUBCASE("NEAREST is never chosen for a downscale") {
		hires_test::Rng rng(77);
		for (int i = 0; i < 5000; i++) {
			const int  w      = 1 + rng.range(0, 5999);
			const int  h      = 1 + rng.range(0, 3999);
			const int  lw     = 1 + rng.range(0, 7999);
			const int  lh     = 1 + rng.range(0, 4999);
			const auto choice = choose_world_filter(lw, lh, w, h, rng.chance(50));
			if (lw < w || lh < h) {
				CHECK(choice.filter != World_filter::Nearest);
			}
			if (choice.filter == World_filter::Halving) {
				CHECK(choice.halvings >= 1);
				CHECK(choice.halvings <= world_max_halvings);
			}
		}
	}
}

TEST_CASE("world scale: tracked_to_phys") {
	SUBCASE("offsets: a 320x200 game in a 355x200 full area at S=6") {
		// The texture starts at logical (-17, 0).
		CHECK(same(tracked_to_phys(-17, 0, 355, 200, 17, 0, 6, 355, 200), World_phys_rect{0, 0, 2130, 1200}));
		CHECK(same(tracked_to_phys(0, 0, 320, 200, 17, 0, 6, 355, 200), World_phys_rect{102, 0, 1920, 1200}));
		CHECK(same(tracked_to_phys(10, 20, 1, 1, 17, 0, 6, 355, 200), World_phys_rect{162, 120, 6, 6}));
		CHECK(same(tracked_to_phys(10, 20, 1, 1, 17, 3, 3, 355, 206), World_phys_rect{81, 69, 3, 3}));
	}
	SUBCASE("negative logical rects are clamped, never negative") {
		CHECK(same(tracked_to_phys(-30, -5, 20, 10, 17, 0, 6, 355, 200), World_phys_rect{0, 0, 42, 30}));
		CHECK(tracked_to_phys(-40, 0, 20, 10, 17, 0, 6, 355, 200).empty());
	}
	SUBCASE("rects partly outside the texture are clamped") {
		CHECK(same(tracked_to_phys(330, 190, 100, 100, 17, 0, 6, 355, 200), World_phys_rect{2082, 1140, 48, 60}));
		CHECK(tracked_to_phys(338, 0, 5, 5, 17, 0, 6, 355, 200).empty());
		CHECK(tracked_to_phys(0, 200, 5, 5, 0, 0, 2, 320, 200).empty());
	}
	SUBCASE("empty input") {
		CHECK(tracked_to_phys(0, 0, 0, 5, 0, 0, 6, 320, 200).empty());
		CHECK(tracked_to_phys(0, 0, 5, -1, 0, 0, 6, 320, 200).empty());
	}
	SUBCASE("random rects: the exact intersection with the texture") {
		hires_test::Rng rng(4242);
		for (int i = 0; i < 20000; i++) {
			const int  S      = 1 + rng.range(0, 7);
			const int  full_w = 1 + rng.range(0, 899);
			const int  full_h = 1 + rng.range(0, 399);
			const int  off_x  = rng.range(0, 59);
			const int  off_y  = rng.range(0, 59);
			const int  x      = rng.range(0, 1999) - 1000;
			const int  y      = rng.range(0, 1999) - 1000;
			const int  w      = rng.range(0, 1199) - 100;
			const int  h      = rng.range(0, 1199) - 100;
			const auto r      = tracked_to_phys(x, y, w, h, off_x, off_y, S, full_w, full_h);
			// Model: logical intersection with [-off, full - off), then x S.
			const int lx0 = std::max(x, -off_x);
			const int ly0 = std::max(y, -off_y);
			const int lx1 = std::min(x + w, full_w - off_x);
			const int ly1 = std::min(y + h, full_h - off_y);
			INFO(i);
			if (w <= 0 || h <= 0 || lx1 <= lx0 || ly1 <= ly0) {
				CHECK(r.empty());
				CHECK(same(r, World_phys_rect{}));
			} else {
				CHECK(same(r, World_phys_rect{(lx0 + off_x) * S, (ly0 + off_y) * S, (lx1 - lx0) * S, (ly1 - ly0) * S}));
				CHECK(r.x >= 0);
				CHECK(r.y >= 0);
				CHECK(r.x + r.w <= full_w * S);
				CHECK(r.y + r.h <= full_h * S);
			}
		}
	}
	SUBCASE("clamp_phys") {
		CHECK(same(clamp_phys(World_phys_rect{-5, -5, 20, 20}, 10, 8), World_phys_rect{0, 0, 10, 8}));
		CHECK(same(clamp_phys(World_phys_rect{3, 2, 4, 4}, 10, 8), World_phys_rect{3, 2, 4, 4}));
		CHECK(clamp_phys(World_phys_rect{10, 0, 4, 4}, 10, 8).empty());
		CHECK(clamp_phys(World_phys_rect{0, 0, 0, 4}, 10, 8).empty());
	}
}
