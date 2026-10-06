/*
 *  hires_dev.h - The hi-res developer loop (DESIGN.md section 3.8): the
 *  HIRES_TOGGLE, HIRES_RELOAD and HIRES_INSPECT key actions, the .reload
 *  trigger poll and the tile inspector (explain_at).
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

#ifndef HIRES_DEV_H
#define HIRES_DEV_H

#include "hires_glue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Hires {
	/*
	 *  What the engine paints at one tile of the current map at one render
	 *  scale, and why (the inspector). Precedence today is tile -> NN; the
	 *  terrain override is reported but not painted before WP-17.
	 */
	struct Inspection {
		int         tx = 0, ty = 0;    // World tile (wrapped).
		int         map = 0;
		int         cx = 0, cy = 0;            // Chunk.
		int         cell_x = 0, cell_y = 0;    // Tile within the chunk.
		int         terrain   = -1;            // Terrain number, -1: none.
		uint64_t    t1        = 0;             // T1 key of the terrain.
		int         own_shape = -1, own_frame = -1;
		const char* own_kind = "none";    // none | flat | flat_void | rle.
		// The tile whose flat is painted here (find_flat_source), or -1.
		int         src_cell  = -1;
		int         src_shape = -1, src_frame = -1;    // frame & 31.
		int         scale   = 1;
		bool        enabled = false;    // Hires::is_enabled().
		Explanation terrain_override;
		Explanation tile_override;
		// The painted result: "TILE <where>", "NN (...)", or "none (no flat
		// source: <kind>)" when nothing is painted.
		std::string result;

		// JSON object (deterministic: no absolute paths, no timings);
		// 'indent' prefixes every line.
		std::string json(const std::string& indent) const;
		// Lines for stdout and the clipboard (absolute paths included).
		std::string text() const;
		// One short line for the screen.
		std::string toast() const;
	};

	// The inspector: tile (tx, ty) of the current map at render scale
	// 'scale'. Loads the terrain's frames, and the store of 'scale'.
	Inspection explain_at(int tx, int ty, int scale);

	/*
	 *  The developer actions without the screen: what the keys do, minus
	 *  the toast and the repaint (the render test calls them).
	 */
	// Flips the A/B toggle; returns the new state.
	bool dev_toggle();
	// Hires::reload(scale) with a short summary for the screen
	// ("x6: 3880 loaded, 5 rejected (see log)").
	std::string dev_reload(int scale);
	// The files the .reload poll watches: <root>/x<art_scale>/.reload and,
	// when the render scale is another S > 1, <root>/x<scale>/.reload.
	std::vector<std::string> reload_triggers(int scale);

	/*
	 *  The .reload poll: at most every 500 ms (ticks in ms), compares the
	 *  modification time of the trigger files with the last poll; a change
	 *  (creation and removal included) calls dev_reload(scale) and returns
	 *  true with the summary in 'summary'. The first poll only records the
	 *  times. Does nothing unless dev_mode().
	 */
	bool dev_poll(uint32_t ticks, int scale, std::string* summary = nullptr);
}    // namespace Hires

// Key actions (keys.cc): HIRES_TOGGLE (Ctrl-Alt-O), HIRES_RELOAD
// (Ctrl-Alt-R), HIRES_INSPECT (Ctrl-Alt-I); dev mode only.
void ActionHiresToggle(const int* params);
void ActionHiresReload(const int* params);
void ActionHiresInspect(const int* params);

// Main loop hook (exult.cc): the .reload poll at the window's world scale,
// with a repaint and a toast when it reloads.
void Hires_dev_poll(uint32_t ticks);

#endif
