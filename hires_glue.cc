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

#include <algorithm>
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

	// path with '/' separators and without empty or "." elements (a leading
	// '/' is kept). The store writes '/' and builds its paths from the root's
	// system path as given, so they keep what get_system_path leaves: '\' on
	// Windows, the "." it appends there to a trailing separator, and a doubled
	// separator. Both sides of a comparison go through this.
	std::string lexical_path(const std::string& path) {
		std::string s = path;
		if constexpr (std::filesystem::path::preferred_separator != '/') {
			std::replace(s.begin(), s.end(), '\\', '/');
		}
		std::string out = !s.empty() && s[0] == '/' ? "/" : "";
		size_t      pos = 0;
		while (pos < s.size()) {
			size_t end = s.find('/', pos);
			if (end == std::string::npos) {
				end = s.size();
			}
			if (end > pos && !(end == pos + 1 && s[pos] == '.')) {
				if (!out.empty() && out.back() != '/') {
					out += '/';
				}
				out.append(s, pos, end - pos);
			}
			pos = end + 1;
		}
		return out;
	}

	// path as "<root label>/<path in the root>" when it lies in one of the
	// roots (the bundle entry "#SSSS_FF" suffix included), else path.
	std::string root_relative(const std::string& path) {
		const std::string file = lexical_path(path);
		for (const auto& root : Hires::roots()) {
			const std::string prefix = lexical_path(root.sys_path);
			if (!prefix.empty() && prefix != "/" && file.size() > prefix.size() && file.compare(0, prefix.size(), prefix) == 0
				&& file[prefix.size()] == '/') {
				return root.label + "/" + file.substr(prefix.size() + 1);
			}
		}
		return path;
	}

	Hires::Explanation entry_explanation(const Hires::Entry_info* e, const char* kind) {
		Hires::Explanation x;
		if (e == nullptr) {
			x.result = "NN";
			x.reason = "no override";
			return x;
		}
		x.path        = e->path;
		x.where       = root_relative(e->path);
		x.reduced     = e->reduced;
		x.from_bundle = e->from_bundle;
		if (e->state == Hires::Entry_state::rejected) {
			x.result = "NN";
			x.reason = std::string("rejected: ") + Hires::rule_name(e->rule);
			if (!e->detail.empty()) {
				x.reason += " " + e->detail;
			}
			return x;
		}
		x.result = kind;
		if (e->state == Hires::Entry_state::indexed) {
			x.reason = "not decoded yet";
		}
		if (e->rule == Hires::Rule::p4_cycling) {
			x.reason = "P4 reject on the last decode: " + e->detail;
		}
		return x;
	}

	Hires::Explanation explain(int scale, const std::function<Hires::Explanation(const Hires::Store&)>& what) {
		Hires::Store_set&  set = stores();
		Hires::Explanation x;
		x.result = "NN";
		if (scale < 2) {
			x.reason = "scale 1";
			return x;
		}
		if (!set.enabled()) {
			x.reason = "overrides disabled";
			return x;
		}
		const Hires::Store* store = set.store(scale);
		if (store == nullptr) {
			x.reason = "overrides failed at this scale, see the log";
			return x;
		}
		return what(*store);
	}
}    // namespace

namespace Hires {
	Tile_view flat(int shape, int frame, int scale) {
		return stores().flat(shape, frame, scale);
	}

	bool terrain(uint64_t key, int scale, const uint8_t* layer1x, uint8_t* dst, int dst_w, int dst_h, int dst_pitch) {
		return stores().terrain(key, scale, layer1x, dst, dst_w, dst_h, dst_pitch);
	}

	size_t terrain_count(int scale) {
		return stores().terrain_count(scale);
	}

	bool has_terrain(uint64_t key, int scale) {
		return stores().has_terrain(key, scale);
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

	const Report* report(int scale) {
		return stores().report(scale);
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

	bool dev_mode() {
		stores();    // Reads the configuration on the first call.
		return glue_config.dev;
	}

	int art_scale() {
		stores();
		return glue_config.art_scale;
	}

	std::string Explanation::text() const {
		if (result == "NN") {
			return "NN (" + reason + ")" + (where.empty() ? "" : " " + where);
		}
		std::string notes = reason;
		if (reduced) {
			notes += std::string(notes.empty() ? "" : "; ") + "reduced from x" + std::to_string(glue_config.art_scale);
		}
		return result + " " + where + (notes.empty() ? "" : " (" + notes + ")");
	}

	Explanation explain_flat(int shape, int frame, int scale) {
		return explain(scale, [shape, frame](const Store& store) {
			return entry_explanation(store.explain_flat(shape, frame & 31), "TILE");
		});
	}

	Explanation explain_terrain(uint64_t key, int scale) {
		return explain(scale, [key](const Store& store) {
			return entry_explanation(store.explain_terrain(key), "TERRAIN");
		});
	}
}    // namespace Hires
