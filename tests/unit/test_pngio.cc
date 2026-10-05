/*
 *  test_pngio.cc - Import_png8 and Import_png32 (shapes/pngio.cc) free what
 *  they allocated when libpng fails half-way through a file (DESIGN.md
 *  section 3.5).
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

#ifdef HAVE_PNG_H
#	include "pngio.h"

#	include <cstddef>
#	include <cstdint>
#	include <cstdio>
#	include <cstring>
#	include <fstream>
#	include <iterator>
#	include <string>
#	include <vector>

namespace {
	// Odd sizes; libpng reads the rows one at a time.
	constexpr int width    = 61;
	constexpr int height   = 37;
	constexpr int pal_size = 16;

	std::vector<unsigned char> test_pixels() {
		std::vector<unsigned char> pixels(width * height);
		for (size_t i = 0; i < pixels.size(); i++) {
			pixels[i] = static_cast<unsigned char>((i * 7 + i / width) % pal_size);
		}
		return pixels;
	}

	std::vector<unsigned char> read_file(const std::string& path) {
		std::ifstream in(path, std::ios::binary);
		return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	void write_file(const std::string& path, const std::vector<unsigned char>& data) {
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
	}

	// Writes the test image with Export_png8 and returns the file's bytes.
	std::vector<unsigned char> make_png(const std::string& path) {
		std::vector<unsigned char> pixels = test_pixels();
		std::vector<unsigned char> palette(3 * pal_size);
		for (size_t i = 0; i < palette.size(); i++) {
			palette[i] = static_cast<unsigned char>(i * 5);
		}
		REQUIRE(Export_png8(path.c_str(), -1, width, height, width, 0, 0, pixels.data(), palette.data(), pal_size) == 1);
		return read_file(path);
	}

	// Offset of the first chunk of the given type, or 0.
	size_t find_chunk(const std::vector<unsigned char>& png, const char* type) {
		size_t pos = 8;    // After the signature.
		while (pos + 8 <= png.size()) {
			const uint32_t len = (uint32_t{png[pos]} << 24) | (uint32_t{png[pos + 1]} << 16) | (uint32_t{png[pos + 2]} << 8)
								 | uint32_t{png[pos + 3]};
			if (std::memcmp(&png[pos + 4], type, 4) == 0) {
				return pos;
			}
			pos += 12 + len;
		}
		return 0;
	}

	struct Broken_png {
		const char*                what;
		std::vector<unsigned char> bytes;
	};

	// Files that libpng fails on at different points: before Import_png8
	// allocates anything, and after it allocated the palette and the pixels.
	std::vector<Broken_png> broken_pngs(const std::vector<unsigned char>& png) {
		const size_t idat = find_chunk(png, "IDAT");
		const size_t iend = find_chunk(png, "IEND");
		REQUIRE(idat > 0);
		REQUIRE(iend > idat + 12);
		auto cut = [&](size_t size) {
			return std::vector<unsigned char>(png.begin(), png.begin() + static_cast<ptrdiff_t>(size));
		};
		std::vector<unsigned char> bad_crc = png;
		bad_crc[iend - 1] ^= 0x55;    // The last CRC byte of the IDAT chunk.
		std::vector<Broken_png> broken;
		broken.push_back({"ends before IDAT (no image data)", cut(idat)});
		broken.push_back({"ends in the IDAT data (rows missing)", cut(idat + 8 + (iend - idat - 12) / 2)});
		broken.push_back({"ends before IEND (all rows read)", cut(iend)});
		broken.push_back({"IDAT has a bad CRC", bad_crc});
		return broken;
	}
}    // namespace

TEST_CASE("pngio: Import_png8 reads back what Export_png8 wrote") {
	const std::string path = hires_test::scratch_path("pngio.png");
	make_png(path);
	int            w        = 0;
	int            h        = 0;
	int            rowbytes = 0;
	int            xoff     = 0;
	int            yoff     = 0;
	int            colours  = 0;
	unsigned char* pixels   = nullptr;
	unsigned char* palette  = nullptr;
	CHECK(Import_png8(path.c_str(), -1, w, h, rowbytes, xoff, yoff, pixels, palette, colours) == 1);
	CHECK(w == width);
	CHECK(h == height);
	CHECK(rowbytes == width);
	CHECK(colours == pal_size);
	REQUIRE(pixels != nullptr);
	REQUIRE(palette != nullptr);
	CHECK(std::vector<unsigned char>(pixels, pixels + width * height) == test_pixels());
	CHECK(palette[3 * 15 + 2] == (3 * 15 + 2) * 5);
	delete[] pixels;
	delete[] palette;
	std::remove(path.c_str());
}

TEST_CASE("pngio: Import_png8 frees its buffers when libpng fails") {
	const std::string path = hires_test::scratch_path("pngio_broken.png");
	for (const auto& broken : broken_pngs(make_png(path))) {
		INFO(broken.what);
		write_file(path, broken.bytes);
		int            w        = 0;
		int            h        = 0;
		int            rowbytes = 0;
		int            xoff     = 0;
		int            yoff     = 0;
		int            colours  = 0;
		unsigned char* pixels   = nullptr;
		unsigned char* palette  = nullptr;
		CHECK(Import_png8(path.c_str(), -1, w, h, rowbytes, xoff, yoff, pixels, palette, colours) == 0);
		CHECK(pixels == nullptr);
		CHECK(palette == nullptr);
		delete[] pixels;    // Only if a check failed.
		delete[] palette;
	}
	std::remove(path.c_str());
}

TEST_CASE("pngio: Import_png32 frees its buffer when libpng fails") {
	const std::string path = hires_test::scratch_path("pngio_broken32.png");
	for (const auto& broken : broken_pngs(make_png(path))) {
		INFO(broken.what);
		write_file(path, broken.bytes);
		int            w        = 0;
		int            h        = 0;
		int            rowbytes = 0;
		int            xoff     = 0;
		int            yoff     = 0;
		unsigned char* pixels   = nullptr;
		CHECK(Import_png32(path.c_str(), w, h, rowbytes, xoff, yoff, pixels) == 0);
		CHECK(pixels == nullptr);
		delete[] pixels;    // Only if a check failed.
	}
	std::remove(path.c_str());
}

#endif    // HAVE_PNG_H
