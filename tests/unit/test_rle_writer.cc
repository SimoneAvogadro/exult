/*
 *  test_rle_writer.cc - Self-test of the test-only RLE writer and decoder
 *  (rle_writer.h) against the engine's encoder and the scale-1 painter.
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
#include "ibuf8.h"
#include "rle_writer.h"
#include "test_support.h"
#include "vgafile.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

using hires_test::decode_rle;
using hires_test::Rle_policy;
using hires_test::Rle_writer;
using hires_test::Rng;

namespace {
	// A w x h raster of transparent (0xff), repeated and literal runs.
	std::vector<unsigned char> random_raster(Rng& rng, int w, int h) {
		std::vector<unsigned char> pixels(static_cast<size_t>(w) * h);
		for (auto it = pixels.begin(); it != pixels.end();) {
			const int  kind = rng.range(0, 2);
			const auto len  = std::min<ptrdiff_t>(rng.range(1, 300), pixels.end() - it);
			if (kind == 0) {
				std::fill_n(it, len, 0xff);
			} else if (kind == 1) {
				std::fill_n(it, len, static_cast<unsigned char>(rng.range(0, 254)));
			} else {
				for (ptrdiff_t i = 0; i < len; i++) {
					it[i] = static_cast<unsigned char>(rng.range(0, 254));
				}
			}
			it += len;
		}
		return pixels;
	}

	// The w x h raster that `data` describes (origin at xleft, yabove), with
	// 0xff where nothing is painted.
	std::vector<unsigned char> decode_to_raster(
			const unsigned char* data, size_t size, int w, int h, int xleft, int yabove, bool& ok) {
		std::vector<unsigned char> raster(static_cast<size_t>(w) * h, 0xff);
		bool                       inside = true;
		ok = decode_rle(data, size, xleft, yabove, Rle_policy::strict, [&](int x, int y, unsigned char pix) {
			if (x < 0 || x >= w || y < 0 || y >= h) {
				inside = false;
				return;
			}
			raster[static_cast<size_t>(y) * w + x] = pix;
		});
		ok = ok && inside;
		return raster;
	}

	// paint_rle on a scale-1 buffer must equal the reference decoder plus the
	// buffer's clip rectangle.
	void check_paint(Image_buffer8& buf, const std::vector<unsigned char>& data, int xoff, int yoff) {
		const int                  w    = static_cast<int>(buf.get_width());
		const int                  h    = static_cast<int>(buf.get_height());
		const size_t               size = static_cast<size_t>(w) * h;
		std::vector<unsigned char> expected(buf.get_bits(), buf.get_bits() + size);
		int                        cx;
		int                        cy;
		int                        cw;
		int                        ch;
		buf.get_clip(cx, cy, cw, ch);
		const bool ok = decode_rle(data, xoff, yoff, Rle_policy::strict, [&](int x, int y, unsigned char pix) {
			if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) {
				expected[static_cast<size_t>(y) * w + x] = pix;
			}
		});
		REQUIRE(ok);
		buf.paint_rle(xoff, yoff, data.data());
		CHECK(std::memcmp(expected.data(), buf.get_bits(), size) == 0);
	}
}    // namespace

TEST_CASE("rle_writer: rasters round-trip and paint like the engine at scale 1") {
	Rng rng(0x51e7'2a5e'0000'0001ULL);
	for (int round = 0; round < 40; round++) {
		const int w      = rng.range(1, round < 20 ? 64 : 300);
		const int h      = rng.range(1, 12);
		const int xleft  = rng.range(0, w - 1);
		const int yabove = rng.range(0, h - 1);
		auto      pixels = random_raster(rng, w, h);
		INFO("round " << round << ": " << w << "x" << h);

		for (const auto style : {Rle_writer::Style::raw, Rle_writer::Style::encoded, Rle_writer::Style::alternate}) {
			const auto data = Rle_writer().raster(pixels.data(), w, h, xleft, yabove, style).finish();
			bool       ok;
			CHECK(decode_to_raster(data.data(), data.size(), w, h, xleft, yabove, ok) == pixels);
			CHECK(ok);

			// Paint with the frame partly outside the buffer and a random clip.
			Image_buffer8 buf(w + 24, h + 10);
			std::memset(buf.get_bits(), 0x11, static_cast<size_t>(w + 24) * (h + 10));
			if (rng.chance(50)) {
				const int clip_w = rng.range(1, w + 24);
				const int clip_h = rng.range(1, h + 10);
				buf.set_clip(rng.range(0, w + 24 - clip_w), rng.range(0, h + 10 - clip_h), clip_w, clip_h);
			}
			check_paint(buf, data, rng.range(-w / 2, w + 12), rng.range(-h / 2, h + 5));
		}

		// The engine's encoder (narrow frames only, P6) decodes to the same raster.
		if (w <= 64) {
			Shape_frame frame(pixels.data(), w, h, xleft, yabove, true);
			bool        ok;
			CHECK(decode_to_raster(frame.get_data(), static_cast<size_t>(frame.get_size()), w, h, xleft, yabove, ok) == pixels);
			CHECK(ok);
		}
	}
}

TEST_CASE("rle_writer: scans of 4096 px and more paint correctly at scale 1") {
	Rng                        rng(0x51e7'2a5e'0000'0002ULL);
	std::vector<unsigned char> long_raw(5000);
	for (auto& pix : long_raw) {
		pix = static_cast<unsigned char>(rng.range(0, 254));
	}
	std::vector<unsigned char> long_runs;
	while (long_runs.size() < 4500) {
		const auto pix = static_cast<unsigned char>(rng.range(0, 254));
		long_runs.insert(long_runs.end(), static_cast<size_t>(rng.range(1, 200)), pix);
	}
	long_runs.resize(4500);
	const auto data = Rle_writer().raw(-300, 0, long_raw).encoded(2000, 1, long_runs).raw(5990, 2, long_raw).finish();

	Image_buffer8 buf(6000, 4);
	std::memset(buf.get_bits(), 0x22, 6000 * 4);
	SUBCASE("full clip") {}
	SUBCASE("narrow clip") {
		buf.set_clip(1000, 0, 4000, 4);
	}
	SUBCASE("clip excludes a row") {
		buf.set_clip(0, 1, 6000, 1);
	}
	check_paint(buf, data, 0, 0);
}

TEST_CASE("rle_writer: the longest scan the format allows paints at scale 1") {
	// 32,767 px in one encoded scan (several hundred runs).
	std::vector<unsigned char> longest(Rle_writer::max_scan);
	for (size_t i = 0; i < longest.size(); i++) {
		longest[i] = static_cast<unsigned char>((i / 97) % 255);
	}
	const auto    data = Rle_writer().encoded(-5, 0, longest).raw(7, 1, longest).finish();
	Image_buffer8 wide(Rle_writer::max_scan + 10, 2);
	std::memset(wide.get_bits(), 0x33, 2 * (Rle_writer::max_scan + 10));
	check_paint(wide, data, 0, 0);
}

TEST_CASE("rle_writer: malformed runs are written on demand") {
	// A repeat run of 20 in a scan that declares 10 pixels.
	const auto data = Rle_writer().begin_encoded(3, 4, 10).repeat_run(20, 7).finish();
	CHECK(data == std::vector<unsigned char>{21, 0, 3, 0, 4, 0, 41, 7, 0, 0});

	std::vector<int> xs;

	auto record = [&](int x, int y, unsigned char pix) {
		CHECK(y == 4);
		CHECK(pix == 7);
		xs.push_back(x);
	};
	CHECK_FALSE(decode_rle(data, 0, 0, Rle_policy::strict, record));
	xs.clear();
	CHECK(decode_rle(data, 0, 0, Rle_policy::clamp, record));
	CHECK(xs == std::vector<int>{3, 4, 5, 6, 7, 8, 9, 10, 11, 12});

	// A literal run of 9 in a scan of 5: under `clamp` the input skips all 9
	// pixels, and the next scan is read correctly.
	const auto data2 = Rle_writer().begin_encoded(0, 0, 5).literal_run({1, 2, 3, 4, 5, 6, 7, 8, 9}).raw(0, 1, {9, 9}).finish();
	std::vector<unsigned char> row0;
	std::vector<unsigned char> row1;
	CHECK(decode_rle(data2, 0, 0, Rle_policy::clamp, [&](int, int y, unsigned char pix) {
		(y == 0 ? row0 : row1).push_back(pix);
	}));
	CHECK(row0 == std::vector<unsigned char>{1, 2, 3, 4, 5});
	CHECK(row1 == std::vector<unsigned char>{9, 9});

	// The writer refuses what the format cannot express.
	CHECK_THROWS_AS(Rle_writer().raw(0, 0, std::vector<unsigned char>(Rle_writer::max_scan + 1)), std::invalid_argument);
	CHECK_THROWS_AS(Rle_writer().repeat_run(Rle_writer::max_run + 1, 0), std::invalid_argument);
	CHECK_THROWS_AS(Rle_writer().raw(40000, 0, {1}), std::invalid_argument);
	// The header of an empty raw scan would be the terminator, and every scan
	// after it would be lost.
	CHECK_THROWS_AS(Rle_writer().raw(0, 0, {}), std::invalid_argument);

	// An empty encoded scan (header word 1) is valid: the scan after it is read,
	// by the decoder and by the engine's painter.
	const auto data3 = Rle_writer().encoded(2, 3, {}).raw(4, 5, {6}).finish();
	CHECK(data3 == std::vector<unsigned char>{1, 0, 2, 0, 3, 0, 2, 0, 4, 0, 5, 0, 6, 0, 0});
	std::vector<int> plotted;
	CHECK(decode_rle(data3, 0, 0, Rle_policy::strict, [&](int x, int y, unsigned char pix) {
		plotted.insert(plotted.end(), {x, y, pix});
	}));
	CHECK(plotted == std::vector<int>{4, 5, 6});
	Image_buffer8 buf(8, 8);
	std::memset(buf.get_bits(), 0x44, 8 * 8);
	check_paint(buf, data3, 0, 0);
}

TEST_CASE("rle_writer: the decoder stops at the end of truncated data") {
	Rng        rng(0x51e7'2a5e'0000'0003ULL);
	auto       pixels = random_raster(rng, 50, 6);
	const auto data   = Rle_writer().raster(pixels.data(), 50, 6, 0, 0, Rle_writer::Style::alternate).finish();
	for (size_t len = 0; len < data.size(); len++) {
		// Copy the prefix, so that ASan sees any read past its end.
		const std::vector<unsigned char> prefix(data.begin(), data.begin() + static_cast<ptrdiff_t>(len));
		const unsigned char* const       begin = prefix.empty() ? nullptr : prefix.data();
		auto                             none  = [](int, int, unsigned char) {};
		CHECK_FALSE(decode_rle(begin, prefix.size(), 0, 0, Rle_policy::strict, none));
		CHECK_FALSE(decode_rle(begin, prefix.size(), 0, 0, Rle_policy::clamp, none));
	}
}
