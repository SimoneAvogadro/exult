/*
 *  ibuf_ops.h - A deterministic stream of Image_buffer8 operations for the
 *  hi-res unit tests.
 *
 *  test_ibuf_golden applies it to scale-1 buffers and compares digests with
 *  tests/data/ibuf_golden.txt, which was recorded on unmodified upstream
 *  imagewin/ibuf8.cc; test_ibuf_scaled (O1) applies the same ops to a scale-1
 *  reference and to scaled buffers. All generated coordinates are logical
 *  (game px), and every op stays inside upstream's scale-1 contract:
 *    - clip rectangles always overlap the buffer (set_clip clamps them; a clip
 *      outside the buffer would let draw_line8 write out of bounds);
 *    - copy() rectangles lie inside the buffer (upstream does not clip them);
 *    - get_pixel8() reads inside the buffer (it is not clipped either);
 *    - RLE frames are at most 64 px wide (encode_rle's runs[200], P6).
 *  The fixtures are synthetic: no game data is used.
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

#ifndef IBUF_OPS_H
#define IBUF_OPS_H

#include "ibuf8.h"
#include "test_support.h"
#include "vgafile.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace hires_test {
	/*
	 *  Fixed inputs of the op stream, derived from a seed.
	 */
	struct Ibuf_fixtures {
		// As many translucency tables as the engine's built-in blend list
		// (shapeid.cc), so the translucent indices are 0xee-0xfe.
		constexpr static int num_xforms  = 17;
		constexpr static int first_xform = 0xff - num_xforms;
		constexpr static int block_size  = 64 * 64;
		constexpr static int line_size   = 4096;

		// Translucency tables built like Palette::create_trans_table on a
		// synthetic palette: each colour blended with a colour, then mapped to
		// the nearest palette entry.
		std::vector<Xform_palette> xforms;
		// Tables for paint_rle_remapped.
		std::vector<std::array<unsigned char, 256>> remaps;
		// Source pixels for copy8 and copy_transparent8 (about 25 % zeros).
		std::vector<unsigned char> block;
		// Source pixels for the hline copies (about 35 % translucent indices).
		std::vector<unsigned char> line;
		// RLE frames built with Shape_frame(pixels, ...), 1-64 px wide.
		std::vector<std::unique_ptr<Shape_frame>> rle_frames;
		// 8x8 flats.
		std::vector<std::unique_ptr<Shape_frame>> flat_frames;

		explicit Ibuf_fixtures(uint64_t seed);
	};

	enum class Ibuf_op_kind {
		set_clip,
		clear_clip,
		fill_all,
		fill_rect,
		fill_hline,
		draw_line,
		copy8,
		copy_hline,
		copy_hline_translucent,
		fill_hline_translucent,
		fill_translucent,
		copy_transparent,
		get_pixel,
		put_pixel,
		copy,
		get,
		put,
		fill_static,
		shape_paint_rle,
		paint_rle,
		paint_rle_remapped,
		paint_rle_translucent,
		paint_rle_transformed,
		paint_rle_outline,
		paint_flat,
		draw_box,
		draw_beveled_box,
		create_another,
		is_visible,
		count
	};

	constexpr int num_ibuf_op_kinds = static_cast<int>(Ibuf_op_kind::count);

	const char* ibuf_op_name(Ibuf_op_kind kind);

	struct Ibuf_op {
		Ibuf_op_kind kind = Ibuf_op_kind::clear_clip;
		// The arguments; their meaning per kind is in make_ibuf_op() and
		// apply_ibuf_op().
		std::array<int, 12> arg{};

		std::string describe() const;
	};

	// What the generator needs to know about the buffers: the logical size of
	// the target and of the second buffer (get, put), and the target's logical
	// offset. The target's logical extent is [-offset_x, width - offset_x) x
	// [-offset_y, height - offset_y); every coordinate is drawn relative to it,
	// so the random draws, and with offset 0 the ops, do not change with it.
	struct Ibuf_geometry {
		int width;
		int height;
		int aux_width;
		int aux_height;
		int offset_x = 0;
		int offset_y = 0;
	};

	// The next op. It depends only on the generator state and the logical
	// geometry, never on buffer contents, so the same stream can drive buffers
	// of different pixel scales.
	Ibuf_op make_ibuf_op(Rng& rng, const Ibuf_geometry& geom, const Ibuf_fixtures& fx);

	// Collects what the ops read back (get_pixel8, is_visible, create_another).
	struct Ibuf_reads {
		uint64_t hash = fnv_offset;

		void add(uint64_t value) {
			hash = fnv1a64_value(value, hash);
		}
	};

	// Applies op to target; aux is the second buffer of get and put. For
	// fill_static it calls std::srand(arg[0]) first. Its output still depends
	// on the C library's rand(), so a test that compares results across
	// platforms handles that op itself.
	void apply_ibuf_op(const Ibuf_op& op, Image_buffer8& target, Image_buffer8& aux, const Ibuf_fixtures& fx, Ibuf_reads& reads);
}    // namespace hires_test

#endif
