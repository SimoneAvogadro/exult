/*
 *  hires_store.h - The hi-res override store (DESIGN.md sections 3.5 and 5):
 *  loads, validates and serves terrain-flat overrides from pack roots.
 *
 *  Pack layout (section 5.1), per root:
 *    <root>/pack.txt              optional manifest; the engine reads palette_crc32 (R1)
 *    <root>/x<S>/flats.bundle     generated art in one file, entries independent (B0)
 *    <root>/x<S>/flats/SSSS_FF.png        single-tile overrides
 *    <root>/x<S>/flats/<group>/SSSS_FF.png  strict groups (G2); "<name>.off" is disabled,
 *                                         directories "_*" and ".*" are ignored, as are
 *                                         non-PNG files and nested directories
 *    <root>/x<S>/terrain/<t1>.png         per-terrain overrides, indexed at load and
 *                                         validated when first decoded
 *  Every *.png file in these folders is a candidate, as in hirescheck.py: a
 *  misnamed one (a hidden "._*" file too) is an N1 reject, which in a group
 *  skips the group.
 *  When <root>/x<S> does not exist and S divides S_art, x<S_art> is used and
 *  reduced at load with the class-preserving mode filter (section 5.6).
 *
 *  Precedence of a key: the first root that has an accepted candidate (roots
 *  come in precedence order: <PATCH>/hires, then <HIRES>); inside a root a loose
 *  file beats the bundle entry, and of two loose files with the same key the
 *  later one (top-level files first, then groups, in name order) wins with a
 *  warning, as in hirescheck.py. Bundle entries may come in any order; of two
 *  with the same key the later accepted one wins.
 *
 *  Per-file rule order (the first reject decides, as in hirescheck.py):
 *  N1, F3, F1, F2, F4 (warning), P0, P4, G1. Unreadable files are F1.
 *
 *  Loading is eager, synchronous and deterministic. Store::load and
 *  Store::terrain catch std::bad_alloc and every other exception, and fail
 *  at the smallest unit (invariant I11): a file or bundle entry that cannot
 *  be checked is rejected alone (severity error), a bundle that cannot be
 *  read is disabled (B0), and any other failure disables its root. Nothing is
 *  thrown. Paths in findings and Entry_info are UTF-8 with '/' separators on
 *  every platform.
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

#ifndef HIRES_STORE_H
#define HIRES_STORE_H

#include "hires_rules.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hires {
	// A pack root: a system path (tags already expanded) and a label for messages.
	struct Root {
		std::string sys_path;
		std::string label;
	};

	// The effective 1x flat of (shape, frame): 64 bytes, or nullptr when the
	// key is out of range, the frame is missing or the shape is RLE.
	using Src_provider = std::function<const uint8_t*(int shape, int frame)>;

	// A served tile: side x side indices (side = 8 * S of the store that produced it).
	struct Tile_view {
		const uint8_t* px   = nullptr;
		int            side = 0;
	};

	// Rule IDs of section 5.5, shared with hirescheck.py; rule_name() gives the
	// ID ("N1"). The names are lower case, so that no system macro can clash with
	// them (<termios.h> defines B0).
	enum class Rule : uint8_t {
		none,
		n1_name,       // N1: name, key range, the key is a flat
		f1_colour,     // F1: colour type 3 (and readable)
		f2_plte,       // F2: PLTE of every used index
		f3_size,       // F3: exact size
		f4_trns,       // F4: tRNS present (warning)
		p0_border,     // P0: index 0xFF
		p4_cycling,    // P4: cycling indices outside the parent's range
		g1_guard,      // G1: source guard (stale or malformed)
		g2_group,      // G2: strict group skipped
		r1_palette,    // R1: pack.txt palette_crc32
		b0_bundle      // B0: bundle header or size
	};

	enum class Severity : uint8_t {
		info,
		warning,
		reject,
		group_skipped,
		root_disabled,
		bundle_disabled,
		error    // The engine failed (out of memory, I/O); not a rule of the art.
	};

	const char* rule_name(Rule rule);
	const char* severity_name(Severity severity);

	struct Finding {
		Rule        rule     = Rule::none;
		Severity    severity = Severity::info;
		std::string path;    // File, "<bundle>#SSSS_FF", directory or pack.txt.
		std::string detail;
		int         shape = -1;
		int         frame = -1;

		// "[N1 reject] <path>: <detail>"
		std::string format() const;
	};

	struct Report {
		int                      tiles          = 0;    // Tile files and bundle entries examined.
		int                      loaded         = 0;    // Keys served by the store.
		int                      rejected       = 0;    // Files and entries rejected (group members included).
		int                      groups_skipped = 0;
		int                      unguarded      = 0;    // Served keys without a source guard.
		int                      warnings       = 0;
		int                      terrains       = 0;    // Terrain overrides indexed.
		int                      bundled        = 0;    // Served keys that come from a bundle.
		int                      roots          = 0;    // Roots examined.
		int                      roots_disabled = 0;
		std::vector<Finding>     findings;
		std::vector<std::string> lines;    // findings formatted, in order
		double                   ms = 0;
		// Store_set only: the load threw before Store::load could report
		// (latched until invalidate()); failure holds the message, if any.
		bool        failed = false;
		std::string failure;

		// One line: "x6: 3885 tiles loaded (bundle 3885), 0 rejected, ...",
		// or "x6: load failed: <failure>".
		std::string summary(int scale) const;
	};

	enum class Entry_state : uint8_t {
		loaded,      // Served.
		rejected,    // Rejected, or (terrain) rejected when decoded.
		indexed      // Terrain: found, not decoded yet (or decoded without a reject).
	};

	// What the store knows about a key (for the inspector): the served
	// candidate, or else the highest-precedence rejected one.
	struct Entry_info {
		Entry_state state = Entry_state::rejected;
		std::string path;
		Rule        rule = Rule::none;    // Rejecting rule.
		std::string detail;
		bool        from_bundle = false;
		bool        guarded     = false;
		bool        reduced     = false;    // Reduced from x<S_art>.
		int         root        = -1;       // Index into the roots passed to load().
	};

	/*
	 *  The result of the per-file rules. rejected() is false when the file is
	 *  accepted; 'warnings' holds F4 and P4 warnings either way, 'pixels' the
	 *  decoded indices of an accepted file (side x side at the file's scale;
	 *  Store::load reduces them to the store's scale).
	 */
	struct File_check {
		Rule                 rule  = Rule::none;
		bool                 error = false;    // The engine failed (I/O, memory): rejected, but by no rule.
		std::string          detail;
		std::vector<Finding> warnings;
		int                  shape   = -1;
		int                  frame   = -1;
		bool                 guarded = false;
		std::vector<uint8_t> pixels;

		bool rejected() const {
			return rule != Rule::none || error;
		}
	};

	// The tile rules for a loose file at 'scale' (the file's own x<scale> folder).
	File_check check_tile_file(const std::filesystem::path& file, int scale, const Pal8& pal, const Src_provider& src);
	// The tile rules for a bundle entry (N1, P0, P4, G1; the header covers F1-F3).
	// 'where' names the entry in findings ("<bundle>#SSSS_FF").
	File_check check_tile_pixels(
			const uint8_t* pixels, int shape, int frame, int scale, bool guarded, uint32_t guard, const Src_provider& src,
			const std::string& where);
	// The terrain rules at decode time: N1 (name), F3, F1, F2, F4, P0, P4 against
	// the terrain's 128x128 1x flat layer, and the optional Exult-Terrain-Key text
	// (G1 when it differs from the name).
	File_check check_terrain_file(const std::filesystem::path& file, int scale, const Pal8& pal, const uint8_t* layer1x);

	// Parses "SSSS_FF.png" (frame not range-checked) and "<16 lowercase hex>.png".
	bool parse_tile_name(const std::string& name, int& shape, int& frame);
	bool parse_terrain_name(const std::string& name, uint64_t& key);
	// "0123_04.png", "9a1c0e44b2d6f001.png".
	std::string tile_name(int shape, int frame);
	std::string terrain_name(uint64_t key);

	// Parses "c9c2c0e7" or "0xC9C2C0E7" (1-8 hex digits, surrounding blanks ignored).
	bool parse_crc_text(const std::string& text, uint32_t& value);

	class Store {
	public:
		/*
		 *  Loads every root (precedence order) for render scale 'scale', with
		 *  the art scale s_art for reduction, the effective palette 0 and the
		 *  live 1x flats. Replaces the previous contents. Never throws.
		 */
		Report load(const std::vector<Root>& roots, int scale, int s_art, const Pal8& pal, const Src_provider& src);

		// The tile for (shape, frame & 31): {8S x 8S indices, 8S}, or {}.
		Tile_view flat(int shape, int frame) const;

		/*
		 *  Decodes the terrain override of T1 key 'key' into dst (dst_w x dst_h
		 *  pixels, pitch dst_pitch bytes). layer1x is the terrain's 128x128 1x
		 *  flat layer, the parent for P4 and the reduction. Returns false, and
		 *  leaves dst untouched, unless the post-reduction size is exactly
		 *  dst_w x dst_h (= 128 S) and the file passes the rules. A reject is
		 *  latched (except P4, which depends on the layer) and reported once
		 *  through take_findings(). Never throws.
		 */
		bool terrain(uint64_t key, const uint8_t* layer1x, uint8_t* dst, int dst_w, int dst_h, int dst_pitch);

		const Entry_info* explain_flat(int shape, int frame) const;
		const Entry_info* explain_terrain(uint64_t key) const;

		// Findings of lazy terrain decodes since the last call.
		std::vector<Finding> take_findings();

		int get_scale() const {
			return scale;
		}

		size_t tile_count() const {
			return index.size();
		}

		size_t terrain_count() const {
			return terrain_index.size();
		}

		void clear();

	private:
		struct Terrain_entry {
			std::filesystem::path path;
			int                   src_scale = 0;
			Entry_info            info;
			bool                  logged = false;
		};

		int                                         scale = 0;
		Pal8                                        pal;
		std::vector<uint8_t>                        tiles;    // tile_count() tiles of (8 S)^2 bytes.
		std::unordered_map<uint32_t, uint32_t>      index;    // key (shape << 5 | frame) -> tile number.
		std::map<uint32_t, Entry_info>              info;
		std::unordered_map<uint64_t, Terrain_entry> terrain_index;
		std::vector<Finding>                        pending;
	};

	/*
	 *  One Store per render scale, loaded lazily, plus the state the engine
	 *  glue exposes (section 3.5): the enable toggle, the generation counter
	 *  and the fail-soft latch. Engine-free: the glue passes a function that
	 *  collects the inputs of a load (roots, palette, provider, S_art) when a
	 *  load happens, and a function that logs the reports.
	 */
	class Store_set {
	public:
		struct Inputs {
			std::vector<Root> roots;
			Pal8              pal;
			Src_provider      src;
			int               s_art = 6;
		};

		using Inputs_fn = std::function<Inputs()>;
		using Report_fn = std::function<void(int scale, const Report& report)>;
		using Text_fn   = std::function<void(const std::string& text)>;

		Store_set(Inputs_fn inputs, Report_fn on_report = nullptr, Text_fn on_text = nullptr);

		// Store::flat of the store for 'scale' ({} when disabled, scale < 2 or failed).
		Tile_view flat(int shape, int frame, int scale);
		// Store::terrain of the store for 'scale' (false when disabled, scale < 2 or failed).
		bool terrain(uint64_t key, int scale, const uint8_t* layer1x, uint8_t* dst, int dst_w, int dst_h, int dst_pitch);

		/*
		 *  The store for 'scale', loading it now if needed; nullptr for
		 *  scale < 2 or when its load failed (latched until invalidate()). An
		 *  exception from the inputs function or the load is caught here; one
		 *  from the report or text function only loses that message, so
		 *  store(), flat() and terrain() never throw. Ignores the enable toggle
		 *  (the inspector reports disabled keys too).
		 */
		Store* store(int scale);

		// The report of the last load of 'scale', or nullptr.
		const Report* report(int scale) const;

		// Drops every store (they reload lazily); generation() changes.
		void invalidate();

		// The toggle; generation() changes when the state changes.
		void set_enabled(bool on);

		bool enabled() const {
			return is_on;
		}

		// Changes whenever flat() or terrain() may answer differently.
		uint32_t generation() const {
			return gen;
		}

	private:
		struct Entry {
			std::unique_ptr<Store> store;
			Report                 report;
			bool                   failed = false;
		};

		Inputs_fn            get_inputs;
		Report_fn            report_fn;
		Text_fn              text_fn;
		std::map<int, Entry> stores;
		bool                 is_on = true;
		uint32_t             gen   = 1;
	};
}    // namespace Hires

#endif
