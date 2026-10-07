/**
 ** Chunkter.cc - Chunk terrain (16x16 flat tiles) on the map.
 **
 ** Written: 7/6/01 - JSF
 **/

/*
Copyright (C) 2001-2022 The Exult Team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif
#include "chunkter.h"

#include "flat_source.h"
#include "gamewin.h"
#include "hires_glue.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <new>
#include <vector>

Chunk_terrain* Chunk_terrain::render_queue = nullptr;
int            Chunk_terrain::queue_size   = 0;

uint32 Chunk_terrain::hires_renders = 0;    // Hi-res.

/*
 *  Insert at start of render queue.  It may already be there, but it's
 *  assumed that it's already tested as not being at the start.
 */

void Chunk_terrain::insert_in_queue() {
	if (render_queue_next) {    // In queue already?
		// !!Assuming it's not at head!!
		render_queue_next->render_queue_prev = render_queue_prev;
		render_queue_prev->render_queue_next = render_queue_next;
	} else {
		queue_size++;    // Adding, so increment count.
	}
	if (!render_queue) {    // First?
		render_queue_next = render_queue_prev = this;
	} else {
		render_queue_next                    = render_queue;
		render_queue_prev                    = render_queue->render_queue_prev;
		render_queue_prev->render_queue_next = this;
		render_queue->render_queue_prev      = this;
	}
	render_queue = this;
}

/*
 *  Remove from render queue.
 */

void Chunk_terrain::remove_from_queue() {
	if (!render_queue_next) {
		return;    // Not in queue.
	}
	queue_size--;
	if (render_queue_next == this) {    // Only element?
		render_queue = nullptr;
	} else {
		if (render_queue == this) {
			render_queue = render_queue_next;
		}
		render_queue_next->render_queue_prev = render_queue_prev;
		render_queue_prev->render_queue_next = render_queue_next;
	}
	render_queue_next = render_queue_prev = nullptr;
}

/*
 *  Hi-res: paint the override of flat 'id' into cell (tilex, tiley) of dst,
 *  at dst's pixel scale (> 1).
 *
 *  Output: false when there is none (overrides off, no art, or the store
 *  failed); the caller then paints the 1x flat (NN).
 */

static bool Paint_hires_flat(Image_buffer8& dst, const ShapeID& id, int tilex, int tiley) {
	const int              side = c_tilesize * dst.get_pixel_scale();
	const Hires::Tile_view hi   = Hires::flat(id.get_shapenum(), id.get_framenum() & 31, dst.get_pixel_scale());
	if (hi.px == nullptr) {
		return false;
	}
	// A view of another scale is a bug (a store that outlived a resize):
	// never read past it, log it once and fall back to NN (I11).
	if (hi.side != side) {
		static bool logged = false;
		if (!logged) {
			logged = true;
			std::cerr << "[hires] flat " << id.get_shapenum() << ':' << id.get_framenum() << " has a " << hi.side
					  << " px view at scale " << dst.get_pixel_scale() << "; painting the 1x flat" << std::endl;
		}
		return false;
	}
	dst.put_phys(hi.px, side, side, side, tilex * side, tiley * side);
	return true;
}

/*
 *  Hi-res: paint the per-terrain override (the whole 128x128 game px layer)
 *  into dst, at dst's pixel scale (> 1).
 *
 *  Output: false when there is none (overrides off, no art for this T1 key,
 *  a rejected file, or the store failed); the caller then paints per tile.
 */

bool Chunk_terrain::paint_hires_terrain(Image_buffer8& dst) {
	const int scale = dst.get_pixel_scale();
	// No terrain art at this scale: skip the key (it would load every frame).
	if (static_cast<int>(dst.get_width()) != c_chunksize || static_cast<int>(dst.get_height()) != c_chunksize
		|| Hires::terrain_count(scale) == 0) {
		return false;
	}
	const uint64 key = get_t1_key();
	if (!Hires::has_terrain(key, scale)) {
		return false;
	}
	try {
		// The 1x flat layer, fill included: the parent pixels of the P4 rule
		// and of the reduction from the art scale.
		Image_buffer8 layer(c_chunksize, c_chunksize);
		paint_flats(layer, false);
		std::vector<unsigned char> layer1x(c_chunksize * c_chunksize);
		for (int y = 0; y < c_chunksize; y++) {
			std::memcpy(&layer1x[y * c_chunksize], layer.get_bits() + y * layer.get_line_width(), c_chunksize);
		}
		const int                  side = c_chunksize * scale;
		std::vector<unsigned char> px(static_cast<size_t>(side) * side);
		if (!Hires::terrain(key, scale, layer1x.data(), px.data(), side, side, side)) {
			return false;
		}
		dst.put_phys(px.data(), side, side, side, 0, 0);
		return true;
	} catch (const std::bad_alloc&) {
		return false;    // Fail soft: per tile (I11).
	}
}

/*
 *  Hi-res: the T1 key of the terrain's own flats (DESIGN.md section 5.2):
 *  FNV-1a-64 over the bitmap of the non-RLE tiles and their 1x pixels.  It
 *  does not depend on the fill heuristic or on the terrain's number.  Cached
 *  until an edit, or until Hires::generation() changes (shapes reloaded).
 */

uint64 Chunk_terrain::get_t1_key() {
	const uint32 gen = Hires::generation();
	if (t1_valid && t1_gen == gen) {
		return t1_key;
	}
	const uint8_t* own[Hires::terrain_tiles];
	for (int t = 0; t < Hires::terrain_tiles; t++) {
		Shape_frame* shape = shapes[t].get_shape();
		own[t]             = shape != nullptr && !shape->is_rle() ? shape->get_data() : nullptr;
	}
	t1_key   = Hires::terrain_key_t1(own);
	t1_gen   = gen;
	t1_valid = true;
	return t1_key;
}

/*
 *  Paint the flats of the chunk (c_chunksize x c_chunksize) into a buffer.
 *  Flat tiles paint themselves.  We still want to draw a flat tile under RLE
 *  shapes to fix black gaps in the ice caves: the original didn't clear its
 *  frame buffer, so its gaps wouldn't normally be visible.  Not replicating
 *  that, a nearby flat (or any flat of the chunk) is used instead; see
 *  find_flat_source().  Cells that get no flat are 0.
 */

void Chunk_terrain::paint_flats(
		Image_buffer8& dst,
		bool           overrides    // Allow hi-res art (false: minimap).
) {
	// Cells that get no flat are not painted: clear them, so they never show
	// uninitialised memory or the previous render.
	dst.fill8(0);
	const int scale = dst.get_pixel_scale();
	// Hi-res precedence: per-terrain, then per-tile, then the 1x flat (NN).
	if (scale > 1 && overrides && paint_hires_terrain(dst)) {
		return;
	}
	for (int tiley = 0; tiley < c_tiles_per_chunk; tiley++) {
		for (int tilex = 0; tilex < c_tiles_per_chunk; tilex++) {
			const int src = get_flat_source(tilex, tiley);
			if (src >= 0) {
				Shape_frame* shape = shapes[src].get_shape();
				if (scale > 1 && overrides && Paint_hires_flat(dst, shapes[src], tilex, tiley)) {
					continue;    // Hi-res: the source flat's override.
				}
				dst.copy8(shape->get_data(), c_tilesize, c_tilesize, tilex * c_tilesize, tiley * c_tilesize);
			}
		}
	}
}

/*
 *  Find the tile whose flat is painted at (tilex, tiley): the tile itself,
 *  or for an RLE tile a nearby flat (see find_flat_source()).
 *
 *  Output: Index (row-major, 0-255) of that tile, or -1 for none.
 */

int Chunk_terrain::get_flat_source(int tilex, int tiley) {
	// The palette cycling void tile.
	const auto is_void = [this](int t) {
		return shapes[t].get_shapenum() == 12 && shapes[t].get_framenum() == 0;
	};
	// Loads the frame.
	const auto kind_of = [this, &is_void](int t) {
		const Shape_frame* shape = shapes[t].get_shape();
		if (!shape) {
			return Tile_kind::None;
		}
		if (shape->is_rle()) {
			return Tile_kind::Rle;
		}
		return is_void(t) ? Tile_kind::Flat_void : Tile_kind::Flat;
	};
	return find_flat_source(tilex, tiley, is_void, kind_of);
}

/*
 *  Create list for a given chunk.
 */

Chunk_terrain::Chunk_terrain(
		const unsigned char* data,        // Chunk data.
		bool                 v2_chunks    // 3 bytes/shape.
		)
		: undo_shapes(nullptr), num_clients(0), modified(false), rendered_flats(nullptr), render_queue_next(nullptr),
		  render_queue_prev(nullptr) {
	for (int tiley = 0; tiley < c_tiles_per_chunk; tiley++) {
		for (int tilex = 0; tilex < c_tiles_per_chunk; tilex++) {
			int shnum;
			int frnum;
			if (v2_chunks) {
				shnum = data[0] + 256 * data[1];
				frnum = data[2];
				data += 3;
			} else {
				shnum = data[0] + 256 * (data[1] & 3);
				frnum = (data[1] >> 2) & 0x1f;
				data += 2;
			}
			const ShapeID id(shnum, frnum);
			shapes[16 * tiley + tilex] = id;
		}
	}
}

/*
 *  Copy another.  The 'modified' flag is set to true.
 */

Chunk_terrain::Chunk_terrain(const Chunk_terrain& c2)
		: undo_shapes(nullptr), num_clients(0), modified(true), rendered_flats(nullptr), render_queue_next(nullptr),
		  render_queue_prev(nullptr) {
	for (int tiley = 0; tiley < c_tiles_per_chunk; tiley++) {
		for (int tilex = 0; tilex < c_tiles_per_chunk; tilex++) {
			shapes[16 * tiley + tilex] = c2.shapes[16 * tiley + tilex];
		}
	}
}

/*
 *  Clean up.
 */

Chunk_terrain::~Chunk_terrain() {
	delete[] undo_shapes;
	delete rendered_flats;
	remove_from_queue();
}

/*
 *  Set tile's shape.
 *  NOTE:  Set's 'modified' flag.
 */

void Chunk_terrain::set_flat(int tilex, int tiley, const ShapeID& id) {
	if (!undo_shapes) {    // Create backup.
		undo_shapes = new ShapeID[256];
		std::memcpy(reinterpret_cast<char*>(undo_shapes), reinterpret_cast<char*>(&shapes[0]), sizeof(shapes));
	}
	shapes[16 * tiley + tilex] = id;
	modified                   = true;
	t1_valid                   = false;
}

/*
 *  Commit changes.
 *
 *  Output: True if this was edited, else false.
 */

bool Chunk_terrain::commit_edits() {
	if (!undo_shapes) {
		return false;
	}
	delete[] undo_shapes;
	undo_shapes = nullptr;
	render_flats(rendered_scale);    // Update with new data.
	return true;
}

/*
 *  Undo changes.   Note:  We don't clear 'modified', since this could
 *  still have been moved to a different position.
 */

void Chunk_terrain::abort_edits() {
	if (undo_shapes) {
		std::memcpy(reinterpret_cast<char*>(&shapes[0]), reinterpret_cast<char*>(undo_shapes), sizeof(shapes));
		delete[] undo_shapes;
		undo_shapes = nullptr;
		t1_valid    = false;
	}
}

/*
 *  Figure max. queue size for given game window.
 */
static int Figure_queue_size() {
	const Game_window* gwin = Game_window::get_instance();
	const int          w    = gwin->get_width();
	const int          h    = gwin->get_height();
	// Figure # chunks, rounding up.
	const int cw = (w + c_chunksize - 1) / c_chunksize;
	const int ch = (h + c_chunksize - 1) / c_chunksize;
	// Add extra in each dir, but never go below the old fixed size.
	return std::max(100, (cw + 3) * (ch + 3));
}

/*
 *  Hi-res: free the least recently used caches until the queue is back
 *  within Figure_queue_size().  Upstream evicts one per new cache, so the
 *  queue never shrinks after the view does; at S > 1 every cache is S*S
 *  larger, so trim it fully.  Stops before reaching this terrain.
 */

void Chunk_terrain::trim_render_queue() {
	const int limit = Figure_queue_size();
	while (queue_size > limit && render_queue) {
		Chunk_terrain* last = render_queue->render_queue_prev;
		if (last == this) {
			break;
		}
		last->free_rendered_flats();
		last->remove_from_queue();
	}
}

/*
 *  Create rendered_flats buffer.
 */

Image_buffer8* Chunk_terrain::render_flats(int scale) {
	if (rendered_flats && rendered_scale != scale) {
		free_rendered_flats();    // Another pixel scale: replace in place.
		trim_render_queue();
		rendered_flats = new Image_buffer8(c_chunksize, c_chunksize, scale);
	}
	if (!rendered_flats) {
		if (queue_size > Figure_queue_size()) {
			// Grown too big.  Remove last.
			Chunk_terrain* last = render_queue->render_queue_prev;
			last->free_rendered_flats();
			render_queue->render_queue_prev            = last->render_queue_prev;
			last->render_queue_prev->render_queue_next = render_queue;
			last->render_queue_next = last->render_queue_prev = nullptr;
			queue_size--;
		}
		if (scale > 1) {
			trim_render_queue();
		}
		rendered_flats = new Image_buffer8(c_chunksize, c_chunksize, scale);
	}
	rendered_scale = scale;
	if (scale > 1) {
		rendered_gen = Hires::generation();    // Hi-res: the overrides painted.
		hires_renders++;
	}
	paint_flats(*rendered_flats, true);
	return rendered_flats;
}

/*
 *  Free pre-rendered landscape.
 */

void Chunk_terrain::free_rendered_flats() {
	delete rendered_flats;
	rendered_flats = nullptr;
}

/*
 *  Hi-res: the cache (at a scale > 1) holds the overrides the store serves
 *  now.  A toggle, reload or game switch changes Hires::generation().
 */

bool Chunk_terrain::hires_flats_current() const {
	return rendered_gen == Hires::generation();
}

/*
 *  This method is only used in 'terrain-editor' mode, NOT in normal
 *  gameplay.
 */

void Chunk_terrain::render_all(
		int cx, int cy, int pass    // Chunk rendering too.
) {
	Image_window8* iwin     = gwin->get_win();
	const int      ctx      = cx * c_tiles_per_chunk;
	const int      cty      = cy * c_tiles_per_chunk;
	const int      scrolltx = gwin->get_scrolltx();
	const int      scrollty = gwin->get_scrollty();
	// Go through array of tiles.
	for (int tiley = 0; tiley < c_tiles_per_chunk; tiley++) {
		for (int tilex = 0; tilex < c_tiles_per_chunk; tilex++) {
			Shape_frame* shape = get_shape(tilex, tiley);
			if (!shape) {
				continue;
			}
			if (!shape->is_rle() && pass == 1) {
				iwin->copy8(
						shape->get_data(), c_tilesize, c_tilesize, (ctx + tilex - scrolltx) * c_tilesize,
						(cty + tiley - scrollty) * c_tilesize);
			} else if (shape->is_rle() && pass == 2) {    // RLE.
				int              x;
				int              y;
				const Tile_coord tile(ctx + tilex, cty + tiley, 0);
				gwin->get_shape_location(tile, x, y);
				sman->paint_shape(x, y, shape);
			}
		}
	}
}

/*
 *  Write out to a chunk.
 *
 *  Output: Length of data stored.
 */

int Chunk_terrain::write_flats(
		unsigned char* chunk_data,
		bool           v2_chunks    // 3 bytes/entry.
) {
	unsigned char* start = chunk_data;
	for (int ty = 0; ty < c_tiles_per_chunk; ty++) {
		for (int tx = 0; tx < c_tiles_per_chunk; tx++) {
			const ShapeID id       = get_flat(tx, ty);
			const int     shapenum = id.get_shapenum();
			const int     framenum = id.get_framenum();
			if (v2_chunks) {
				Write1(chunk_data, shapenum & 0xff);
				Write1(chunk_data, (shapenum >> 8) & 0xff);
				Write1(chunk_data, framenum);
			} else {
				Write1(chunk_data, shapenum & 0xff);
				Write1(chunk_data, ((shapenum >> 8) & 3) | (framenum << 2));
			}
		}
	}
	return chunk_data - start;
}

bool Chunk_terrain::need_extended_shapes() const {
	for (int ty = 0; ty < c_tiles_per_chunk; ty++) {
		for (int tx = 0; tx < c_tiles_per_chunk; tx++) {
			const ShapeID id       = get_flat(tx, ty);
			const int     shapenum = id.get_shapenum();
			const int     framenum = id.get_framenum();
			if (shapenum > 0x3ff || framenum > 0x3f) {
				return true;
			}
		}
	}
	return false;
}
