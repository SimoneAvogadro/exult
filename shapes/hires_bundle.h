/*
 *  hires_bundle.h - Reader (and test writer) of x<S>/flats.bundle, the
 *  one-file container for generated flat art (DESIGN.md section 5.7).
 *
 *  Format, little-endian (tools/hires/u7hires/pack.py writes the same):
 *    header, 16 bytes:  "U7HB" | u16 version = 1 | u16 scale | u32 palette_crc32 | u32 count
 *    count entries, written sorted by (shape, frame), each 8 + (8 * scale)^2 bytes:
 *                       u16 shape | u8 frame | u8 flags (bit 0: guard present) | u32 guard |
 *                       (8 * scale)^2 raw indices, row-major
 *  The file size must equal 16 + count * (8 + (8 * scale)^2).
 *
 *  The reader takes the whole file in one read, after the header and the
 *  file size have been checked (rule B0, the checks of pack.py read_bundle()),
 *  and before any entry is used. Entries are independent: their rules (N1,
 *  P0, P4, G1) are checked one by one by the store, in file order, which need
 *  not be sorted.
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

#ifndef HIRES_BUNDLE_H
#define HIRES_BUNDLE_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Hires {
	constexpr size_t   bundle_header_size       = 16;
	constexpr size_t   bundle_entry_header_size = 8;
	constexpr uint16_t bundle_version           = 1;
	constexpr uint8_t  bundle_flag_guard        = 0x01;

	// Bytes per entry at a scale: the entry header plus (8 * scale)^2 indices.
	constexpr size_t bundle_entry_size(int scale) {
		return bundle_entry_header_size + static_cast<size_t>(8 * scale) * static_cast<size_t>(8 * scale);
	}

	struct Bundle_header {
		uint16_t version       = 0;
		uint16_t scale         = 0;
		uint32_t palette_crc32 = 0;
		uint32_t count         = 0;
	};

	struct Bundle_entry {
		int            shape   = 0;
		int            frame   = 0;
		bool           guarded = false;
		uint32_t       guard   = 0;          // CRC32 of the 64-byte 1x flat, when guarded.
		const uint8_t* pixels  = nullptr;    // (8 * scale)^2 indices inside the bundle's buffer.
	};

	class Bundle {
		std::vector<uint8_t> data;
		Bundle_header        hdr;

	public:
		/*
		 *  Reads and checks a bundle (B0): magic, version, scale == expect_scale,
		 *  palette_crc32 == expect_palette_crc, file size == header + count
		 *  entries. On failure returns false with the reason in 'error' and
		 *  holds no entries. Throws only when the file buffer cannot be allocated.
		 */
		bool read(const std::filesystem::path& file, int expect_scale, uint32_t expect_palette_crc, std::string& error);
		// The same checks on bytes in memory (takes ownership).
		bool parse(std::vector<uint8_t> bytes, int expect_scale, uint32_t expect_palette_crc, std::string& error);

		const Bundle_header& header() const {
			return hdr;
		}

		size_t size() const {
			return data.empty() ? 0 : hdr.count;
		}

		Bundle_entry entry(size_t i) const;
	};

	// Writer for the tests and engine tools (mkpack.py writes the same bytes).
	// The entries are sorted by (shape, frame), stably (entries with the same
	// key keep their order); each pixel vector holds (8 * scale)^2 indices.
	// Returns an empty vector on invalid input.
	struct Bundle_source {
		int                  shape   = 0;
		int                  frame   = 0;
		bool                 guarded = false;
		uint32_t             guard   = 0;
		std::vector<uint8_t> pixels;
	};

	std::vector<uint8_t> build_bundle(int scale, uint32_t palette_crc32, std::vector<Bundle_source> entries);
}    // namespace Hires

#endif
