/*
 *  rle_writer.h - Test-only writer and reference decoder for the RLE frame
 *  data that Image_buffer8::paint_rle reads.
 *
 *  Shape_frame::encode_rle (shapes/vgafile.cc) writes the same format, but only
 *  well-formed frames, and its fixed runs[200] array (P6) limits the width it
 *  can encode safely. This writer emits scans of any length the format allows
 *  (15 bits: 32,767 px) and malformed data on demand, such as runs that extend
 *  past the length in the scan header. The scaled RLE painters of WP-04 are
 *  tested with it (wide scans, run clamping); the decoder is their reference.
 *
 *  Format (little endian). Per scan: u16 (length << 1 | encoded), then s16 x
 *  and s16 y relative to the paint origin. A raw scan (encoded = 0) continues
 *  with `length` pixels. An encoded scan continues with runs until `length`
 *  pixels are covered; a run is one byte (count << 1 | repeat), count 0-127,
 *  followed by one pixel (repeat) or `count` pixels (literal). A u16 0 ends
 *  the data, so a raw scan needs at least one pixel (its header would be that
 *  terminator); an encoded scan may be empty. bytes() can still write an early
 *  terminator on purpose.
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

#ifndef RLE_WRITER_H
#define RLE_WRITER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace hires_test {
	class Rle_writer {
	public:
		constexpr static int max_scan = 32767;    // 15-bit scan length.
		constexpr static int max_run  = 127;      // 7-bit run count.

		enum class Style {
			raw,         // Raw scans only.
			encoded,     // Encoded scans only.
			alternate    // Raw and encoded scans in turn.
		};

		// A raw scan of pixels.size() pixels (at least one).
		Rle_writer& raw(int x, int y, const std::vector<unsigned char>& pixels) {
			header(static_cast<int>(pixels.size()), false, x, y);
			out.insert(out.end(), pixels.begin(), pixels.end());
			return *this;
		}

		// An encoded scan that covers `pixels` exactly with well-formed runs:
		// a repeat run for 2 or more equal pixels, literal runs otherwise.
		Rle_writer& encoded(int x, int y, const std::vector<unsigned char>& pixels) {
			const int n = static_cast<int>(pixels.size());
			header(n, true, x, y);
			for (int i = 0; i < n;) {
				int same = 1;
				while (i + same < n && same < max_run && pixels[i + same] == pixels[i]) {
					same++;
				}
				if (same >= 2) {
					repeat_run(same, pixels[i]);
					i += same;
					continue;
				}
				int end = i;
				while (end < n && end - i < max_run && (end == n - 1 || pixels[end] != pixels[end + 1])) {
					end++;
				}
				literal_run(std::vector<unsigned char>(pixels.begin() + i, pixels.begin() + end));
				i = end;
			}
			return *this;
		}

		// Building blocks for malformed data: a header that declares `length`
		// pixels, followed by runs whose counts need not add up to it.
		Rle_writer& begin_encoded(int x, int y, int length) {
			header(length, true, x, y);
			return *this;
		}

		Rle_writer& repeat_run(int count, unsigned char pixel) {
			check_run(count);
			out.push_back(static_cast<unsigned char>((count << 1) | 1));
			out.push_back(pixel);
			return *this;
		}

		Rle_writer& literal_run(const std::vector<unsigned char>& pixels) {
			const int count = static_cast<int>(pixels.size());
			check_run(count);
			out.push_back(static_cast<unsigned char>(count << 1));
			out.insert(out.end(), pixels.begin(), pixels.end());
			return *this;
		}

		// Arbitrary bytes, for truncated or garbage data.
		Rle_writer& bytes(const std::vector<unsigned char>& data) {
			out.insert(out.end(), data.begin(), data.end());
			return *this;
		}

		// The w x h raster `pixels` (row by row), leaving out `transparent`
		// pixels, with its origin at (xleft, yabove) as in
		// Shape_frame::encode_rle. Segments longer than max_scan are split.
		Rle_writer& raster(
				const unsigned char* pixels, int w, int h, int xleft, int yabove, Style style, unsigned char transparent = 0xff) {
			bool use_raw = style != Style::encoded;
			for (int y = 0; y < h; y++) {
				const unsigned char* row = pixels + static_cast<size_t>(y) * w;
				for (int x = 0; x < w;) {
					if (row[x] == transparent) {
						x++;
						continue;
					}
					int end = x;
					while (end < w && end - x < max_scan && row[end] != transparent) {
						end++;
					}
					const std::vector<unsigned char> scan(row + x, row + end);
					if (use_raw) {
						raw(x - xleft, y - yabove, scan);
					} else {
						encoded(x - xleft, y - yabove, scan);
					}
					if (style == Style::alternate) {
						use_raw = !use_raw;
					}
					x = end;
				}
			}
			return *this;
		}

		// Appends the terminating u16 0 and returns the data; the writer is
		// empty afterwards.
		std::vector<unsigned char> finish() {
			put16(0);
			return std::exchange(out, std::vector<unsigned char>());
		}

	private:
		std::vector<unsigned char> out;

		void put16(int value) {
			out.push_back(static_cast<unsigned char>(value & 0xff));
			out.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
		}

		void header(int length, bool is_encoded, int x, int y) {
			if (length < 0 || length > max_scan) {
				throw std::invalid_argument("Rle_writer: scan length out of range");
			}
			if (length == 0 && !is_encoded) {
				// The header word would be 0, the end of the data.
				throw std::invalid_argument("Rle_writer: a raw scan needs at least one pixel");
			}
			if (x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX) {
				throw std::invalid_argument("Rle_writer: scan position out of range");
			}
			put16((length << 1) | (is_encoded ? 1 : 0));
			put16(x & 0xffff);
			put16(y & 0xffff);
		}

		static void check_run(int count) {
			if (count < 0 || count > max_run) {
				throw std::invalid_argument("Rle_writer: run count out of range");
			}
		}
	};

	enum class Rle_policy {
		strict,    // A run past the scan length is an error.
		clamp      // Such a run is cut at the scan length; the input still
				   // skips the whole run, and the scan ends there.
	};

	/*
	 *  Decodes RLE data and calls plot(x, y, pixel) for each pixel, at
	 *  positions relative to (xoff, yoff). Returns false for malformed data
	 *  (truncated, no terminator, or under `strict` a run past the scan
	 *  length); it never reads past data + size.
	 */
	template <typename Plot>
	bool decode_rle(const unsigned char* data, size_t size, int xoff, int yoff, Rle_policy policy, Plot&& plot) {
		size_t pos = 0;

		auto read16 = [&](int& value) {
			if (size - pos < 2) {
				return false;
			}
			value = data[pos] | (data[pos + 1] << 8);
			pos += 2;
			return true;
		};
		for (;;) {
			int word;
			if (!read16(word)) {
				return false;
			}
			if (word == 0) {
				return true;
			}
			const int length  = word >> 1;
			const int encoded = word & 1;
			int       sx;
			int       sy;
			if (!read16(sx) || !read16(sy)) {
				return false;
			}
			const int x = xoff + static_cast<int16_t>(static_cast<uint16_t>(sx));
			const int y = yoff + static_cast<int16_t>(static_cast<uint16_t>(sy));
			if (!encoded) {
				if (size - pos < static_cast<size_t>(length)) {
					return false;
				}
				for (int i = 0; i < length; i++) {
					plot(x + i, y, data[pos + i]);
				}
				pos += length;
				continue;
			}
			for (int b = 0; b < length;) {
				if (pos >= size) {
					return false;
				}
				const int run    = data[pos++];
				const int count  = run >> 1;
				const int repeat = run & 1;
				if (count > length - b && policy == Rle_policy::strict) {
					return false;
				}
				const int shown = std::min(count, length - b);
				if (repeat) {
					if (pos >= size) {
						return false;
					}
					const unsigned char pixel = data[pos++];
					for (int i = 0; i < shown; i++) {
						plot(x + b + i, y, pixel);
					}
				} else {
					if (size - pos < static_cast<size_t>(count)) {
						return false;
					}
					for (int i = 0; i < shown; i++) {
						plot(x + b + i, y, data[pos + i]);
					}
					pos += count;
				}
				b += count;
			}
		}
	}

	template <typename Plot>
	bool decode_rle(const std::vector<unsigned char>& data, int xoff, int yoff, Rle_policy policy, Plot&& plot) {
		return decode_rle(data.data(), data.size(), xoff, yoff, policy, std::forward<Plot>(plot));
	}
}    // namespace hires_test

#endif
