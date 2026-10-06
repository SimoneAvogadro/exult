/*
 *  hires_bundle.cc - Reader (and test writer) of x<S>/flats.bundle.
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

#include "hires_bundle.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace {
	uint16_t get_le16(const uint8_t* p) {
		return static_cast<uint16_t>(p[0] | (p[1] << 8));
	}

	uint32_t get_le32(const uint8_t* p) {
		return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16)
			   | (static_cast<uint32_t>(p[3]) << 24);
	}

	void put_le16(std::vector<uint8_t>& out, uint32_t v) {
		out.push_back(static_cast<uint8_t>(v));
		out.push_back(static_cast<uint8_t>(v >> 8));
	}

	void put_le32(std::vector<uint8_t>& out, uint32_t v) {
		put_le16(out, v & 0xffffU);
		put_le16(out, v >> 16);
	}

	std::string hex8(uint32_t v) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%08" PRIx32, v);
		return buf;
	}

	/*
	 *  The header checks of B0, in the order of tools/hires/u7hires/pack.py
	 *  read_bundle(), with its messages. 'file_size' is the size of the whole
	 *  file; the entries are not looked at.
	 */
	bool check_header(
			const uint8_t* p, uint64_t file_size, int expect_scale, uint32_t expect_crc, Hires::Bundle_header& hdr,
			std::string& error) {
		if (file_size < Hires::bundle_header_size) {
			error = "truncated header";
			return false;
		}
		if (std::memcmp(p, "U7HB", 4) != 0) {
			error = "bad magic";
			return false;
		}
		hdr.version       = get_le16(p + 4);
		hdr.scale         = get_le16(p + 6);
		hdr.palette_crc32 = get_le32(p + 8);
		hdr.count         = get_le32(p + 12);
		if (hdr.version != Hires::bundle_version) {
			error = "version " + std::to_string(hdr.version);
			return false;
		}
		if (static_cast<int>(hdr.scale) != expect_scale) {
			error = "scale " + std::to_string(hdr.scale) + " != " + std::to_string(expect_scale);
			return false;
		}
		if (hdr.palette_crc32 != expect_crc) {
			error = "palette_crc32 " + hex8(hdr.palette_crc32) + " != " + hex8(expect_crc);
			return false;
		}
		if (hdr.scale < 1) {
			error = "scale 0";
			return false;
		}
		const uint64_t expected
				= Hires::bundle_header_size + static_cast<uint64_t>(hdr.count) * Hires::bundle_entry_size(hdr.scale);
		if (file_size != expected) {
			error = "size " + std::to_string(file_size) + " != header + " + std::to_string(hdr.count) + " entries";
			return false;
		}
		return true;
	}
}    // namespace

namespace Hires {
	bool Bundle::parse(std::vector<uint8_t> bytes, int expect_scale, uint32_t expect_palette_crc, std::string& error) {
		data.clear();
		hdr = Bundle_header();
		if (!check_header(bytes.data(), bytes.size(), expect_scale, expect_palette_crc, hdr, error)) {
			return false;
		}
		// The entries are not looked at: each one is checked on its own (their
		// order and duplicate keys are no B0 matter, as in pack.py read_bundle()).
		data = std::move(bytes);
		return true;
	}

	bool Bundle::read(const std::filesystem::path& file, int expect_scale, uint32_t expect_palette_crc, std::string& error) {
		data.clear();
		hdr = Bundle_header();
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			error = "cannot open";
			return false;
		}
		in.seekg(0, std::ios::end);
		const std::streamoff size = in.tellg();
		in.seekg(0, std::ios::beg);
		if (size < 0 || !in) {
			error = "cannot read";
			return false;
		}
		// Check the header and the size first, so a bogus file is never read whole.
		uint8_t      head[bundle_header_size] = {};
		const size_t n = static_cast<size_t>(std::min<std::streamoff>(size, static_cast<std::streamoff>(sizeof(head))));
		in.read(reinterpret_cast<char*>(head), static_cast<std::streamsize>(n));
		if (!in) {
			error = "cannot read";
			return false;
		}
		Bundle_header probe;
		if (!check_header(head, static_cast<uint64_t>(size), expect_scale, expect_palette_crc, probe, error)) {
			return false;
		}
		// One read for the whole file.
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		in.seekg(0, std::ios::beg);
		in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!in) {
			error = "cannot read";
			return false;
		}
		return parse(std::move(bytes), expect_scale, expect_palette_crc, error);
	}

	Bundle_entry Bundle::entry(size_t i) const {
		Bundle_entry e;
		if (i >= size()) {
			return e;
		}
		const uint8_t* p = data.data() + bundle_header_size + i * bundle_entry_size(hdr.scale);
		e.shape          = get_le16(p);
		e.frame          = p[2];
		e.guarded        = (p[3] & bundle_flag_guard) != 0;
		e.guard          = get_le32(p + 4);
		e.pixels         = p + bundle_entry_header_size;
		return e;
	}

	std::vector<uint8_t> build_bundle(int scale, uint32_t palette_crc32, std::vector<Bundle_source> entries) {
		std::vector<uint8_t> out;
		if (scale < 1 || scale > 0xffff) {
			return out;
		}
		const size_t side2 = static_cast<size_t>(8 * scale) * static_cast<size_t>(8 * scale);
		// Stable, as pack.py's sorted(): entries with the same key keep their order.
		std::stable_sort(entries.begin(), entries.end(), [](const Bundle_source& a, const Bundle_source& b) {
			return a.shape != b.shape ? a.shape < b.shape : a.frame < b.frame;
		});
		for (const auto& e : entries) {
			if (e.shape < 0 || e.shape > 0xffff || e.frame < 0 || e.frame > 0xff || e.pixels.size() != side2) {
				return out;
			}
		}
		out.reserve(bundle_header_size + entries.size() * bundle_entry_size(scale));
		out.insert(out.end(), {'U', '7', 'H', 'B'});
		put_le16(out, bundle_version);
		put_le16(out, static_cast<uint32_t>(scale));
		put_le32(out, palette_crc32);
		put_le32(out, static_cast<uint32_t>(entries.size()));
		for (const auto& e : entries) {
			put_le16(out, static_cast<uint32_t>(e.shape));
			out.push_back(static_cast<uint8_t>(e.frame));
			out.push_back(e.guarded ? bundle_flag_guard : 0);
			put_le32(out, e.guarded ? e.guard : 0);
			out.insert(out.end(), e.pixels.begin(), e.pixels.end());
		}
		return out;
	}
}    // namespace Hires
