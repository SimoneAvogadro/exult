/*
 *  flat_source.h - The fill rule of a chunk's flat layer: which flat tile is
 *  painted at each 8x8 cell.
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

#ifndef FLAT_SOURCE_H
#define FLAT_SOURCE_H

#include "exult_constants.h"

#include <cstdint>

/*
 *  What a tile of a chunk (16x16 tiles) resolves to.
 */
enum class Tile_kind : uint8_t {
	None,         // No frame: missing shape or frame.  Nothing is painted.
	Flat,         // An 8x8 flat frame.
	Flat_void,    // The flat of the palette-cycling void tile, shape 12 frame 0.
	Rle           // An RLE frame (an object); the cell under it gets a fill.
};

inline bool Is_flat(Tile_kind kind) {
	return kind == Tile_kind::Flat || kind == Tile_kind::Flat_void;
}

/*
 *  Find the tile whose flat frame is painted at tile (tx, ty) of a chunk.
 *  The tiles are numbered 0 to 255, row by row.
 *
 *  is_void(t):  true if tile t's ShapeID is the void tile, shape 12 frame 0.
 *               It looks at the ID only and never loads a frame.
 *  kind_of(t):  the Tile_kind of tile t.  The engine loads the frame here, so
 *               this function calls it in the order of the original loop: the
 *               tile itself, then the neighbourhood, then the chunk, stopping
 *               at the first flat.
 *
 *  Output: the tile number of the source, or -1 if the cell gets nothing.
 *
 *  A flat tile paints itself.  Under an RLE tile, the first flat of the 3x3
 *  neighbourhood is used, else the first flat of the whole chunk; both are
 *  searched row by row, and both skip the void tile
 *  (tests/unit/test_flat_source.cc pins the rule).
 */
template <class Is_void_fn, class Kind_fn>
int find_flat_source(int tx, int ty, Is_void_fn is_void, Kind_fn kind_of) {
	const int       self = c_tiles_per_chunk * ty + tx;
	const Tile_kind own  = kind_of(self);
	if (own != Tile_kind::Rle) {
		return Is_flat(own) ? self : -1;
	}
	// Look at the tiles around this one for a suitable flat.
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			const int x = tx + dx;
			const int y = ty + dy;
			if (x >= 0 && x < c_tiles_per_chunk && y >= 0 && y < c_tiles_per_chunk) {
				const int t = c_tiles_per_chunk * y + x;
				// Skip the palette cycling void tile.
				if (!is_void(t) && Is_flat(kind_of(t))) {
					return t;
				}
			}
		}
	}
	// Couldn't find a nearby flat, so search the entire chunk.
	for (int t = 0; t < c_tiles_per_chunk * c_tiles_per_chunk; t++) {
		if (!is_void(t) && Is_flat(kind_of(t))) {
			return t;
		}
	}
	return -1;
}

#endif
