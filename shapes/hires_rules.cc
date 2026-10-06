/*
 *  hires_rules.cc - Index semantics and hashes of the hi-res overrides.
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

#include "hires_rules.h"

#include <array>
#include <cstring>
#include <vector>

namespace {
	std::array<uint32_t, 256> make_crc_table() {
		std::array<uint32_t, 256> table{};
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) {
				c = (c & 1U) != 0 ? 0xedb88320U ^ (c >> 1) : c >> 1;
			}
			table[i] = c;
		}
		return table;
	}

	std::array<int8_t, 256> make_cycle_table() {
		std::array<int8_t, 256> table{};
		table.fill(-1);
		// Game_window::rotatecolours: (start, count) per range.
		constexpr int ranges[Hires::num_cycle_ranges][2] = {
				{0xe0, 8},
                {0xe8, 8},
                {0xf0, 4},
                {0xf4, 4},
                {0xf8, 4},
                {0xfc, 3}
        };
		for (int r = 0; r < Hires::num_cycle_ranges; r++) {
			for (int i = 0; i < ranges[r][1]; i++) {
				table[ranges[r][0] + i] = static_cast<int8_t>(r);
			}
		}
		return table;
	}

	const std::array<int8_t, 256>& cycle_table() {
		static const std::array<int8_t, 256> table = make_cycle_table();
		return table;
	}

	// Bit r set: a pixel of cycle range r is allowed (P4) above this parent pixel.
	std::vector<uint8_t> p4_allowed(const uint8_t* parent, int w, int h) {
		const auto&          cyc = cycle_table();
		std::vector<uint8_t> allowed(static_cast<size_t>(w) * h);
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				uint8_t bits = 0;
				for (int dy = -1; dy <= 1; dy++) {
					const int ny = y + dy;
					if (ny < 0 || ny >= h) {
						continue;    // Clamped: the edge pixel is already in the set.
					}
					for (int dx = -1; dx <= 1; dx++) {
						const int nx = x + dx;
						if (nx < 0 || nx >= w) {
							continue;
						}
						const int r = cyc[parent[static_cast<size_t>(ny) * w + nx]];
						if (r >= 0) {
							bits |= static_cast<uint8_t>(1U << r);
						}
					}
				}
				allowed[static_cast<size_t>(y) * w + x] = bits;
			}
		}
		return allowed;
	}
}    // namespace

namespace Hires {
	uint32_t crc32(const void* data, size_t len, uint32_t crc) {
		static const std::array<uint32_t, 256> table = make_crc_table();

		const auto* bytes = static_cast<const uint8_t*>(data);
		uint32_t    c     = crc ^ 0xffffffffU;
		for (size_t i = 0; i < len; i++) {
			c = table[(c ^ bytes[i]) & 0xffU] ^ (c >> 8);
		}
		return c ^ 0xffffffffU;
	}

	uint64_t fnv1a64(const void* data, size_t len, uint64_t hash) {
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < len; i++) {
			hash ^= bytes[i];
			hash *= 0x100000001b3ULL;
		}
		return hash;
	}

	Pal8 pal8_from_6bit(const uint8_t rgb6[768]) {
		Pal8 pal;
		for (int i = 0; i < 768; i++) {
			pal.rgb[i] = color8_from_6bit(rgb6[i]);
		}
		return pal;
	}

	uint32_t palette_crc32(const Pal8& pal) {
		return crc32(pal.rgb, sizeof(pal.rgb));
	}

	uint64_t terrain_key_t1(const uint8_t own[32], const uint8_t pix[terrain_side1x * terrain_side1x]) {
		static const uint8_t prefix[5] = {'U', '7', 'T', 'K', 0x01};

		uint64_t key = fnv1a64(prefix, sizeof(prefix));
		key          = fnv1a64(own, 32, key);
		return fnv1a64(pix, static_cast<size_t>(terrain_side1x) * terrain_side1x, key);
	}

	uint64_t terrain_key_t1(const uint8_t* const tiles[terrain_tiles]) {
		uint8_t              own[32] = {};
		std::vector<uint8_t> pix(static_cast<size_t>(terrain_side1x) * terrain_side1x);
		for (int t = 0; t < terrain_tiles; t++) {
			if (tiles[t] == nullptr) {
				continue;
			}
			own[t >> 3] |= static_cast<uint8_t>(1U << (t & 7));
			const int tx = t % 16;
			const int ty = t / 16;
			for (int y = 0; y < 8; y++) {
				std::memcpy(&pix[static_cast<size_t>(ty * 8 + y) * terrain_side1x + tx * 8], tiles[t] + y * 8, 8);
			}
		}
		return terrain_key_t1(own, pix.data());
	}

	int cycle_range(uint8_t index) {
		return cycle_table()[index];
	}

	long p4_violations(const uint8_t* art, int scale, const uint8_t* parent, int parent_w, int parent_h) {
		if (art == nullptr || parent == nullptr || scale < 1 || parent_w < 1 || parent_h < 1) {
			return -1;
		}
		const auto&                cyc     = cycle_table();
		const std::vector<uint8_t> allowed = p4_allowed(parent, parent_w, parent_h);
		const int                  w       = parent_w * scale;
		const int                  h       = parent_h * scale;
		long                       bad     = 0;
		for (int y = 0; y < h; y++) {
			const uint8_t* row      = art + static_cast<size_t>(y) * w;
			const uint8_t* allowrow = allowed.data() + static_cast<size_t>(y / scale) * parent_w;
			for (int x = 0; x < w; x++) {
				const int r = cyc[row[x]];
				if (r >= 0 && (allowrow[x / scale] & (1U << r)) == 0) {
					bad++;
				}
			}
		}
		return bad;
	}

	P4_verdict p4_verdict(long noncompliant, long total) {
		if (noncompliant * 200 > total) {
			return P4_verdict::reject;
		}
		return noncompliant > 0 ? P4_verdict::warning : P4_verdict::ok;
	}

	bool reduce_mode(const uint8_t* src, int s_from, int s_to, const uint8_t* parent, int parent_w, int parent_h, uint8_t* dst) {
		if (src == nullptr || parent == nullptr || dst == nullptr || s_to < 1 || s_from < s_to || s_from % s_to != 0 || parent_w < 1
			|| parent_h < 1) {
			return false;
		}
		const auto& cyc         = cycle_table();
		const int   m           = s_from / s_to;
		const int   src_w       = parent_w * s_from;
		const int   dst_w       = parent_w * s_to;
		const int   dst_h       = parent_h * s_to;
		uint16_t    counts[256] = {};
		for (int by = 0; by < dst_h; by++) {
			for (int bx = 0; bx < dst_w; bx++) {
				const uint8_t* block = src + static_cast<size_t>(by) * m * src_w + static_cast<size_t>(bx) * m;
				for (int j = 0; j < m; j++) {
					for (int i = 0; i < m; i++) {
						counts[block[static_cast<size_t>(j) * src_w + i]]++;
					}
				}
				const int pr = cyc[parent[static_cast<size_t>(by / s_to) * parent_w + bx / s_to]];
				// The most frequent index of the block among those that 'pick'
				// accepts; ties go to the smallest index.
				auto mode = [&](auto pick) {
					int best  = -1;
					int bestn = 0;
					for (int j = 0; j < m; j++) {
						for (int i = 0; i < m; i++) {
							const int v = block[static_cast<size_t>(j) * src_w + i];
							if (!pick(v)) {
								continue;
							}
							const int n = counts[v];
							if (n > bestn || (n == bestn && v < best)) {
								best  = v;
								bestn = n;
							}
						}
					}
					return best;
				};
				int out = -1;
				if (pr >= 0) {
					out = mode([&](int v) {
						return cyc[v] == pr;
					});
				}
				if (out < 0) {
					out = mode([&](int v) {
						return cyc[v] < 0;
					});
				}
				if (out < 0) {
					out = mode([](int) {
						return true;
					});
				}
				dst[static_cast<size_t>(by) * dst_w + bx] = static_cast<uint8_t>(out);
				for (int j = 0; j < m; j++) {
					for (int i = 0; i < m; i++) {
						counts[block[static_cast<size_t>(j) * src_w + i]] = 0;
					}
				}
			}
		}
		return true;
	}

	void nn_upscale(const uint8_t* src, int w, int h, int scale, uint8_t* dst) {
		const int dw = w * scale;
		for (int y = 0; y < h * scale; y++) {
			const uint8_t* row = src + static_cast<size_t>(y / scale) * w;
			uint8_t*       out = dst + static_cast<size_t>(y) * dw;
			for (int x = 0; x < dw; x++) {
				out[x] = row[x / scale];
			}
		}
	}
}    // namespace Hires
