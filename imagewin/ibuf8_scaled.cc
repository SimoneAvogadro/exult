/*
 *  ibuf8_scaled.cc - The scaled storage of the 8-bit image buffer, for the
 *  hi-res render scale.
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

/*
 *  A buffer whose pixel_scale S is not 1 keeps its logical API: width,
 *  height, the offsets, the clip rectangle and every coordinate argument are
 *  logical (game px). Only the storage is scaled: logical (x, y) is the S x S
 *  block of physical pixels at bits + y * S * line_width + x * S, and
 *  line_width is the physical pitch.
 *
 *  Each primitive in ibuf8.cc starts with a hook that calls its counterpart
 *  here when pixel_scale != 1, so the scale-1 bodies stay as they are. The
 *  counterparts clip in logical coordinates with the helpers of imagebuf.h,
 *  report the clipped logical rectangle to the write tracker (if there is
 *  one), and then write only the physical blocks of that rectangle. On
 *  nearest-neighbour content each one gives the nearest-neighbour upscale of
 *  its scale-1 result. Translucency tables are applied to every physical
 *  pixel, because the destination may hold detail drawn at the physical
 *  resolution.
 *
 *  A physical row copy happens only between buffers of equal scale, and it
 *  uses each buffer's own line_width. Across scales (get, put, blit) each
 *  logical pixel is the top-left physical sample of the source, replicated to
 *  the destination's scale.
 */

#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif

#include "common_types.h"
#include "endianio.h"
#include "ibuf8.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <vector>

namespace {
	// The physical storage of a buffer.
	struct Phys {
		unsigned char* bits;          // Physical address of logical (0, 0).
		int            line_width;    // Physical pitch.
		int            scale;

		// The top-left physical pixel of logical (x, y).
		unsigned char* at(int x, int y) const {
			return bits + (static_cast<std::ptrdiff_t>(y) * line_width + x) * scale;
		}
	};

	void track(Image_buffer::Write_tracker* tracker, int x, int y, int w, int h) {
		if (tracker != nullptr) {
			tracker->add(x, y, w, h);
		}
	}

	template <int Scale>
	void rep_row_fixed(unsigned char* dst, const unsigned char* src, int w) {
		for (int i = 0; i < w; i++, dst += Scale) {
			const unsigned char pix = src[i];
			for (int k = 0; k < Scale; k++) {
				dst[k] = pix;
			}
		}
	}

	// Writes each of the w bytes at src scale times to dst.
	void rep_row(unsigned char* dst, const unsigned char* src, int w, int scale) {
		switch (scale) {
		case 2:
			rep_row_fixed<2>(dst, src, w);
			break;
		case 3:
			rep_row_fixed<3>(dst, src, w);
			break;
		case 6:
			rep_row_fixed<6>(dst, src, w);
			break;
		default:
			for (int i = 0; i < w; i++, dst += scale) {
				std::memset(dst, src[i], scale);
			}
			break;
		}
	}

	// Copies the physical row at first, bytes long, to the scale - 1 rows
	// below it.
	void dup_rows(unsigned char* first, int bytes, int line_width, int scale) {
		for (int k = 1; k < scale; k++) {
			std::memcpy(first + static_cast<std::ptrdiff_t>(k) * line_width, first, bytes);
		}
	}

	// Fills the blocks of the logical rectangle (x, y, w, h).
	void fill_blocks(const Phys& p, int x, int y, int w, int h, unsigned char pix) {
		unsigned char* row   = p.at(x, y);
		const int      bytes = w * p.scale;
		for (int r = 0; r < h * p.scale; r++, row += p.line_width) {
			std::memset(row, pix, bytes);
		}
	}

	// Applies xform to every physical pixel of the logical rectangle.
	void xform_blocks(const Phys& p, int x, int y, int w, int h, const Xform_palette& xform) {
		unsigned char* row   = p.at(x, y);
		const int      bytes = w * p.scale;
		for (int r = 0; r < h * p.scale; r++, row += p.line_width) {
			for (int i = 0; i < bytes; i++) {
				row[i] = xform[row[i]];
			}
		}
	}

	// Copies the logical rectangle of w x h at (sx, sy) in src to (dx, dy) in
	// dst: physical rows between equal scales, else the top-left sample of
	// each source block replicated to the destination's scale.
	void copy_blocks(const Phys& dst, int dx, int dy, const Phys& src, int sx, int sy, int w, int h) {
		if (dst.scale == src.scale) {
			const int bytes = w * dst.scale;
			for (int r = 0; r < h; r++) {
				unsigned char*       to   = dst.at(dx, dy + r);
				const unsigned char* from = src.at(sx, sy + r);
				for (int k = 0; k < dst.scale; k++, to += dst.line_width, from += src.line_width) {
					std::memmove(to, from, bytes);
				}
			}
			return;
		}
		for (int r = 0; r < h; r++) {
			unsigned char*       to   = dst.at(dx, dy + r);
			const unsigned char* from = src.at(sx, sy + r);
			for (int i = 0; i < w; i++, from += src.scale) {
				std::memset(to + i * dst.scale, *from, dst.scale);
			}
			dup_rows(to, w * dst.scale, dst.line_width, dst.scale);
		}
	}

	// One axis of copy(): clips the source [src, src + size) and the
	// destination [dst, dst + size) to [lo, lo + len), moving both starts
	// together. False if nothing is left.
	bool clip_to_extent(int& src, int& dst, int& size, int lo, int len) {
		if (size <= 0) {
			return false;
		}
		long long       s    = src;
		long long       d    = dst;
		long long       n    = size;
		const long long hi   = static_cast<long long>(lo) + len;
		const long long skip = std::max(lo - s, lo - d);
		if (skip > 0) {
			s += skip;
			d += skip;
			n -= skip;
		}
		n = std::min({n, hi - s, hi - d});
		if (n <= 0) {
			return false;
		}
		src  = static_cast<int>(s);
		dst  = static_cast<int>(d);
		size = static_cast<int>(n);
		return true;
	}

	// Floor and ceiling of a / b, for b > 0.
	int floor_div(long long a, int b) {
		return static_cast<int>(a >= 0 ? a / b : -((-a + b - 1) / b));
	}

	int ceil_div(long long a, int b) {
		return -floor_div(-a, b);
	}

	// A mixed-scale copy loses the detail of the finer buffer, so it is
	// logged, once per process for each operation ("get", "put", "blit").
	void log_mixed_scale(const char* what, int from_scale, int to_scale) {
		// what is one of a few string literals; the first call of each
		// takes a slot.
		static const char* logged[4] = {};
		size_t             i         = 0;
		while (i < std::size(logged) && logged[i] != nullptr && std::strcmp(logged[i], what) != 0) {
			i++;
		}
		if (i < std::size(logged) && logged[i] == nullptr) {
			logged[i] = what;
			std::cout << "[hires] mixed-scale " << what << ": scale " << from_scale << " to scale " << to_scale
					  << " (sampled or replicated; logged once)" << std::endl;
		}
	}

	// The row buffer of the RLE painters: one per process (the engine draws
	// from one thread), grown to the longest scan so far.
	std::vector<unsigned char>& rle_row() {
		static std::vector<unsigned char> row;
		return row;
	}

	// The line clipping of draw_line8 (ibuf8.cc), unchanged: it clips
	// dimension 0 to [startc, endc]; the order of the points doesn't matter.
	bool clipline(int* start0, int* end0, int* start1, int* end1, int startc, int endc) {
		// Order points so start is before end
		if (*end0 < *start0) {
			std::swap(end0, start0);
			std::swap(end1, start1);
		}
		if (*start0 > endc) {
			return false;
		}
		if (*end0 < startc) {
			return false;
		}
		if (*start0 < startc) {
			const int olddelta0 = *end0 - *start0;
			const int newdelta0 = *end0 - startc;

			const int olddelta1 = *end1 - *start1;
			const int newdelta1 = (olddelta1 * newdelta0) / olddelta0;

			*start0 = startc;
			*start1 = *end1 - newdelta1;
		}
		if (*end0 > endc) {
			const int olddelta0 = *end0 - *start0;
			const int newdelta0 = endc - *start0;

			const int olddelta1 = *end1 - *start1;

			if (olddelta0 == 0 && (olddelta1 == 0 || newdelta0 == 0)) {
				*end0 = endc;
				*end1 = *start1;
			} else if (olddelta0 == 0) {
				return false;
			}
			const int newdelta1 = (olddelta1 * newdelta0) / olddelta0;

			*end0 = endc;
			*end1 = *start1 + newdelta1;
		}
		return true;
	}

	constexpr int point_side_of_line(int startx, int starty, int endx, int endy, int pointx, int pointy) {
		return (endx - startx) * (pointy - starty) - (endy - starty) * (pointx - startx);
	}

	constexpr bool isoob(int x, int y, int cx, int cw, int cy, int ch) {
		return x < cx || x > cx + cw || y < cy || y > cy + ch;
	}
}    // namespace

/*
 *  Constructors.
 */

Image_buffer8::Image_buffer8(unsigned int w, unsigned int h, int scale) : Image_buffer(w, h, 8), bits_owned(true) {
	assert(scale >= 1);
	pixel_scale = std::max(scale, 1);
	line_width  = static_cast<int>(w) * pixel_scale;
	bits        = new unsigned char[static_cast<size_t>(line_width) * h * pixel_scale]();
}

Image_buffer8::Image_buffer8(unsigned char* origin, int pitch, int w, int h, int off_x, int off_y, int scale)
		: Image_buffer(w, h, 8), bits_owned(false) {
	assert(scale >= 1 && w >= 0 && h >= 0 && pitch >= w * scale);
	pixel_scale = std::max(scale, 1);
	line_width  = pitch;
	offset_x    = off_x;
	offset_y    = off_y;
	bits        = origin + (static_cast<std::ptrdiff_t>(off_y) * pitch + off_x) * pixel_scale;
	clear_clip();
}

/*
 *  Depth-independent methods.
 */

// Clipped, unlike the scale-1 body: the source and destination rectangles are
// both cut to the logical extent, each shifting the other. The guard band is
// less than one game px at S > 1, so a rectangle out of range would overrun
// the storage S-fold.
void Image_buffer8::s_copy(int srcx, int srcy, int srcw, int srch, int destx, int desty) {
	if (!clip_to_extent(srcx, destx, srcw, -offset_x, width) || !clip_to_extent(srcy, desty, srch, -offset_y, height)) {
		return;
	}
	track(tracker, destx, desty, srcw, srch);
	const Phys           p{bits, line_width, pixel_scale};
	const int            bytes = srcw * pixel_scale;
	const int            rows  = srch * pixel_scale;
	unsigned char*       to    = p.at(destx, desty);
	const unsigned char* from  = p.at(srcx, srcy);
	std::ptrdiff_t       next  = line_width;
	if (srcy < desty) {    // Moving down: start with the last row.
		to += (rows - 1) * next;
		from += (rows - 1) * next;
		next = -next;
	}
	for (int r = 0; r < rows; r++, to += next, from += next) {
		std::memmove(to, from, bytes);
	}
}

// dest's storage extent, logical [-offset_x, width - offset_x) x
// [-offset_y, height - offset_y) of dest, gets this buffer's pixels so that
// dest's logical (0, 0) takes (srcx, srcy). As at scale 1, only this
// buffer's clip applies.
void Image_buffer8::s_get(Image_buffer* dest, int srcx, int srcy) {
	if (dest->pixel_scale != pixel_scale) {
		log_mixed_scale("get", pixel_scale, dest->pixel_scale);
	}
	int destx = -dest->offset_x;
	int desty = -dest->offset_y;
	int srcw  = dest->width;
	int srch  = dest->height;
	srcx += destx;
	srcy += desty;
	if (!clip(destx, desty, srcw, srch, srcx, srcy)) {
		return;
	}
	track(dest->tracker, destx, desty, srcw, srch);
	copy_blocks(
			Phys{dest->bits, dest->line_width, dest->pixel_scale}, destx, desty, Phys{bits, line_width, pixel_scale}, srcx, srcy,
			srcw, srch);
}

void Image_buffer8::s_put(Image_buffer* src, int destx, int desty) {
	s_blit(*src, destx, desty, "put");
}

void Image_buffer8::blit(const Image_buffer& src, int destx, int desty) {
	s_blit(src, destx, desty, "blit");
}

void Image_buffer8::s_blit(const Image_buffer& src, int destx, int desty, const char* op) {
	if (src.bits == nullptr) {
		std::cerr << "WTF! src.bits in Image_buffer8::" << op << " was 0!" << std::endl;
		return;
	}
	if (src.pixel_scale != pixel_scale) {
		log_mixed_scale(op, src.pixel_scale, pixel_scale);
	}
	int srcx = -src.offset_x;
	int srcy = -src.offset_y;
	int srcw = src.width;
	int srch = src.height;
	destx += srcx;
	desty += srcy;
	if (!clip(srcx, srcy, srcw, srch, destx, desty)) {
		return;
	}
	track(tracker, destx, desty, srcw, srch);
	copy_blocks(
			Phys{bits, line_width, pixel_scale}, destx, desty, Phys{src.bits, src.line_width, src.pixel_scale}, srcx, srcy, srcw,
			srch);
}

// The tracker gets every logical block the physical rectangle touches (floor
// for the start, ceiling for the end).
void Image_buffer8::put_phys(const unsigned char* src, int pw, int ph, int src_pitch, int px, int py) {
	if (src == nullptr || pw <= 0 || ph <= 0) {
		return;
	}
	const int       scale = pixel_scale;
	const long long x0    = std::max<long long>(px, static_cast<long long>(clipx) * scale);
	const long long y0    = std::max<long long>(py, static_cast<long long>(clipy) * scale);
	const long long x1    = std::min<long long>(static_cast<long long>(px) + pw, (static_cast<long long>(clipx) + clipw) * scale);
	const long long y1    = std::min<long long>(static_cast<long long>(py) + ph, (static_cast<long long>(clipy) + cliph) * scale);
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	const int lx0 = floor_div(x0, scale);
	const int ly0 = floor_div(y0, scale);
	track(tracker, lx0, ly0, ceil_div(x1, scale) - lx0, ceil_div(y1, scale) - ly0);
	const auto bytes = static_cast<size_t>(x1 - x0);
	for (long long y = y0; y < y1; y++) {
		std::memcpy(bits + y * line_width + x0, src + (y - py) * src_pitch + (x0 - px), bytes);
	}
}

// One std::rand() per logical pixel, row by row as at scale 1, so the
// generator advances as it does there.
void Image_buffer8::s_fill_static(int black, int gray, int white) {
	track(tracker, -offset_x, -offset_y, width, height);
	const Phys p{bits, line_width, pixel_scale};
	for (int y = 0; y < height; ++y) {
		unsigned char* row = p.at(-offset_x, y - offset_y);
		unsigned char* to  = row;
		for (int x = 0; x < width; ++x, to += pixel_scale) {
			int pix;
			switch (std::rand() % 5) {
			case 0:
			case 1:
				pix = black;
				break;
			case 2:
			case 3:
				pix = gray;
				break;
			default:
				pix = white;
				break;
			}
			std::memset(to, pix, pixel_scale);
		}
		dup_rows(row, width * pixel_scale, line_width, pixel_scale);
	}
}

std::unique_ptr<Image_buffer> Image_buffer8::s_create_another(int w, int h) {
	return std::make_unique<Image_buffer8>(w, h, pixel_scale);
}

/*
 *  8-bit color methods.
 */

void Image_buffer8::s_fill8(unsigned char pix) {
	track(tracker, -offset_x, -offset_y, width, height);
	fill_blocks(Phys{bits, line_width, pixel_scale}, -offset_x, -offset_y, width, height, pix);
}

void Image_buffer8::s_fill8(unsigned char pix, int srcw, int srch, int destx, int desty) {
	int srcx = 0;
	int srcy = 0;
	if (!clip(srcx, srcy, srcw, srch, destx, desty)) {
		return;
	}
	track(tracker, destx, desty, srcw, srch);
	fill_blocks(Phys{bits, line_width, pixel_scale}, destx, desty, srcw, srch, pix);
}

void Image_buffer8::s_fill_hline8(unsigned char pix, int srcw, int destx, int desty) {
	int srcx = 0;
	if (!clip_x(srcx, srcw, destx, desty)) {
		return;
	}
	track(tracker, destx, desty, srcw, 1);
	fill_blocks(Phys{bits, line_width, pixel_scale}, destx, desty, srcw, 1, pix);
}

// The clipping and the walk of draw_line8 in game px; each logical pixel is
// an S x S block (lines are S physical px thick).
void Image_buffer8::s_draw_line8(unsigned char val, int startx, int starty, int endx, int endy, const Xform_palette* xform) {
	// 16:16 fixed point
	using fixedu1616 = uint32;

	int cx;
	int cy;
	int cw;
	int ch;
	get_clip(cx, cy, cw, ch);
	// shrink clip by 1 pixel
	cw--;
	ch--;
	// check if entirely outside clip region
	if ((startx > cx + cw && endx > cx + cw) || (endx < cx && startx < cx) || (starty > cy + ch && endy > cy + ch)
		|| (endy < cy && starty < cy)) {
		return;
	}
	// If both points are oob it might be off screen but not always so make sure
	if (isoob(startx, starty, cx, cw, cy, ch) && isoob(endx, endy, cx, cw, cy, ch)) {
		const int tl = point_side_of_line(startx, starty, endx, endy, clipx, clipy);
		const int tr = point_side_of_line(startx, starty, endx, endy, clipx + clipw, clipy);
		const int bl = point_side_of_line(startx, starty, endx, endy, clipx, clipy + cliph);
		const int br = point_side_of_line(startx, starty, endx, endy, clipx + clipw, clipy + cliph);
		if ((tl < 0 && tr < 0 && br < 0 && bl < 0) || (tl > 0 && tr > 0 && br > 0 && bl > 0)) {
			return;
		}
	}
	if (!clipline(&startx, &endx, &starty, &endy, cx, cx + cw)) {
		return;
	}
	if (!clipline(&starty, &endy, &startx, &endx, cy, cy + ch)) {
		return;
	}

	// Dimension 0 is the longer one; dimension 1 is negated when it runs
	// backwards, as in the scale-1 walk.
	const bool steep  = std::abs(endx - startx) < std::abs(endy - starty);
	int        start0 = steep ? starty : startx;
	int        end0   = steep ? endy : endx;
	int        start1 = steep ? startx : starty;
	int        end1   = steep ? endx : endy;
	if (end0 < start0) {
		std::swap(end0, start0);
		std::swap(end1, start1);
	}
	int dir1 = 1;
	if (end1 < start1) {
		dir1   = -1;
		end1   = -end1;
		start1 = -start1;
	}
	const int delta0 = end0 - start0;
	const int delta1 = end1 - start1;
	// change in dim1 for each increment of dim0
	const fixedu1616 slope = (delta1 << 16) / std::max(delta0, 1);
	// relative dim1 of current pixel, offset by the rounding error of the
	// slope so that the end point is drawn
	fixedu1616 cur1 = (delta1 << 16) - slope * delta0;
	int        pos1 = start1;
	for (int pos0 = start0;; pos0++) {
		const int x = steep ? dir1 * pos1 : pos0;
		const int y = steep ? pos0 : dir1 * pos1;
		if (xform == nullptr) {
			s_fill8(val, 1, 1, x, y);
		} else {
			s_fill_translucent8(1, 1, x, y, *xform);
		}
		if (pos0 >= end0) {
			break;
		}
		cur1 += slope;
		pos1 += static_cast<int>(cur1 >> 16);
		cur1 &= 0xffff;
	}
}

void Image_buffer8::s_copy8(const unsigned char* src_pixels, int srcw, int srch, int destx, int desty) {
	if (!src_pixels) {
		std::cerr << "WTF! src_pixels in Image_buffer8::copy8 was 0!" << std::endl;
		return;
	}
	int       srcx      = 0;
	int       srcy      = 0;
	const int src_width = srcw;    // Save full source width.
	if (!clip(srcx, srcy, srcw, srch, destx, desty)) {
		return;
	}
	track(tracker, destx, desty, srcw, srch);
	const Phys           p{bits, line_width, pixel_scale};
	const unsigned char* from = src_pixels + srcy * src_width + srcx;
	for (int r = 0; r < srch; r++, from += src_width) {
		unsigned char* to = p.at(destx, desty + r);
		rep_row(to, from, srcw, pixel_scale);
		dup_rows(to, srcw * pixel_scale, line_width, pixel_scale);
	}
}

void Image_buffer8::s_copy_hline8(const unsigned char* src_pixels, int srcw, int destx, int desty) {
	int srcx = 0;
	if (!clip_x(srcx, srcw, destx, desty)) {
		return;
	}
	track(tracker, destx, desty, srcw, 1);
	unsigned char* to = Phys{bits, line_width, pixel_scale}.at(destx, desty);
	rep_row(to, src_pixels + srcx, srcw, pixel_scale);
	dup_rows(to, srcw * pixel_scale, line_width, pixel_scale);
}

// An opaque source pixel gives a block; a translucent one transforms each
// physical pixel under it.
void Image_buffer8::s_copy_hline_translucent8(
		const unsigned char* src_pixels, int srcw, int destx, int desty, int first_translucent, int last_translucent,
		const Xform_palette* xforms) {
	int srcx = 0;
	if (!clip_x(srcx, srcw, destx, desty)) {
		return;
	}
	track(tracker, destx, desty, srcw, 1);
	const unsigned char* from = src_pixels + srcx;
	unsigned char*       row  = Phys{bits, line_width, pixel_scale}.at(destx, desty);
	for (int k = 0; k < pixel_scale; k++, row += line_width) {
		unsigned char* to = row;
		for (int i = 0; i < srcw; i++, to += pixel_scale) {
			const unsigned char c = from[i];
			if (c >= first_translucent && c <= last_translucent) {
				const Xform_palette& xform = xforms[c - first_translucent];
				for (int j = 0; j < pixel_scale; j++) {
					to[j] = xform[to[j]];
				}
			} else {
				std::memset(to, c, pixel_scale);
			}
		}
	}
}

void Image_buffer8::s_fill_hline_translucent8(int srcw, int destx, int desty, const Xform_palette& xform) {
	int srcx = 0;
	if (!clip_x(srcx, srcw, destx, desty)) {
		return;
	}
	track(tracker, destx, desty, srcw, 1);
	xform_blocks(Phys{bits, line_width, pixel_scale}, destx, desty, srcw, 1, xform);
}

void Image_buffer8::s_fill_translucent8(int srcw, int srch, int destx, int desty, const Xform_palette& xform) {
	int srcx = 0;
	int srcy = 0;
	if (!clip(srcx, srcy, srcw, srch, destx, desty)) {
		return;
	}
	track(tracker, destx, desty, srcw, srch);
	xform_blocks(Phys{bits, line_width, pixel_scale}, destx, desty, srcw, srch, xform);
}

void Image_buffer8::s_copy_transparent8(const unsigned char* src_pixels, int srcw, int srch, int destx, int desty) {
	int       srcx      = 0;
	int       srcy      = 0;
	const int src_width = srcw;    // Save full source width.
	if (!clip(srcx, srcy, srcw, srch, destx, desty)) {
		return;
	}
	track(tracker, destx, desty, srcw, srch);
	const Phys           p{bits, line_width, pixel_scale};
	const unsigned char* from = src_pixels + srcy * src_width + srcx;
	for (int r = 0; r < srch; r++, from += src_width) {
		for (int i = 0; i < srcw; i++) {
			if (from[i] != 0) {
				fill_blocks(p, destx + i, desty + r, 1, 1, from[i]);
			}
		}
	}
}

unsigned char Image_buffer8::s_get_pixel8(int x, int y) {
	return *Phys{bits, line_width, pixel_scale}.at(x, y);
}

void Image_buffer8::s_put_pixel8(unsigned char pix, int x, int y) {
	if (x >= clipx && x < clipx + clipw && y >= clipy && y < clipy + cliph) {
		track(tracker, x, y, 1, 1);
		fill_blocks(Phys{bits, line_width, pixel_scale}, x, y, 1, 1, pix);
	}
}

// Every scan of the RLE data is one contiguous span, so each scan on screen
// is decoded (through trans, if not null) into the row buffer and drawn with
// copy_hline8, which clips it. A run is cut where it would pass the end of
// its scan, while the input still skips the whole run: well-formed data
// parses exactly as at scale 1, and malformed data cannot write past the
// row buffer.
void Image_buffer8::s_paint_rle(int xoff, int yoff, const unsigned char* in, const unsigned char* trans) {
	std::vector<unsigned char>& row    = rle_row();
	const int                   right  = clipx + clipw;
	const int                   bottom = clipy + cliph;
	int                         scanlen;
	while ((scanlen = little_endian::Read2(in)) != 0) {
		const int encoded    = scanlen & 1;    // Is it encoded?
		scanlen              = scanlen >> 1;
		const int  scanx     = xoff + static_cast<sint16>(little_endian::Read2(in));
		const int  scany     = yoff + static_cast<sint16>(little_endian::Read2(in));
		const bool on_screen = scanlen > 0 && !(scanx >= right || scany >= bottom || scany < clipy || scanx + scanlen < clipx);
		if (on_screen && (encoded || trans != nullptr) && row.size() < static_cast<size_t>(scanlen)) {
			row.resize(scanlen);
		}
		if (!encoded) {    // Raw data.
			if (on_screen) {
				if (trans == nullptr) {
					s_copy_hline8(in, scanlen, scanx, scany);
				} else {
					for (int i = 0; i < scanlen; i++) {
						row[i] = trans[in[i]];
					}
					s_copy_hline8(row.data(), scanlen, scanx, scany);
				}
			}
			in += scanlen;
			continue;
		}
		for (int b = 0; b < scanlen;) {
			const int run   = Read1(in);
			const int count = run >> 1;
			const int shown = std::min(count, scanlen - b);
			if (run & 1) {    // Repeat the next byte.
				const unsigned char pix = Read1(in);
				if (on_screen) {
					std::memset(row.data() + b, trans == nullptr ? pix : trans[pix], shown);
				}
			} else {
				if (on_screen) {
					for (int i = 0; i < shown; i++) {
						row[b + i] = trans == nullptr ? in[i] : trans[in[i]];
					}
				}
				in += count;
			}
			b += count;
		}
		if (on_screen) {
			s_copy_hline8(row.data(), scanlen, scanx, scany);
		}
	}
}
