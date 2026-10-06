/*
 *  test_hires_png.cc - The raw-index PNG reader and writer of the hi-res
 *  store (DESIGN.md section 6.2, test_hires_png). Run under ASan/LSan in
 *  build-asan: the failure paths must not leak.
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
#include "hires_png.h"
#include "hires_rules.h"
#include "hires_store.h"
#include "hires_test_util.h"
#include "test_support.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using Hires::Indexed_png;
using Hires::Png_status;
using hires_test::Png_builder;
using hires_test::Rng;
using hires_test::Temp_dir;
namespace fs = std::filesystem;

#ifdef HAVE_PNG_H

namespace {
	std::vector<uint8_t> random_indices(Rng& rng, int n, int max_index) {
		std::vector<uint8_t> px(static_cast<size_t>(n));
		for (auto& v : px) {
			v = static_cast<uint8_t>(rng.range(0, max_index));
		}
		return px;
	}

	// A valid 48x48 palette PNG built chunk by chunk.
	std::vector<uint8_t> small_png(const std::vector<uint8_t>& px, const Hires::Pal8& pal) {
		return Png_builder()
				.ihdr(48, 48, 8, 3)
				.plte(pal.rgb, 256)
				.idat(hires_test::filtered_rows(px.data(), 48, 48))
				.iend()
				.bytes();
	}
}    // namespace

TEST_CASE("hires png: 8-bit round trip with PLTE, tRNS and text before and after IDAT") {
	Rng                      rng(0x9001);
	const Hires::Pal8        pal = hires_test::test_palette();
	const auto               px  = random_indices(rng, 48 * 48, 255);
	Hires::Png_write_options opt;
	opt.trns        = {0, 128};
	opt.text_before = {
			{"Exult-Src-CRC32",  "0123abcd"},
            {   "Exult-Origin", "unit test"}
    };
	opt.text_after = {
			{     "Comment",  "after"},
            {"Exult-Origin", "second"}
    };
	std::vector<uint8_t> bytes;
	REQUIRE(Hires::encode_indexed_png(48, 48, px.data(), pal.rgb, 256, opt, bytes) == Png_status::ok);

	Indexed_png png;
	REQUIRE(Hires::read_indexed_png(bytes.data(), bytes.size(), 48, 48, png) == Png_status::ok);
	CHECK(png.width == 48);
	CHECK(png.height == 48);
	CHECK(png.color_type == 3);
	CHECK(png.bit_depth == 8);
	CHECK(png.pixels == px);
	CHECK(png.num_palette == 256);
	CHECK(png.plte == std::vector<uint8_t>(pal.rgb, pal.rgb + 768));
	CHECK(png.has_trns);
	REQUIRE(png.text.size() == 4);
	CHECK(png.text[0].key == "Exult-Src-CRC32");
	CHECK(png.text[2].key == "Comment");
	CHECK(png.text[2].value == "after");
	REQUIRE(png.find_text("Exult-Origin") != nullptr);
	CHECK(*png.find_text("Exult-Origin") == "unit test");    // the first one wins
	CHECK(png.find_text("Missing") == nullptr);

	// The same through a file, and the writer is deterministic.
	Temp_dir tmp;
	REQUIRE(Hires::write_indexed_png(tmp / "a.png", 48, 48, px.data(), pal.rgb, 256, opt) == Png_status::ok);
	CHECK(hires_test::read_file(tmp / "a.png") == bytes);
	Indexed_png again;
	REQUIRE(Hires::read_indexed_png(tmp / "a.png", 48, 48, again) == Png_status::ok);
	CHECK(again.pixels == px);
	CHECK(again.has_trns);
}

TEST_CASE("hires png: depths 1, 2 and 4 are expanded to raw indices; Adam7 interlacing") {
	Rng               rng(0x9002);
	const Hires::Pal8 pal = hires_test::test_palette();
	for (const int depth : {1, 2, 4, 8}) {
		for (const bool interlace : {false, true}) {
			INFO("depth " << depth << " interlace " << interlace);
			const int                w  = 13 + depth;    // rows that do not end on a byte boundary
			const int                h  = 11;
			const auto               px = random_indices(rng, w * h, (1 << depth) - 1);
			Hires::Png_write_options opt;
			opt.bit_depth = depth;
			opt.interlace = interlace;
			std::vector<uint8_t> bytes;
			REQUIRE(Hires::encode_indexed_png(w, h, px.data(), pal.rgb, 1 << depth, opt, bytes) == Png_status::ok);
			Indexed_png png;
			REQUIRE(Hires::read_indexed_png(bytes.data(), bytes.size(), w, h, png) == Png_status::ok);
			CHECK(png.bit_depth == depth);
			CHECK(png.interlace == (interlace ? 1 : 0));
			CHECK(png.num_palette == (1 << depth));
			CHECK(png.pixels == px);
		}
	}
}

TEST_CASE("hires png: other colour types are F1 (not_palette), after the size check (F3)") {
	std::vector<uint8_t> rgb(48 * 48 * 3, 0x40);
	const auto  good = Png_builder().ihdr(48, 48, 8, 2).idat(hires_test::filtered_rows(rgb.data(), 48 * 3, 48)).iend().bytes();
	Indexed_png png;
	CHECK(Hires::read_indexed_png(good.data(), good.size(), 48, 48, png) == Png_status::not_palette);
	CHECK(png.color_type == 2);
	CHECK(png.pixels.empty());
	// Wrong size and wrong colour type: the size is checked first, as hirescheck.py does.
	std::vector<uint8_t> rgb40(40 * 40 * 3, 0x40);
	const auto small = Png_builder().ihdr(40, 40, 8, 2).idat(hires_test::filtered_rows(rgb40.data(), 40 * 3, 40)).iend().bytes();
	CHECK(Hires::read_indexed_png(small.data(), small.size(), 48, 48, png) == Png_status::wrong_size);
	// Grey and RGBA too.
	std::vector<uint8_t> grey(48 * 48, 7);
	const auto           g = Png_builder().ihdr(48, 48, 8, 0).idat(hires_test::filtered_rows(grey.data(), 48, 48)).iend().bytes();
	CHECK(Hires::read_indexed_png(g.data(), g.size(), 48, 48, png) == Png_status::not_palette);
	std::vector<uint8_t> rgba(48 * 48 * 4, 9);
	const auto a = Png_builder().ihdr(48, 48, 8, 6).idat(hires_test::filtered_rows(rgba.data(), 48 * 4, 48)).iend().bytes();
	CHECK(Hires::read_indexed_png(a.data(), a.size(), 48, 48, png) == Png_status::not_palette);
}

TEST_CASE("hires png: tEXt, zTXt and iTXt before and after IDAT, in file order") {
	Rng               rng(0x9003);
	const Hires::Pal8 pal   = hires_test::test_palette();
	const auto        px    = random_indices(rng, 48 * 48, 200);
	const auto        bytes = Png_builder()
							   .ihdr(48, 48, 8, 3)
							   .plte(pal.rgb, 256)
							   .ztxt("Exult-Origin", "compressed route")
							   .idat(hires_test::filtered_rows(px.data(), 48, 48))
							   .text("Exult-Src-CRC32", "deadbeef")
							   .itxt("Note", "utf-8 text")
							   .text("Exult-Src-CRC32", "00000000")
							   .iend()
							   .bytes();
	Indexed_png png;
	REQUIRE(Hires::read_indexed_png(bytes.data(), bytes.size(), 48, 48, png) == Png_status::ok);
	REQUIRE(png.text.size() == 4);
	CHECK(png.text[0].key == "Exult-Origin");
	CHECK(png.text[0].value == "compressed route");
	REQUIRE(png.find_text("Exult-Src-CRC32") != nullptr);
	CHECK(*png.find_text("Exult-Src-CRC32") == "deadbeef");
	REQUIRE(png.find_text("Note") != nullptr);
	CHECK(*png.find_text("Note") == "utf-8 text");
	CHECK(png.pixels == px);
}

TEST_CASE("hires png: truncated and garbage files are read errors (no leaks under ASan)") {
	Rng                      rng(0x9004);
	const Hires::Pal8        pal = hires_test::test_palette();
	const auto               px  = random_indices(rng, 48 * 48, 255);
	Hires::Png_write_options opt;
	opt.text_after = {
			{"Exult-Src-CRC32", "0123abcd"}
    };
	std::vector<uint8_t> bytes;
	REQUIRE(Hires::encode_indexed_png(48, 48, px.data(), pal.rgb, 256, opt, bytes) == Png_status::ok);
	Indexed_png png;
	for (size_t len = 0; len < bytes.size(); len++) {
		INFO("prefix " << len << " of " << bytes.size());
		CHECK(Hires::read_indexed_png(bytes.data(), len, 48, 48, png) == Png_status::read_error);
		CHECK(png.pixels.empty());
		CHECK_FALSE(png.error.empty());
	}
	CHECK(Hires::read_indexed_png(bytes.data(), bytes.size(), 48, 48, png) == Png_status::ok);
	// Garbage, with and without a PNG signature.
	for (int round = 0; round < 50; round++) {
		std::vector<uint8_t> junk(static_cast<size_t>(rng.range(0, 3000)));
		for (auto& v : junk) {
			v = rng.byte();
		}
		if (round % 2 == 1 && junk.size() >= 8) {
			std::copy(bytes.begin(), bytes.begin() + 8, junk.begin());
		}
		CHECK(Hires::read_indexed_png(junk.data(), junk.size(), 48, 48, png) == Png_status::read_error);
	}
	// A missing file.
	Temp_dir tmp;
	CHECK(Hires::read_indexed_png(tmp / "missing.png", 48, 48, png) == Png_status::open_error);
	// A truncated file on disk.
	hires_test::write_file(
			tmp / "cut.png", std::vector<uint8_t>(bytes.begin(), bytes.begin() + static_cast<long>(bytes.size() / 2)));
	CHECK(Hires::read_indexed_png(tmp / "cut.png", 48, 48, png) == Png_status::read_error);
}

TEST_CASE("hires png: errors inside libpng take the longjmp path cleanly") {
	Rng               rng(0x9005);
	const Hires::Pal8 pal = hires_test::test_palette();
	const auto        px  = random_indices(rng, 48 * 48, 255);
	Indexed_png       png;
	// IDAT with a valid CRC but no valid deflate stream.
	std::vector<uint8_t> junk(300);
	for (auto& v : junk) {
		v = rng.byte();
	}
	const auto bad_zlib = Png_builder().ihdr(48, 48, 8, 3).plte(pal.rgb, 256).chunk("IDAT", junk).iend().bytes();
	CHECK(Hires::read_indexed_png(bad_zlib.data(), bad_zlib.size(), 48, 48, png) == Png_status::read_error);
	CHECK(png.error.find("libpng") != std::string::npos);
	// Too little image data.
	const auto short_idat
			= Png_builder().ihdr(48, 48, 8, 3).plte(pal.rgb, 256).idat(hires_test::filtered_rows(px.data(), 48, 20)).iend().bytes();
	CHECK(Hires::read_indexed_png(short_idat.data(), short_idat.size(), 48, 48, png) == Png_status::read_error);
	// A bad filter type byte.
	auto rows             = hires_test::filtered_rows(px.data(), 48, 48);
	rows[49 * 7]          = 9;
	const auto bad_filter = Png_builder().ihdr(48, 48, 8, 3).plte(pal.rgb, 256).idat(rows).iend().bytes();
	CHECK(Hires::read_indexed_png(bad_filter.data(), bad_filter.size(), 48, 48, png) == Png_status::read_error);
	// An invalid depth for a palette image.
	const auto depth16 = Png_builder().ihdr(48, 48, 16, 3).plte(pal.rgb, 256).idat(rows).iend().bytes();
	CHECK(Hires::read_indexed_png(depth16.data(), depth16.size(), 48, 48, png) == Png_status::read_error);
	// No PLTE.
	const auto no_plte = Png_builder().ihdr(48, 48, 8, 3).idat(hires_test::filtered_rows(px.data(), 48, 48)).iend().bytes();
	CHECK(Hires::read_indexed_png(no_plte.data(), no_plte.size(), 48, 48, png) == Png_status::read_error);
	// The reader still works afterwards.
	const auto good = small_png(px, pal);
	CHECK(Hires::read_indexed_png(good.data(), good.size(), 48, 48, png) == Png_status::ok);
	CHECK(png.pixels == px);
}

TEST_CASE("hires png: chunk structure as hirescheck.py reads it") {
	Rng               rng(0x9006);
	const Hires::Pal8 pal  = hires_test::test_palette();
	const auto        px   = random_indices(rng, 48 * 48, 255);
	const auto        rows = hires_test::filtered_rows(px.data(), 48, 48);
	Indexed_png       png;
	// A critical chunk with a bad CRC: unreadable.
	const auto bad_plte = Png_builder()
								  .ihdr(48, 48, 8, 3)
								  .chunk("PLTE", std::vector<uint8_t>(pal.rgb, pal.rgb + 768), true)
								  .idat(rows)
								  .iend()
								  .bytes();
	CHECK(Hires::read_indexed_png(bad_plte.data(), bad_plte.size(), 48, 48, png) == Png_status::read_error);
	CHECK(png.error.find("CRC error in PLTE") != std::string::npos);
	const auto bad_ihdr = Png_builder()
								  .chunk("IHDR", {0, 0, 0, 48, 0, 0, 0, 48, 8, 3, 0, 0, 0}, true)
								  .plte(pal.rgb, 256)
								  .idat(rows)
								  .iend()
								  .bytes();
	CHECK(Hires::read_indexed_png(bad_ihdr.data(), bad_ihdr.size(), 48, 48, png) == Png_status::read_error);
	// An ancillary chunk with a bad CRC is skipped.
	const auto bad_text
			= Png_builder().ihdr(48, 48, 8, 3).plte(pal.rgb, 256).chunk("tEXt", {'K', 0, 'v'}, true).idat(rows).iend().bytes();
	CHECK(Hires::read_indexed_png(bad_text.data(), bad_text.size(), 48, 48, png) == Png_status::ok);
	CHECK(png.find_text("K") == nullptr);
	// IHDR must come first.
	const auto late_ihdr = Png_builder().text("a", "b").ihdr(48, 48, 8, 3).plte(pal.rgb, 256).idat(rows).iend().bytes();
	CHECK(Hires::read_indexed_png(late_ihdr.data(), late_ihdr.size(), 48, 48, png) == Png_status::read_error);
	CHECK(png.error == "IHDR is not the first chunk");
	// PLTE length not a multiple of 3.
	const auto bad_len
			= Png_builder().ihdr(48, 48, 8, 3).chunk("PLTE", std::vector<uint8_t>(pal.rgb, pal.rgb + 10)).idat(rows).iend().bytes();
	CHECK(Hires::read_indexed_png(bad_len.data(), bad_len.size(), 48, 48, png) == Png_status::read_error);
	// No IEND.
	const auto no_iend = Png_builder().ihdr(48, 48, 8, 3).plte(pal.rgb, 256).idat(rows).bytes();
	CHECK(Hires::read_indexed_png(no_iend.data(), no_iend.size(), 48, 48, png) == Png_status::read_error);
	CHECK(png.error == "truncated PNG (no IEND)");
	// Bytes after IEND are ignored.
	auto trailing = small_png(px, pal);
	trailing.insert(trailing.end(), {1, 2, 3, 4, 5});
	CHECK(Hires::read_indexed_png(trailing.data(), trailing.size(), 48, 48, png) == Png_status::ok);
}

TEST_CASE("hires png: a 30000 x 30000 IHDR is rejected (F3) before any large allocation") {
	const Hires::Pal8    pal = hires_test::test_palette();
	std::vector<uint8_t> row(30001, 0);
	const auto           huge = Png_builder().ihdr(30000, 30000, 8, 3).plte(pal.rgb, 256).idat(row).iend().bytes();
	Temp_dir             tmp;
	hires_test::write_file(tmp / "huge.png", huge);
	Indexed_png png;
	Hires::reset_png_alloc_stats();
	CHECK(Hires::read_indexed_png(tmp / "huge.png", 48, 48, png) == Png_status::wrong_size);
	CHECK(png.width == 30000);
	CHECK(png.height == 30000);
	const Hires::Png_alloc_stats stats = Hires::png_alloc_stats();
	CHECK(stats.largest <= huge.size());    // the file buffer, nothing image-sized
	CHECK(stats.largest < 65536);
	// With no expected size the limit is 2048 x 2048.
	CHECK(Hires::read_indexed_png(tmp / "huge.png", 0, 0, png) == Png_status::wrong_size);
	CHECK(Hires::png_alloc_stats().largest < 65536);
	// The hook does see the pixel buffer of a normal read.
	Rng        rng(0x9007);
	const auto px   = random_indices(rng, 48 * 48, 255);
	const auto good = small_png(px, pal);
	Hires::reset_png_alloc_stats();
	CHECK(Hires::read_indexed_png(good.data(), good.size(), 48, 48, png) == Png_status::ok);
	CHECK(Hires::png_alloc_stats().largest >= 48 * 48);
	CHECK(Hires::png_alloc_stats().calls > 1);
}

TEST_CASE("hires png: big metadata chunks do not make a file unreadable (as in hirescheck.py)") {
	Rng                  rng(0x9008);
	const Hires::Pal8    pal = hires_test::test_palette();
	const auto           px  = random_indices(rng, 48 * 48, 255);
	std::vector<uint8_t> blob(1500000, 0x55);    // 1.5 MB: over libpng's 1 MiB chunk limit.
	const std::string    xmp(1500000, 'x');
	const std::string    note(900000, 'n');    // A text under the limit is still read.
	const auto           bytes = Png_builder()
							   .ihdr(48, 48, 8, 3)
							   .plte(pal.rgb, 256)
							   .text("Exult-Src-CRC32", "0123abcd")
							   .chunk("iCCP", blob)
							   .chunk("eXIf", blob)
							   .itxt("XML:com.adobe.xmp", xmp)
							   .itxt("Note", note)
							   .chunk("zzZz", blob)
							   .idat(hires_test::filtered_rows(px.data(), 48, 48))
							   .text("Comment", xmp)
							   .text("Small", "after")
							   .iend()
							   .bytes();
	Temp_dir tmp;
	hires_test::write_file(tmp / "meta.png", bytes);
	for (const bool from_file : {false, true}) {
		INFO("from file " << from_file);
		Indexed_png      png;
		const Png_status status = from_file ? Hires::read_indexed_png(tmp / "meta.png", 48, 48, png)
											: Hires::read_indexed_png(bytes.data(), bytes.size(), 48, 48, png);
		INFO(png.error);
		REQUIRE(status == Png_status::ok);
		CHECK(png.pixels == px);
		CHECK(png.num_palette == 256);
		REQUIRE(png.find_text("Exult-Src-CRC32") != nullptr);
		CHECK(*png.find_text("Exult-Src-CRC32") == "0123abcd");
		REQUIRE(png.find_text("Note") != nullptr);
		CHECK(png.find_text("Note")->size() == note.size());
		REQUIRE(png.find_text("Small") != nullptr);
		CHECK(png.find_text("XML:com.adobe.xmp") == nullptr);    // over 1 MiB: dropped
		CHECK(png.find_text("Comment") == nullptr);
	}
	// A tRNS chunk among them still counts (F4).
	const auto with_trns = Png_builder()
								   .ihdr(48, 48, 8, 3)
								   .plte(pal.rgb, 256)
								   .chunk("tRNS", {0})
								   .chunk("zzZz", blob)
								   .idat(hires_test::filtered_rows(px.data(), 48, 48))
								   .iend()
								   .bytes();
	Indexed_png png;
	REQUIRE(Hires::read_indexed_png(with_trns.data(), with_trns.size(), 48, 48, png) == Png_status::ok);
	CHECK(png.has_trns);
	// The structure is still checked: a critical chunk after them with a bad CRC.
	const auto bad = Png_builder()
							 .ihdr(48, 48, 8, 3)
							 .plte(pal.rgb, 256)
							 .chunk("zzZz", blob)
							 .chunk("IDAT", hires_test::zlib_compress(hires_test::filtered_rows(px.data(), 48, 48)), true)
							 .iend()
							 .bytes();
	CHECK(Hires::read_indexed_png(bad.data(), bad.size(), 48, 48, png) == Png_status::read_error);
	CHECK(png.error.find("CRC error in IDAT") != std::string::npos);
}

TEST_CASE("hires png: files larger than the cap are judged on their IHDR alone") {
	const Hires::Pal8    pal = hires_test::test_palette();
	std::vector<uint8_t> row(49, 0);
	Temp_dir             tmp;
	// Twice the raw image plus 64 MiB; the rest of the file is sparse.
	const uintmax_t over = 2 * 48 * 49 + (uintmax_t{64} << 20) + 1;
	for (const int side : {48, 64}) {
		const fs::path file = tmp / ("big" + std::to_string(side) + ".png");
		hires_test::write_file(file, Png_builder().ihdr(side, side, 8, 3).plte(pal.rgb, 256).idat(row).iend().bytes());
		std::error_code ec;
		fs::resize_file(file, over, ec);
		REQUIRE_FALSE(ec);
		Indexed_png png;
		Hires::reset_png_alloc_stats();
		if (side == 48) {
			CHECK(Hires::read_indexed_png(file, 48, 48, png) == Png_status::read_error);
			CHECK(png.error.find("too large") != std::string::npos);
		} else {
			CHECK(Hires::read_indexed_png(file, 48, 48, png) == Png_status::wrong_size);    // F3 from the first bytes
		}
		CHECK(Hires::png_alloc_stats().largest < 65536);
	}
}

TEST_CASE("hires png: a stray file under the cap is rejected on its IHDR without being read") {
	const Hires::Pal8    pal = hires_test::test_palette();
	std::vector<uint8_t> row(49, 0);
	Temp_dir             tmp;
	// A few MiB, well under the 48 x 48 cap of about 64 MiB; the rest is sparse.
	const uintmax_t size = uintmax_t{8} << 20;

	struct Case {
		const char* name;
		uint32_t    side;
		int         color_type;
		Png_status  want;
	};

	const Case cases[] = {
			{"screenshot.png", 4096, 3,  Png_status::wrong_size},
			{       "rgb.png",   48, 2, Png_status::not_palette},
	};
	for (const Case& c : cases) {
		const fs::path file = tmp / c.name;
		hires_test::write_file(
				file, Png_builder().ihdr(c.side, c.side, 8, c.color_type).plte(pal.rgb, 256).idat(row).iend().bytes());
		std::error_code ec;
		fs::resize_file(file, size, ec);
		REQUIRE_FALSE(ec);
		Indexed_png png;
		Hires::reset_png_alloc_stats();
		CHECK(Hires::read_indexed_png(file, 48, 48, png) == c.want);
		CHECK(png.width == static_cast<int>(c.side));
		CHECK(Hires::png_alloc_stats().largest < 65536);
	}
	// A valid 48 x 48 file is still read whole and decoded.
	Rng        rng(0x900a);
	const auto px = random_indices(rng, 48 * 48, 255);
	hires_test::write_file(tmp / "good.png", small_png(px, pal));
	Indexed_png png;
	CHECK(Hires::read_indexed_png(tmp / "good.png", 48, 48, png) == Png_status::ok);
	CHECK(png.pixels == px);
	// A bad head still gets the message of the full structure scan.
	hires_test::write_file(tmp / "short.png", {0x89, 'P', 'N', 'G'});
	CHECK(Hires::read_indexed_png(tmp / "short.png", 48, 48, png) == Png_status::read_error);
	CHECK(png.error == "not a PNG (bad signature)");
}

TEST_CASE("hires png: a used index beyond the PLTE loads raw, and F2 rejects it") {
	const Hires::Pal8    pal = hires_test::test_palette();
	std::vector<uint8_t> px(48 * 48, 2);
	px[100] = 9;
	const auto bytes
			= Png_builder().ihdr(48, 48, 8, 3).plte(pal.rgb, 4).idat(hires_test::filtered_rows(px.data(), 48, 48)).iend().bytes();
	Indexed_png png;
	REQUIRE(Hires::read_indexed_png(bytes.data(), bytes.size(), 48, 48, png) == Png_status::ok);
	CHECK(png.num_palette == 4);
	CHECK(png.pixels == px);    // raw: libpng only warns about index 9

	// The rule F2 on that file: "index 9 >= PLTE size 4".
	Temp_dir                tmp;
	hires_test::Fake_flats  flats(10);
	std::array<uint8_t, 64> flat{};
	flat.fill(2);
	flats.set(3, 0, flat);
	hires_test::write_file(tmp / "0003_00.png", bytes);
	Hires::Pal8 pal_for_rule = pal;
	const auto  check        = Hires::check_tile_file(tmp / "0003_00.png", 6, pal_for_rule, flats.provider());
	CHECK(check.rule == Hires::Rule::f2_plte);
	CHECK(check.detail == "index 9 >= PLTE size 4");
	// The boundary: index 4 with 4 entries.
	px[100] = 4;
	const auto edge
			= Png_builder().ihdr(48, 48, 8, 3).plte(pal.rgb, 4).idat(hires_test::filtered_rows(px.data(), 48, 48)).iend().bytes();
	hires_test::write_file(tmp / "0003_00.png", edge);
	const auto check4 = Hires::check_tile_file(tmp / "0003_00.png", 6, pal_for_rule, flats.provider());
	CHECK(check4.rule == Hires::Rule::f2_plte);
	CHECK(check4.detail == "index 4 >= PLTE size 4");
}

TEST_CASE("hires png: the writer refuses invalid input") {
	const Hires::Pal8        pal = hires_test::test_palette();
	std::vector<uint8_t>     px(16, 20);
	std::vector<uint8_t>     out;
	Hires::Png_write_options opt;
	opt.bit_depth = 4;    // index 20 does not fit
	CHECK(Hires::encode_indexed_png(4, 4, px.data(), pal.rgb, 16, opt, out) == Png_status::write_error);
	opt.bit_depth = 3;
	CHECK(Hires::encode_indexed_png(4, 4, px.data(), pal.rgb, 16, opt, out) == Png_status::write_error);
	opt.bit_depth = 8;
	CHECK(Hires::encode_indexed_png(4, 4, px.data(), pal.rgb, 0, opt, out) == Png_status::write_error);
	CHECK(Hires::encode_indexed_png(0, 4, px.data(), pal.rgb, 256, opt, out) == Png_status::write_error);
	opt.text_before = {
			{std::string(80, 'k'), "too long a keyword"}
    };
	CHECK(Hires::encode_indexed_png(4, 4, px.data(), pal.rgb, 256, opt, out) == Png_status::write_error);
	opt.text_before.clear();
	opt.trns.assign(17, 0);
	CHECK(Hires::encode_indexed_png(4, 4, px.data(), pal.rgb, 16, opt, out) == Png_status::write_error);
	CHECK(out.empty());
	CHECK(Hires::have_png_support());
}

#else

TEST_CASE("hires png: without libpng every read reports no PNG support") {
	Hires::Indexed_png png;
	const uint8_t      byte = 0;
	CHECK_FALSE(Hires::have_png_support());
	CHECK(Hires::read_indexed_png(&byte, 1, 48, 48, png) == Hires::Png_status::no_png_support);
}

#endif
