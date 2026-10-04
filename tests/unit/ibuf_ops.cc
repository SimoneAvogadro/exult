/*
 *  ibuf_ops.cc - A deterministic stream of Image_buffer8 operations for the
 *  hi-res unit tests (see ibuf_ops.h).
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

#include "ibuf_ops.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <utility>

namespace hires_test {
	namespace {
		using Kind = Ibuf_op_kind;

		// Names and relative frequencies, in the order of Ibuf_op_kind. The
		// weights add up to 100; fill_all and fill_static overwrite the whole
		// buffer, so they are rare.
		constexpr const char* op_names[] = {
				"set_clip",
				"clear_clip",
				"fill_all",
				"fill_rect",
				"fill_hline",
				"draw_line",
				"copy8",
				"copy_hline",
				"copy_hline_translucent",
				"fill_hline_translucent",
				"fill_translucent",
				"copy_transparent",
				"get_pixel",
				"put_pixel",
				"copy",
				"get",
				"put",
				"fill_static",
				"shape_paint_rle",
				"paint_rle",
				"paint_rle_remapped",
				"paint_rle_translucent",
				"paint_rle_transformed",
				"paint_rle_outline",
				"paint_flat",
				"draw_box",
				"draw_beveled_box",
				"create_another",
				"is_visible",
		};

		constexpr int op_weights[] = {
				5,    // set_clip
				2,    // clear_clip
				1,    // fill_all
				7,    // fill_rect
				6,    // fill_hline
				8,    // draw_line
				6,    // copy8
				5,    // copy_hline
				5,    // copy_hline_translucent
				4,    // fill_hline_translucent
				4,    // fill_translucent
				4,    // copy_transparent
				3,    // get_pixel
				4,    // put_pixel
				4,    // copy
				3,    // get
				3,    // put
				1,    // fill_static
				4,    // shape_paint_rle
				3,    // paint_rle
				3,    // paint_rle_remapped
				3,    // paint_rle_translucent
				2,    // paint_rle_transformed
				2,    // paint_rle_outline
				2,    // paint_flat
				2,    // draw_box
				2,    // draw_beveled_box
				1,    // create_another
				1,    // is_visible
		};

		static_assert(sizeof(op_names) / sizeof(op_names[0]) == num_ibuf_op_kinds, "op_names must name every kind");
		static_assert(sizeof(op_weights) / sizeof(op_weights[0]) == num_ibuf_op_kinds, "op_weights must weigh every kind");

		Kind pick_kind(Rng& rng) {
			int total = 0;
			for (const int weight : op_weights) {
				total += weight;
			}
			int r = rng.range(0, total - 1);
			for (int k = 0; k < num_ibuf_op_kinds; k++) {
				if (r < op_weights[k]) {
					return static_cast<Kind>(k);
				}
				r -= op_weights[k];
			}
			return Kind::clear_clip;
		}

		// A pixel value that is translucent for the fixtures' tables 30 % of
		// the time; never 0xff (encode_rle's transparent value).
		unsigned char frame_colour(Rng& rng) {
			if (rng.chance(30)) {
				return static_cast<unsigned char>(rng.range(Ibuf_fixtures::first_xform, 0xfe));
			}
			return static_cast<unsigned char>(rng.range(0, Ibuf_fixtures::first_xform - 1));
		}

		unsigned char nearest_colour(const std::array<std::array<int, 3>, 256>& pal, int r, int g, int b) {
			int best      = 0;
			int best_dist = -1;
			for (int i = 0; i < 256; i++) {
				const int dr   = pal[i][0] - r;
				const int dg   = pal[i][1] - g;
				const int db   = pal[i][2] - b;
				const int dist = dr * dr + dg * dg + db * db;
				if (best_dist < 0 || dist < best_dist) {
					best      = i;
					best_dist = dist;
				}
			}
			return static_cast<unsigned char>(best);
		}

		unsigned char as_pixel(int value) {
			return static_cast<unsigned char>(value);
		}
	}    // namespace

	Ibuf_fixtures::Ibuf_fixtures(uint64_t seed) {
		Rng rng(seed);

		// A synthetic palette: 16 ramps of 14 shades (like a game palette),
		// then 32 random colours.
		std::array<std::array<int, 3>, 256> pal{};
		for (int ramp = 0; ramp < 16; ramp++) {
			const int base[3] = {rng.range(40, 255), rng.range(40, 255), rng.range(40, 255)};
			for (int shade = 0; shade < 14; shade++) {
				for (int c = 0; c < 3; c++) {
					pal[ramp * 14 + shade][c] = base[c] * (shade + 1) / 14;
				}
			}
		}
		for (int i = 16 * 14; i < 256; i++) {
			for (int c = 0; c < 3; c++) {
				pal[i][c] = rng.range(0, 255);
			}
		}

		// Translucency tables, as Palette::create_trans_table computes them.
		xforms.resize(num_xforms);
		for (auto& xform : xforms) {
			const int br    = rng.range(0, 255);
			const int bg    = rng.range(0, 255);
			const int bb    = rng.range(0, 255);
			const int alpha = rng.range(32, 224);
			for (int i = 0; i < 256; i++) {
				const int r     = (br * alpha) / 255 + (pal[i][0] * (255 - alpha)) / 255;
				const int g     = (bg * alpha) / 255 + (pal[i][1] * (255 - alpha)) / 255;
				const int b     = (bb * alpha) / 255 + (pal[i][2] * (255 - alpha)) / 255;
				xform.colors[i] = nearest_colour(pal, r, g, b);
			}
		}

		// Remap tables: a ramp shift, a swap of ramp pairs, random values and
		// a random permutation.
		remaps.resize(4);
		for (int i = 0; i < 256; i++) {
			remaps[0][i] = as_pixel((i & 0xf0) | ((i + 5) & 0x0f));
			remaps[1][i] = as_pixel(i < 0xe0 ? i ^ 0x10 : i);
			remaps[2][i] = rng.byte();
			remaps[3][i] = as_pixel(i);
		}
		for (int i = 255; i > 0; i--) {
			std::swap(remaps[3][i], remaps[3][rng.range(0, i)]);
		}

		block.resize(block_size);
		for (auto& pix : block) {
			const int r = rng.range(0, 99);
			pix         = r < 25 ? 0 : r < 30 ? 0xff : as_pixel(rng.range(1, 254));
		}
		line.resize(line_size);
		for (auto& pix : line) {
			const int r = rng.range(0, 99);
			pix         = r < 35 ? as_pixel(rng.range(first_xform, 0xfe)) : r < 40 ? 0xff : rng.byte();
		}

		// RLE frames: rows of transparent runs, repeated runs and literal runs.
		// Frame 0 is 64 px wide (the widest), frame 1 is 1 px wide, frame 2 is
		// 1 px tall and frame 3 is fully transparent (no scans at all).
		for (int f = 0; f < 24; f++) {
			const int w      = f == 0 ? 64 : f == 1 ? 1 : rng.range(1, 64);
			const int h      = f == 2 ? 1 : rng.range(1, 40);
			const int xleft  = rng.range(0, w - 1);
			const int yabove = rng.range(0, h - 1);

			std::vector<unsigned char> pixels(static_cast<size_t>(w) * h, 0xff);
			if (f != 3) {
				for (int y = 0; y < h; y++) {
					unsigned char* row = pixels.data() + static_cast<size_t>(y) * w;
					for (int x = 0; x < w;) {
						const int kind = rng.range(0, 9);
						int       len;
						if (kind < 3) {
							len = std::min(rng.range(1, 12), w - x);
							std::fill_n(row + x, len, 0xff);
						} else if (kind < 6) {
							len                     = std::min(rng.range(2, 24), w - x);
							const unsigned char pix = frame_colour(rng);
							std::fill_n(row + x, len, pix);
						} else {
							len = std::min(rng.range(1, 16), w - x);
							for (int i = 0; i < len; i++) {
								row[x + i] = frame_colour(rng);
							}
						}
						x += len;
					}
				}
			}
			rle_frames.push_back(std::make_unique<Shape_frame>(pixels.data(), w, h, xleft, yabove, true));
		}

		for (int f = 0; f < 4; f++) {
			std::vector<unsigned char> pixels(c_num_tile_bytes);
			for (auto& pix : pixels) {
				pix = as_pixel(rng.range(0, 254));
			}
			flat_frames.push_back(
					std::make_unique<Shape_frame>(pixels.data(), c_tilesize, c_tilesize, c_tilesize, c_tilesize, false));
		}
	}

	const char* ibuf_op_name(Ibuf_op_kind kind) {
		const int index = static_cast<int>(kind);
		return index >= 0 && index < num_ibuf_op_kinds ? op_names[index] : "?";
	}

	std::string Ibuf_op::describe() const {
		std::ostringstream out;
		out << ibuf_op_name(kind) << '(';
		for (size_t i = 0; i < arg.size(); i++) {
			out << (i ? "," : "") << arg[i];
		}
		out << ')';
		return out.str();
	}

	Ibuf_op make_ibuf_op(Rng& rng, const Ibuf_geometry& geom, const Ibuf_fixtures& fx) {
		const int W  = geom.width;
		const int H  = geom.height;
		const int AW = geom.aux_width;
		const int AH = geom.aux_height;
		assert(W > 0 && H > 0 && AW > 0 && AH > 0);
		// The logical extent is [X0, X0 + W) x [Y0, Y0 + H). Coordinates are
		// drawn relative to it, so the random draws do not depend on the offset.
		const int X0 = -geom.offset_x;
		const int Y0 = -geom.offset_y;

		const int num_rle  = static_cast<int>(fx.rle_frames.size());
		const int num_flat = static_cast<int>(fx.flat_frames.size());
		const int num_xf   = static_cast<int>(fx.xforms.size());
		const int num_map  = static_cast<int>(fx.remaps.size());

		Ibuf_op op;
		op.kind = pick_kind(rng);
		auto& a = op.arg;

		auto pix = [&rng]() {
			return static_cast<int>(rng.byte());
		};
		// A colour that is 0xff ("do not draw") 15 % of the time.
		auto box_colour = [&rng, &pix]() {
			return rng.chance(15) ? 0xff : pix();
		};
		// Logical x and y in [X0 + lo, X0 + hi] and [Y0 + lo, Y0 + hi].
		auto rx = [&rng, X0](int lo, int hi) {
			return X0 + rng.range(lo, hi);
		};
		auto ry = [&rng, Y0](int lo, int hi) {
			return Y0 + rng.range(lo, hi);
		};

		switch (op.kind) {
		case Kind::set_clip:
			// The rectangle always overlaps the buffer: set_clip() clamps it,
			// and an empty or outside clip breaks draw_line8 (see ibuf_ops.h).
			if (rng.chance(25)) {
				a[0] = rx(0, W - 1);
				a[1] = ry(0, H - 1);
				a[2] = rng.range(1, W - (a[0] - X0));
				a[3] = rng.range(1, H - (a[1] - Y0));
			} else {
				a[2] = rng.range(1, W + 16);
				a[3] = rng.range(1, H + 16);
				a[0] = rx(1 - a[2], W - 1);
				a[1] = ry(1 - a[3], H - 1);
			}
			break;
		case Kind::clear_clip:
			break;
		case Kind::fill_all:
			a[0] = pix();
			break;
		case Kind::fill_rect:
		case Kind::fill_translucent:
			// pix, w, h, x, y [, xform]; sizes <= 0 are allowed (no-ops).
			a[0] = pix();
			a[1] = rng.range(-2, W + 8);
			a[2] = rng.range(-2, H + 8);
			a[3] = rx(-W / 2 - 8, W + 4);
			a[4] = ry(-H / 2 - 8, H + 4);
			a[5] = rng.range(0, num_xf - 1);
			break;
		case Kind::fill_hline:
		case Kind::fill_hline_translucent:
			// pix, w, x, y [, xform]
			a[0] = pix();
			a[1] = rng.range(-2, W + 8);
			a[2] = rx(-W / 2 - 8, W + 4);
			a[3] = ry(-4, H + 3);
			a[4] = rng.range(0, num_xf - 1);
			break;
		case Kind::draw_line: {
			// pix, x0, y0, x1, y1, xform (-1: none)
			a[0] = pix();
			a[1] = rx(-W / 2 - 4, W + W / 2 + 4);
			a[2] = ry(-H / 2 - 4, H + H / 2 + 4);
			switch (rng.range(0, 9)) {
			case 0:    // A single point.
				a[3] = a[1];
				a[4] = a[2];
				break;
			case 1:    // Vertical.
				a[3] = a[1];
				a[4] = ry(-H / 2 - 4, H + H / 2 + 4);
				break;
			case 2:    // Horizontal.
				a[3] = rx(-W / 2 - 4, W + W / 2 + 4);
				a[4] = a[2];
				break;
			case 3:
			case 4:    // Short.
				a[3] = a[1] + rng.range(-4, 4);
				a[4] = a[2] + rng.range(-4, 4);
				break;
			default:
				a[3] = rx(-W / 2 - 4, W + W / 2 + 4);
				a[4] = ry(-H / 2 - 4, H + H / 2 + 4);
				break;
			}
			a[5] = rng.chance(30) ? rng.range(0, num_xf - 1) : -1;
			break;
		}
		case Kind::copy8:
		case Kind::copy_transparent:
			// offset in block, w, h, x, y
			a[1] = rng.range(1, 64);
			a[2] = rng.range(1, 64);
			a[0] = rng.range(0, Ibuf_fixtures::block_size - a[1] * a[2]);
			a[3] = rx(-a[1] - 4, W + 4);
			a[4] = ry(-a[2] - 4, H + 4);
			break;
		case Kind::copy_hline:
		case Kind::copy_hline_translucent:
			// offset in line, w, x, y
			a[1] = rng.chance(5) ? rng.range(1, Ibuf_fixtures::line_size)
								 : rng.range(1, std::min(Ibuf_fixtures::line_size, 2 * W + 16));
			a[0] = rng.range(0, Ibuf_fixtures::line_size - a[1]);
			a[2] = rx(-a[1] - 4, W + 4);
			a[3] = ry(-3, H + 2);
			break;
		case Kind::get_pixel:
			// x, y: inside the buffer (get_pixel8 does not clip).
			a[0] = rx(0, W - 1);
			a[1] = ry(0, H - 1);
			break;
		case Kind::put_pixel:
			// pix, x, y
			a[0] = pix();
			a[1] = rx(-3, W + 2);
			a[2] = ry(-3, H + 2);
			break;
		case Kind::copy: {
			// srcx, srcy, w, h, destx, desty: inside the buffer (copy() does
			// not clip at scale 1). Often a short move, like a scroll.
			a[2] = rng.range(1, W);
			a[3] = rng.range(1, H);
			a[0] = rx(0, W - a[2]);
			a[1] = ry(0, H - a[3]);
			if (rng.chance(40)) {
				a[4] = std::clamp(a[0] + rng.range(-8, 8), X0, X0 + W - a[2]);
				a[5] = std::clamp(a[1] + rng.range(-8, 8), Y0, Y0 + H - a[3]);
			} else {
				a[4] = rx(0, W - a[2]);
				a[5] = ry(0, H - a[3]);
			}
			break;
		}
		case Kind::get:
		case Kind::put:
			// x, y of the aux buffer's top-left corner in the target.
			a[0] = rx(-AW - 2, W + 1);
			a[1] = ry(-AH - 2, H + 1);
			break;
		case Kind::fill_static:
			// seed for std::srand, black, gray, white
			a[0] = static_cast<int>(rng.next32() & 0x7fffffff);
			a[1] = pix();
			a[2] = pix();
			a[3] = pix();
			break;
		case Kind::shape_paint_rle:
		case Kind::paint_rle:
		case Kind::paint_rle_remapped:
		case Kind::paint_rle_translucent:
		case Kind::paint_rle_transformed:
		case Kind::paint_rle_outline:
			// frame, x, y, then remap table, xform or outline colour.
			a[0] = rng.range(0, num_rle - 1);
			a[1] = rx(-72, W + 72);
			a[2] = ry(-48, H + 48);
			a[3] = op.kind == Kind::paint_rle_remapped      ? rng.range(0, num_map - 1)
				   : op.kind == Kind::paint_rle_transformed ? rng.range(0, num_xf - 1)
															: pix();
			break;
		case Kind::paint_flat:
			// frame, x, y (Shape_frame::paint draws a flat at x - 8, y - 8).
			a[0] = rng.range(0, num_flat - 1);
			a[1] = rx(-4, W + 12);
			a[2] = ry(-4, H + 12);
			break;
		case Kind::draw_box:
			// x, y, w, h, stroke width, fill colour, stroke colour
			a[0] = rx(-8, W);
			a[1] = ry(-8, H);
			a[2] = rng.range(-2, W / 2 + 12);
			a[3] = rng.range(-2, H / 2 + 12);
			a[4] = rng.range(0, 3);
			a[5] = box_colour();
			a[6] = box_colour();
			break;
		case Kind::draw_beveled_box:
			// x, y, w, h, depth, fill, top, top-right, bottom, bottom-left,
			// top-left/bottom-right (-1: none, the fill colour is used)
			a[0]  = rx(-8, W);
			a[1]  = ry(-8, H);
			a[2]  = rng.range(-2, W / 2 + 12);
			a[3]  = rng.range(-2, H / 2 + 12);
			a[4]  = rng.range(0, 3);
			a[5]  = box_colour();
			a[6]  = box_colour();
			a[7]  = box_colour();
			a[8]  = box_colour();
			a[9]  = box_colour();
			a[10] = rng.chance(50) ? -1 : box_colour();
			break;
		case Kind::create_another:
			// w, h, fill colour, x, y of the copy taken from the target.
			a[0] = rng.range(1, 24);
			a[1] = rng.range(1, 24);
			a[2] = pix();
			a[3] = rx(-30, W + 6);
			a[4] = ry(-30, H + 6);
			break;
		case Kind::is_visible:
			// x, y, w, h
			a[0] = rx(-W, 2 * W);
			a[1] = ry(-H, 2 * H);
			a[2] = rng.range(-4, W);
			a[3] = rng.range(-4, H);
			break;
		case Kind::count:
			break;
		}
		return op;
	}

	void apply_ibuf_op(const Ibuf_op& op, Image_buffer8& target, Image_buffer8& aux, const Ibuf_fixtures& fx, Ibuf_reads& reads) {
		const auto& a = op.arg;
		switch (op.kind) {
		case Kind::set_clip:
			target.set_clip(a[0], a[1], a[2], a[3]);
			break;
		case Kind::clear_clip:
			target.clear_clip();
			break;
		case Kind::fill_all:
			target.fill8(as_pixel(a[0]));
			break;
		case Kind::fill_rect:
			target.fill8(as_pixel(a[0]), a[1], a[2], a[3], a[4]);
			break;
		case Kind::fill_hline:
			target.fill_hline8(as_pixel(a[0]), a[1], a[2], a[3]);
			break;
		case Kind::draw_line:
			target.draw_line8(as_pixel(a[0]), a[1], a[2], a[3], a[4], a[5] < 0 ? nullptr : &fx.xforms[a[5]]);
			break;
		case Kind::copy8:
			target.copy8(fx.block.data() + a[0], a[1], a[2], a[3], a[4]);
			break;
		case Kind::copy_hline:
			target.copy_hline8(fx.line.data() + a[0], a[1], a[2], a[3]);
			break;
		case Kind::copy_hline_translucent:
			target.copy_hline_translucent8(
					fx.line.data() + a[0], a[1], a[2], a[3], Ibuf_fixtures::first_xform, 0xfe, fx.xforms.data());
			break;
		case Kind::fill_hline_translucent:
			target.fill_hline_translucent8(as_pixel(a[0]), a[1], a[2], a[3], fx.xforms[a[4]]);
			break;
		case Kind::fill_translucent:
			target.fill_translucent8(as_pixel(a[0]), a[1], a[2], a[3], a[4], fx.xforms[a[5]]);
			break;
		case Kind::copy_transparent:
			target.copy_transparent8(fx.block.data() + a[0], a[1], a[2], a[3], a[4]);
			break;
		case Kind::get_pixel:
			reads.add(target.get_pixel8(a[0], a[1]));
			break;
		case Kind::put_pixel:
			target.put_pixel8(as_pixel(a[0]), a[1], a[2]);
			break;
		case Kind::copy:
			target.copy(a[0], a[1], a[2], a[3], a[4], a[5]);
			break;
		case Kind::get:
			target.get(&aux, a[0], a[1]);
			break;
		case Kind::put:
			target.put(&aux, a[0], a[1]);
			break;
		case Kind::fill_static:
			std::srand(static_cast<unsigned int>(a[0]));
			target.fill_static(a[1], a[2], a[3]);
			break;
		case Kind::shape_paint_rle:
			fx.rle_frames[a[0]]->paint_rle(&target, a[1], a[2]);
			break;
		case Kind::paint_rle:
			target.paint_rle(a[1], a[2], fx.rle_frames[a[0]]->get_data());
			break;
		case Kind::paint_rle_remapped:
			fx.rle_frames[a[0]]->paint_rle_remapped(&target, a[1], a[2], fx.remaps[a[3]].data());
			break;
		case Kind::paint_rle_translucent:
			fx.rle_frames[a[0]]->paint_rle_translucent(&target, a[1], a[2], fx.xforms.data(), Ibuf_fixtures::num_xforms);
			break;
		case Kind::paint_rle_transformed:
			fx.rle_frames[a[0]]->paint_rle_transformed(&target, a[1], a[2], fx.xforms[a[3]]);
			break;
		case Kind::paint_rle_outline:
			fx.rle_frames[a[0]]->paint_rle_outline(&target, a[1], a[2], as_pixel(a[3]));
			break;
		case Kind::paint_flat:
			fx.flat_frames[a[0]]->paint(&target, a[1], a[2]);
			break;
		case Kind::draw_box:
			target.draw_box(a[0], a[1], a[2], a[3], a[4], as_pixel(a[5]), as_pixel(a[6]));
			break;
		case Kind::draw_beveled_box: {
			std::optional<uint8> tlbr;
			if (a[10] >= 0) {
				tlbr = as_pixel(a[10]);
			}
			target.draw_beveled_box(
					a[0], a[1], a[2], a[3], a[4], as_pixel(a[5]), as_pixel(a[6]), as_pixel(a[7]), as_pixel(a[8]), as_pixel(a[9]),
					tlbr);
			break;
		}
		case Kind::create_another: {
			// The new buffer's contents are uninitialised until filled. It is
			// read through the logical API, so the reads do not depend on the
			// pixel scale.
			auto other = target.create_another(a[0], a[1]);
			other->fill8(as_pixel(a[2]));
			target.get(other.get(), a[3], a[4]);
			const int w = static_cast<int>(other->get_width());
			const int h = static_cast<int>(other->get_height());
			reads.add(static_cast<uint64_t>(w));
			reads.add(static_cast<uint64_t>(h));
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					reads.add(other->get_pixel8(x, y));
				}
			}
			break;
		}
		case Kind::is_visible:
			reads.add(target.is_visible(a[0], a[1], a[2], a[3]) ? 1 : 0);
			break;
		case Kind::count:
			break;
		}
	}
}    // namespace hires_test
