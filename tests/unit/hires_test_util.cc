/*
 *  hires_test_util.cc - Helpers of the hi-res store tests.
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

#include "hires_test_util.h"

#include "U7obj.h"
#include "hires_vga.h"
#include "ignore_unused_variable_warning.h"
#include "test_support.h"
#include "vgafile.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <system_error>

#ifdef HAVE_PNG_H
#	include <zlib.h>
#endif

#ifdef _WIN32
#	include <process.h>
#	define HIRES_TEST_GETPID _getpid
#else
#	include <unistd.h>
#	define HIRES_TEST_GETPID getpid
#endif

namespace hires_test {
	Temp_dir::Temp_dir() {
		static int     counter = 0;
		const char*    base    = std::getenv("HIRES_TEST_TMP");
		const fs::path root    = base != nullptr && *base != '\0' ? fs::path(base) : fs::current_path();
		for (int attempt = 0; attempt < 1000; attempt++) {
			const fs::path candidate
					= root / ("hires_unit." + std::to_string(HIRES_TEST_GETPID()) + "." + std::to_string(counter++));
			std::error_code ec;
			if (fs::create_directories(candidate, ec) && !ec) {
				dir = candidate;
				return;
			}
		}
		throw std::runtime_error("cannot create a scratch directory under " + root.string());
	}

	Temp_dir::~Temp_dir() {
		std::error_code ec;
		fs::remove_all(dir, ec);
	}

	void write_file(const fs::path& file, const std::vector<uint8_t>& bytes) {
		std::error_code ec;
		fs::create_directories(file.parent_path(), ec);
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!out) {
			throw std::runtime_error("cannot write " + file.string());
		}
	}

	void write_text(const fs::path& file, const std::string& text) {
		write_file(file, std::vector<uint8_t>(text.begin(), text.end()));
	}

	std::vector<uint8_t> read_file(const fs::path& file) {
		std::ifstream in(file, std::ios::binary);
		return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	namespace {
		void put_be32(std::vector<uint8_t>& out, uint32_t v) {
			out.push_back(static_cast<uint8_t>(v >> 24));
			out.push_back(static_cast<uint8_t>(v >> 16));
			out.push_back(static_cast<uint8_t>(v >> 8));
			out.push_back(static_cast<uint8_t>(v));
		}
	}    // namespace

	Png_builder::Png_builder() : out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'} {}

	Png_builder& Png_builder::chunk(const char* type, const std::vector<uint8_t>& data, bool bad_crc) {
		put_be32(out, static_cast<uint32_t>(data.size()));
		std::vector<uint8_t> body(type, type + 4);
		body.insert(body.end(), data.begin(), data.end());
		out.insert(out.end(), body.begin(), body.end());
		put_be32(out, Hires::crc32(body.data(), body.size()) ^ (bad_crc ? 1U : 0U));
		return *this;
	}

	Png_builder& Png_builder::ihdr(uint32_t w, uint32_t h, int depth, int color_type, int interlace) {
		std::vector<uint8_t> d;
		put_be32(d, w);
		put_be32(d, h);
		d.push_back(static_cast<uint8_t>(depth));
		d.push_back(static_cast<uint8_t>(color_type));
		d.push_back(0);
		d.push_back(0);
		d.push_back(static_cast<uint8_t>(interlace));
		return chunk("IHDR", d);
	}

	Png_builder& Png_builder::plte(const uint8_t* rgb, int entries) {
		return chunk("PLTE", std::vector<uint8_t>(rgb, rgb + 3 * entries));
	}

	Png_builder& Png_builder::text(const std::string& key, const std::string& value) {
		std::vector<uint8_t> d(key.begin(), key.end());
		d.push_back(0);
		d.insert(d.end(), value.begin(), value.end());
		return chunk("tEXt", d);
	}

	Png_builder& Png_builder::ztxt(const std::string& key, const std::string& value) {
		std::vector<uint8_t> d(key.begin(), key.end());
		d.push_back(0);
		d.push_back(0);    // Compression method: deflate.
		const auto packed = zlib_compress(std::vector<uint8_t>(value.begin(), value.end()));
		d.insert(d.end(), packed.begin(), packed.end());
		return chunk("zTXt", d);
	}

	Png_builder& Png_builder::itxt(const std::string& key, const std::string& value) {
		std::vector<uint8_t> d(key.begin(), key.end());
		d.push_back(0);
		d.push_back(0);    // Not compressed.
		d.push_back(0);
		d.push_back(0);    // No language tag.
		d.push_back(0);    // No translated keyword.
		d.insert(d.end(), value.begin(), value.end());
		return chunk("iTXt", d);
	}

	Png_builder& Png_builder::idat(const std::vector<uint8_t>& filtered) {
		return chunk("IDAT", zlib_compress(filtered));
	}

	Png_builder& Png_builder::iend() {
		return chunk("IEND", {});
	}

	std::vector<uint8_t> filtered_rows(const uint8_t* data, int row_bytes, int rows) {
		std::vector<uint8_t> out;
		out.reserve(static_cast<size_t>(rows) * (static_cast<size_t>(row_bytes) + 1));
		for (int y = 0; y < rows; y++) {
			out.push_back(0);
			out.insert(out.end(), data + static_cast<size_t>(y) * row_bytes, data + static_cast<size_t>(y + 1) * row_bytes);
		}
		return out;
	}

	std::vector<uint8_t> zlib_compress(const std::vector<uint8_t>& data) {
#ifdef HAVE_PNG_H
		const uLong          src_len = data.size();
		uLongf               len     = compressBound(src_len);
		std::vector<uint8_t> out(len);
		if (compress2(out.data(), &len, data.data(), src_len, 9) != Z_OK) {
			throw std::runtime_error("compress2 failed");
		}
		out.resize(len);
		return out;
#else
		ignore_unused_variable_warning(data);
		throw std::runtime_error("no zlib without PNG support");
#endif
	}

	Hires::Pal8 test_palette() {
		Hires::Pal8 pal;
		for (int i = 0; i < 256; i++) {
			pal.rgb[3 * i]     = static_cast<uint8_t>(i);
			pal.rgb[3 * i + 1] = static_cast<uint8_t>((i * 7 + 3) & 0xff);
			pal.rgb[3 * i + 2] = static_cast<uint8_t>(255 - i);
		}
		return pal;
	}

	void write_tile(
			const fs::path& file, const std::vector<uint8_t>& px, int side, const Hires::Pal8& pal, const std::string& guard,
			Hires::Png_write_options options) {
		if (!guard.empty()) {
			options.text_before.push_back({"Exult-Src-CRC32", guard});
		}
		std::error_code ec;
		fs::create_directories(file.parent_path(), ec);
		if (Hires::write_indexed_png(file, side, side, px.data(), pal.rgb, 256, options) != Hires::Png_status::ok) {
			throw std::runtime_error("cannot write " + file.string());
		}
	}

	std::vector<uint8_t> nn_tile(const uint8_t* flat64, int scale) {
		std::vector<uint8_t> out(static_cast<size_t>(64) * scale * scale);
		Hires::nn_upscale(flat64, 8, 8, scale, out.data());
		return out;
	}

	std::string hex8(uint32_t v) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%08" PRIx32, v);
		return buf;
	}

	std::string guard_of(const uint8_t* flat64) {
		return hex8(Hires::crc32(flat64, 64));
	}

	void Fake_flats::set(int shape, int frame, const std::array<uint8_t, 64>& px) {
		flats[{shape, frame}] = px;
	}

	const uint8_t* Fake_flats::get(int shape, int frame) const {
		if (shape < 0 || shape >= num_shapes) {
			return nullptr;
		}
		const auto it = flats.find({shape, frame & 31});
		return it == flats.end() ? nullptr : it->second.data();
	}

	Hires::Src_provider Fake_flats::provider() const {
		return [this](int shape, int frame) {
			return get(shape, frame);
		};
	}

	Hires::Src_provider Synth_game::provider() const {
		Vga_file* file = vga.get();
		return [file](int shape, int frame) {
			return Hires::flat_from_vga(*file, shape, frame);
		};
	}

	Synth_game& synth_game() {
		static Synth_game game = [] {
			Synth_game     g;
			size_t         len = 0;
			const U7object obj(File_spec(data_path("hires/rules/game/palettes.flx")), 0);
			const auto     buf = obj.retrieve(len);
			if (!buf || len < 768) {
				throw std::runtime_error("cannot read the synthetic palette");
			}
			g.pal6.assign(buf.get(), buf.get() + 768);
			g.pal = Hires::pal8_from_6bit(g.pal6.data());
			g.vga = std::make_unique<Vga_file>(data_path("hires/rules/game/shapes.vga").c_str());
			return g;
		}();
		return game;
	}
}    // namespace hires_test
