/*
 *  hires_vga.h - The source provider of the hi-res store on a Vga_file
 *  (DESIGN.md section 3.5): the effective 1x flat of (shape, frame).
 *
 *  Header-only, so the engine glue and the unit tests run the same code.
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

#ifndef HIRES_VGA_H
#define HIRES_VGA_H

#include "vgafile.h"

#include <cstdint>

namespace Hires {
	/*
	 *  The 64-byte flat of (shape, frame) in 'vga', or nullptr when the shape is
	 *  out of range, empty or RLE, or the frame does not exist.
	 *  - The shape is bound-checked first: Vga_file::get_shape indexes its
	 *    shape table without a range check, and pack names admit 0-9999.
	 *  - Frame 0 is fetched before the frame count: get_num_frames indexes the
	 *    import table without a check when an imported shape's source is missing.
	 *  - The frame is checked against the frame count before it is fetched, so
	 *    a pack naming a missing frame does not make Shape::store_frame complain
	 *    on stderr for every file.
	 */
	inline const uint8_t* flat_from_vga(Vga_file& vga, int shape, int frame) {
		if (shape < 0 || shape >= vga.get_num_shapes() || frame < 0 || frame >= 32) {
			return nullptr;
		}
		const Shape_frame* first = vga.get_shape(shape, 0);
		if (first == nullptr || first->is_rle() || frame >= vga.get_num_frames(shape)) {
			return nullptr;
		}
		Shape_frame* f = vga.get_shape(shape, frame);
		if (f == nullptr || f->is_rle()) {
			return nullptr;
		}
		return f->get_data();
	}
}    // namespace Hires

#endif
