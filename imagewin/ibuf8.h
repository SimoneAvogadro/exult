/*
 *  ibuf8.h - 8-bit image buffer.
 *
 *  Copyright (C) 1998-1999  Jeffrey S. Freedman
 *  Copyright (C) 2000-2022  The Exult Team
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

#ifndef INCL_IBUF8
#define INCL_IBUF8 1

#include "imagebuf.h"

/*
 *  An 8-bit image buffer:
 */
class Image_buffer8 : public Image_buffer {
	bool bits_owned;

	// Private ctor. for Image_window8.
	Image_buffer8(unsigned int w, unsigned int h, Image_buffer*) : Image_buffer(w, h, 8), bits_owned(false) {}

public:
	Image_buffer8(unsigned int w, unsigned int h) : Image_buffer(w, h, 8), bits_owned(true) {
		bits = new unsigned char[w * h];
	}
	friend class Image_window8;

	// for Image_window::Layer to create a buffer backed by a SDL_Surface with a guard band
	Image_buffer8(unsigned int w, unsigned int h, unsigned int pitch, unsigned char* bits, int guard_band)
			: Image_buffer(w - guard_band * 2, h - guard_band * 2, 8), bits_owned(false) {
		this->line_width = pitch;
		this->bits       = bits + guard_band + pitch * guard_band;
	}

	// Hi-res render scale (ibuf8_scaled.cc). An owned buffer of w x h game px,
	// each stored as scale x scale physical px: (w * scale) x (h * scale)
	// bytes, zero-filled, line_width = w * scale.
	Image_buffer8(unsigned int w, unsigned int h, int scale);
	// A view, not owned, of w x h game px at the given scale in memory with
	// the given physical pitch. origin is the first stored byte: the top-left
	// physical pixel of logical (-off_x, -off_y). The logical extent is
	// [-off_x, w - off_x) x [-off_y, h - off_y), and the clip is set to it.
	Image_buffer8(unsigned char* origin, int pitch, int w, int h, int off_x, int off_y, int scale);

	~Image_buffer8() {
		if (!bits_owned) {
			bits = nullptr;
		}
	}

	/*
	 *  Depth-independent methods:
	 */
	std::unique_ptr<Image_buffer> create_another(int w, int h) override {
		if (pixel_scale != 1) {
			return s_create_another(w, h);
		}
		return std::make_unique<Image_buffer8>(w, h);
	}

	// Copy within itself.
	void copy(int srcx, int srcy, int srcw, int srch, int destx, int desty) override;
	// Get rect. into another buf.
	void get(Image_buffer* dest, int srcx, int srcy) override;
	// Put rect. back.
	void put(Image_buffer* src, int destx, int desty) override;

	void fill_static(int black, int gray, int white) override;

	/*
	 *  8-bit color methods:
	 */
	// Fill with given (8-bit) value.
	void fill8(unsigned char pix) override;
	// Fill rect. wth pixel.
	void fill8(unsigned char pix, int srcw, int srch, int destx, int desty) override;
	// Fill line with pixel.
	void fill_hline8(unsigned char pix, int srcw, int destx, int desty) override;
	// Draw an arbitrary line from any point to any point inclusive. Accuracy
	// not guarenteed
	void draw_line8(unsigned char val, int startx, int starty, int endx, int endy, const Xform_palette* xform = nullptr) override;
	// Copy rectangle into here.
	void copy8(const unsigned char* src_pixels, int srcw, int srch, int destx, int desty) override;
	// Copy line to here.
	void copy_hline8(const unsigned char* src_pixels, int srcw, int destx, int desty) override;
	// Copy with translucency table.
	void copy_hline_translucent8(
			const unsigned char* src_pixels, int srcw, int destx, int desty, int first_translucent, int last_translucent,
			const Xform_palette* xforms) override;
	// Apply translucency to a line.
	void fill_hline_translucent8(unsigned char val, int srcw, int destx, int desty, const Xform_palette& xform) override;
	// Apply translucency to a rectangle
	void fill_translucent8(unsigned char val, int srcw, int srch, int destx, int desty, const Xform_palette& xform) override;
	// Copy rect. with transp. color.
	void copy_transparent8(const unsigned char* src_pixels, int srcw, int srch, int destx, int desty) override;

	// Get/put a single pixel.
	unsigned char get_pixel8(int x, int y) override {
		if (pixel_scale != 1) {
			return s_get_pixel8(x, y);
		}
		return bits[y * line_width + x];
	}

	void put_pixel8(unsigned char pix, int x, int y) override {
		if (pixel_scale != 1) {
			return s_put_pixel8(pix, x, y);
		}
		if (x >= clipx && x < clipx + clipw && y >= clipy && y < clipy + cliph) {
			bits[y * line_width + x] = pix;
		}
	}

	void paint_rle(int xoff, int yoff, const unsigned char* in);
	void paint_rle_remapped(int xoff, int yoff, const unsigned char* inptr, const unsigned char*& trans);

	void draw_beveled_box(
			int x, int y, int w, int h, int depth, uint8 colfill, uint8 coltop, uint8 coltr, uint8 colbottom, uint8 colbl,
			std::optional<uint8> coltlbr = {}) override;

	/*
	 *  Hi-res render scale (ibuf8_scaled.cc).
	 */
	// Copies src's storage extent, logical [-off_x, w - off_x) x
	// [-off_y, h - off_y) of src, so that src's logical (0, 0) lands on
	// (destx, desty), clipped to this buffer's clip. Physical rows (src's own
	// pitch) between equal scales; across scales each logical pixel is the
	// top-left physical sample of the source, replicated to this scale.
	void blit(const Image_buffer& src, int destx, int desty);
	// Copies pw x ph physical pixels (pitch src_pitch) to the physical
	// position (px, py) relative to logical (0, 0), clipped to the clip
	// rectangle times the pixel scale.
	void put_phys(const unsigned char* src, int pw, int ph, int src_pitch, int px, int py);

private:
	// The bodies of the primitives at pixel_scale != 1; each primitive calls
	// its counterpart first thing.
	void s_copy(int srcx, int srcy, int srcw, int srch, int destx, int desty);
	void s_get(Image_buffer* dest, int srcx, int srcy);
	void s_put(Image_buffer* src, int destx, int desty);
	// The body of blit() and s_put(); op names the caller in the
	// mixed-scale log line.
	void s_blit(const Image_buffer& src, int destx, int desty, const char* op);
	void s_fill_static(int black, int gray, int white);
	void s_fill8(unsigned char pix);
	void s_fill8(unsigned char pix, int srcw, int srch, int destx, int desty);
	void s_fill_hline8(unsigned char pix, int srcw, int destx, int desty);
	void s_draw_line8(unsigned char val, int startx, int starty, int endx, int endy, const Xform_palette* xform);
	void s_copy8(const unsigned char* src_pixels, int srcw, int srch, int destx, int desty);
	void s_copy_hline8(const unsigned char* src_pixels, int srcw, int destx, int desty);
	void s_copy_hline_translucent8(
			const unsigned char* src_pixels, int srcw, int destx, int desty, int first_translucent, int last_translucent,
			const Xform_palette* xforms);
	void                          s_fill_hline_translucent8(int srcw, int destx, int desty, const Xform_palette& xform);
	void                          s_fill_translucent8(int srcw, int srch, int destx, int desty, const Xform_palette& xform);
	void                          s_copy_transparent8(const unsigned char* src_pixels, int srcw, int srch, int destx, int desty);
	unsigned char                 s_get_pixel8(int x, int y);
	void                          s_put_pixel8(unsigned char pix, int x, int y);
	std::unique_ptr<Image_buffer> s_create_another(int w, int h);
	// paint_rle (trans null) and paint_rle_remapped.
	void s_paint_rle(int xoff, int yoff, const unsigned char* in, const unsigned char* trans);
};

#endif
