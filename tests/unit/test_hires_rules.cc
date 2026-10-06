/*
 *  test_hires_rules.cc - Hashes, the T1 terrain key, the effective palette,
 *  rule P4 and the class-preserving reduction (DESIGN.md section 6.2,
 *  test_hires_rules).
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

#include "U7obj.h"
#include "doctest.h"
#include "hires_rules.h"
#include "hires_test_util.h"
#include "test_support.h"

#include <array>
#include <cinttypes>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using hires_test::data_path;
using hires_test::Rng;

namespace {
	std::vector<uint8_t> from_hex(const std::string& hex) {
		std::vector<uint8_t> out;
		if (hex == "-") {
			return out;
		}
		REQUIRE(hex.size() % 2 == 0);
		for (size_t i = 0; i < hex.size(); i += 2) {
			out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
		}
		return out;
	}

	// The T1 vector pattern: own tile t's pixel (x, y) = (a*t + b*x + c*y + d) & 0xff.
	uint8_t t1_pixel(const std::array<int, 4>& p, int t, int x, int y) {
		return static_cast<uint8_t>((p[0] * t + p[1] * x + p[2] * y + p[3]) & 0xff);
	}

	std::vector<uint8_t> random_tile(Rng& rng, int w, int h, int cycling_percent) {
		std::vector<uint8_t> px(static_cast<size_t>(w) * h);
		for (auto& v : px) {
			v = static_cast<uint8_t>(rng.chance(cycling_percent) ? rng.range(0xe0, 0xfe) : rng.range(0x01, 0xdf));
		}
		return px;
	}

	std::vector<uint8_t> nn(const std::vector<uint8_t>& src, int w, int h, int s) {
		std::vector<uint8_t> out(static_cast<size_t>(w) * h * s * s);
		Hires::nn_upscale(src.data(), w, h, s, out.data());
		return out;
	}
}    // namespace

TEST_CASE("hires rules: shared hash vectors (tests/data/hires/hash_vectors.txt)") {
	std::ifstream in(data_path("hires/hash_vectors.txt"));
	REQUIRE(in);
	int         counts[5] = {};
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::istringstream fields(line);
		std::string        kind;
		std::string        input;
		std::string        output;
		if (!(fields >> kind >> input >> output)) {
			continue;    // A blank line (or a lone '\r' in a CRLF checkout).
		}
		INFO(line.substr(0, 120));
		if (kind == "crc32") {
			const auto data = from_hex(input);
			CHECK(hires_test::hex8(Hires::crc32(data.data(), data.size())) == output);
			counts[0]++;
		} else if (kind == "fnv1a64") {
			const auto data = from_hex(input);
			char       buf[24];
			std::snprintf(buf, sizeof(buf), "%016" PRIx64, Hires::fnv1a64(data.data(), data.size()));
			CHECK(std::string(buf) == output);
			counts[1]++;
		} else if (kind == "get_color8") {
			CHECK(static_cast<int>(Hires::color8_from_6bit(static_cast<uint8_t>(std::stoi(input)))) == std::stoi(output));
			counts[2]++;
		} else if (kind == "t1") {
			const size_t slash = input.find('/');
			REQUIRE(slash == 64);
			const auto         own = from_hex(input.substr(0, 64));
			std::array<int, 4> pat{};
			std::istringstream nums(input.substr(slash + 1));
			char               dot = 0;
			nums >> pat[0] >> dot >> pat[1] >> dot >> pat[2] >> dot >> pat[3];
			std::vector<uint8_t>                 pix(128 * 128, 0);
			std::vector<std::array<uint8_t, 64>> flats(256);
			const uint8_t*                       tiles[256] = {};
			for (int t = 0; t < 256; t++) {
				if ((own[t >> 3] & (1 << (t & 7))) == 0) {
					continue;
				}
				for (int y = 0; y < 8; y++) {
					for (int x = 0; x < 8; x++) {
						flats[t][y * 8 + x]                                                 = t1_pixel(pat, t, x, y);
						pix[static_cast<size_t>((t / 16) * 8 + y) * 128 + (t % 16) * 8 + x] = t1_pixel(pat, t, x, y);
					}
				}
				tiles[t] = flats[t].data();
			}
			char buf[24];
			std::snprintf(buf, sizeof(buf), "%016" PRIx64, Hires::terrain_key_t1(own.data(), pix.data()));
			CHECK(std::string(buf) == output);
			CHECK(Hires::terrain_key_t1(tiles) == Hires::terrain_key_t1(own.data(), pix.data()));
			counts[3]++;
		} else if (kind == "pal8_crc32") {
			const auto pal6 = from_hex(input);
			REQUIRE(pal6.size() == 768);
			const Hires::Pal8 pal = Hires::pal8_from_6bit(pal6.data());
			CHECK(hires_test::hex8(Hires::palette_crc32(pal)) == output);
			counts[4]++;
		}
	}
	CHECK(counts[0] >= 6);
	CHECK(counts[1] >= 5);
	CHECK(counts[2] >= 9);
	CHECK(counts[3] >= 4);
	CHECK(counts[4] >= 1);
}

TEST_CASE("hires rules: CRC32 runs in pieces; FNV-1a check values") {
	const std::string text  = "The quick brown fox jumps over the lazy dog";
	const uint32_t    whole = Hires::crc32(text.data(), text.size());
	CHECK(whole == 0x414fa339U);
	CHECK(Hires::crc32(text.data() + 10, text.size() - 10, Hires::crc32(text.data(), 10)) == whole);
	CHECK(Hires::crc32("123456789", 9) == 0xcbf43926U);
	CHECK(Hires::crc32(nullptr, 0) == 0U);
	CHECK(Hires::fnv1a64("", 0) == 0xcbf29ce484222325ULL);
	CHECK(Hires::fnv1a64("a", 1) == 0xaf63dc4c8601ec8cULL);
	CHECK(Hires::fnv1a64("bar", 3, Hires::fnv1a64("foo", 3)) == Hires::fnv1a64("foobar", 6));
}

TEST_CASE("hires rules: palette 0 conversion clamps like Get_color8") {
	// Get_color8(v, 63, 100) = v * 25500 / 6300, clamped at 255.
	for (int v = 0; v < 256; v++) {
		const int expect = v * 25500 / 6300;
		CHECK(static_cast<int>(Hires::color8_from_6bit(static_cast<uint8_t>(v))) == (expect > 255 ? 255 : expect));
	}
	// The synthetic palette has BG's out-of-range index 255 = (250, 64, 1).
	const auto& game = hires_test::synth_game();
	REQUIRE(game.pal6.size() == 768);
	CHECK(game.pal6[765] == 250);
	CHECK(game.pal6[766] == 64);
	CHECK(game.pal6[767] == 1);
	CHECK(game.pal.rgb[765] == 255);
	CHECK(game.pal.rgb[766] == 255);
	CHECK(game.pal.rgb[767] == 4);
	// A uint8 wrap would give another CRC (BG: 0x78d19732 instead of 0xc9c2c0e7).
	Hires::Pal8 wrapped;
	for (int i = 0; i < 768; i++) {
		wrapped.rgb[i] = static_cast<uint8_t>(game.pal6[i] * 255 / 63);
	}
	CHECK(Hires::palette_crc32(wrapped) != Hires::palette_crc32(game.pal));
}

TEST_CASE("hires rules: BG palette 0 CRC (game data, U7_BG_STATIC)") {
	const char* stat = std::getenv("U7_BG_STATIC");
	if (stat == nullptr || *stat == '\0') {
		MESSAGE("skipped: U7_BG_STATIC is not set");
		return;
	}
	// As Palette::load + set_loaded: 768 bytes, or a double palette (even bytes).
	size_t         len = 0;
	const U7object obj(File_spec(std::string(stat) + "/palettes.flx"), 0);
	const auto     buf = obj.retrieve(len);
	REQUIRE(buf);
	REQUIRE((len == 768 || len >= 1536));
	uint8_t rgb6[768];
	for (int i = 0; i < 768; i++) {
		rgb6[i] = len == 768 ? buf[i] : buf[2 * i];
	}
	CHECK(rgb6[765] == 250);
	CHECK(rgb6[766] == 64);
	CHECK(rgb6[767] == 1);
	CHECK(Hires::palette_crc32(Hires::pal8_from_6bit(rgb6)) == 0xc9c2c0e7U);
	Hires::Pal8 wrapped;
	for (int i = 0; i < 768; i++) {
		wrapped.rgb[i] = static_cast<uint8_t>(rgb6[i] * 255 / 63);
	}
	CHECK(Hires::palette_crc32(wrapped) == 0x78d19732U);
}

TEST_CASE("hires rules: T1 key depends on own pixels and the own bitmap, not on the fill") {
	Rng                                  rng(0x7111);
	std::vector<std::array<uint8_t, 64>> flats(256);
	const uint8_t*                       tiles[256] = {};
	for (int t = 0; t < 256; t++) {
		for (auto& v : flats[t]) {
			v = rng.byte();
		}
		tiles[t] = rng.chance(80) ? flats[t].data() : nullptr;    // 20 % RLE (not own)
	}
	const uint64_t key = Hires::terrain_key_t1(tiles);
	// What the fill paints under the RLE tiles does not enter: those cells are
	// absent from the key's input (own bit clear, zero pixels).
	uint8_t              own[32] = {};
	std::vector<uint8_t> pix(128 * 128, 0);
	for (int t = 0; t < 256; t++) {
		if (tiles[t] != nullptr) {
			own[t >> 3] |= static_cast<uint8_t>(1 << (t & 7));
			for (int y = 0; y < 8; y++) {
				for (int x = 0; x < 8; x++) {
					pix[static_cast<size_t>((t / 16) * 8 + y) * 128 + (t % 16) * 8 + x] = tiles[t][y * 8 + x];
				}
			}
		}
	}
	CHECK(Hires::terrain_key_t1(own, pix.data()) == key);
	// One own pixel changes the key.
	int first_own = 0;
	while (tiles[first_own] == nullptr) {
		first_own++;
	}
	flats[first_own][37] ^= 1;
	CHECK(Hires::terrain_key_t1(tiles) != key);
	flats[first_own][37] ^= 1;
	CHECK(Hires::terrain_key_t1(tiles) == key);
	// The own bitmap changes the key, even for an all-zero flat.
	static const std::array<uint8_t, 64> zeros{};
	int                                  rle = 0;
	while (tiles[rle] != nullptr) {
		rle++;
	}
	tiles[rle] = zeros.data();
	CHECK(Hires::terrain_key_t1(tiles) != key);
	tiles[rle]       = nullptr;
	tiles[first_own] = nullptr;
	CHECK(Hires::terrain_key_t1(tiles) != key);
}

TEST_CASE("hires rules: cycle ranges of Game_window::rotatecolours") {
	for (int i = 0; i < 0xe0; i++) {
		CHECK(Hires::cycle_range(static_cast<uint8_t>(i)) == -1);
	}
	const int expect[][3] = {
			{0xe0, 0xe7, 0},
            {0xe8, 0xef, 1},
            {0xf0, 0xf3, 2},
            {0xf4, 0xf7, 3},
            {0xf8, 0xfb, 4},
            {0xfc, 0xfe, 5}
    };
	for (const auto& r : expect) {
		for (int i = r[0]; i <= r[1]; i++) {
			CHECK(Hires::cycle_range(static_cast<uint8_t>(i)) == r[2]);
		}
	}
	CHECK(Hires::cycle_range(0xff) == -1);
}

TEST_CASE("hires rules: P4 on crafted tiles") {
	constexpr int        S = 6;
	std::vector<uint8_t> parent(64, 0x10);
	parent[3 * 8 + 3]        = 0xe3;    // E0-E7 at (3, 3)
	parent[0]                = 0xfc;    // FC-FE at the corner (0, 0)
	std::vector<uint8_t> art = nn(parent, 8, 8, S);
	CHECK(Hires::p4_violations(art.data(), S, parent.data(), 8, 8) == 0);

	auto put = [&](int px, int py, uint8_t v) {
		art[static_cast<size_t>(py * S) * 48 + px * S] = v;    // top-left sub-pixel of a block
	};
	put(2, 2, 0xe6);    // neighbour of (3, 3), same range: compliant
	put(4, 4, 0xe1);
	CHECK(Hires::p4_violations(art.data(), S, parent.data(), 8, 8) == 0);
	put(5, 3, 0xe1);    // two pixels away: not compliant
	CHECK(Hires::p4_violations(art.data(), S, parent.data(), 8, 8) == 1);
	put(3, 4, 0xf0);    // next to (3, 3) but another range: not compliant
	CHECK(Hires::p4_violations(art.data(), S, parent.data(), 8, 8) == 2);
	put(1, 1, 0xfe);    // corner neighbourhood, clamped at the edge: compliant
	put(0, 1, 0xfd);
	CHECK(Hires::p4_violations(art.data(), S, parent.data(), 8, 8) == 2);
	put(6, 6, 0xff);    // 0xFF is not a cycling index (P0's business)
	CHECK(Hires::p4_violations(art.data(), S, parent.data(), 8, 8) == 2);

	// A cycling pixel whose only in-range neighbour lies in the next tile: not
	// compliant for the tile (neighbours are in-tile), compliant in a layer
	// that holds both tiles.
	std::vector<uint8_t> left(64, 0x20);
	std::vector<uint8_t> right(64, 0x20);
	right[4 * 8 + 0]                                  = 0xe2;    // the right tile's left column
	std::vector<uint8_t> left_art                     = nn(left, 8, 8, S);
	left_art[static_cast<size_t>(4 * S) * 48 + 7 * S] = 0xe5;    // the left tile's right column
	CHECK(Hires::p4_violations(left_art.data(), S, left.data(), 8, 8) == 1);
	std::vector<uint8_t> layer(16 * 8);
	std::vector<uint8_t> layer_art(static_cast<size_t>(16 * S) * 8 * S);
	const auto           right_art = nn(right, 8, 8, S);
	for (int y = 0; y < 8; y++) {
		for (int x = 0; x < 8; x++) {
			layer[y * 16 + x]     = left[y * 8 + x];
			layer[y * 16 + 8 + x] = right[y * 8 + x];
		}
	}
	for (int y = 0; y < 8 * S; y++) {
		for (int x = 0; x < 8 * S; x++) {
			layer_art[static_cast<size_t>(y) * 16 * S + x]         = left_art[static_cast<size_t>(y) * 8 * S + x];
			layer_art[static_cast<size_t>(y) * 16 * S + 8 * S + x] = right_art[static_cast<size_t>(y) * 8 * S + x];
		}
	}
	CHECK(Hires::p4_violations(layer_art.data(), S, layer.data(), 16, 8) == 0);

	CHECK(Hires::p4_violations(nullptr, S, parent.data(), 8, 8) == -1);
	CHECK(Hires::p4_violations(art.data(), 0, parent.data(), 8, 8) == -1);
}

TEST_CASE("hires rules: P4 thresholds (reject above 0.5 %)") {
	CHECK(Hires::p4_verdict(0, 2304) == Hires::P4_verdict::ok);
	CHECK(Hires::p4_verdict(1, 2304) == Hires::P4_verdict::warning);
	CHECK(Hires::p4_verdict(11, 2304) == Hires::P4_verdict::warning);
	CHECK(Hires::p4_verdict(12, 2304) == Hires::P4_verdict::reject);    // 12 * 200 > 2304
	CHECK(Hires::p4_verdict(1, 200) == Hires::P4_verdict::warning);     // exactly 0.5 %
	CHECK(Hires::p4_verdict(2, 200) == Hires::P4_verdict::reject);
	CHECK(Hires::p4_verdict(2949, 589824) == Hires::P4_verdict::warning);    // 768^2 terrain
	CHECK(Hires::p4_verdict(2950, 589824) == Hires::P4_verdict::reject);
}

TEST_CASE("hires rules: reduce_mode keeps the cycling class of the parent") {
	// One 2x2 block (6 -> 3 on an 8x8 tile): check the block at (0, 0) only.
	auto reduce_block = [](uint8_t parent_px, const std::array<uint8_t, 4>& block) {
		std::vector<uint8_t> parent(64, 0x10);
		parent[0]                = parent_px;
		std::vector<uint8_t> src = nn(parent, 8, 8, 6);
		src[0]                   = block[0];
		src[1]                   = block[1];
		src[48]                  = block[2];
		src[49]                  = block[3];
		std::vector<uint8_t> dst(24 * 24);
		REQUIRE(Hires::reduce_mode(src.data(), 6, 3, parent.data(), 8, 8, dst.data()));
		return dst[0];
	};
	// Parent in E0-E7 and the block holds E0-E7 indices: the most frequent of those.
	CHECK(reduce_block(0xe2, {0xe4, 0x10, 0x10, 0x10}) == 0xe4);
	CHECK(reduce_block(0xe2, {0xe4, 0xe5, 0xe5, 0x10}) == 0xe5);
	// Parent cycling but no index of its range in the block: the non-cycling mode.
	CHECK(reduce_block(0xe2, {0xf1, 0x30, 0x30, 0xf1}) == 0x30);
	// Static parent: the most frequent non-cycling index, even against a cycling majority.
	CHECK(reduce_block(0x10, {0xe4, 0xe4, 0xe4, 0x20}) == 0x20);
	// No non-cycling index at all: the most frequent index.
	CHECK(reduce_block(0x10, {0xe4, 0xe4, 0xf1, 0xe5}) == 0xe4);
	// Ties go to the smallest index.
	CHECK(reduce_block(0x10, {0x20, 0x11, 0x20, 0x11}) == 0x11);
	CHECK(reduce_block(0xe2, {0xe6, 0xe1, 0xe1, 0xe6}) == 0xe1);
	CHECK(reduce_block(0x10, {0xe6, 0xf2, 0xf2, 0xe6}) == 0xe6);

	std::vector<uint8_t> buf(64 * 36);
	std::vector<uint8_t> parent(64, 0x10);
	CHECK_FALSE(Hires::reduce_mode(buf.data(), 6, 4, parent.data(), 8, 8, buf.data()));    // 6 % 4 != 0
	CHECK_FALSE(Hires::reduce_mode(buf.data(), 3, 6, parent.data(), 8, 8, buf.data()));    // upscaling
	CHECK_FALSE(Hires::reduce_mode(buf.data(), 6, 3, nullptr, 8, 8, buf.data()));
}

TEST_CASE("hires rules: reduce(NN6(x)) == NN_k(x) for random tiles and layers") {
	Rng rng(0x5ed6);
	for (int round = 0; round < 40; round++) {
		const bool layer = round % 10 == 9;    // some 128x128 terrain layers too
		const int  w     = layer ? 128 : 8;
		const auto x     = random_tile(rng, w, w, round % 3 == 0 ? 0 : 30);
		const auto x6    = nn(x, w, w, 6);
		for (const int k : {3, 2, 1}) {
			std::vector<uint8_t> out(static_cast<size_t>(w) * w * k * k);
			REQUIRE(Hires::reduce_mode(x6.data(), 6, k, x.data(), w, w, out.data()));
			CHECK(out == nn(x, w, w, k));
		}
		// Through an intermediate scale, too: 6 -> 3 -> 1.
		const auto           x3 = nn(x, w, w, 3);
		std::vector<uint8_t> out1(static_cast<size_t>(w) * w);
		REQUIRE(Hires::reduce_mode(x3.data(), 3, 1, x.data(), w, w, out1.data()));
		CHECK(out1 == x);
	}
}
