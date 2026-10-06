/*
 *  hires_dev.cc - The hi-res developer loop (DESIGN.md section 3.8).
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
 *  Enabled by config/video/hires/dev=yes. The three actions are cheat keys
 *  (keys.cc, data/<game>/defaultkeys.txt):
 *    HIRES_TOGGLE   Ctrl-Alt-O  overrides on/off at the same S (A/B)
 *    HIRES_RELOAD   Ctrl-Alt-R  rescan and revalidate every root
 *    HIRES_INSPECT  Ctrl-Alt-I  explain the tile under the mouse
 *  The caches follow Hires::generation(), so a toggle or a reload only needs
 *  a full repaint. Tools trigger a reload by touching <root>/x<S>/.reload
 *  (written last, after the files: they write *.tmp and rename), which the
 *  main loop polls every 500 ms. The reload is the only sync point: a file
 *  that is read half written is rejected and retried on the next reload.
 */

#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif

#include "hires_dev.h"

#include "chunks.h"
#include "chunkter.h"
#include "effects.h"
#include "exult_constants.h"
#include "flat_source.h"
#include "gamemap.h"
#include "gamewin.h"
#include "hires_rules.h"
#include "ignore_unused_variable_warning.h"
#include "iwin8.h"
#include "mouse.h"
#include "vgafile.h"

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <system_error>

#ifdef __GNUC__
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wold-style-cast"
#	pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#	if !defined(__llvm__) && !defined(__clang__)
#		pragma GCC diagnostic ignored "-Wuseless-cast"
#	endif
#endif    // __GNUC__
#include <SDL3/SDL.h>
#ifdef __GNUC__
#	pragma GCC diagnostic pop
#endif    // __GNUC__

namespace {
	constexpr uint32_t poll_interval_ms = 500;
	constexpr size_t   toast_max_chars  = 44;    // About a 320 px game area in font 0.

	const char* kind_name(Tile_kind kind) {
		switch (kind) {
		case Tile_kind::Flat:
			return "flat";
		case Tile_kind::Flat_void:
			return "flat_void";
		case Tile_kind::Rle:
			return "rle";
		case Tile_kind::None:
			break;
		}
		return "none";
	}

	// The kind of tile (tilex, tiley) of a terrain, as Chunk_terrain sees it
	// (loads the frame).
	Tile_kind tile_kind(Chunk_terrain* ter, int tilex, int tiley) {
		const Shape_frame* fr = ter->get_shape(tilex, tiley);
		if (fr == nullptr) {
			return Tile_kind::None;
		}
		if (fr->is_rle()) {
			return Tile_kind::Rle;
		}
		const ShapeID id = ter->get_flat(tilex, tiley);
		return id.get_shapenum() == 12 && id.get_framenum() == 0 ? Tile_kind::Flat_void : Tile_kind::Flat;
	}

	std::string hex64(uint64_t v) {
		char text[20];
		snprintf(text, sizeof(text), "%016" PRIx64, v);
		return text;
	}

	std::string shape_frame(int shape, int frame) {
		char text[24];
		snprintf(text, sizeof(text), "%04d:%02d", shape, frame);
		return text;
	}

	std::string json_string(const std::string& s) {
		std::string out = "\"";
		for (const char c : s) {
			if (c == '"' || c == '\\') {
				out += '\\';
				out += c;
			} else if (static_cast<unsigned char>(c) < 0x20) {
				char esc[8];
				snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
				out += esc;
			} else {
				out += c;
			}
		}
		return out + "\"";
	}

	std::string json_explanation(const Hires::Explanation& x) {
		std::ostringstream o;
		o << "{\"result\": " << json_string(x.result) << ", \"reason\": " << json_string(x.reason)
		  << ", \"where\": " << json_string(x.where) << ", \"reduced\": " << (x.reduced ? "true" : "false")
		  << ", \"bundle\": " << (x.from_bundle ? "true" : "false") << "}";
		return o.str();
	}

	void show_toast(const std::string& text) {
		Game_window* gwin = Game_window::get_instance();
		if (gwin == nullptr || gwin->get_effects() == nullptr) {
			return;
		}
		std::string line = text;
		if (line.size() > toast_max_chars) {
			line = line.substr(0, toast_max_chars - 3) + "...";
		}
		gwin->get_effects()->center_text(line.c_str());
	}

	int world_scale() {
		Game_window* gwin = Game_window::get_instance();
		return gwin != nullptr && gwin->get_win() != nullptr ? gwin->get_win()->get_world_scale() : 1;
	}

	// Gates the keys; tells why when dev mode is off.
	bool dev_keys_on() {
		if (Hires::dev_mode()) {
			return true;
		}
		std::cout << "[hires] the hi-res dev keys need config/video/hires/dev=yes" << std::endl;
		show_toast("Hi-res dev mode is off");
		return false;
	}

	// The modification times the .reload poll saw last.
	struct Reload_watch {
		struct Stamp {
			bool                            exists = false;
			std::filesystem::file_time_type time{};
		};

		std::map<std::string, Stamp> seen;
		uint32_t                     last   = 0;
		bool                         primed = false;
	};

	Reload_watch reload_watch;
}    // namespace

namespace Hires {
	Inspection explain_at(int tx, int ty, int scale) {
		Inspection r;
		r.tx      = ((tx % c_num_tiles) + c_num_tiles) % c_num_tiles;
		r.ty      = ((ty % c_num_tiles) + c_num_tiles) % c_num_tiles;
		r.cx      = r.tx / c_tiles_per_chunk;
		r.cy      = r.ty / c_tiles_per_chunk;
		r.cell_x  = r.tx % c_tiles_per_chunk;
		r.cell_y  = r.ty % c_tiles_per_chunk;
		r.scale   = scale;
		r.enabled = is_enabled();

		// The chunk's terrain when the chunk is in memory, else the map's
		// terrain number (what a paint would read in); creates no chunk.
		Game_window* gwin = Game_window::get_instance();
		Game_map*    map  = gwin != nullptr ? gwin->get_map() : nullptr;
		if (map == nullptr) {
			r.result = "none (no map)";
			return r;
		}
		r.map     = map->get_num();
		r.terrain = map->get_terrain_num(r.cx, r.cy);

		Map_chunk*     chunk = map->get_chunk_unsafe(r.cx, r.cy);
		Chunk_terrain* ter   = chunk != nullptr ? chunk->get_terrain() : nullptr;
		if (ter == nullptr && r.terrain >= 0 && r.terrain < map->get_num_chunk_terrains()) {
			ter = Game_map::get_terrain(r.terrain);
		}
		if (ter == nullptr) {
			r.result = "none (no terrain)";
			return r;
		}

		// The T1 key over the terrain's own flats (as --dump-art computes it).
		const uint8_t* own[terrain_tiles];
		for (int t = 0; t < terrain_tiles; t++) {
			const int       x    = t % c_tiles_per_chunk;
			const int       y    = t / c_tiles_per_chunk;
			const Tile_kind kind = tile_kind(ter, x, y);
			own[t]               = Is_flat(kind) ? ter->get_shape(x, y)->get_data() : nullptr;
			if (x == r.cell_x && y == r.cell_y) {
				const ShapeID id = ter->get_flat(x, y);
				r.own_shape      = id.get_shapenum();
				r.own_frame      = id.get_framenum();
				r.own_kind       = kind_name(kind);
			}
		}
		r.t1 = terrain_key_t1(own);

		r.src_cell = ter->get_flat_source(r.cell_x, r.cell_y);
		if (r.src_cell >= 0) {
			const ShapeID src = ter->get_flat(r.src_cell % c_tiles_per_chunk, r.src_cell / c_tiles_per_chunk);
			r.src_shape       = src.get_shapenum();
			r.src_frame       = src.get_framenum() & 31;
		}
		r.terrain_override = explain_terrain(r.t1, scale);
		if (r.src_cell < 0) {
			r.result = std::string("none (no flat source: ") + r.own_kind + ")";
			return r;
		}
		r.tile_override = explain_flat(r.src_shape, r.src_frame, scale);
		r.result        = r.tile_override.text();
		return r;
	}

	std::string Inspection::json(const std::string& indent) const {
		std::ostringstream o;
		const std::string  in = indent + "  ";
		o << indent << "{\n";
		o << in << "\"tile\": [" << tx << ", " << ty << "],\n";
		o << in << "\"map\": " << map << ",\n";
		o << in << "\"chunk\": [" << cx << ", " << cy << "],\n";
		o << in << "\"cell\": [" << cell_x << ", " << cell_y << "],\n";
		o << in << "\"terrain\": " << terrain << ",\n";
		o << in << "\"t1\": " << json_string(hex64(t1)) << ",\n";
		o << in << "\"own\": {\"shape\": " << own_shape << ", \"frame\": " << own_frame << ", \"kind\": " << json_string(own_kind)
		  << "},\n";
		o << in << "\"source\": ";
		if (src_cell < 0) {
			o << "null,\n";
		} else {
			o << "{\"cell\": [" << src_cell % c_tiles_per_chunk << ", " << src_cell / c_tiles_per_chunk
			  << "], \"shape\": " << src_shape << ", \"frame\": " << src_frame << "},\n";
		}
		o << in << "\"scale\": " << scale << ",\n";
		o << in << "\"overrides\": " << (enabled ? "true" : "false") << ",\n";
		o << in << "\"terrain_override\": " << json_explanation(terrain_override) << ",\n";
		o << in << "\"tile_override\": ";
		o << (src_cell < 0 ? std::string("null") : json_explanation(tile_override)) << ",\n";
		o << in << "\"result\": " << json_string(result) << "\n";
		o << indent << "}";
		return o.str();
	}

	std::string Inspection::text() const {
		std::ostringstream o;
		o << "tile " << tx << "," << ty << " map " << map << " chunk " << cx << "," << cy << " cell " << cell_x << "," << cell_y
		  << " x" << scale << (enabled ? "" : " (overrides off)") << '\n';
		if (terrain_override.result.empty()) {
			o << "terrain " << terrain << '\n';
		} else {
			o << "terrain " << terrain << " T1 " << hex64(t1) << ": " << terrain_override.text()
			  << (terrain_override.result == "TERRAIN" ? " [not painted before WP-17]" : "") << '\n';
		}
		o << "own " << shape_frame(own_shape, own_frame) << " " << own_kind << ", source ";
		if (src_cell < 0) {
			o << "none";
		} else {
			o << "cell " << src_cell % c_tiles_per_chunk << "," << src_cell / c_tiles_per_chunk << " "
			  << shape_frame(src_shape, src_frame);
		}
		o << '\n' << "result " << result << '\n';
		if (!tile_override.path.empty()) {
			o << "file " << tile_override.path << '\n';
		}
		return o.str();
	}

	std::string Inspection::toast() const {
		std::string s = std::to_string(tx) + "," + std::to_string(ty) + " ";
		if (src_cell >= 0) {
			s += shape_frame(src_shape, src_frame) + " ";
		}
		if (src_cell < 0) {
			s += "none";
		} else if (tile_override.result == "NN") {
			s += "NN: " + tile_override.reason;
		} else {
			s += tile_override.result;
			if (tile_override.reduced) {
				s += " (reduced)";
			}
		}
		return s;
	}

	bool dev_toggle() {
		set_enabled(!is_enabled());
		return is_enabled();
	}

	std::string dev_reload(int scale) {
		reload(scale);    // The store logs the report of the load.
		const std::string tag = "x" + std::to_string(scale) + ": ";
		if (scale < 2) {
			return "reloaded (world scale 1)";
		}
		const Report* rep = report(scale);
		if (rep == nullptr || rep->failed) {
			return tag + "load failed (see log)";
		}
		if (rep->roots == 0) {
			return tag + "no override roots";
		}
		std::string s = tag + std::to_string(rep->loaded) + " loaded, " + std::to_string(rep->rejected) + " rejected";
		if (rep->rejected > 0 || rep->warnings > 0) {
			s += " (see log)";
		}
		return s;
	}

	std::vector<std::string> reload_triggers(int scale) {
		std::vector<std::string> out;
		std::vector<int>         scales{art_scale()};
		if (scale > 1 && scale != scales[0]) {
			scales.push_back(scale);
		}
		for (const auto& root : roots()) {
			for (const int s : scales) {
				out.push_back(root.sys_path + "/x" + std::to_string(s) + "/.reload");
			}
		}
		return out;
	}

	bool dev_poll(uint32_t ticks, int scale, std::string* summary) {
		if (!dev_mode()) {
			reload_watch = Reload_watch();    // Starts again from a baseline.
			return false;
		}
		Reload_watch& w = reload_watch;
		if (w.primed && ticks - w.last < poll_interval_ms) {
			return false;
		}
		w.last       = ticks;
		bool changed = false;
		for (const auto& file : reload_triggers(scale)) {
			Reload_watch::Stamp now;
			std::error_code     ec;
			now.time   = std::filesystem::last_write_time(file, ec);
			now.exists = !ec;
			if (!now.exists) {
				now.time = {};
			}
			auto it = w.seen.find(file);
			if (it == w.seen.end()) {
				w.seen.emplace(file, now);    // A new root or scale: a baseline only.
				continue;
			}
			if (it->second.exists != now.exists || it->second.time != now.time) {
				changed    = true;
				it->second = now;
			}
		}
		const bool first = !w.primed;
		w.primed         = true;
		if (first || !changed) {
			return false;
		}
		std::cout << "[hires] .reload changed: reloading" << std::endl;
		const std::string text = dev_reload(scale);
		if (summary != nullptr) {
			*summary = text;
		}
		return true;
	}
}    // namespace Hires

void ActionHiresToggle(const int* params) {
	ignore_unused_variable_warning(params);
	if (!dev_keys_on()) {
		return;
	}
	const bool        on   = Hires::dev_toggle();
	const std::string text = std::string("Hi-res overrides ") + (on ? "on" : "off") + " (x" + std::to_string(world_scale()) + ")";
	std::cout << "[hires] " << text << std::endl;
	Game_window::get_instance()->set_all_dirty();
	show_toast(text);
}

void ActionHiresReload(const int* params) {
	ignore_unused_variable_warning(params);
	if (!dev_keys_on()) {
		return;
	}
	const std::string text = "Hi-res " + Hires::dev_reload(world_scale());
	std::cout << "[hires] reload: " << text << std::endl;
	Game_window::get_instance()->set_all_dirty();
	show_toast(text);
}

void ActionHiresInspect(const int* params) {
	ignore_unused_variable_warning(params);
	if (!dev_keys_on()) {
		return;
	}
	Game_window* gwin = Game_window::get_instance();
	const int    x    = Mouse::mouse()->get_mousex();
	const int    y    = Mouse::mouse()->get_mousey();
	const auto   info
			= Hires::explain_at(gwin->get_scrolltx() + x / c_tilesize, gwin->get_scrollty() + y / c_tilesize, world_scale());
	const std::string  text = info.text();
	std::istringstream lines(text);
	std::string        line;
	while (std::getline(lines, line)) {
		std::cout << "[hires] inspect: " << line << std::endl;
	}
	if (!SDL_SetClipboardText(text.c_str())) {
		std::cout << "[hires] inspect: clipboard: " << SDL_GetError() << std::endl;
	}
	show_toast(info.toast());
}

void Hires_dev_poll(uint32_t ticks) {
	std::string text;
	if (Hires::dev_poll(ticks, world_scale(), &text)) {
		std::cout << "[hires] reload: Hi-res " << text << std::endl;
		Game_window::get_instance()->set_all_dirty();
		show_toast("Hi-res " + text);
	}
}
