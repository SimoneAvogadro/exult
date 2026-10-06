/*
 *  hires_glue.cc - Engine side of the hi-res override store.
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

#include "hires_glue.h"

#include "Configuration.h"
#include "fnames.h"
#include "gamewin.h"
#include "hires_vga.h"
#include "palette.h"
#include "shapeid.h"
#include "utils.h"

#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace {
	struct Glue_config {
		bool dev       = false;
		int  art_scale = 6;
	};

	Glue_config glue_config;

	void log_line(const std::string& text) {
		std::cout << "[hires] " << text << std::endl;
	}

	void print_report(int scale, const Hires::Report& rep) {
		if (rep.roots == 0) {
			log_line("x" + std::to_string(scale) + ": no override roots (<PATCH>/hires, <HIRES>)");
			return;
		}
		log_line(rep.summary(scale));
		const size_t limit = glue_config.dev ? rep.findings.size() : 50;
		size_t       shown = 0;
		size_t       left  = 0;
		for (const auto& f : rep.findings) {
			if (f.severity == Hires::Severity::info && !glue_config.dev) {
				continue;
			}
			if (shown < limit) {
				log_line(f.format());
				shown++;
			} else {
				left++;
			}
		}
		if (left > 0) {
			log_line("... " + std::to_string(left) + " more (config/video/hires/dev=yes shows all)");
		}
	}

	Hires::Store_set::Inputs gather_inputs() {
		Hires::Store_set::Inputs in;
		in.roots = Hires::roots();
		in.pal   = Hires::effective_palette0();
		in.src   = Hires::source_flat;
		in.s_art = glue_config.art_scale;
		return in;
	}

	// config/video/hires/{dev,art_scale}; 'overrides' only on the first read.
	void read_config(Hires::Store_set& set, bool first) {
		if (config == nullptr) {
			return;
		}
		std::string value;
		if (first) {
			config->value("config/video/hires/overrides", value, "yes");
			set.set_enabled(value != "no");
		}
		config->value("config/video/hires/dev", value, "no");
		glue_config.dev = value == "yes";
		int art         = 6;
		config->value("config/video/hires/art_scale", art, 6);
		if (art < 1 || art > 16) {
			log_line("config/video/hires/art_scale " + std::to_string(art) + " is out of range 1-16, using 6");
			art = 6;
		}
		glue_config.art_scale = art;
	}

	Hires::Store_set& stores() {
		static Hires::Store_set set(gather_inputs, print_report, log_line);
		static bool             configured = false;
		if (!configured) {
			configured = true;
			read_config(set, true);
		}
		return set;
	}

	std::string entry_text(const Hires::Entry_info* e, const char* kind) {
		if (e == nullptr) {
			return "NN (no override)";
		}
		if (e->state == Hires::Entry_state::rejected) {
			return std::string("NN (rejected: ") + Hires::rule_name(e->rule) + " " + e->detail + ") " + e->path;
		}
		std::string s = std::string(kind) + " " + e->path;
		if (e->reduced) {
			s += " (reduced from x" + std::to_string(glue_config.art_scale) + ")";
		}
		if (e->rule == Hires::Rule::p4_cycling) {
			s += " (P4 reject on the last decode: " + e->detail + ")";
		}
		return s;
	}

	std::string explain(int scale, const std::function<std::string(const Hires::Store&)>& what) {
		Hires::Store_set& set = stores();
		if (scale < 2) {
			return "NN (scale 1)";
		}
		if (!set.enabled()) {
			return "NN (overrides disabled)";
		}
		const Hires::Store* store = set.store(scale);
		return store != nullptr ? what(*store) : "NN (overrides failed at this scale, see the log)";
	}
}    // namespace

namespace Hires {
	Tile_view flat(int shape, int frame, int scale) {
		return stores().flat(shape, frame, scale);
	}

	bool terrain(uint64_t key, int scale, const uint8_t* layer1x, uint8_t* dst, int dst_w, int dst_h, int dst_pitch) {
		return stores().terrain(key, scale, layer1x, dst, dst_w, dst_h, dst_pitch);
	}

	uint32_t generation() {
		return stores().generation();
	}

	void invalidate() {
		stores().invalidate();
	}

	std::string reload(int scale) {
		Store_set& set = stores();
		set.invalidate();
		read_config(set, false);
		if (scale < 2) {
			return std::string();
		}
		if (set.store(scale) == nullptr) {
			return "x" + std::to_string(scale) + ": overrides disabled (see the log)";
		}
		return set.report(scale)->summary(scale);
	}

	void set_enabled(bool enabled) {
		stores().set_enabled(enabled);
	}

	bool is_enabled() {
		return stores().enabled();
	}

	std::vector<Root> roots() {
		std::vector<Root> out;

		const auto add = [&out](const char* tag, const char* sub, const char* label) {
			if (!is_system_path_defined(tag)) {
				return;
			}
			const std::string path = get_system_path(std::string(tag) + sub);
			std::error_code   ec;
			if (!std::filesystem::is_directory(path, ec)) {
				return;
			}
			for (const auto& root : out) {
				if (std::filesystem::equivalent(root.sys_path, path, ec)) {
					return;    // <HIRES> pointing at <PATCH>/hires: load it once.
				}
			}
			out.push_back({path, label});
		};
		add("<PATCH>", "/hires", "<PATCH>/hires");
		add("<HIRES>", "", "<HIRES>");
		return out;
	}

	Pal8 effective_palette0() {
		if (Game_window::get_instance() == nullptr) {
			throw std::runtime_error("palette 0 is not available before the game window exists");
		}
		Palette pal;
		pal.load(PALETTES_FLX, PATCH_PALETTES, 0);
		uint8_t rgb6[768];
		bool    any = false;
		for (int i = 0; i < 256; i++) {
			rgb6[3 * i]     = pal.get_red(i);
			rgb6[3 * i + 1] = pal.get_green(i);
			rgb6[3 * i + 2] = pal.get_blue(i);
			any             = any || rgb6[3 * i] != 0 || rgb6[3 * i + 1] != 0 || rgb6[3 * i + 2] != 0;
		}
		if (!any) {
			// Palette::load leaves the palette black when it cannot read it.
			throw std::runtime_error("palette 0 could not be read from " PALETTES_FLX);
		}
		return pal8_from_6bit(rgb6);
	}

	const uint8_t* source_flat(int shape, int frame) {
		Shape_manager* sman = Shape_manager::get_instance();
		return sman != nullptr ? flat_from_vga(sman->get_shapes(), shape, frame) : nullptr;
	}

	std::string explain_flat(int shape, int frame, int scale) {
		return explain(scale, [shape, frame](const Store& store) {
			return entry_text(store.explain_flat(shape, frame & 31), "TILE");
		});
	}

	std::string explain_terrain(uint64_t key, int scale) {
		return explain(scale, [key](const Store& store) {
			return entry_text(store.explain_terrain(key), "TERRAIN");
		});
	}
}    // namespace Hires
