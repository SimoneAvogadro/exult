/*
 *  hires_rules.h - Index semantics and hashes of the hi-res overrides
 *  (DESIGN.md sections 5.2-5.6): CRC32, FNV-1a-64, the T1 terrain key, the
 *  effective palette 0, the cycling ranges, rule P4 and the class-preserving
 *  reduction.
 *
 *  SDL-free and PNG-free. tools/hires (hirescheck.py, mkpack.py) implements
 *  the same definitions; both sides test them against the shared vectors and
 *  fixtures in tests/data/hires.
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

#ifndef HIRES_RULES_H
#define HIRES_RULES_H

#include <cstddef>
#include <cstdint>

namespace Hires {
	// CRC-32/ISO-HDLC (IEEE 802.3; zlib and PNG use it): reflected polynomial
	// 0xedb88320, initial value and final xor 0xffffffff. Table-driven, no zlib.
	// Pass the previous result as 'crc' to continue a running checksum.
	uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);

	// FNV-1a, 64 bit: offset 0xcbf29ce484222325, prime 0x100000001b3.
	constexpr uint64_t fnv1a64_offset = 0xcbf29ce484222325ULL;
	uint64_t           fnv1a64(const void* data, size_t len, uint64_t hash = fnv1a64_offset);

	// The engine's 6-bit to 8-bit conversion: Get_color8(v, 63, 100) in
	// imagewin/iwin8.cc, i.e. v * 255 / 63 clamped to 255. The clamp matters
	// for out-of-range 6-bit values such as BG palette 0, index 255 =
	// (250, 64, 1), which becomes (255, 255, 4) and not a uint8 wrap.
	constexpr uint8_t color8_from_6bit(uint8_t v) {
		const uint32_t c = (static_cast<uint32_t>(v) * 100U * 255U) / (100U * 63U);
		return c <= 255U ? static_cast<uint8_t>(c) : uint8_t{255};
	}

	// The effective palette 0 as the art sees it: 256 RGB triplets, 8 bit.
	struct Pal8 {
		uint8_t rgb[768] = {};
	};

	// Converts palette 0 as Palette::load leaves it (6-bit values, 768 bytes).
	Pal8 pal8_from_6bit(const uint8_t rgb6[768]);

	// CRC32 of the 768 bytes of Pal8.rgb (pack.txt palette_crc32, bundle header).
	// BG: 0xc9c2c0e7 (with a uint8 wrap at index 255 it would be 0x78d19732).
	uint32_t palette_crc32(const Pal8& pal);

	/*
	 *  T1 terrain key (section 5.2): FNV-1a-64 over the byte stream
	 *  "U7TK" | 0x01 | own[32] | pix[16384].
	 *  own: 256-bit map in row-major tile order, LSB first; bit t is set when
	 *       tile t resolves to a flat (non-RLE) frame.
	 *  pix: the 128x128 raster in which own cells hold their 64 flat pixels and
	 *       all other cells zeros.
	 *  The key does not depend on the fill under RLE tiles.
	 */
	constexpr int terrain_tiles  = 256;
	constexpr int terrain_side1x = 128;
	uint64_t      terrain_key_t1(const uint8_t own[32], const uint8_t pix[terrain_side1x * terrain_side1x]);
	// The same key from the terrain's tiles: tiles[t] is the 64-byte flat of
	// tile t when that tile is own, else nullptr.
	uint64_t terrain_key_t1(const uint8_t* const tiles[terrain_tiles]);

	/*
	 *  Index classes. Game_window::rotatecolours (gamewin.cc) cycles the ranges
	 *  E0-E7, E8-EF, F0-F3, F4-F7, F8-FB and FC-FE. Index 0xFF is not cycled:
	 *  it is the border/transparent colour and never allowed in an override (P0).
	 */
	constexpr int     num_cycle_ranges = 6;
	constexpr uint8_t border_index     = 0xff;

	// The cycle range (0-5) of an index, or -1 for a static index.
	int cycle_range(uint8_t index);

	/*
	 *  P4, the one cycling rule shared by the engine, hirescheck.py, QA and the
	 *  generators: a pixel of art in cycle range R is compliant iff its 1x parent
	 *  pixel or one of the parent's 8 neighbours is in R. The neighbours are
	 *  taken inside the override's own 1x source, clamped at its edge (the 8x8
	 *  flat of a tile, the 128x128 flat layer of a terrain).
	 *  art is (parent_w * scale) x (parent_h * scale), row-major, no padding.
	 *  Returns the number of non-compliant pixels, or -1 if a size is invalid.
	 */
	long p4_violations(const uint8_t* art, int scale, const uint8_t* parent, int parent_w, int parent_h);

	enum class P4_verdict {
		ok,
		warning,    // 0 < non-compliant pixels <= 0.5 %
		reject      // more than 0.5 %: 200 * noncompliant > total
	};

	P4_verdict p4_verdict(long noncompliant, long total);

	/*
	 *  Class-preserving mode reduction (section 5.6) from scale s_from to s_to
	 *  (s_from % s_to == 0, m = s_from / s_to). Per m x m block: if the 1x parent
	 *  pixel is in cycle range R and the block holds indices in R, the most
	 *  frequent of those; otherwise the most frequent non-cycling index (the
	 *  most frequent index if there is none). Ties go to the smallest index.
	 *  src is (parent_w * s_from) x (parent_h * s_from); dst receives
	 *  (parent_w * s_to) x (parent_h * s_to) pixels, row-major, no padding.
	 *  reduce(NN_s_from(x)) == NN_s_to(x) for every 1x image x.
	 *  Returns false (and writes nothing) if the arguments are invalid.
	 */
	bool reduce_mode(const uint8_t* src, int s_from, int s_to, const uint8_t* parent, int parent_w, int parent_h, uint8_t* dst);

	// Nearest-neighbour replication of a w x h image by an integer factor.
	void nn_upscale(const uint8_t* src, int w, int h, int scale, uint8_t* dst);
}    // namespace Hires

#endif
