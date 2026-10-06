/*
 *  hires_glue.h - Engine side of the hi-res override store (DESIGN.md
 *  section 3.5): configuration, pack roots from path tags, the effective
 *  palette 0, the source provider, and one lazily loaded Store per render
 *  scale.
 *
 *  Configuration (config/video/hires/..., section 4.1):
 *    overrides  yes | no   initial state of set_enabled (default yes)
 *    art_scale  1-16       S_art, the scale of the pack folders (default 6)
 *    dev        no | yes   developer loop (hires_dev.cc) and the verbose
 *                          [hires] log (all findings, info included)
 *  Roots, in precedence order: <PATCH>/hires, then <HIRES>
 *  (config/disk/game/<game>/hires_path, default $game_path/hires; not
 *  mod-specific), each only when the tag is defined and the folder exists.
 *
 *  Nothing here runs at S = 1: the paint path asks for a scale > 1 only.
 *  Messages go to stdout with the prefix "[hires]".
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

#ifndef HIRES_GLUE_H
#define HIRES_GLUE_H

#include "hires_store.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Hires {
	/*
	 *  The override for flat (shape, frame & 31) at render scale 'scale':
	 *  {(8 scale)^2 indices, 8 scale}, or {} (no override, overrides disabled,
	 *  scale < 2, or the store failed). The first call for a scale after
	 *  invalidate() or reload() loads that scale's store synchronously.
	 */
	Tile_view flat(int shape, int frame, int scale);

	/*
	 *  Decodes the terrain override of T1 key 'key' at render scale 'scale' into
	 *  dst (dst_w x dst_h, pitch dst_pitch), with layer1x the terrain's 128x128
	 *  1x flat layer. False (dst untouched) when there is none, it is rejected,
	 *  or dst is not 128 scale square.
	 */
	bool terrain(uint64_t key, int scale, const uint8_t* layer1x, uint8_t* dst, int dst_w, int dst_h, int dst_pitch);

	// Changes whenever flat() or terrain() may answer differently: caches that
	// hold overrides compare it (WP-09).
	uint32_t generation();

	// Shapes, palette or game changed (Shape_manager::load, reload_shapes):
	// drop every store; they reload lazily. generation() changes.
	void invalidate();

	// Rescans every root (dev reload): drops every store and re-reads the
	// configuration, then loads 'scale' now when it is > 1 and returns its
	// summary line (empty otherwise). generation() changes.
	std::string reload(int scale);

	// The A/B toggle (config overrides=no starts disabled). generation()
	// changes when the state changes.
	void set_enabled(bool enabled);
	bool is_enabled();

	// The report of the last load of 'scale', failed loads included (then
	// Report::failed is set and the counts are 0); nullptr when 'scale' has
	// not been loaded since the last invalidate().
	const Report* report(int scale);

	// The roots of the current game, in precedence order.
	std::vector<Root> roots();

	// The effective palette 0 (Palette::load of PALETTES_FLX and PATCH_PALETTES,
	// no apply()), converted like Get_color8 at brightness 100. Throws
	// std::runtime_error before the game window exists or when the palette
	// cannot be read (a store load then fails soft, with that message).
	Pal8 effective_palette0();

	// The source provider: the effective shapes.vga flat of (shape, frame).
	const uint8_t* source_flat(int shape, int frame);

	// config/video/hires/dev (read with the configuration, again on reload()).
	bool dev_mode();
	// config/video/hires/art_scale: S_art, the scale of the pack folders.
	int art_scale();

	// What the store answers for a flat or a terrain key at a scale (the
	// inspector, hires_dev.cc).
	struct Explanation {
		std::string result;    // "TILE", "TERRAIN" or "NN".
		// NN: why ("no override", "overrides disabled", "scale 1", "overrides
		// failed at this scale, ...", "rejected: <rule> <detail>"); otherwise
		// a note ("not decoded yet", a P4 reject of a terrain) or empty.
		std::string reason;
		std::string path;                   // The file or bundle entry (absolute), or empty.
		std::string where;                  // path as "<root label>/<path in the root>", or empty.
		bool        reduced     = false;    // Reduced from x<art_scale>.
		bool        from_bundle = false;

		// One line: "TILE <where>", "NN (rejected: P4 ...) <where>", ...
		std::string text() const;
	};

	Explanation explain_flat(int shape, int frame, int scale);
	Explanation explain_terrain(uint64_t key, int scale);
}    // namespace Hires

#endif
