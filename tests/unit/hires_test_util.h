/*
 *  hires_test_util.h - Helpers of the hi-res store tests: scratch
 *  directories, a chunk-level PNG builder, a fake source provider and the
 *  synthetic game of tests/data/hires/rules/game.
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

#ifndef HIRES_TEST_UTIL_H
#define HIRES_TEST_UTIL_H

#include "hires_png.h"
#include "hires_rules.h"
#include "hires_store.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Vga_file;

namespace hires_test {
	namespace fs = std::filesystem;

	/*
	 *  A scratch directory, removed with its contents on destruction. It is
	 *  created under $HIRES_TEST_TMP when set, else in the current directory
	 *  (the build tree under "make check"), never in the system temp dir.
	 */
	class Temp_dir {
		fs::path dir;

	public:
		Temp_dir();
		~Temp_dir();
		Temp_dir(const Temp_dir&)            = delete;
		Temp_dir& operator=(const Temp_dir&) = delete;

		const fs::path& path() const {
			return dir;
		}

		fs::path operator/(const std::string& rel) const {
			return dir / rel;
		}
	};

	// Writes a file (creating its directories).
	void                 write_file(const fs::path& file, const std::vector<uint8_t>& bytes);
	void                 write_text(const fs::path& file, const std::string& text);
	std::vector<uint8_t> read_file(const fs::path& file);

	/*
	 *  Builds a PNG chunk by chunk: for inputs the libpng writer cannot make
	 *  (other colour types, a huge IHDR with a tiny IDAT, bad CRCs, bad
	 *  deflate data) and for exact chunk layouts.
	 */
	class Png_builder {
		std::vector<uint8_t> out;

	public:
		Png_builder();    // The signature.
		Png_builder& chunk(const char* type, const std::vector<uint8_t>& data, bool bad_crc = false);
		Png_builder& ihdr(uint32_t w, uint32_t h, int depth, int color_type, int interlace = 0);
		Png_builder& plte(const uint8_t* rgb, int entries);
		Png_builder& text(const std::string& key, const std::string& value);
		Png_builder& ztxt(const std::string& key, const std::string& value);
		Png_builder& itxt(const std::string& key, const std::string& value);
		Png_builder& idat(const std::vector<uint8_t>& filtered);    // zlib-compressed here
		Png_builder& iend();

		const std::vector<uint8_t>& bytes() const {
			return out;
		}
	};

	// Scanlines with filter type 0: a zero byte, then row_bytes bytes, per row.
	std::vector<uint8_t> filtered_rows(const uint8_t* data, int row_bytes, int rows);
	std::vector<uint8_t> zlib_compress(const std::vector<uint8_t>& data);

	// A synthetic palette: every entry distinct.
	Hires::Pal8 test_palette();

	// A palette PNG of side x side indices with the whole palette as PLTE and,
	// when guard is not empty, tEXt Exult-Src-CRC32 = guard (before IDAT).
	void write_tile(
			const fs::path& file, const std::vector<uint8_t>& px, int side, const Hires::Pal8& pal,
			const std::string& guard = std::string(), Hires::Png_write_options options = Hires::Png_write_options());

	// NN replication of a 64-byte flat to (8 scale)^2.
	std::vector<uint8_t> nn_tile(const uint8_t* flat64, int scale);

	std::string hex8(uint32_t v);

	// The guard text of a flat: its CRC32 as 8 lowercase hex digits.
	std::string guard_of(const uint8_t* flat64);

	// In-memory flats with the provider's bound check (shape < num_shapes).
	class Fake_flats {
		std::map<std::pair<int, int>, std::array<uint8_t, 64>> flats;
		int                                                    num_shapes;

	public:
		explicit Fake_flats(int nshapes) : num_shapes(nshapes) {}

		void           set(int shape, int frame, const std::array<uint8_t, 64>& px);
		const uint8_t* get(int shape, int frame) const;

		// Valid while this object lives.
		Hires::Src_provider provider() const;
	};

	/*
	 *  The synthetic game of tests/data/hires/rules/game (written by
	 *  tools/hires/tests/make_rule_fixtures.py): palette 0 read with
	 *  U7object, shapes.vga with Vga_file, and the engine's provider
	 *  Hires::flat_from_vga on it.
	 */
	struct Synth_game {
		std::vector<uint8_t>      pal6;
		Hires::Pal8               pal;
		std::unique_ptr<Vga_file> vga;
		Hires::Src_provider       provider() const;
	};

	Synth_game& synth_game();
}    // namespace hires_test

#endif
