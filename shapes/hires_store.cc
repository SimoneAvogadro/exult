/*
 *  hires_store.cc - The hi-res override store.
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

#include "hires_store.h"

#include "hires_bundle.h"
#include "hires_png.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace {
	constexpr const char* guard_text_key   = "Exult-Src-CRC32";
	constexpr const char* terrain_text_key = "Exult-Terrain-Key";
	constexpr int         max_scale        = 16;

	/*
	 *  A path for messages: UTF-8 in the generic format ('/' separators) on
	 *  every platform, so findings and Entry_info paths read the same
	 *  everywhere. A name without a UTF-8 form (an unpaired surrogate on
	 *  Windows) does not throw: its ASCII characters are kept and the others
	 *  show as '?'. Only std::bad_alloc propagates.
	 */
	std::string utf8(const fs::path& p) {
		try {
#ifdef __cpp_char8_t
			const auto s = p.generic_u8string();
			return std::string(s.begin(), s.end());
#else
			return p.generic_u8string();
#endif
		} catch (const std::bad_alloc&) {
			throw;
		} catch (const std::exception&) {
			std::string out;
			for (const auto c : p.native()) {
				const auto u = static_cast<unsigned long>(c);
				if (u == static_cast<unsigned long>(fs::path::preferred_separator)) {
					out += '/';
				} else {
					out += (u > 0 && u < 0x80) ? static_cast<char>(u) : '?';
				}
			}
			return out;
		}
	}

	// A root's system path as given (it may not convert to a path at all), with
	// the '/' separators of utf8().
	std::string root_text(std::string s) {
		if constexpr (fs::path::preferred_separator != '/') {
			std::replace(s.begin(), s.end(), '\\', '/');
		}
		return s;
	}

	std::string hex8(uint32_t v) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%08" PRIx32, v);
		return buf;
	}

	std::string hex16(uint64_t v) {
		char buf[24];
		std::snprintf(buf, sizeof(buf), "%016" PRIx64, v);
		return buf;
	}

	std::string rgb_text(const uint8_t* p) {
		return "(" + std::to_string(p[0]) + ", " + std::to_string(p[1]) + ", " + std::to_string(p[2]) + ")";
	}

	bool is_blank(char c) {
		return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
	}

	std::string trim(const std::string& s) {
		size_t b = 0;
		size_t e = s.size();
		while (b < e && is_blank(s[b])) {
			b++;
		}
		while (e > b && is_blank(s[e - 1])) {
			e--;
		}
		return s.substr(b, e - b);
	}

	int hex_value(char c) {
		if (c >= '0' && c <= '9') {
			return c - '0';
		}
		if (c >= 'a' && c <= 'f') {
			return c - 'a' + 10;
		}
		if (c >= 'A' && c <= 'F') {
			return c - 'A' + 10;
		}
		return -1;
	}

	// hirescheck.py's guard syntax: ^(0x)?[0-9a-fA-F]{8}$ after strip().
	bool parse_guard(const std::string& text, uint32_t& value) {
		std::string s = trim(text);
		if (s.size() == 10 && s[0] == '0' && s[1] == 'x') {
			s = s.substr(2);
		}
		if (s.size() != 8) {
			return false;
		}
		value = 0;
		for (const char c : s) {
			const int d = hex_value(c);
			if (d < 0) {
				return false;
			}
			value = (value << 4) | static_cast<uint32_t>(d);
		}
		return true;
	}

	bool ends_with_png(const std::string& name) {
		if (name.size() < 4) {
			return false;
		}
		std::string ext = name.substr(name.size() - 4);
		std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) {
			return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
		});
		return ext == ".png";
	}

	bool is_dir(const fs::path& p) {
		std::error_code ec;
		return fs::is_directory(p, ec);
	}

	bool is_file(const fs::path& p) {
		std::error_code ec;
		return fs::is_regular_file(p, ec);
	}

	struct Dir_item {
		std::string name;
		fs::path    path;
		bool        directory = false;
		bool        file      = false;
	};

	// The entries of a directory, sorted by name (hidden ones included, as
	// hirescheck.py lists them; the callers skip "_*" and ".*" directories).
	std::vector<Dir_item> list_dir(const fs::path& dir) {
		std::vector<Dir_item>        items;
		std::error_code              ec;
		fs::directory_iterator       it(dir, ec);
		const fs::directory_iterator end;
		for (; !ec && it != end; it.increment(ec)) {
			Dir_item item;
			item.path = it->path();
			item.name = utf8(item.path.filename());
			if (item.name.empty()) {
				continue;
			}
			std::error_code est;
			const auto      st = it->status(est);
			item.directory     = !est && fs::is_directory(st);
			item.file          = !est && fs::is_regular_file(st);
			items.push_back(std::move(item));
		}
		std::sort(items.begin(), items.end(), [](const Dir_item& a, const Dir_item& b) {
			return a.name < b.name;
		});
		return items;
	}

	uint32_t tile_key(int shape, int frame) {
		return (static_cast<uint32_t>(shape) << 5) | static_cast<uint32_t>(frame & 31);
	}

	// "0012_06": a bundle entry in findings, as hirescheck.py names it.
	std::string key_text(int shape, int frame) {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%04d_%02d", shape, frame);
		return buf;
	}

	Hires::File_check& fail(Hires::File_check& c, Hires::Rule rule, std::string detail) {
		c.rule   = rule;
		c.detail = std::move(detail);
		c.pixels.clear();
		return c;
	}

	Hires::Severity severity_of(const Hires::File_check& c) {
		return c.error ? Hires::Severity::error : Hires::Severity::reject;
	}

	Hires::File_check check_failed(int shape, int frame, const char* what) {
		Hires::File_check c;
		c.error  = true;
		c.detail = std::string("cannot check: ") + what;
		c.shape  = shape;
		c.frame  = frame;
		return c;
	}

	/*
	 *  Runs the check of one file or bundle entry (shape and frame from its
	 *  name, -1 when unknown): an exception (out of memory, I/O, the provider)
	 *  rejects that file or entry alone, with severity error.
	 */
	template <typename Check>
	Hires::File_check guarded(int shape, int frame, const Check& check) {
		try {
			return check();
		} catch (const std::bad_alloc&) {
			return check_failed(shape, frame, "out of memory");
		} catch (const std::exception& e) {
			return check_failed(shape, frame, e.what());
		}
	}

	Hires::Finding make_finding(
			Hires::Rule rule, Hires::Severity sev, std::string path, std::string detail, int shape = -1, int frame = -1) {
		Hires::Finding f;
		f.rule     = rule;
		f.severity = sev;
		f.path     = std::move(path);
		f.detail   = std::move(detail);
		f.shape    = shape;
		f.frame    = frame;
		return f;
	}

	// F2: for every used index i (ascending), i < PLTE size, then PLTE[i] == pal0[i].
	bool check_plte(const Hires::Indexed_png& png, const Hires::Pal8& pal, std::string& detail) {
		bool used[256] = {};
		for (const uint8_t v : png.pixels) {
			used[v] = true;
		}
		for (int i = 0; i < 256; i++) {
			if (!used[i]) {
				continue;
			}
			if (i >= png.num_palette) {
				detail = "index " + std::to_string(i) + " >= PLTE size " + std::to_string(png.num_palette);
				return false;
			}
			if (std::memcmp(&png.plte[3 * i], &pal.rgb[3 * i], 3) != 0) {
				detail = "PLTE[" + std::to_string(i) + "] = " + rgb_text(&png.plte[3 * i]) + " != pal0[" + std::to_string(i)
						 + "] = " + rgb_text(&pal.rgb[3 * i]) + " (load palette/pal0.gpl, keep indices)";
				return false;
			}
		}
		return true;
	}

	// P0 and P4 on decoded pixels against their 1x parent.
	bool check_pixels(
			const uint8_t* px, int scale, const uint8_t* parent, int parent_w, int parent_h, const std::string& path,
			Hires::File_check& c) {
		const size_t n   = static_cast<size_t>(parent_w) * scale * static_cast<size_t>(parent_h) * scale;
		const auto   nff = std::count(px, px + n, Hires::border_index);
		if (nff > 0) {
			fail(c, Hires::Rule::p0_border, std::to_string(nff) + " pixels with index 0xFF");
			return false;
		}
		const long        bad    = Hires::p4_violations(px, scale, parent, parent_w, parent_h);
		const auto        v      = Hires::p4_verdict(bad, static_cast<long>(n));
		const std::string detail = std::to_string(bad) + " non-compliant cycling pixels of " + std::to_string(n);
		if (v == Hires::P4_verdict::reject) {
			fail(c, Hires::Rule::p4_cycling, detail);
			return false;
		}
		if (v == Hires::P4_verdict::warning) {
			c.warnings.push_back(make_finding(Hires::Rule::p4_cycling, Hires::Severity::warning, path, detail, c.shape, c.frame));
		}
		return true;
	}

	// The PNG steps shared by tiles and terrains: read (F3, F1), F2, F4.
	bool check_png(
			const fs::path& file, int side, const Hires::Pal8& pal, const std::string& path, Hires::Indexed_png& png,
			Hires::File_check& c) {
		switch (Hires::read_indexed_png(file, side, side, png)) {
		case Hires::Png_status::ok:
			break;
		case Hires::Png_status::wrong_size:
			fail(c, Hires::Rule::f3_size, png.error);
			return false;
		case Hires::Png_status::not_palette:
			fail(c, Hires::Rule::f1_colour, png.error + ": quantize to palette 0 first (tools/hires/quantize)");
			return false;
		default:
			fail(c, Hires::Rule::f1_colour, "unreadable PNG: " + png.error);
			return false;
		}
		std::string detail;
		if (!check_plte(png, pal, detail)) {
			fail(c, Hires::Rule::f2_plte, detail);
			return false;
		}
		if (png.has_trns) {
			c.warnings.push_back(make_finding(
					Hires::Rule::f4_trns, Hires::Severity::warning, path, "tRNS present (ignored, indices are read raw)", c.shape,
					c.frame));
		}
		return true;
	}

	// pack.txt: key=value lines; blank lines, '#' comments and lines without '='
	// are skipped (hirescheck.py's read_pack_txt). A UTF-8 BOM is ignored.
	bool read_pack_txt(const fs::path& file, std::map<std::string, std::string>& meta) {
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			return false;
		}
		std::string line;
		bool        first = true;
		while (std::getline(in, line)) {
			if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) {
				line.erase(0, 3);
			}
			first = false;
			line  = trim(line);
			if (line.empty() || line[0] == '#') {
				continue;
			}
			const size_t eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			meta[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
		}
		return !in.bad();
	}
}    // namespace

namespace Hires {
	const char* rule_name(Rule rule) {
		switch (rule) {
		case Rule::none:
			return "--";
		case Rule::n1_name:
			return "N1";
		case Rule::f1_colour:
			return "F1";
		case Rule::f2_plte:
			return "F2";
		case Rule::f3_size:
			return "F3";
		case Rule::f4_trns:
			return "F4";
		case Rule::p0_border:
			return "P0";
		case Rule::p4_cycling:
			return "P4";
		case Rule::g1_guard:
			return "G1";
		case Rule::g2_group:
			return "G2";
		case Rule::r1_palette:
			return "R1";
		case Rule::b0_bundle:
			return "B0";
		}
		return "?";
	}

	const char* severity_name(Severity severity) {
		switch (severity) {
		case Severity::info:
			return "info";
		case Severity::warning:
			return "warning";
		case Severity::reject:
			return "reject";
		case Severity::group_skipped:
			return "group-skipped";
		case Severity::root_disabled:
			return "root-disabled";
		case Severity::bundle_disabled:
			return "bundle-disabled";
		case Severity::error:
			return "error";
		}
		return "?";
	}

	std::string Finding::format() const {
		std::string s = std::string("[") + rule_name(rule) + " " + severity_name(severity) + "] " + path;
		if (!detail.empty()) {
			s += (path.empty() ? "" : ": ") + detail;
		}
		return s;
	}

	std::string Report::summary(int scale) const {
		std::ostringstream out;
		if (failed) {
			out << "x" << scale << ": load failed: " << failure;
			return out.str();
		}
		out << "x" << scale << ": " << loaded << " tiles loaded (bundle " << bundled << "), " << rejected << " rejected, "
			<< groups_skipped << " groups skipped, " << unguarded << " unguarded, " << warnings << " warnings, " << terrains
			<< " terrains; " << roots << " roots";
		if (roots_disabled > 0) {
			out << " (" << roots_disabled << " disabled)";
		}
		char ms_text[32];
		std::snprintf(ms_text, sizeof(ms_text), "%.1f", ms);
		out << ", " << ms_text << " ms";
		return out.str();
	}

	bool parse_tile_name(const std::string& name, int& shape, int& frame) {
		if (name.size() != 11 || name[4] != '_' || name.compare(7, 4, ".png") != 0) {
			return false;
		}
		int v[6];
		for (int i = 0; i < 6; i++) {
			const char c = name[i < 4 ? i : i + 1];
			if (c < '0' || c > '9') {
				return false;
			}
			v[i] = c - '0';
		}
		shape = ((v[0] * 10 + v[1]) * 10 + v[2]) * 10 + v[3];
		frame = v[4] * 10 + v[5];
		return true;
	}

	bool parse_terrain_name(const std::string& name, uint64_t& key) {
		if (name.size() != 20 || name.compare(16, 4, ".png") != 0) {
			return false;
		}
		key = 0;
		for (int i = 0; i < 16; i++) {
			const char c = name[i];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
				return false;
			}
			key = (key << 4) | static_cast<uint64_t>(hex_value(c));
		}
		return true;
	}

	std::string tile_name(int shape, int frame) {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%04d_%02d.png", shape, frame);
		return buf;
	}

	std::string terrain_name(uint64_t key) {
		return hex16(key) + ".png";
	}

	bool parse_crc_text(const std::string& text, uint32_t& value) {
		std::string s = trim(text);
		if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			s = s.substr(2);
		}
		if (s.empty() || s.size() > 8) {
			return false;
		}
		value = 0;
		for (const char c : s) {
			const int d = hex_value(c);
			if (d < 0) {
				return false;
			}
			value = (value << 4) | static_cast<uint32_t>(d);
		}
		return true;
	}

	File_check check_tile_file(const fs::path& file, int scale, const Pal8& pal, const Src_provider& src) {
		File_check        c;
		const std::string name  = utf8(file.filename());
		const std::string path  = utf8(file);
		int               shape = 0;
		int               frame = 0;
		if (!parse_tile_name(name, shape, frame)) {
			return fail(c, Rule::n1_name, "name does not match SSSS_FF.png");
		}
		c.shape = shape;
		c.frame = frame;
		if (frame >= 32) {
			return fail(c, Rule::n1_name, "frame " + std::to_string(frame) + " >= 32");
		}
		const uint8_t* flat = src ? src(shape, frame) : nullptr;
		if (flat == nullptr) {
			return fail(
					c, Rule::n1_name,
					"(" + std::to_string(shape) + "," + std::to_string(frame)
							+ ") is not a flat of the effective shapes.vga (shape out of range, missing frame or RLE)");
		}
		Indexed_png png;
		if (!check_png(file, 8 * scale, pal, path, png, c)) {
			return c;
		}
		if (!check_pixels(png.pixels.data(), scale, flat, 8, 8, path, c)) {
			return c;
		}
		if (const std::string* g = png.find_text(guard_text_key)) {
			c.guarded      = true;
			uint32_t guard = 0;
			if (!parse_guard(*g, guard)) {
				return fail(c, Rule::g1_guard, std::string("malformed ") + guard_text_key + " '" + *g + "'");
			}
			const uint32_t live = crc32(flat, 64);
			if (guard != live) {
				return fail(
						c, Rule::g1_guard,
						"stale: built against different shapes.vga (guard " + hex8(guard) + ", live " + hex8(live) + ")");
			}
		}
		c.pixels = std::move(png.pixels);
		return c;
	}

	File_check check_tile_pixels(
			const uint8_t* pixels, int shape, int frame, int scale, bool guarded, uint32_t guard, const Src_provider& src,
			const std::string& where) {
		File_check c;
		c.shape   = shape;
		c.frame   = frame;
		c.guarded = guarded;
		if (frame < 0 || frame >= 32) {
			return fail(c, Rule::n1_name, "frame " + std::to_string(frame) + " >= 32");
		}
		const uint8_t* flat = src ? src(shape, frame) : nullptr;
		if (flat == nullptr) {
			return fail(
					c, Rule::n1_name,
					"(" + std::to_string(shape) + "," + std::to_string(frame)
							+ ") is not a flat of the effective shapes.vga (shape out of range, missing frame or RLE)");
		}
		if (pixels == nullptr || scale < 1) {
			return fail(c, Rule::f3_size, "no pixels");
		}
		if (!check_pixels(pixels, scale, flat, 8, 8, where, c)) {
			return c;
		}
		if (guarded) {
			const uint32_t live = crc32(flat, 64);
			if (guard != live) {
				return fail(c, Rule::g1_guard, "stale guard (guard " + hex8(guard) + ", live " + hex8(live) + ")");
			}
		}
		const size_t side = static_cast<size_t>(8 * scale);
		c.pixels.assign(pixels, pixels + side * side);
		return c;
	}

	File_check check_terrain_file(const fs::path& file, int scale, const Pal8& pal, const uint8_t* layer1x) {
		File_check        c;
		const std::string name = utf8(file.filename());
		const std::string path = utf8(file);
		uint64_t          key  = 0;
		if (!parse_terrain_name(name, key)) {
			return fail(c, Rule::n1_name, "name does not match <16 lowercase hex digits>.png");
		}
		if (layer1x == nullptr) {
			return fail(c, Rule::p4_cycling, "no 1x flat layer to check against");
		}
		Indexed_png png;
		if (!check_png(file, terrain_side1x * scale, pal, path, png, c)) {
			return c;
		}
		if (!check_pixels(png.pixels.data(), scale, layer1x, terrain_side1x, terrain_side1x, path, c)) {
			return c;
		}
		if (const std::string* t = png.find_text(terrain_text_key)) {
			std::string s = trim(*t);
			if (s.size() == 18 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
				s = s.substr(2);
			}
			uint64_t v  = 0;
			bool     ok = s.size() == 16;
			for (size_t i = 0; ok && i < s.size(); i++) {
				const int d = hex_value(s[i]);
				ok          = d >= 0;
				v           = (v << 4) | static_cast<uint64_t>(d < 0 ? 0 : d);
			}
			if (!ok || v != key) {
				return fail(
						c, Rule::g1_guard,
						std::string(terrain_text_key) + " '" + *t + "' does not match the file name key " + hex16(key)
								+ " (renamed?)");
			}
		}
		c.pixels = std::move(png.pixels);
		return c;
	}

	void Store::clear() {
		scale = 0;
		pal   = Pal8();
		tiles.clear();
		tiles.shrink_to_fit();
		index.clear();
		info.clear();
		terrain_index.clear();
		pending.clear();
	}

	Report Store::load(const std::vector<Root>& roots, int new_scale, int s_art, const Pal8& new_pal, const Src_provider& src) {
		const auto t0 = std::chrono::steady_clock::now();
		clear();
		Report     rep;
		const auto finish = [&]() {
			for (const auto& f : rep.findings) {
				rep.warnings += f.severity == Severity::warning ? 1 : 0;
			}
			try {
				for (const auto& f : rep.findings) {
					rep.lines.push_back(f.format());
				}
			} catch (const std::bad_alloc&) {
				rep.lines.clear();    // The findings stay; only their text is missing.
			}
			rep.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		};
		if (new_scale < 1 || new_scale > max_scale || s_art < 1 || s_art > max_scale) {
			rep.findings.push_back(make_finding(
					Rule::none, Severity::error, "",
					"invalid scale " + std::to_string(new_scale) + " (art scale " + std::to_string(s_art) + ")"));
			finish();
			return rep;
		}
		try {
			const uint32_t live_crc = palette_crc32(new_pal);
			const size_t   side     = static_cast<size_t>(8 * new_scale);

			struct Candidate {
				std::vector<uint8_t> px;
				Entry_info           info;
			};

			std::map<uint32_t, Candidate>               served;     // Winners, at the store's scale.
			std::map<uint32_t, Entry_info>              rejects;    // Highest-precedence rejection per key.
			std::unordered_map<uint64_t, Terrain_entry> terrains;

			for (size_t r = 0; r < roots.size(); r++) {
				rep.roots++;
				// A root is all or nothing: its results are merged at the end, and an
				// exception that no file or bundle check caught disables this root alone.
				try {
					const fs::path root(roots[r].sys_path);

					const auto disable = [&](Rule rule, const std::string& where, const std::string& detail) {
						rep.roots_disabled++;
						rep.findings.push_back(make_finding(rule, Severity::root_disabled, where, detail));
					};
					// R1: the manifest's palette must be the effective palette 0.
					const fs::path ptxt = root / "pack.txt";
					if (is_file(ptxt)) {
						std::map<std::string, std::string> meta;
						if (!read_pack_txt(ptxt, meta)) {
							disable(Rule::r1_palette, utf8(ptxt), "cannot read pack.txt");
							continue;
						}
						const auto it = meta.find("palette_crc32");
						if (it != meta.end()) {
							uint32_t want = 0;
							if (!parse_crc_text(it->second, want) || want != live_crc) {
								disable(Rule::r1_palette, utf8(ptxt),
										"palette_crc32 " + it->second + " != palette 0 CRC " + hex8(live_crc) + " (wrong game?)");
								continue;
							}
						}
					}
					// x<S>, else x<S_art> reduced.
					int      src_scale = 0;
					fs::path xdir      = root / ("x" + std::to_string(new_scale));
					if (is_dir(xdir)) {
						src_scale = new_scale;
					} else if (new_scale < s_art && s_art % new_scale == 0 && is_dir(root / ("x" + std::to_string(s_art)))) {
						src_scale = s_art;
						xdir      = root / ("x" + std::to_string(s_art));
					} else {
						rep.findings.push_back(make_finding(
								Rule::none, Severity::info, utf8(root),
								"no x" + std::to_string(new_scale)
										+ (new_scale < s_art && s_art % new_scale == 0 ? " or x" + std::to_string(s_art)
																					   : std::string())
										+ " folder"));
						continue;
					}
					const bool reduced = src_scale != new_scale;

					std::map<uint32_t, Candidate>               local;             // This root's accepted tiles.
					std::map<uint32_t, Entry_info>              local_rejects;     // Later candidates override earlier ones.
					std::unordered_map<uint64_t, Terrain_entry> local_terrains;    // This root's terrain overrides.

					// An accepted tile, reduced from the folder's scale to the store's
					// scale (part of the guarded check of its file or entry).
					const auto to_store_scale = [&](File_check c) {
						if (c.rejected() || src_scale == new_scale) {
							return c;
						}
						std::vector<uint8_t> out(side * side);
						if (!reduce_mode(c.pixels.data(), src_scale, new_scale, src(c.shape, c.frame), 8, 8, out.data())) {
							c.error  = true;
							c.detail = "cannot reduce from x" + std::to_string(src_scale);
							c.pixels.clear();
							return c;
						}
						c.pixels = std::move(out);
						return c;
					};
					auto note_reject = [&](const File_check& c, const std::string& where, bool bundle) {
						if (c.shape < 0 || c.frame < 0 || c.frame >= 32 || c.shape > 0xffff) {
							return;
						}
						Entry_info e;
						e.state       = Entry_state::rejected;
						e.path        = where;
						e.rule        = c.rule;
						e.detail      = c.detail;
						e.from_bundle = bundle;
						e.guarded     = c.guarded;
						e.reduced     = reduced;
						e.root        = static_cast<int>(r);

						local_rejects[tile_key(c.shape, c.frame)] = std::move(e);
					};
					auto accept = [&](File_check& c, const std::string& where, bool bundle) {
						Candidate cand;
						cand.px               = std::move(c.pixels);
						cand.info.state       = Entry_state::loaded;
						cand.info.path        = where;
						cand.info.from_bundle = bundle;
						cand.info.guarded     = c.guarded;
						cand.info.reduced     = reduced;
						cand.info.root        = static_cast<int>(r);

						local[tile_key(c.shape, c.frame)] = std::move(cand);
					};

					// The bundle: entries independent, in any order, no groups.
					const fs::path bpath = xdir / "flats.bundle";
					if (is_file(bpath)) {
						Bundle      bundle;
						std::string error;
						bool        readable = false;
						try {
							readable = bundle.read(bpath, src_scale, live_crc, error);
						} catch (const std::bad_alloc&) {
							error = "cannot read: out of memory";
						} catch (const std::exception& e) {
							error = std::string("cannot read: ") + e.what();
						}
						if (!readable) {
							rep.findings.push_back(make_finding(Rule::b0_bundle, Severity::bundle_disabled, utf8(bpath), error));
						} else {
							const std::string bname = utf8(bpath) + "#";
							for (size_t i = 0; i < bundle.size(); i++) {
								const Bundle_entry e     = bundle.entry(i);
								const std::string  where = bname + key_text(e.shape, e.frame);
								File_check         c     = guarded(e.shape, e.frame, [&]() {
                                    return to_store_scale(check_tile_pixels(
                                            e.pixels, e.shape, e.frame, src_scale, e.guarded, e.guard, src, where));
                                });
								rep.tiles++;
								rep.findings.insert(rep.findings.end(), c.warnings.begin(), c.warnings.end());
								if (c.rejected()) {
									rep.rejected++;
									rep.findings.push_back(make_finding(c.rule, severity_of(c), where, c.detail, e.shape, e.frame));
									note_reject(c, where, true);
									continue;
								}
								accept(c, where, true);
							}
						}
					}

					// Loose files: the top level, then the groups, in name order.
					const fs::path fdir = xdir / "flats";
					if (is_dir(fdir)) {
						struct Unit {
							std::string           group;    // Empty for the top level.
							fs::path              dir;
							std::vector<Dir_item> files;
						};

						std::vector<Unit>     units(1);
						std::vector<Dir_item> top = list_dir(fdir);
						units[0].dir              = fdir;
						for (const auto& item : top) {
							if (item.file && ends_with_png(item.name)) {
								units[0].files.push_back(item);
							}
						}
						for (const auto& item : top) {
							// Directories "_*" (work folders) and ".*" are not groups, as in hirescheck.py.
							if (!item.directory || item.name[0] == '_' || item.name[0] == '.') {
								continue;
							}
							if (item.name.size() > 4 && item.name.compare(item.name.size() - 4, 4, ".off") == 0) {
								rep.findings.push_back(
										make_finding(Rule::g2_group, Severity::info, utf8(item.path), "disabled group (.off)"));
								continue;
							}
							Unit unit;
							unit.group = item.name;
							unit.dir   = item.path;
							for (auto& sub : list_dir(item.path)) {
								if (sub.file && ends_with_png(sub.name)) {
									unit.files.push_back(std::move(sub));
								}
							}
							units.push_back(std::move(unit));
						}
						std::map<uint32_t, std::string> seen;    // Accepted loose keys of this root.
						for (auto& unit : units) {
							std::vector<std::pair<std::string, File_check>> results;
							results.reserve(unit.files.size());
							for (const auto& file : unit.files) {
								int shape = -1;
								int frame = -1;
								parse_tile_name(file.name, shape, frame);
								results.emplace_back(utf8(file.path), guarded(shape, frame, [&]() {
														 return to_store_scale(check_tile_file(file.path, src_scale, new_pal, src));
													 }));
								rep.tiles++;
							}
							const auto first_bad = std::find_if(results.begin(), results.end(), [](const auto& res) {
								return res.second.rejected();
							});
							const bool group_bad = !unit.group.empty() && first_bad != results.end();
							for (auto& res : results) {
								File_check& c = res.second;
								rep.findings.insert(rep.findings.end(), c.warnings.begin(), c.warnings.end());
								if (c.rejected()) {
									rep.rejected++;
									rep.findings.push_back(
											make_finding(c.rule, severity_of(c), res.first, c.detail, c.shape, c.frame));
									note_reject(c, res.first, false);
									continue;
								}
								if (group_bad) {
									rep.rejected++;
									c.rule   = Rule::g2_group;
									c.detail = "group " + unit.group + " skipped";
									note_reject(c, res.first, false);
									continue;
								}
								const uint32_t key = tile_key(c.shape, c.frame);
								const auto     dup = seen.find(key);
								if (dup != seen.end()) {
									rep.findings.push_back(make_finding(
											Rule::n1_name, Severity::warning, res.first, "duplicate key, also " + dup->second,
											c.shape, c.frame));
								}
								seen[key] = res.first;
								accept(c, res.first, false);
							}
							if (group_bad) {
								const File_check& bad = first_bad->second;
								rep.groups_skipped++;
								rep.findings.push_back(make_finding(
										Rule::g2_group, Severity::group_skipped, utf8(unit.dir),
										"group skipped: " + first_bad->first
												+ (bad.error ? std::string(" could not be checked")
															 : std::string(" failed ") + rule_name(bad.rule))));
							}
						}
					}

					// Terrain overrides: indexed now, validated when decoded.
					const fs::path tdir = xdir / "terrain";
					if (is_dir(tdir)) {
						for (const auto& item : list_dir(tdir)) {
							if (!item.file || !ends_with_png(item.name)) {
								continue;
							}
							uint64_t key = 0;
							if (!parse_terrain_name(item.name, key)) {
								rep.rejected++;
								rep.findings.push_back(make_finding(
										Rule::n1_name, Severity::reject, utf8(item.path),
										"name does not match <16 lowercase hex digits>.png"));
								continue;
							}
							Terrain_entry t;
							t.path              = item.path;
							t.src_scale         = src_scale;
							t.info.state        = Entry_state::indexed;
							t.info.path         = utf8(item.path);
							t.info.reduced      = reduced;
							t.info.root         = static_cast<int>(r);
							local_terrains[key] = std::move(t);
						}
					}

					// The first root with a candidate serves the key (and explains a
					// rejected one): merge() keeps the entries already there. Reserved
					// first, so the merge itself allocates nothing.
					terrains.reserve(terrains.size() + local_terrains.size());
					served.merge(local);
					rejects.merge(local_rejects);
					terrains.merge(local_terrains);
				} catch (const std::bad_alloc&) {
					rep.roots_disabled++;
					rep.findings.push_back(make_finding(
							Rule::none, Severity::root_disabled, root_text(roots[r].sys_path), "load failed: out of memory"));
				} catch (const std::exception& e) {
					rep.roots_disabled++;
					rep.findings.push_back(make_finding(
							Rule::none, Severity::root_disabled, root_text(roots[r].sys_path),
							std::string("load failed: ") + e.what()));
				}
			}

			// Commit: one arena in key order.
			std::vector<uint8_t>                   arena(served.size() * side * side);
			std::unordered_map<uint32_t, uint32_t> new_index;
			std::map<uint32_t, Entry_info>         new_info;
			uint32_t                               n = 0;
			for (auto& entry : served) {
				// Every accepted tile has side x side pixels; never write past its slot.
				std::copy_n(
						entry.second.px.begin(), std::min(entry.second.px.size(), side * side),
						arena.begin() + static_cast<std::ptrdiff_t>(n * side * side));
				new_index.emplace(entry.first, n++);
				rep.bundled += entry.second.info.from_bundle ? 1 : 0;
				rep.unguarded += entry.second.info.guarded ? 0 : 1;
				new_info.emplace(entry.first, std::move(entry.second.info));
			}
			for (auto& entry : rejects) {
				new_info.emplace(entry.first, std::move(entry.second));    // Only where nothing is served.
			}
			rep.loaded    = static_cast<int>(n);
			rep.terrains  = static_cast<int>(terrains.size());
			scale         = new_scale;
			pal           = new_pal;
			tiles         = std::move(arena);
			index         = std::move(new_index);
			info          = std::move(new_info);
			terrain_index = std::move(terrains);
		} catch (const std::bad_alloc&) {
			clear();
			rep.loaded = rep.bundled = rep.unguarded = rep.terrains = 0;
			rep.findings.push_back(make_finding(Rule::none, Severity::error, "", "out of memory: no overrides loaded"));
		} catch (const std::exception& e) {
			clear();
			rep.loaded = rep.bundled = rep.unguarded = rep.terrains = 0;
			rep.findings.push_back(make_finding(Rule::none, Severity::error, "", std::string("load failed: ") + e.what()));
		}
		finish();
		return rep;
	}

	Tile_view Store::flat(int shape, int frame) const {
		if (shape < 0 || shape > 0xffff || index.empty()) {
			return {};
		}
		const auto it = index.find(tile_key(shape, frame));
		if (it == index.end()) {
			return {};
		}
		const int side = 8 * scale;
		return {tiles.data() + static_cast<size_t>(it->second) * side * side, side};
	}

	bool Store::terrain(uint64_t key, const uint8_t* layer1x, uint8_t* dst, int dst_w, int dst_h, int dst_pitch) {
		const auto it = terrain_index.find(key);
		if (it == terrain_index.end()) {
			return false;
		}
		Terrain_entry& t    = it->second;
		const int      side = terrain_side1x * scale;
		if (t.info.state == Entry_state::rejected || dst == nullptr || layer1x == nullptr || dst_w != side || dst_h != side
			|| dst_pitch < side) {
			return false;
		}
		try {
			File_check c = check_terrain_file(t.path, t.src_scale, pal, layer1x);
			if (!t.logged) {
				pending.insert(pending.end(), c.warnings.begin(), c.warnings.end());
			}
			if (c.rejected()) {
				if (!t.logged) {
					pending.push_back(make_finding(c.rule, Severity::reject, t.info.path, c.detail));
				}
				t.logged      = true;
				t.info.rule   = c.rule;
				t.info.detail = c.detail;
				if (c.rule != Rule::p4_cycling) {
					t.info.state = Entry_state::rejected;    // P4 depends on the layer: not latched.
				}
				return false;
			}
			t.logged = true;
			std::vector<uint8_t> px;
			if (t.src_scale != scale) {
				px.resize(static_cast<size_t>(side) * side);
				if (!reduce_mode(c.pixels.data(), t.src_scale, scale, layer1x, terrain_side1x, terrain_side1x, px.data())) {
					return false;
				}
			} else {
				px = std::move(c.pixels);
			}
			for (int y = 0; y < side; y++) {
				std::memcpy(dst + static_cast<size_t>(y) * dst_pitch, px.data() + static_cast<size_t>(y) * side, side);
			}
			t.info.rule = Rule::none;
			t.info.detail.clear();
			return true;
		} catch (const std::exception& e) {
			// std::bad_alloc included: this terrain falls back, nothing propagates into paint.
			try {
				pending.push_back(
						make_finding(Rule::none, Severity::error, t.info.path, std::string("decode failed: ") + e.what()));
			} catch (const std::bad_alloc&) {
				// Not even the message fits: the fallback alone.
			}
			return false;
		}
	}

	const Entry_info* Store::explain_flat(int shape, int frame) const {
		if (shape < 0 || shape > 0xffff) {
			return nullptr;
		}
		const auto it = info.find(tile_key(shape, frame));
		return it == info.end() ? nullptr : &it->second;
	}

	const Entry_info* Store::explain_terrain(uint64_t key) const {
		const auto it = terrain_index.find(key);
		return it == terrain_index.end() ? nullptr : &it->second.info;
	}

	std::vector<Finding> Store::take_findings() {
		std::vector<Finding> out;
		out.swap(pending);
		return out;
	}

	Store_set::Store_set(Inputs_fn inputs, Report_fn on_report, Text_fn on_text)
			: get_inputs(std::move(inputs)), report_fn(std::move(on_report)), text_fn(std::move(on_text)) {}

	Store* Store_set::store(int scale) {
		if (scale < 2) {
			return nullptr;
		}
		Entry* entry = nullptr;
		try {
			entry = &stores[scale];
			if (entry->store != nullptr || entry->failed) {
				return entry->store.get();
			}
			auto         store  = std::make_unique<Store>();
			const Inputs inputs = get_inputs ? get_inputs() : Inputs();
			entry->report       = store->load(inputs.roots, scale, inputs.s_art, inputs.pal, inputs.src);
			entry->store        = std::move(store);
		} catch (const std::exception& e) {
			// Fail soft (I11): no overrides at this scale until the next invalidate().
			if (entry == nullptr) {
				return nullptr;    // No memory even for the entry: tried again on the next call.
			}
			entry->store.reset();
			entry->failed        = true;
			entry->report        = Report();
			entry->report.failed = true;
			try {
				entry->report.failure = e.what();
			} catch (const std::exception&) {
				// No message; the failed flag stands.
			}
			try {
				if (text_fn) {
					text_fn("x" + std::to_string(scale) + ": overrides disabled: " + e.what());
				}
			} catch (const std::exception&) {
				// The message is lost; the latch stands.
			}
			return nullptr;
		}
		try {
			if (report_fn) {
				report_fn(scale, entry->report);
			}
		} catch (const std::exception&) {
			// The report is lost; the store stands.
		}
		return entry->store.get();
	}

	const Report* Store_set::report(int scale) const {
		const auto it = stores.find(scale);
		return it != stores.end() && (it->second.store != nullptr || it->second.failed) ? &it->second.report : nullptr;
	}

	Tile_view Store_set::flat(int shape, int frame, int scale) {
		if (!is_on || scale < 2) {
			return {};
		}
		const Store* s = store(scale);
		return s != nullptr ? s->flat(shape, frame & 31) : Tile_view();
	}

	bool Store_set::terrain(uint64_t key, int scale, const uint8_t* layer1x, uint8_t* dst, int dst_w, int dst_h, int dst_pitch) {
		if (!is_on || scale < 2) {
			return false;
		}
		Store* s = store(scale);
		if (s == nullptr) {
			return false;
		}
		const bool done = s->terrain(key, layer1x, dst, dst_w, dst_h, dst_pitch);
		try {
			for (const auto& f : s->take_findings()) {
				if (text_fn) {
					text_fn(f.format());
				}
			}
		} catch (const std::exception&) {
			// The messages are lost; the answer stands (nothing propagates into paint).
		}
		return done;
	}

	void Store_set::invalidate() {
		stores.clear();
		gen++;
	}

	void Store_set::set_enabled(bool on) {
		if (is_on != on) {
			is_on = on;
			gen++;
		}
	}
}    // namespace Hires
