/*
 *  dump_art.cc - The engine's reference set for hi-res art production
 *  (--dump-art, DESIGN.md section 4.2).
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

/*
 *  The dump makes the engine the single source of truth for the art inputs:
 *  the flats come from the effective shapes.vga (patch included), palette 0
 *  is the one the override store checks (rule R1), the terrains are painted
 *  by Chunk_terrain::paint_flats with the overrides off, and the fill under
 *  RLE tiles is Chunk_terrain::get_flat_source. The game is loaded the way
 *  --buildmap loads it (gamma 1, palette 0, scale 1); nothing is written into
 *  the game's directories. The output does not depend on time, paths or heap
 *  order, so two runs of one binary give identical trees.
 *
 *  Layout of <dir> (all PNGs: colour type 3, raw engine indices, no ipack
 *  rotation, PLTE = palette 0 with 256 entries, deterministic bytes):
 *
 *    ref.txt             key=value: format, game, mod, engine version and git
 *                        revision, palette_crc32, conventions, counts, which
 *                        inputs came from <PATCH>
 *    palette/pal0.gpl    GIMP/Aseprite palette, 8 bit = min(255, v*255/63)
 *    palette/pal0.act    the same, 768 bytes RGB
 *    palette/classes.txt "<index> <class>": static, cycle:E0-E7 ... cycle:FC-FE,
 *                        reserved (0xff)
 *    flats/SSSS_FF.png   every flat (shape, frame < 32) at 1x, tEXt
 *                        Exult-Src-CRC32 = CRC32 of its 64 bytes
 *    flats.txt           shape frame crc32 map_uses cycle_px used_on_map
 *    templates/pack.txt and templates/x6/flats/SSSS/SSSS_FF.png
 *                        NN x6 of every flat with the guard: the identity
 *                        pack, a pack root as it is
 *    terrain/<t1>.png    the 1x 128x128 flat layer of each distinct T1 key, as
 *                        paint_flats paints it (fill and P3 zeros included),
 *                        tEXt Exult-Terrain-Key
 *    terrain.txt         tnum t1 uses own_cells rle_cells missing_cells
 *                        duplicate_of same_layer
 *    terrain_tiles.bin   header + per terrain: 256 x {own shape u16, frame u8,
 *                        kind u8}, then 256 x {source shape u16, frame & 31 u8,
 *                        source tile u8}
 *    terrain_map.bin     header + per map: {map number u32, 192 x 192 u16
 *                        terrain numbers, row by row (cy outer, cx inner)}
 *    manifest.txt        crc32 size path of every file above except ref.txt
 *
 *  The binary files start with a 16-byte header, all little-endian:
 *  "U7HR", version u16 (1), type u16 (1 = terrain_tiles, 2 = terrain_map),
 *  record size u32, record count u32.
 *
 *  A dump into an earlier dump replaces the entries above wholesale. So that
 *  no work is lost (templates/ is a modder's starting point), the run is
 *  refused when an entry holds a file the manifest does not list, or one
 *  whose size or CRC changed since; files outside the entries are kept.
 */

#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif

#include "dump_art.h"

#include "Audio.h"
#include "chunkter.h"
#include "exult_constants.h"
#include "flat_source.h"
#include "fnames.h"
#include "game.h"
#include "gamemap.h"
#include "gamewin.h"
#include "hires_glue.h"
#include "hires_png.h"
#include "hires_rules.h"
#include "ibuf8.h"
#include "iwin8.h"
#include "palette.h"
#include "shapeid.h"
#include "utils.h"
#include "version.h"
#include "vgafile.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::vector;

namespace fs = std::filesystem;

namespace {
	constexpr int template_scale  = 6;     // S_art of the identity templates.
	constexpr int max_flat_frames = 32;    // Keys are (shape, frame & 31).
	constexpr int tiles           = c_tiles_per_chunk * c_tiles_per_chunk;
	constexpr int layer_side      = c_chunksize;
	constexpr int layer_bytes     = layer_side * layer_side;
	constexpr int max_maps_probe  = 10;     // As the cheat screen looks for maps.
	constexpr int window_w        = 320;    // The (unused) game window.
	constexpr int window_h        = 200;

	constexpr size_t max_listed = 10;    // Lost files named in the refusal.

	const char* const format_id     = "exult-dump-art/1";
	const char* const guard_key     = "Exult-Src-CRC32";
	const char* const ref_name      = "ref.txt";
	const char* const manifest_name = "manifest.txt";

	// The entries a dump owns; a re-dump into the same directory replaces them.
	const char* const dump_entries[] = {"ref.txt", "manifest.txt", "palette",           "flats",          "flats.txt", "templates",
										"terrain", "terrain.txt",  "terrain_tiles.bin", "terrain_map.bin"};

	struct Flat {
		int                     shape;
		int                     frame;
		std::array<uint8_t, 64> px;
		uint32_t                crc;
		uint64_t                map_uses    = 0;    // Own-tile cells on all maps.
		uint64_t                used_on_map = 0;    // Cells painted with it on all maps.
	};

	struct Terrain {
		uint16_t             own_shape[tiles];
		uint8_t              own_frame[tiles];
		Tile_kind            kind[tiles];
		int                  src[tiles];    // Source tile, or -1.
		std::vector<uint8_t> layer;         // 128 x 128, paint_flats at 1x.
		uint64_t             t1    = 0;
		uint64_t             uses  = 0;     // Map chunks, all maps.
		int                  canon = -1;    // Terrain whose layer is terrain/<t1>.png.
	};

	// The path without a trailing separator, which weakly_canonical keeps.
	fs::path without_separator(const fs::path& path) {
		return !path.has_filename() && path.has_relative_path() ? path.parent_path() : path;
	}

	// Size and CRC-32 of a file; false when it cannot be read.
	bool file_crc(const fs::path& path, uint64_t& size, uint32_t& crc) {
		std::ifstream f(path, std::ios::binary);
		if (!f) {
			return false;
		}
		std::vector<char> buf(1 << 16);
		size = 0;
		crc  = 0;
		while (f) {
			f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
			const auto n = static_cast<size_t>(f.gcount());
			crc          = Hires::crc32(buf.data(), n, crc);
			size += n;
		}
		return !f.bad();
	}

	string hex32(uint32_t v) {
		char text[12];
		snprintf(text, sizeof(text), "%08" PRIx32, v);
		return text;
	}

	string hex64(uint64_t v) {
		char text[20];
		snprintf(text, sizeof(text), "%016" PRIx64, v);
		return text;
	}

	string flat_name(int shape, int frame) {
		char text[16];
		snprintf(text, sizeof(text), "%04d_%02d", shape, frame);
		return text;
	}

	string shape_dir(int shape) {
		char text[8];
		snprintf(text, sizeof(text), "%04d", shape);
		return text;
	}

	void put_le(std::string& out, uint64_t v, int bytes) {
		for (int i = 0; i < bytes; i++) {
			out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
		}
	}

	string bin_header(int type, uint32_t record_size, uint32_t count) {
		string out("U7HR");
		put_le(out, 1, 2);
		put_le(out, static_cast<uint64_t>(type), 2);
		put_le(out, record_size, 4);
		put_le(out, count, 4);
		return out;
	}

	// The game type, as pack.txt names it: BG, SI, DEVEL (NONE otherwise).
	const char* game_label() {
		if (GAME_BG) {
			return "BG";
		}
		if (GAME_SI) {
			return "SI";
		}
		return Game::get_game_type() == EXULT_DEVEL_GAME ? "DEVEL" : "NONE";
	}

	// The variant of the game type: FOV, SS, SIB, or none.
	const char* game_variant() {
		if (GAME_FOV) {
			return "FOV";
		}
		if (GAME_SIB) {
			return "SIB";
		}
		if (GAME_SS) {
			return "SS";
		}
		return "none";
	}

	// "yes" when the game file 'name' (a <PATCH> path) exists.
	const char* patched(const char* name) {
		return is_system_path_defined("<PATCH>") && U7exists(name) ? "yes" : "no";
	}

	// The class of a palette index (rule P4's ranges, gamewin.cc rotatecolours).
	string index_class(int index) {
		static const char* const ranges[Hires::num_cycle_ranges] = {"E0-E7", "E8-EF", "F0-F3", "F4-F7", "F8-FB", "FC-FE"};
		if (index == Hires::border_index) {
			return "reserved";
		}
		const int range = Hires::cycle_range(static_cast<uint8_t>(index));
		return range < 0 ? string("static") : string("cycle:") + ranges[range];
	}

	class Dumper {
		fs::path                              out;
		Hires::Pal8                           pal;
		uint32_t                              pal_crc = 0;
		vector<Flat>                          flats;
		vector<Terrain>                       terrains;
		vector<int>                           map_nums;
		vector<vector<uint16_t>>              maps;    // [map][cy * 192 + cx].
		uint64_t                              bad_terrain_refs = 0;
		std::map<std::pair<int, int>, size_t> flat_index;
		vector<string>                        written;    // For manifest.txt.
		bool                                  failed = false;

		fs::path file(const string& rel) const {
			return out / fs::u8path(rel);
		}

		void fail(const string& text) {
			cerr << "--dump-art: " << text << endl;
			failed = true;
		}

		bool write_file(const string& rel, const string& data) {
			std::ofstream f(file(rel), std::ios::binary | std::ios::trunc);
			f.write(data.data(), static_cast<std::streamsize>(data.size()));
			f.close();
			if (!f) {
				fail("cannot write " + file(rel).string());
				return false;
			}
			written.push_back(rel);
			return true;
		}

		bool write_png(const string& rel, int w, int h, const uint8_t* px, const vector<Hires::Png_text>& text) {
			Hires::Png_write_options opt;
			opt.text_before            = text;
			const Hires::Png_status st = Hires::write_indexed_png(file(rel), w, h, px, pal.rgb, 256, opt);
			if (st != Hires::Png_status::ok) {
				fail("cannot write " + file(rel).string() + " (" + Hires::png_status_name(st) + ")");
				return false;
			}
			written.push_back(rel);
			return true;
		}

		bool make_dir(const string& rel) {
			std::error_code ec;
			fs::create_directories(file(rel), ec);
			if (ec) {
				fail("cannot create " + file(rel).string() + ": " + ec.message());
				return false;
			}
			return true;
		}

		/*
		 *  The files of an earlier dump that a re-dump would lose: files in
		 *  the dump's entries that its manifest.txt does not list, or whose
		 *  size or CRC changed since (sorted). Returns false when the earlier
		 *  dump has no manifest.
		 */
		bool find_foreign(vector<string>& lost) const {
			std::ifstream mf(out / manifest_name);
			if (!mf) {
				return false;
			}
			std::map<string, std::pair<uint64_t, uint32_t>> listed;
			string                                          line;
			while (std::getline(mf, line)) {
				if (line.empty() || line[0] == '#') {
					continue;
				}
				std::istringstream in(line);
				string             crc_text;
				uint64_t           size = 0;
				string             rel;
				in >> crc_text >> size >> std::ws;
				std::getline(in, rel);
				listed[rel] = {size, static_cast<uint32_t>(std::strtoul(crc_text.c_str(), nullptr, 16))};
			}
			for (const char* name : dump_entries) {
				if (std::strcmp(name, ref_name) == 0 || std::strcmp(name, manifest_name) == 0) {
					continue;
				}
				std::error_code       ec;
				const fs::path        top = out / name;
				const fs::file_status st  = fs::symlink_status(top, ec);
				if (ec || !fs::exists(st)) {
					continue;
				}
				vector<std::pair<fs::path, fs::file_status>> files;
				if (fs::is_directory(st)) {
					fs::recursive_directory_iterator it(top, ec);
					for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
						const fs::file_status est = it->symlink_status(ec);
						if (!ec && !fs::is_directory(est)) {
							files.emplace_back(it->path(), est);
						}
					}
					if (ec) {
						lost.push_back(fs::path(name).generic_string() + " (cannot list: " + ec.message() + ")");
					}
				} else {
					files.emplace_back(top, st);
				}
				for (const auto& [path, fst] : files) {
					const string rel  = path.lexically_relative(out).generic_string();
					const auto   it   = listed.find(rel);
					uint64_t     size = 0;
					uint32_t     crc  = 0;
					if (!fs::is_regular_file(fst)) {
						lost.push_back(rel + " (not a regular file)");
					} else if (it == listed.end()) {
						lost.push_back(rel + " (not written by the dump)");
					} else if (!file_crc(path, size, crc) || it->second != std::make_pair(size, crc)) {
						lost.push_back(rel + " (changed since the dump)");
					}
				}
			}
			std::sort(lost.begin(), lost.end());
			return true;
		}

		/*
		 *  The output directory: created when missing; an existing one must be
		 *  empty or hold an earlier dump, whose own entries are removed when
		 *  they hold nothing but the files the earlier dump wrote (its
		 *  manifest). It must not lie inside the game's static directory.
		 */
		bool prepare_output() {
			std::error_code ec;
			if (fs::exists(out, ec) && !fs::is_directory(out, ec)) {
				cerr << "--dump-art: " << out.string() << " exists and is not a directory" << endl;
				return false;
			}
			if (is_system_path_defined("<STATIC>")) {
				std::error_code ec_stat;
				std::error_code ec_dest;
				const fs::path  stat = without_separator(fs::weakly_canonical(fs::u8path(get_system_path("<STATIC>")), ec_stat));
				const fs::path  dest = without_separator(fs::weakly_canonical(out, ec_dest));
				if (!ec_stat && !ec_dest) {
					const auto mism = std::mismatch(stat.begin(), stat.end(), dest.begin(), dest.end());
					if (mism.first == stat.end()) {
						cerr << "--dump-art: " << out.string() << " lies inside the game's static directory " << stat.string()
							 << endl;
						return false;
					}
				}
			}
			if (fs::is_directory(out, ec) && !fs::is_empty(out, ec)) {
				bool          earlier = false;
				string        line;
				std::ifstream ref(out / "ref.txt");
				while (!earlier && std::getline(ref, line)) {
					earlier = line == string("format=") + format_id;
				}
				if (!earlier) {
					cerr << "--dump-art: " << out.string()
						 << " is not empty and holds no earlier dump (ref.txt with format=" << format_id << ")" << endl;
					return false;
				}
				vector<string> lost;
				if (!find_foreign(lost)) {
					cerr << "--dump-art: the earlier dump in " << out.string() << " has no " << manifest_name
						 << ", so its files cannot be told from added ones; delete it by hand" << endl;
					return false;
				}
				if (!lost.empty()) {
					cerr << "--dump-art: " << out.string() << " holds " << lost.size()
						 << " file(s) that a new dump would delete: a dump replaces its entries wholesale. Copy them out "
							"of the dump (or delete them) and run again:"
						 << endl;
					for (size_t i = 0; i < lost.size() && i < max_listed; i++) {
						cerr << "  " << lost[i] << endl;
					}
					if (lost.size() > max_listed) {
						cerr << "  ..." << endl;
					}
					return false;
				}
				for (const char* name : dump_entries) {
					fs::remove_all(out / name, ec);
					if (ec) {
						cerr << "--dump-art: cannot remove " << (out / name).string() << ": " << ec.message() << endl;
						return false;
					}
				}
			}
			fs::create_directories(out, ec);
			if (ec) {
				cerr << "--dump-art: cannot create " << out.string() << ": " << ec.message() << endl;
				return false;
			}
			return true;
		}

		// Every flat of the effective shapes.vga, in (shape, frame) order.
		void gather_flats() {
			const int num_shapes = Shape_manager::get_instance()->get_shapes().get_num_shapes();
			for (int shape = 0; shape < num_shapes; shape++) {
				for (int frame = 0; frame < max_flat_frames; frame++) {
					const uint8_t* px = Hires::source_flat(shape, frame);
					if (px == nullptr) {
						continue;
					}
					Flat f{shape, frame, {}, 0};
					std::memcpy(f.px.data(), px, f.px.size());
					f.crc                      = Hires::crc32(f.px.data(), f.px.size());
					flat_index[{shape, frame}] = flats.size();
					flats.push_back(f);
				}
			}
		}

		// Map 0 and every further map of the game (<STATIC> or <PATCH> mapNN).
		void gather_maps(Game_window* gwin) {
			int num = 0;
			while (num >= 0) {
				const Game_map*  gmap = gwin->get_map(num);
				vector<uint16_t> tmap(static_cast<size_t>(c_num_chunks) * c_num_chunks);
				for (int cy = 0; cy < c_num_chunks; cy++) {
					for (int cx = 0; cx < c_num_chunks; cx++) {
						tmap[static_cast<size_t>(cy) * c_num_chunks + cx] = static_cast<uint16_t>(gmap->get_terrain_num(cx, cy));
					}
				}
				map_nums.push_back(num);
				maps.push_back(std::move(tmap));
				num = Find_next_map(num + 1, max_maps_probe);
			}
		}

		// The tiles, the fill and the 1x layer of every terrain.
		void gather_terrains(Game_window* gwin) {
			const int     count = gwin->get_map(0)->get_num_chunk_terrains();
			Image_buffer8 buf(layer_side, layer_side, 1);
			terrains.resize(static_cast<size_t>(count));
			for (int tnum = 0; tnum < count; tnum++) {
				Chunk_terrain* ter = Game_map::get_terrain(tnum);
				Terrain&       t   = terrains[static_cast<size_t>(tnum)];
				const uint8_t* own[tiles];
				for (int ty = 0; ty < c_tiles_per_chunk; ty++) {
					for (int tx = 0; tx < c_tiles_per_chunk; tx++) {
						const int     i  = ty * c_tiles_per_chunk + tx;
						const ShapeID id = ter->get_flat(tx, ty);
						Shape_frame*  fr = ter->get_shape(tx, ty);
						t.own_shape[i]   = static_cast<uint16_t>(id.get_shapenum());
						t.own_frame[i]   = static_cast<uint8_t>(id.get_framenum());
						if (fr == nullptr) {
							t.kind[i] = Tile_kind::None;
						} else if (fr->is_rle()) {
							t.kind[i] = Tile_kind::Rle;
						} else {
							const bool is_void = id.get_shapenum() == 12 && id.get_framenum() == 0;
							t.kind[i]          = is_void ? Tile_kind::Flat_void : Tile_kind::Flat;
						}
						own[i] = Is_flat(t.kind[i]) ? fr->get_data() : nullptr;
					}
				}
				t.t1 = Hires::terrain_key_t1(own);
				for (int ty = 0; ty < c_tiles_per_chunk; ty++) {
					for (int tx = 0; tx < c_tiles_per_chunk; tx++) {
						t.src[ty * c_tiles_per_chunk + tx] = ter->get_flat_source(tx, ty);
					}
				}
				ter->paint_flats(buf, false);
				t.layer.resize(layer_bytes);
				const unsigned char* bits = buf.get_bits();
				for (int y = 0; y < layer_side; y++) {
					std::memcpy(
							&t.layer[static_cast<size_t>(y) * layer_side], bits + static_cast<size_t>(y) * buf.get_line_width(),
							layer_side);
				}
			}
		}

		// Map usage of the terrains and the flats; the canonical terrain of each
		// T1 key (the first used one, else the first one).
		void count_uses() {
			for (const auto& tmap : maps) {
				for (const uint16_t tnum : tmap) {
					if (tnum < terrains.size()) {
						terrains[tnum].uses++;
					} else {
						bad_terrain_refs++;
					}
				}
			}
			std::map<uint64_t, int> canon;
			for (int pass = 0; pass < 2; pass++) {
				for (size_t tnum = 0; tnum < terrains.size(); tnum++) {
					const Terrain& t = terrains[tnum];
					if ((pass == 0) == (t.uses > 0)) {
						canon.emplace(t.t1, static_cast<int>(tnum));
					}
				}
			}
			for (auto& t : terrains) {
				t.canon = canon[t.t1];
				if (t.uses == 0) {
					continue;
				}
				for (int i = 0; i < tiles; i++) {
					if (Is_flat(t.kind[i])) {
						const auto it = flat_index.find({t.own_shape[i], t.own_frame[i] & 31});
						if (it != flat_index.end()) {
							flats[it->second].map_uses += t.uses;
						}
					}
					if (t.src[i] >= 0) {
						const auto it = flat_index.find({t.own_shape[t.src[i]], t.own_frame[t.src[i]] & 31});
						if (it != flat_index.end()) {
							flats[it->second].used_on_map += t.uses;
						}
					}
				}
			}
		}

		void write_palette() {
			if (!make_dir("palette")) {
				return;
			}
			std::ostringstream gpl;
			std::ostringstream classes;
			gpl << "GIMP Palette\nName: Exult " << game_label() << " palette 0\nColumns: 16\n"
				<< "# Raw engine indices (no rotation). 8 bit = min(255, v * 255 / 63). CRC32 " << hex32(pal_crc) << '\n';
			classes << "# index class (static, cycle:<range>, reserved)\n";
			string act;
			for (int i = 0; i < 256; i++) {
				const uint8_t* c = &pal.rgb[3 * i];
				char           line[64];
				snprintf(line, sizeof(line), "%3d %3d %3d\t%02x %s\n", c[0], c[1], c[2], i, index_class(i).c_str());
				gpl << line;
				snprintf(line, sizeof(line), "%02x %s\n", i, index_class(i).c_str());
				classes << line;
				act.append(reinterpret_cast<const char*>(c), 3);
			}
			write_file("palette/pal0.gpl", gpl.str());
			write_file("palette/pal0.act", act);
			write_file("palette/classes.txt", classes.str());
		}

		void write_flats() {
			if (!make_dir("flats")) {
				return;
			}
			const int          side = c_tilesize * template_scale;
			vector<uint8_t>    nn(static_cast<size_t>(side) * side);
			std::ostringstream txt;
			txt << "# shape frame crc32 map_uses cycle_px used_on_map\n"
				<< "# map_uses: map cells (all maps) whose own tile is this flat; cycle_px: pixels in a cycle range;\n"
				<< "# used_on_map: map cells painted with this flat (own tiles and the fill under RLE tiles)\n";
			for (const Flat& f : flats) {
				const string name  = flat_name(f.shape, f.frame);
				const string guard = hex32(f.crc);
				if (!write_png(
							"flats/" + name + ".png", c_tilesize, c_tilesize, f.px.data(),
							{
									{guard_key, guard}
                })) {
					return;
				}
				const string dir = "templates/x" + std::to_string(template_scale) + "/flats/" + shape_dir(f.shape);
				if (!make_dir(dir)) {
					return;
				}
				Hires::nn_upscale(f.px.data(), c_tilesize, c_tilesize, template_scale, nn.data());
				if (!write_png(
							dir + "/" + name + ".png", side, side, nn.data(),
							{
									{     guard_key,      guard},
                                    {"Exult-Origin", "identity"}
                })) {
					return;
				}
				int cycle_px = 0;
				for (const uint8_t v : f.px) {
					cycle_px += Hires::cycle_range(v) >= 0 ? 1 : 0;
				}
				txt << f.shape << ' ' << f.frame << ' ' << guard << ' ' << f.map_uses << ' ' << cycle_px << ' ' << f.used_on_map
					<< '\n';
			}
			write_file("flats.txt", txt.str());
			write_file(
					"templates/pack.txt", string("game=") + game_label() + "\nscale=" + std::to_string(template_scale)
												  + "\npalette_crc32=" + hex32(pal_crc)
												  + "\nedge=none\nroute=identity\ntitle=identity templates from --dump-art "
													"(NN of the 1x flats; never commit; copy them out of the dump "
													"before editing)\n");
		}

		void write_terrains() {
			if (!make_dir("terrain")) {
				return;
			}
			std::ostringstream txt;
			txt << "# tnum t1 uses own_cells rle_cells missing_cells duplicate_of same_layer\n"
				<< "# uses: map chunks (all maps); missing_cells: cells that get no flat (0 in the layer);\n"
				<< "# duplicate_of: the terrain with the same T1 key whose layer is terrain/<t1>.png, or -;\n"
				<< "# same_layer: 1 when this terrain's 1x layer equals that one, 0 when the fill differs, or -\n";
			string tiles_bin = bin_header(1, 8 * tiles, static_cast<uint32_t>(terrains.size()));
			for (size_t tnum = 0; tnum < terrains.size(); tnum++) {
				const Terrain& t       = terrains[tnum];
				int            own     = 0;
				int            rle     = 0;
				int            missing = 0;
				for (int i = 0; i < tiles; i++) {
					own += Is_flat(t.kind[i]) ? 1 : 0;
					rle += t.kind[i] == Tile_kind::Rle ? 1 : 0;
					missing += t.src[i] < 0 ? 1 : 0;
					put_le(tiles_bin, t.own_shape[i], 2);
					put_le(tiles_bin, t.own_frame[i], 1);
					put_le(tiles_bin, static_cast<uint64_t>(t.kind[i]), 1);
				}
				for (int i = 0; i < tiles; i++) {
					if (t.src[i] < 0) {
						put_le(tiles_bin, 0xffff, 2);
						put_le(tiles_bin, 0xff, 1);
						put_le(tiles_bin, 0xff, 1);
					} else {
						put_le(tiles_bin, t.own_shape[t.src[i]], 2);
						put_le(tiles_bin, t.own_frame[t.src[i]] & 31U, 1);
						put_le(tiles_bin, static_cast<uint64_t>(t.src[i]), 1);
					}
				}
				const string key = hex64(t.t1);
				txt << tnum << ' ' << key << ' ' << t.uses << ' ' << own << ' ' << rle << ' ' << missing << ' ';
				if (t.canon == static_cast<int>(tnum)) {
					txt << "- -\n";
					if (!write_png(
								"terrain/" + key + ".png", layer_side, layer_side, t.layer.data(),
								{
										{"Exult-Terrain-Key", key}
                    })) {
						return;
					}
				} else {
					txt << t.canon << ' ' << (t.layer == terrains[static_cast<size_t>(t.canon)].layer ? 1 : 0) << '\n';
				}
			}
			write_file("terrain.txt", txt.str());
			write_file("terrain_tiles.bin", tiles_bin);
			const uint32_t record  = 4 + 2 * c_num_chunks * c_num_chunks;
			string         map_bin = bin_header(2, record, static_cast<uint32_t>(maps.size()));
			for (size_t m = 0; m < maps.size(); m++) {
				put_le(map_bin, static_cast<uint64_t>(map_nums[m]), 4);
				for (const uint16_t tnum : maps[m]) {
					put_le(map_bin, tnum, 2);
				}
			}
			write_file("terrain_map.bin", map_bin);
		}

		// The number of distinct T1 keys (one canonical terrain each).
		size_t num_keys() const {
			size_t keys = 0;
			for (size_t tnum = 0; tnum < terrains.size(); tnum++) {
				keys += terrains[tnum].canon == static_cast<int>(tnum) ? 1 : 0;
			}
			return keys;
		}

		// manifest.txt: every file written so far (all but ref.txt), with its
		// size and CRC, read back from the disk.
		void write_manifest() {
			std::ostringstream txt;
			txt << "# crc32 size path of every file of this dump but ref.txt and this one. A new --dump-art into\n"
				<< "# this directory refuses to run while its entries hold a file not listed here or a changed one.\n";
			for (const string& rel : written) {
				uint64_t size = 0;
				uint32_t crc  = 0;
				if (!file_crc(file(rel), size, crc)) {
					fail("cannot read back " + file(rel).string());
					return;
				}
				txt << hex32(crc) << ' ' << size << ' ' << rel << '\n';
			}
			write_file(manifest_name, txt.str());
		}

		void write_ref() {
			size_t used = 0;
			for (const auto& t : terrains) {
				used += t.uses > 0 ? 1 : 0;
			}
			size_t flats_used = 0;
			for (const auto& f : flats) {
				flats_used += f.used_on_map > 0 ? 1 : 0;
			}
			string maps_list;
			for (const int m : map_nums) {
				maps_list += (maps_list.empty() ? "" : ",") + std::to_string(m);
			}
			const std::string_view rev = VersionGetGitRevision(false);
			std::ostringstream     ref;
			ref << "# Exult --dump-art reference set (hi-res DESIGN.md section 4.2). Derived from the game: never commit.\n"
				<< "# A new dump into this directory replaces it: copy templates/ out before editing (manifest.txt).\n"
				<< "format=" << format_id << '\n'
				<< "game=" << game_label() << '\n'
				<< "game_variant=" << game_variant() << '\n'
				<< "game_title=" << Game::get_gametitle() << '\n'
				<< "mod=" << Game::get_modtitle() << '\n'
				<< "engine_version=" << VERSION << '\n'
				<< "engine_rev=" << (rev.empty() ? string("unknown") : string(rev)) << '\n'
				<< "palette_crc32=" << hex32(pal_crc) << '\n'
				<< "png=raw-index,no-rotation\n"
				<< "terrain_key=T1\n"
				<< "crc=C1\n"
				<< "template_scale=" << template_scale << '\n'
				<< "shapes=" << Shape_manager::get_instance()->get_shapes().get_num_shapes() << '\n'
				<< "flats=" << flats.size() << '\n'
				<< "flats_used_on_map=" << flats_used << '\n'
				<< "terrains=" << terrains.size() << '\n'
				<< "terrains_used=" << used << '\n'
				<< "terrain_keys=" << num_keys() << '\n'
				<< "maps=" << maps_list << '\n'
				<< "bad_terrain_refs=" << bad_terrain_refs << '\n'
				<< "patch_shapes=" << patched(PATCH_SHAPES) << '\n'
				<< "patch_palettes=" << patched(PATCH_PALETTES) << '\n'
				<< "patch_u7chunks=" << patched(PATCH_U7CHUNKS) << '\n'
				<< "patch_u7map=" << patched(PATCH_U7MAP) << '\n';
			write_file(ref_name, ref.str());
		}

	public:
		explicit Dumper(const string& dir) : out(fs::u8path(dir)) {}

		int run(BaseGameInfo* game) {
			Image_window8::set_gamma(1, 1, 1);
			Image_window::set_render_scale_override("off");    // The dump is 1x.
			auto* gwin = new Game_window(
					window_w, window_h, false, window_w, window_h, 1, Image_window::point, Image_window::Fit, Image_window::point);
			Audio::Init();
			Game::create_game(game);
			gwin->init_files(false);    // init, but don't show plasma
			gwin->get_map()->init();
			gwin->set_map(0);
			gwin->get_pal()->set(0);
			if (!prepare_output()) {
				return 2;
			}
			try {
				pal = Hires::effective_palette0();
			} catch (const std::exception& e) {
				fail(e.what());
				return 1;
			}
			pal_crc = Hires::palette_crc32(pal);
			gather_flats();
			gather_maps(gwin);
			gather_terrains(gwin);
			count_uses();
			// ref.txt last: a dump without it is incomplete.
			write_palette();
			if (!failed) {
				write_flats();
			}
			if (!failed) {
				write_terrains();
			}
			if (!failed) {
				write_manifest();
			}
			if (!failed) {
				write_ref();
			}
			if (failed) {
				return 1;
			}
			cout << "--dump-art: " << flats.size() << " flats, " << terrains.size() << " terrains (" << num_keys() << " T1 keys), "
				 << maps.size() << " map(s), palette_crc32 " << hex32(pal_crc) << " -> " << out.string() << endl;
			return 0;
		}
	};
}    // namespace

int Dump_art(BaseGameInfo* game, const std::string& dir) {
	Dumper    dumper(dir);
	const int result = dumper.run(game);
	Audio::Destroy();
	return result;
}
