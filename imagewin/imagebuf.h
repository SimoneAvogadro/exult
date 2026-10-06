/**
 ** Imagebuf.h - A buffer for blitting.
 **
 ** Written: 8/13/98 - JSF
 **/

/*
Copyright (C) 1998 Jeffrey S. Freedman
Copyright (C) 2000-2022 The Exult Team

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with this library; if not, write to the
Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA  02111-1307, USA.
*/

#ifndef INCL_IMAGEBUF
#define INCL_IMAGEBUF 1

#include "common_types.h"
#include "ignore_unused_variable_warning.h"
#include "rect.h"

#include <algorithm>
#include <climits>
#include <memory>
#include <optional>

class IDataSource;

// Table for translating palette vals.:
/*
 *  This class represents a single transparent color by providing a
 *  palette for its effect on all the other colors.
 */
class Xform_palette {
public:
	unsigned char colors[256];    // For transforming 8-bit colors.

	unsigned char operator[](int i) const {
		return colors[i];
	}
};

/*
 *  Here's a generic off-screen buffer.  It's up to the derived classes
 *  to set the data.
 */
class Image_buffer {
public:
	/*
	 *  Hi-res render scale: the bounding box of the writes since the last
	 *  take(), in logical coordinates (game px, relative to the logical
	 *  origin, so often negative). The scaled primitives add the clipped
	 *  logical rectangle of every write; it never holds physical coordinates.
	 */
	struct Write_tracker {
		int x0 = INT_MAX;
		int y0 = INT_MAX;
		int x1 = INT_MIN;    // Exclusive.
		int y1 = INT_MIN;    // Exclusive.

		void add(int x, int y, int w, int h) {
			if (w <= 0 || h <= 0) {
				return;
			}
			x0 = std::min(x0, x);
			y0 = std::min(y0, y);
			x1 = std::max(x1, x + w);
			y1 = std::max(y1, y + h);
		}

		bool empty() const {
			return x0 >= x1 || y0 >= y1;
		}

		// The box (empty: 0, 0, 0, 0); the tracker is empty afterwards.
		TileRect take() {
			const TileRect box = empty() ? TileRect(0, 0, 0, 0) : TileRect(x0, y0, x1 - x0, y1 - y0);
			reset();
			return box;
		}

		void reset() {
			x0 = y0 = INT_MAX;
			x1 = y1 = INT_MIN;
		}

		// Exactly this rectangle, never a union with what was there.
		void mark_all(int x, int y, int w, int h) {
			reset();
			add(x, y, w, h);
		}
	};

protected:
	int            width, height;    // Dimensions (in pixels).
	int            offset_x, offset_y;
	int            depth;         // # bits/pixel.
	int            pixel_size;    // # bytes/pixel.
	unsigned char* bits;          // Allocated image buffer.
	int            line_width;    // # words/scan-line.
	// Hi-res render scale: physical pixels per logical (game) pixel on each
	// axis. It affects the storage only: width, height, the offsets, the clip
	// and every coordinate argument stay logical, while bits is the physical
	// address of logical (0, 0) and line_width the physical pitch.
	int pixel_scale = 1;
	// Set only on the main buffer when pixel_scale > 1.
	Write_tracker* tracker = nullptr;

private:
	int clipx, clipy, clipw, cliph;    // Clip rectangle.

	// Clip.  Rets. false if nothing to draw.
	bool clip_internal(int& src, int& size, int& dest, int clips, int clipl) {
		// size less than or equal to zero, get rid of it
		// There is no other sensible way for this function to deal with that
		if (size <= 0) {
			return false;
		}

		if (dest < clips) {
			if ((size += (dest - clips)) <= 0) {
				return false;
			}
			src -= (dest - clips);
			dest = clips;
		}
		if (dest + size > (clips + clipl)) {
			if ((size = ((clips + clipl) - dest)) <= 0) {
				return false;
			}
		}
		return true;
	}

protected:
	bool clip_x(int& srcx, int& width, int& destx, int desty) {
		return desty < clipy || desty >= clipy + cliph ? false : clip_internal(srcx, width, destx, clipx, clipw);
	}

	// Clip rectangle.
	bool clip(int& srcx, int& srcy, int& width, int& height, int& destx, int& desty) {
		// Start with x-dim.
		return clip_internal(srcx, width, destx, clipx, clipw) && clip_internal(srcy, height, desty, clipy, cliph);
	}

	Image_buffer(unsigned int w, unsigned int h, int dpth);

public:
	// class to create objects that get the clip rect from the input
	// image_Buffer and restores the clip rect when the object goes out of scope
	class ClipRectSave {
		Image_buffer* buf;
		TileRect      clip;

	public:
		ClipRectSave(Image_buffer* buf) : buf(buf) {
			buf->get_clip(clip.x, clip.y, clip.w, clip.h);
		}

		~ClipRectSave() {
			Restore();
		}

		operator const TileRect&() const {
			return clip;
		}

		const TileRect& Rect() const {
			return clip;
		}

		void Restore() {
			if (buf) {
				buf->set_clip(clip.x, clip.y, clip.w, clip.h);
			}
		}

		void Clear() {
			buf = nullptr;
		}
	};

	ClipRectSave SaveClip() {
		return ClipRectSave(this);
	}
	friend class Image_buffer8;

	virtual ~Image_buffer() {
		delete[] bits;    // In case Image_window didn't.
	}
	friend class Image_window;

	unsigned char* get_bits() {    // Get ->data.
		return bits;
	}

	unsigned int get_width() {
		return width;
	}

	unsigned int get_height() {
		return height;
	}

	unsigned int get_line_width() {
		return line_width;
	}

	// Physical pixels per logical pixel (1 unless the buffer is scaled).
	int get_pixel_scale() const {
		return pixel_scale;
	}

	// The tracker that the scaled primitives report their writes to.
	void set_tracker(Write_tracker* t) {
		tracker = t;
	}

	Write_tracker* get_tracker() const {
		return tracker;
	}

	// A copy would share bits and delete[] them twice.
	Image_buffer(const Image_buffer&)            = delete;
	Image_buffer& operator=(const Image_buffer&) = delete;

	void clear_clip() {    // Reset clip to whole window.
		clipx = -offset_x;
		clipy = -offset_y;
		clipw = width;
		cliph = height;
	}

	// Set clip.
	void set_clip(int x, int y, int w, int h) {
		// clipx = x;
		// clipy = y;
		// clipw = w;
		// cliph = h;
		x += offset_x;
		y += offset_y;
		if (x < 0) {
			w += x;
			x = 0;
		}
		if (x + w > width) {
			w = width - x;
		}
		if (y < 0) {
			h += y;
			y = 0;
		}
		if (y + h > height) {
			h = height - y;
		}
		clipx = x - offset_x;
		clipy = y - offset_y;
		clipw = w;
		cliph = h;
	}

	void get_clip(int& x, int& y, int& w, int& h) {
		x = clipx;
		y = clipy;
		w = clipw;
		h = cliph;
	}

	// Is rect. visible within clip?
	bool is_visible(int x, int y, int w, int h) {
		return x < clipx + clipw && y < clipy + cliph && x + w > clipx && y + h > clipy;
	}

	/*
	 *  8-bit color methods:
	 */
	// Fill with given (8-bit) value.
	virtual void fill8(unsigned char val) = 0;
	// Fill rect. with pixel.
	virtual void fill8(unsigned char val, int srcw, int srch, int destx, int desty) = 0;
	// Fill horizontal line with pixel.
	virtual void fill_hline8(unsigned char val, int srcw, int destx, int desty) = 0;
	// Draw an arbitrary line from any point to any point. Accuracy not
	// guarenteed
	virtual void draw_line8(unsigned char val, int startx, int starty, int endx, int endy, const Xform_palette* xform = nullptr)
			= 0;
	// Copy rectangle into here.
	virtual void copy8(const unsigned char* src_pixels, int srcw, int srch, int destx, int desty) = 0;
	// Copy line to here.
	virtual void copy_hline8(const unsigned char* src_pixels, int srcw, int destx, int desty) = 0;
	// Copy with translucency table.
	virtual void copy_hline_translucent8(
			const unsigned char* src_pixels, int srcw, int destx, int desty, int first_translucent, int last_translucent,
			const Xform_palette* xforms)
			= 0;
	// Apply translucency to a line.
	virtual void fill_hline_translucent8(unsigned char val, int srcw, int destx, int desty, const Xform_palette& xform) = 0;
	// Apply translucency to a rectangle
	virtual void fill_translucent8(unsigned char val, int srcw, int srch, int destx, int desty, const Xform_palette& xform) = 0;
	// Copy rect. with transp. color.
	virtual void copy_transparent8(const unsigned char* src_pixels, int srcw, int srch, int destx, int desty) = 0;

	// Get/put a single pixel.
	virtual unsigned char get_pixel8(int x, int y) = 0;

	virtual void put_pixel8(unsigned char pix, int x, int y) = 0;

	/*
	 *  Depth-independent methods:
	 */
	virtual std::unique_ptr<Image_buffer> create_another(int w, int h) = 0;
	// Copy within itself.
	virtual void copy(int srcx, int srcy, int srcw, int srch, int destx, int desty) = 0;
	// Get rect. into another buf.
	virtual void get(Image_buffer* dest, int srcx, int srcy) = 0;
	// Put rect. back.
	virtual void put(Image_buffer* src, int destx, int desty) = 0;

	virtual void fill_static(int black, int gray, int white) = 0;

	// Turning off clang format so it doesn't mess up the comments
	// clang-format off


	//! Draw a box with a fake 3d beveled edge
	//! /param x X coord of top left
	//! /param y Y coord of top left
	//! /param w Outer Width of the box to draw
	//! /param h Outer Height of the Box to draw
	//! /param depth The depth of the 3d effect, the beveled edge will be this
	//! many pixels wide 
	//! /param colfill Colour to fill the box 
	//! /param coltop Colour for the the top edge and the right edge 
	//! /param coltr Colour for the topright corner 
	//! /param colbottom Colour for the bottom edge and the left edge 
	//! /param colbl Colour for the bottom left corner 
	//! /param coltlbr Optional colour to use for top left and bottom right. If
	//! not specified colfill is used
	//! /remark set colours to 0xff to not draw that element
	virtual void draw_beveled_box(
			int x, int y, int w, int h, int depth, uint8 colfill, uint8 coltop,
			uint8 coltr, uint8 colbottom, uint8 colbl, std::optional <uint8>coltlbr={})
			= 0;

	//! Draw a box witha given stroke and fill
	//! /param x X coord of top left
	//! /param y Y coord of top left
	//! /param w Outer Width of the box to draw
	//! /param h Outer Height of the Box to draw
	//! /param strokewidth With of line around box set to 0 to not draw
	//! /param colfill THe colour to use to fill the box
	//~ /param colstroke The stroke colour of the box edge lines
	virtual void draw_box(
			int x, int y, int w, int h, int strokewidth, uint8 colfill,
			uint8 colstroke);
	// clang-format on
};

#endif
