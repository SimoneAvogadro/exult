/**
 ** Chunkter.h - Chunk terrain (16x16 flat tiles) on the map.
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

#ifndef CHUNKTER_H
#define CHUNKTER_H

#include "shapeid.h"

class Image_buffer8;

/*
 *  The flat landscape, 16x16 tiles:
 */
class Chunk_terrain : public Game_singletons {
	ShapeID shapes[256];    // Id's.  The flat (non-RLE's) are
	//   rendered here, the others are
	//   turned into Game_objects in the
	//   chunks that point to us.
	ShapeID*       undo_shapes;       // Set to prev. values when editing.
	int            num_clients;       // # of Chunk's that point to us.
	bool           modified;          // Changed (by map-editor).
	Image_buffer8* rendered_flats;    // Flats rendered for entire chunk.
	int            rendered_scale = 1;    // Its pixel scale.
	uint32         rendered_gen   = 0;    // Hires::generation() it was painted at (scale > 1).

	// Hi-res: the T1 key of the terrain's own flats (per-terrain overrides),
	//   valid while t1_valid and t1_gen == Hires::generation().
	uint64 t1_key   = 0;
	uint32 t1_gen   = 0;
	bool   t1_valid = false;
	// Most-recently used circular queue
	//   for rendered_flats:
	static Chunk_terrain* render_queue;
	static int            queue_size;
	static uint32         hires_renders;    // Hi-res: render_flats calls at a scale > 1.
	Chunk_terrain *       render_queue_next, *render_queue_prev;
	//   Kept only for nearby chunks.
	void insert_in_queue();    // Queue methods.
	void remove_from_queue();
	void trim_render_queue();    // Hi-res: evict down to the limit.
	// Create rendered_flats at the given pixel scale.
	Image_buffer8* render_flats(int scale = 1);
	void           free_rendered_flats();
	// Hi-res: the cache was painted with the current overrides.
	bool hires_flats_current() const;
	// Hi-res: paint the per-terrain override into dst (scale > 1); false
	//   when there is none.
	bool paint_hires_terrain(Image_buffer8& dst);

public:
	// Create from 16x16x2 data:
	Chunk_terrain(const unsigned char* data, bool v2_chunks);
	// Copy-constructor:
	Chunk_terrain(const Chunk_terrain& c2);
	~Chunk_terrain();

	inline void add_client() {
		num_clients++;
	}

	inline void remove_client() {
		num_clients--;
	}

	inline bool is_modified() {
		return modified;
	}

	inline void set_modified(bool tf = true) {
		modified = tf;
	}

	// Get tile's shape ID.
	inline ShapeID get_flat(int tilex, int tiley) const {
		return shapes[16 * tiley + tilex];
	}

	inline Shape_frame* get_shape(int tilex, int tiley) {
		return shapes[16 * tiley + tilex].get_shape();
	}

	// Set tile's shape.
	void set_flat(int tilex, int tiley, const ShapeID& id);
	bool commit_edits();    // Commit changes.  Rets. true if
	//   edited.
	void abort_edits();    // Undo changes.

	// The flats at the given pixel scale (that of the buffer they are
	//   painted into); re-rendered when the cached scale differs.
	Image_buffer8* get_rendered_flats(int scale = 1) {
		if (render_queue != this) {    // Not already first in queue?
			// Move to front of queue.
			insert_in_queue();
		}
		// Hi-res: at a scale > 1, also the overrides it was painted with.
		const bool current = rendered_flats && rendered_scale == scale && (scale == 1 || hires_flats_current());
		return current ? rendered_flats : render_flats(scale);
	}

	// Hi-res: the number of render_flats calls at a scale > 1 so far
	//   (the render test's check of the cache).
	static uint32 get_hires_renders() {
		return hires_renders;
	}

	// Index (row-major, 0-255) of the tile whose flat is painted at
	//   (tilex, tiley), or -1; see find_flat_source().  Loads frames.
	int get_flat_source(int tilex, int tiley);

	// Paint the flats (c_chunksize x c_chunksize) into dst.
	void paint_flats(Image_buffer8& dst, bool overrides);

	// Hi-res: the T1 key of the terrain's own flats (DESIGN.md section 5.2),
	//   the key of its per-terrain override.  Cached; loads frames.
	uint64 get_t1_key();

	void render_all(int cx, int cy, int pass);    // Render terrain-editing mode.
	// Write out to chunk.
	int  write_flats(unsigned char* chunk_data, bool v2_chunks);
	bool need_extended_shapes() const;
};

#endif
