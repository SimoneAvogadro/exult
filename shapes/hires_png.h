/*
 *  hires_png.h - Raw-index PNG reader and writer for the hi-res overrides
 *  (DESIGN.md sections 3.5 and 5.3).
 *
 *  The reader returns the raw palette indices (no ipack/Studio rotation, no
 *  palette lookup), the PLTE, the presence of tRNS and the text chunks from
 *  before and after IDAT. It is hardened for files from untrusted pack
 *  directories:
 *    - the file is read once (up to twice the raw image size plus 64 MiB; a
 *      larger file is judged on its IHDR alone), its chunk structure and
 *      CRCs are checked, and the IHDR size must equal the expected size
 *      (rule F3) before libpng decodes anything or any pixel buffer is
 *      allocated;
 *    - libpng gets only the chunks the reader uses (the critical ones, tRNS
 *      and text chunks up to 1 MiB), so editor metadata such as ICC
 *      profiles, EXIF or XMP never makes a file unreadable, as in
 *      hirescheck.py;
 *    - libpng runs with user limits and a chunk allocation cap, under a
 *      setjmp frame whose locals are POD and whose heap memory is owned by
 *      objects constructed before setjmp, so a longjmp on a truncated or
 *      corrupt file skips no destructor and leaks nothing.
 *  Without libpng (HAVE_PNG_H undefined) both functions return
 *  Png_status::no_png_support and the engine falls back to NN.
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

#ifndef HIRES_PNG_H
#define HIRES_PNG_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Hires {
	enum class Png_status {
		ok,
		no_png_support,    // Built without libpng.
		open_error,        // The file cannot be opened or read.
		read_error,        // Not a PNG, truncated, CRC error, bad data, libpng error, too large.
		wrong_size,        // F3: the IHDR size differs from the expected size.
		not_palette,       // F1: colour type is not 3 (palette).
		write_error        // Writer only.
	};

	const char* png_status_name(Png_status status);

	// True when the reader and writer are compiled in (HAVE_PNG_H).
	bool have_png_support();

	struct Png_text {
		std::string key;
		std::string value;
	};

	struct Indexed_png {
		int                   width      = 0;
		int                   height     = 0;
		int                   bit_depth  = 0;
		int                   color_type = -1;
		int                   interlace  = 0;
		std::vector<uint8_t>  pixels;    // width * height raw indices, row-major (depths < 8 expanded).
		std::vector<uint8_t>  plte;      // num_palette RGB triplets.
		int                   num_palette = 0;
		bool                  has_trns    = false;
		std::vector<Png_text> text;     // tEXt, zTXt and iTXt up to 1 MiB, before and after IDAT, in file order.
		std::string           error;    // Reason for a failed read.

		// The first text with this key (file order, as hirescheck.py), or nullptr.
		const std::string* find_text(const char* key) const;
	};

	/*
	 *  Reads a palette PNG. With expect_w and expect_h > 0 the IHDR size must
	 *  equal them (else wrong_size, before decoding); with 0 any size up to
	 *  2048 x 2048 is accepted. Check order, as hirescheck.py: unreadable file
	 *  (read_error), size (wrong_size), colour type (not_palette), decode.
	 *  The status of a failed read leaves 'out' with the IHDR fields known so
	 *  far and 'error' set.
	 */
	Png_status read_indexed_png(const std::filesystem::path& file, int expect_w, int expect_h, Indexed_png& out);
	Png_status read_indexed_png(const uint8_t* data, size_t len, int expect_w, int expect_h, Indexed_png& out);

	struct Png_write_options {
		int                   bit_depth = 8;    // 1, 2, 4 or 8; the indices must fit.
		std::vector<uint8_t>  trns;             // tRNS alpha values; empty = no tRNS chunk.
		std::vector<Png_text> text_before;      // tEXt chunks written before IDAT.
		std::vector<Png_text> text_after;       // tEXt chunks written after IDAT.
		bool                  interlace = false;
	};

	/*
	 *  Writes a palette PNG of raw indices with a PLTE of num_palette entries
	 *  (1-256). The output is deterministic: no time chunk, filter none, zlib
	 *  level 9. The file variant writes the complete image to 'file' in one
	 *  go (tools should write to *.tmp and rename).
	 */
	Png_status encode_indexed_png(
			int width, int height, const uint8_t* pixels, const uint8_t* plte, int num_palette, const Png_write_options& options,
			std::vector<uint8_t>& out);
	Png_status write_indexed_png(
			const std::filesystem::path& file, int width, int height, const uint8_t* pixels, const uint8_t* plte, int num_palette,
			const Png_write_options& options = Png_write_options());

	/*
	 *  Allocation statistics of the reader (test hook, section 6.2): every
	 *  allocation libpng makes through the reader's allocator, plus the
	 *  reader's own file and pixel buffers. Not thread-safe, like the engine.
	 */
	struct Png_alloc_stats {
		size_t calls   = 0;
		size_t bytes   = 0;
		size_t largest = 0;
	};

	Png_alloc_stats png_alloc_stats();
	void            reset_png_alloc_stats();
}    // namespace Hires

#endif
