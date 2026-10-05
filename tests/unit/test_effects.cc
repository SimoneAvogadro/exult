/*
 *  test_effects.cc - Where a sprite effect is painted
 *  (Sprites_effect::get_paint_position in effects.h).
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

#include "doctest.h"
#include "effects.h"

namespace {
	struct Position {
		int x;
		int y;
	};

	Position paint_position(const Tile_coord& pos, int xoff, int yoff, int scrolltx_lo, int scrollty_lo) {
		Position p{};
		Sprites_effect::get_paint_position(pos, xoff, yoff, 5, 7, scrolltx_lo, scrollty_lo, p.x, p.y);
		return p;
	}
}    // namespace

TEST_CASE("effects: a sprite is painted at its tile, less half its lift, relative to the view") {
	// Tile (10,20) at lift 4 (2 tiles up and left), moved by (3,-2) pixels;
	// the view is at tile (5,7), not between tiles.
	const Position p = paint_position(Tile_coord(10, 20, 4), 3, -2, 0, 0);
	CHECK(p.x == 3 + (10 - 2 - 5) * c_tilesize);
	CHECK(p.y == -2 + (20 - 2 - 7) * c_tilesize);
}

TEST_CASE("effects: a sprite follows the smooth scroll, each axis by its own offset") {
	const Tile_coord pos(10, 20, 4);
	const Position   base = paint_position(pos, 3, -2, 0, 0);
	for (int lo = 1; lo < c_tilesize; lo++) {
		INFO("sub-tile scroll ", lo);
		// Scrolling right moves the sprite left, and not up.
		const Position right = paint_position(pos, 3, -2, lo, 0);
		CHECK(right.x == base.x - lo);
		CHECK(right.y == base.y);
		// Scrolling down moves the sprite up, and not left.
		const Position down = paint_position(pos, 3, -2, 0, lo);
		CHECK(down.x == base.x);
		CHECK(down.y == base.y - lo);
	}
}
