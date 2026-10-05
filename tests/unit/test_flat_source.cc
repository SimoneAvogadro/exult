/*
 *  test_flat_source.cc - Pins the fill rule of a chunk's flat layer,
 *  find_flat_source() (objs/flat_source.h), to the loop it replaced in
 *  Chunk_terrain::paint_tile, with the row-0 bound fixed and the void tile
 *  skipped everywhere (DESIGN.md sections 3.4, 6.2).
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
#include "objs/flat_source.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <vector>

using hires_test::Rng;

namespace {
	constexpr int tiles_per_chunk = c_tiles_per_chunk;
	constexpr int num_tiles       = tiles_per_chunk * tiles_per_chunk;

	int tile_num(int x, int y) {
		return tiles_per_chunk * y + x;
	}

	// A tile as the rule sees it: its ShapeID and what get_shape() returns
	// for it (no frame, a flat frame or an RLE frame).
	struct Tile {
		int       shape      = 150;
		int       frame      = 0;
		Tile_kind frame_kind = Tile_kind::Rle;
	};

	// A chunk; every tile starts as an RLE frame (an object).
	struct Chunk {
		std::array<Tile, num_tiles> tiles;

		void set(int x, int y, int shape, int frame, Tile_kind frame_kind) {
			tiles[tile_num(x, y)] = Tile{shape, frame, frame_kind};
		}

		void flat(int x, int y) {
			set(x, y, 5, 0, Tile_kind::Flat);
		}

		void void_flat(int x, int y) {
			set(x, y, 12, 0, Tile_kind::Flat);
		}

		bool is_void(int t) const {
			return tiles[t].shape == 12 && tiles[t].frame == 0;
		}

		// The engine's classification (Chunk_terrain::paint_flats).
		Tile_kind kind(int t) const {
			const Tile_kind frame_kind = tiles[t].frame_kind;
			if (frame_kind == Tile_kind::Flat && is_void(t)) {
				return Tile_kind::Flat_void;
			}
			return frame_kind;
		}

		// find_flat_source() on this chunk; its kind_of() calls go to *calls.
		int source(int tx, int ty, std::vector<int>* calls = nullptr) const {
			return find_flat_source(
					tx, ty,
					[this](int t) {
						return is_void(t);
					},
					[this, calls](int t) {
						if (calls != nullptr) {
							calls->push_back(t);
						}
						return kind(t);
					});
		}
	};

	// Where reference_source() found the source.
	enum class Found {
		Nothing,
		Itself,
		Neighbour,
		Chunk_search
	};

	/*
	 *  The selection of the original Chunk_terrain::paint_tile (upstream
	 *  8b6ab6b43, objs/chunkter.cc:86-133), transcribed statement by statement,
	 *  with the neighbourhood bound "tiley + y > 0" fixed to ">= 0" and the
	 *  void tile skipped by the whole-chunk search too.
	 *  get_shape() logs each call: in the engine each call can load a frame,
	 *  so the calls must come in the same order.  The painted frame is the
	 *  one the last call returned.
	 */
	int reference_source(const Chunk& c, int tilex, int tiley, std::vector<int>& calls, Found& found) {
		struct Frame {
			bool rle;

			bool is_rle() const {
				return rle;
			}
		};

		static const Frame flat_frame{false};
		static const Frame rle_frame{true};
		const auto         get_shape = [&](int x, int y) -> const Frame* {
            const int t = tile_num(x, y);
            calls.push_back(t);
            switch (c.tiles[t].frame_kind) {
            case Tile_kind::None:
                return nullptr;
            case Tile_kind::Rle:
                return &rle_frame;
            default:
                return &flat_frame;
            }
		};
		const auto get_flat = [&](int x, int y) {
			return c.tiles[tile_num(x, y)];
		};

		found              = Found::Nothing;
		const Frame* shape = get_shape(tilex, tiley);
		if (shape && !shape->is_rle()) {
			found = Found::Itself;
			return calls.back();
		} else if (shape && shape->is_rle()) {
			shape = nullptr;
			for (int y = -1; !shape && y <= 1; y++) {
				for (int x = -1; !shape && x <= 1; x++) {
					if (tilex + x >= 0 && tilex + x < tiles_per_chunk && tiley + y >= 0 && tiley + y < tiles_per_chunk) {
						auto sid = get_flat(tilex + x, tiley + y);
						if (sid.shape == 12 && sid.frame == 0) {
							continue;
						}
						shape = get_shape(tilex + x, tiley + y);
					}
					if (shape && shape->is_rle()) {
						shape = nullptr;
					}
				}
			}
			found = shape ? Found::Neighbour : Found::Chunk_search;
			for (int y = 0; !shape && y < tiles_per_chunk; y++) {
				for (int x = 0; !shape && x < tiles_per_chunk; x++) {
					auto sid = get_flat(x, y);
					if (sid.shape == 12 && sid.frame == 0) {
						continue;
					}
					shape = get_shape(x, y);
					if (shape && shape->is_rle()) {
						shape = nullptr;
					}
				}
			}
			if (shape) {
				return calls.back();
			}
			found = Found::Nothing;
		}
		return -1;
	}

	// A random chunk: dense or sparse flats (sparse ones reach the whole-chunk
	// search), tiles without a frame, void tiles of every frame kind, and the
	// near misses 12/1 and 13/0.
	Chunk random_chunk(Rng& rng) {
		Chunk     c;
		const int flat_pct = rng.chance(40) ? rng.range(0, 3) : rng.range(4, 60);
		const int none_pct = rng.range(0, 10);
		const int void_pct = rng.range(0, 25);
		for (auto& tile : c.tiles) {
			const int r     = rng.range(0, 99);
			tile.frame_kind = r < none_pct ? Tile_kind::None : (r < none_pct + flat_pct ? Tile_kind::Flat : Tile_kind::Rle);
			if (rng.chance(void_pct)) {
				tile.shape = 12;
				tile.frame = 0;
			} else if (rng.chance(5)) {
				tile.shape = rng.chance(50) ? 12 : 13;
				tile.frame = tile.shape == 12 ? 1 : 0;
			} else {
				tile.shape = rng.range(0, 1023);
				tile.frame = rng.range(tile.shape == 12 ? 1 : 0, 31);
			}
		}
		return c;
	}
}    // namespace

TEST_CASE("flat source: a flat tile paints itself, the void tile too; no frame gets nothing") {
	Chunk c;
	c.flat(3, 4);
	c.void_flat(3, 3);
	c.set(6, 6, 7, 1, Tile_kind::None);
	c.flat(5, 5);
	c.flat(7, 7);
	CHECK(c.source(3, 4) == tile_num(3, 4));
	CHECK(c.source(3, 3) == tile_num(3, 3));
	CHECK(c.source(6, 6) == -1);
	CHECK(c.kind(tile_num(3, 3)) == Tile_kind::Flat_void);
	CHECK(c.kind(tile_num(3, 4)) == Tile_kind::Flat);
}

TEST_CASE("flat source: under an RLE tile the first flat neighbour wins, row by row") {
	Chunk c;
	c.flat(9, 7);
	c.flat(7, 8);
	c.flat(0, 0);
	CHECK(c.source(8, 8) == tile_num(9, 7));
	// Neighbours without a frame or with an RLE frame do not count.
	Chunk d;
	d.set(7, 7, 7, 1, Tile_kind::None);
	d.flat(9, 9);
	d.flat(0, 0);
	CHECK(d.source(8, 8) == tile_num(9, 9));
	// The chunk's edges bound the neighbourhood.
	Chunk e;
	e.flat(1, 15);
	e.flat(0, 2);
	CHECK(e.source(0, 15) == tile_num(1, 15));
	CHECK(e.source(15, 15) == tile_num(0, 2));
}

TEST_CASE("flat source: row 0 fills its neighbours like any other row") {
	// The row-0 neighbour (4,0) wins over (0,0), the first flat of the chunk.
	Chunk c;
	c.flat(4, 0);
	c.flat(0, 0);
	CHECK(c.source(5, 1) == tile_num(4, 0));
	// A tile in row 0 looks at rows 0 and 1: (4,0) comes first.
	Chunk d;
	d.flat(4, 0);
	d.flat(6, 0);
	d.flat(6, 1);
	CHECK(d.source(5, 0) == tile_num(4, 0));
	// Row 1 itself is a valid neighbourhood row.
	Chunk e;
	e.flat(0, 0);
	e.flat(4, 1);
	CHECK(e.source(5, 1) == tile_num(4, 1));
}

TEST_CASE("flat source: the void tile 12/0 never fills the cell under an RLE tile") {
	// Neighbourhood: (7,7) is void, so (9,9), the last neighbour, wins.
	Chunk c;
	c.void_flat(7, 7);
	c.flat(9, 9);
	std::vector<int> calls;
	CHECK(c.source(8, 8, &calls) == tile_num(9, 9));
	// kind_of() is never asked about the void neighbour; the tile itself is
	// asked again in the loop, as the original did.
	const std::vector<int> expected{136, 120, 121, 135, 136, 137, 151, 152, 153};
	CHECK(calls == expected);
	// Chunk search: no flat neighbour; the void tile (7,7) is the first flat
	// of the chunk, but (15,15) is used, and the void tile is never asked.
	Chunk d;
	d.void_flat(7, 7);
	d.flat(15, 15);
	calls.clear();
	CHECK(d.source(8, 8, &calls) == tile_num(15, 15));
	CHECK(std::find(calls.begin(), calls.end(), tile_num(7, 7)) == calls.end());
	// The near misses 12/1 and 13/0 are ordinary flats.
	Chunk e;
	e.set(7, 7, 12, 1, Tile_kind::Flat);
	e.set(9, 9, 13, 0, Tile_kind::Flat);
	CHECK(e.source(8, 8) == tile_num(7, 7));
	CHECK(e.kind(tile_num(7, 7)) == Tile_kind::Flat);
	// A void tile without a flat frame is never a source.
	Chunk f;
	f.set(0, 0, 12, 0, Tile_kind::Rle);
	f.set(1, 0, 12, 0, Tile_kind::None);
	f.flat(15, 15);
	CHECK(f.source(8, 8) == tile_num(15, 15));
}

TEST_CASE("flat source: an RLE tile in a chunk without flats gets nothing") {
	Chunk c;
	c.set(3, 3, 7, 0, Tile_kind::None);
	c.set(4, 4, 12, 0, Tile_kind::Rle);
	for (int t = 0; t < num_tiles; t++) {
		CHECK(c.source(t % tiles_per_chunk, t / tiles_per_chunk) == -1);
	}
}

TEST_CASE("flat source: equals the reference loop, frame loads included") {
	Rng rng(0xf1a750u);
	int mismatches  = 0;
	int itself      = 0;
	int neighbour   = 0;
	int searched    = 0;
	int nothing     = 0;
	int void_source = 0;    // The chunk search returned the void tile.
	int void_passed = 0;    // The chunk search passed over a flat void tile.
	int row0_used   = 0;    // A flat row-0 neighbour was used.
	for (int n = 0; n < 2000; n++) {
		const Chunk c = random_chunk(rng);
		for (int ty = 0; ty < tiles_per_chunk; ty++) {
			for (int tx = 0; tx < tiles_per_chunk; tx++) {
				std::vector<int> want_calls;
				std::vector<int> got_calls;
				Found            found;
				const int        want = reference_source(c, tx, ty, want_calls, found);
				const int        got  = c.source(tx, ty, &got_calls);
				if (got != want || got_calls != want_calls) {
					if (mismatches++ == 0) {
						INFO("chunk ", n, ", tile (", tx, ",", ty, "): reference ", want, ", find_flat_source ", got);
						CHECK(got == want);
						CHECK(got_calls == want_calls);
					}
					continue;
				}
				switch (found) {
				case Found::Nothing:
					nothing++;
					break;
				case Found::Itself:
					itself++;
					break;
				case Found::Neighbour:
					neighbour++;
					row0_used += want < tiles_per_chunk ? 1 : 0;    // In row 0.
					break;
				case Found::Chunk_search:
					searched++;
					void_source += c.is_void(want) ? 1 : 0;
					for (int t = 0; t < want; t++) {
						if (c.kind(t) == Tile_kind::Flat_void) {
							void_passed++;
							break;
						}
					}
					break;
				}
			}
		}
	}
	CHECK(mismatches == 0);
	// Every path was taken, both quirks included.
	MESSAGE("itself ", itself, ", neighbour ", neighbour, " (row 0 ", row0_used, "), chunk search ", searched,
			" (void tile passed over ", void_passed, "), nothing ", nothing);
	CHECK(itself > 0);
	CHECK(neighbour > 0);
	CHECK(searched > 0);
	CHECK(nothing > 0);
	CHECK(void_source == 0);
	CHECK(void_passed > 0);
	CHECK(row0_used > 0);
}
