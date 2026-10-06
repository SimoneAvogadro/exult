/*
 *  hires_png.cc - Raw-index PNG reader and writer for the hi-res overrides.
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

#include "hires_png.h"

#include "hires_rules.h"
#include "ignore_unused_variable_warning.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

#ifdef HAVE_PNG_H
#	include <png.h>

#	include <csetjmp>
#endif

namespace {
	Hires::Png_alloc_stats alloc_stats;

#ifdef HAVE_PNG_H
	void note_alloc(size_t size) {
		alloc_stats.calls++;
		alloc_stats.bytes += size;
		alloc_stats.largest = std::max(alloc_stats.largest, size);
	}

	// Size limit for "any size" reads (expect_w/expect_h = 0): 128 x S for S up to 16.
	constexpr int max_any_side = 2048;

	// libpng's limit for one ancillary chunk and for decompressed text
	// (png_set_chunk_malloc_max): a longer chunk is a hard libpng error.
	constexpr size_t chunk_limit = size_t{1} << 20;

	// Room in a file for chunks besides the image data (metadata such as ICC
	// profiles, EXIF or XMP, which strip_chunks() drops before decoding).
	constexpr size_t max_metadata_bytes = size_t{64} << 20;

	uint32_t get_be32(const uint8_t* p) {
		return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8)
			   | static_cast<uint32_t>(p[3]);
	}

	constexpr uint8_t png_signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

	/*
	 *  The chunk structure of a PNG, checked like hirescheck.py's parser does:
	 *  signature, complete chunks up to IEND, CRC of every critical chunk
	 *  (ancillary chunks with a bad CRC are skipped, as libpng does), IHDR
	 *  first and 13 bytes long, PLTE length a multiple of 3 in 3..768.
	 *  'complete' = false checks only the signature and the IHDR (used on the
	 *  first bytes of a file that is too large to read).
	 */
	struct Png_structure {
		uint32_t width      = 0;
		uint32_t height     = 0;
		int      bit_depth  = 0;
		int      color_type = -1;
		int      interlace  = 0;
		bool     has_plte   = false;
		bool     has_trns   = false;
	};

	bool scan_structure(const uint8_t* data, size_t len, bool complete, Png_structure& out, std::string& error) {
		if (len < sizeof(png_signature) || std::memcmp(data, png_signature, sizeof(png_signature)) != 0) {
			error = "not a PNG (bad signature)";
			return false;
		}
		size_t pos       = sizeof(png_signature);
		bool   seen_ihdr = false;
		while (true) {
			if (len - pos < 8) {
				error = "truncated PNG (no IEND)";
				return false;
			}
			const uint32_t n    = get_be32(data + pos);
			const uint8_t* type = data + pos + 4;
			if (len - pos - 8 < 4 || static_cast<uint64_t>(n) > len - pos - 12) {
				error = "truncated chunk";
				return false;
			}
			const uint8_t* body     = data + pos + 8;
			const uint32_t crc      = get_be32(body + n);
			const bool     critical = (type[0] & 0x20) == 0;
			const char     name[5]
					= {static_cast<char>(type[0]), static_cast<char>(type[1]), static_cast<char>(type[2]),
					   static_cast<char>(type[3]), '\0'};
			pos += 12 + static_cast<size_t>(n);
			if (Hires::crc32(type, 4U + n) != crc) {
				if (critical) {
					error = std::string("CRC error in ") + name;
					return false;
				}
				continue;
			}
			if (std::memcmp(type, "IHDR", 4) == 0) {
				if (n != 13) {
					error = "bad IHDR";
					return false;
				}
				out.width      = get_be32(body);
				out.height     = get_be32(body + 4);
				out.bit_depth  = body[8];
				out.color_type = body[9];
				out.interlace  = body[12];
				seen_ihdr      = true;
				if (!complete) {
					return true;
				}
			} else if (!seen_ihdr) {
				error = "IHDR is not the first chunk";
				return false;
			} else if (std::memcmp(type, "PLTE", 4) == 0) {
				if (n % 3 != 0 || n == 0 || n > 768) {
					error = "bad PLTE length";
					return false;
				}
				out.has_plte = true;
			} else if (std::memcmp(type, "tRNS", 4) == 0) {
				out.has_trns = true;
			} else if (std::memcmp(type, "IEND", 4) == 0) {
				return true;
			}
		}
	}

	// The checks hirescheck.py makes before decoding, in its order: F3, then F1.
	Hires::Png_status check_header(const Png_structure& st, int expect_w, int expect_h, Hires::Indexed_png& out) {
		out.width      = static_cast<int>(std::min<uint32_t>(st.width, 0x7fffffffU));
		out.height     = static_cast<int>(std::min<uint32_t>(st.height, 0x7fffffffU));
		out.bit_depth  = st.bit_depth;
		out.color_type = st.color_type;
		out.interlace  = st.interlace;
		const bool any = expect_w <= 0 || expect_h <= 0;
		if (any ? (st.width == 0 || st.height == 0 || st.width > max_any_side || st.height > max_any_side)
				: (st.width != static_cast<uint32_t>(expect_w) || st.height != static_cast<uint32_t>(expect_h))) {
			out.error = "size " + std::to_string(st.width) + "x" + std::to_string(st.height) + ", expected "
						+ (any ? std::string("at most ") + std::to_string(max_any_side) + "x" + std::to_string(max_any_side)
							   : std::to_string(expect_w) + "x" + std::to_string(expect_h));
			return Hires::Png_status::wrong_size;
		}
		if (st.color_type != 3) {
			out.error = "colour type " + std::to_string(st.color_type);
			return Hires::Png_status::not_palette;
		}
		return Hires::Png_status::ok;
	}

	// Largest file the reader loads: twice the raw image plus room for metadata.
	size_t max_file_size(int expect_w, int expect_h) {
		const size_t w = static_cast<size_t>(expect_w > 0 ? expect_w : max_any_side);
		const size_t h = static_cast<size_t>(expect_h > 0 ? expect_h : max_any_side);
		return 2 * h * (w + 1) + max_metadata_bytes;
	}

	// The chunks libpng gets: the critical ones, and tRNS and the text chunks up
	// to chunk_limit bytes. hirescheck.py reads nothing else either.
	bool keep_chunk(const uint8_t* type, size_t length) {
		if ((type[0] & 0x20) == 0) {
			return true;
		}
		const bool used = std::memcmp(type, "tRNS", 4) == 0 || std::memcmp(type, "tEXt", 4) == 0
						  || std::memcmp(type, "zTXt", 4) == 0 || std::memcmp(type, "iTXt", 4) == 0;
		return used && length <= chunk_limit;
	}

	/*
	 *  Copies the PNG without the ancillary chunks it does not use (ICC
	 *  profiles, EXIF, XMP, physical size, private chunks, oversized text),
	 *  up to IEND. The structure scan has checked those chunks already, so
	 *  big metadata from an editor can neither hit libpng's chunk limit nor
	 *  make the file unreadable, as with hirescheck.py. Returns false, and
	 *  leaves 'out' empty, when nothing is dropped.
	 */
	bool strip_chunks(const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
		out.clear();
		// Calls f(offset, size with header and CRC, kept) for each chunk up to IEND.
		const auto walk = [data, len](const auto& f) {
			size_t pos = sizeof(png_signature);
			while (len - pos >= 12) {
				const size_t n = get_be32(data + pos);
				if (n > len - pos - 12) {
					return;    // Cannot happen after the structure scan.
				}
				const uint8_t* type = data + pos + 4;
				f(pos, 12 + n, keep_chunk(type, n));
				pos += 12 + n;
				if (std::memcmp(type, "IEND", 4) == 0) {
					return;
				}
			}
		};
		size_t kept    = sizeof(png_signature);
		bool   dropped = false;
		walk([&kept, &dropped](size_t, size_t size, bool keep) {
			kept += keep ? size : 0;
			dropped = dropped || !keep;
		});
		if (!dropped) {
			return false;
		}
		note_alloc(kept);
		out.reserve(kept);
		out.insert(out.end(), data, data + sizeof(png_signature));
		walk([data, &out](size_t pos, size_t size, bool keep) {
			if (keep) {
				out.insert(out.end(), data + pos, data + pos + size);
			}
		});
		return true;
	}

	png_voidp PNGCBAPI png_malloc_cb(png_structp, png_alloc_size_t size) {
		note_alloc(size);
		return std::malloc(size);
	}

	void PNGCBAPI png_free_cb(png_structp, png_voidp ptr) {
		std::free(ptr);
	}

	/*
	 *  Everything a read or write owns. It is constructed before setjmp and
	 *  holds only memory and libpng handles, so a longjmp out of libpng skips
	 *  no destructor: the destructor of this object frees everything when the
	 *  function returns, on success and on failure alike.
	 */
	struct Png_frame {
		png_structp png          = nullptr;
		png_infop   info         = nullptr;
		png_infop   end_info     = nullptr;
		bool        writing      = false;
		char        message[200] = {};    // libpng's error text (POD; written by the error callback).

		// Reader input.
		const uint8_t* data = nullptr;
		size_t         size = 0;
		size_t         pos  = 0;

		// Writer output: a buffer sized up front, so the write callback never
		// allocates (an exception must not cross libpng's C frames).
		std::vector<uint8_t> out;
		size_t               used = 0;

		std::vector<png_bytep> rows;

		Png_frame()                            = default;
		Png_frame(const Png_frame&)            = delete;
		Png_frame& operator=(const Png_frame&) = delete;

		~Png_frame() {
			if (png == nullptr) {
				return;
			}
			if (writing) {
				png_destroy_write_struct(&png, info != nullptr ? &info : nullptr);
			} else {
				png_destroy_read_struct(&png, info != nullptr ? &info : nullptr, end_info != nullptr ? &end_info : nullptr);
			}
		}
	};

	[[noreturn]] void PNGCBAPI png_error_cb(png_structp png, png_const_charp msg) {
		auto* frame = static_cast<Png_frame*>(png_get_error_ptr(png));
		if (frame != nullptr && msg != nullptr) {
			std::strncpy(frame->message, msg, sizeof(frame->message) - 1);
			frame->message[sizeof(frame->message) - 1] = '\0';
		}
		png_longjmp(png, 1);
	}

	void PNGCBAPI png_warning_cb(png_structp, png_const_charp) {
		// Warnings (benign errors included) are not reported: the rules decide.
	}

	void PNGCBAPI png_read_cb(png_structp png, png_bytep dest, size_t len) {
		auto* frame = static_cast<Png_frame*>(png_get_io_ptr(png));
		if (len > frame->size - frame->pos) {
			png_error(png, "unexpected end of file");
		}
		std::memcpy(dest, frame->data + frame->pos, len);
		frame->pos += len;
	}

	void PNGCBAPI png_write_cb(png_structp png, png_bytep src, size_t len) {
		auto* frame = static_cast<Png_frame*>(png_get_io_ptr(png));
		if (len > frame->out.size() - frame->used) {
			png_error(png, "output buffer overflow");
		}
		std::memcpy(frame->out.data() + frame->used, src, len);
		frame->used += len;
	}

	void PNGCBAPI png_flush_cb(png_structp) {}

	/*
	 *  All libpng calls of a read. Rule: the locals after setjmp are POD, and
	 *  every heap buffer belongs to 'frame' or 'out', which exist before
	 *  setjmp. The pixel buffer and the row table are allocated before setjmp
	 *  because the size is already known (and checked) from the structure scan.
	 *  After a longjmp nothing is built here: out.error stays empty and the
	 *  caller makes it from frame.message.
	 */
	Hires::Png_status decode_png(Png_frame& frame, Hires::Indexed_png& out) {
		const auto w = static_cast<png_uint_32>(out.width);
		const auto h = static_cast<png_uint_32>(out.height);
		note_alloc(static_cast<size_t>(w) * h);
		out.pixels.assign(static_cast<size_t>(w) * h, 0);
		frame.rows.resize(h);
		for (png_uint_32 y = 0; y < h; y++) {
			frame.rows[y] = out.pixels.data() + static_cast<size_t>(y) * w;
		}
		frame.png = png_create_read_struct_2(
				PNG_LIBPNG_VER_STRING, &frame, png_error_cb, png_warning_cb, &frame, png_malloc_cb, png_free_cb);
		if (frame.png == nullptr) {
			out.error = "libpng: cannot create read struct";
			return Hires::Png_status::read_error;
		}
		frame.info     = png_create_info_struct(frame.png);
		frame.end_info = png_create_info_struct(frame.png);
		if (frame.info == nullptr || frame.end_info == nullptr) {
			out.error = "libpng: cannot create info struct";
			return Hires::Png_status::read_error;
		}
		if (setjmp(png_jmpbuf(frame.png))) {
			return Hires::Png_status::read_error;
		}
		png_set_read_fn(frame.png, &frame, png_read_cb);
#	ifdef PNG_SET_USER_LIMITS_SUPPORTED
		// Defence in depth: the structure scan has checked the size already.
		png_set_user_limits(frame.png, w, h);
		png_set_chunk_malloc_max(frame.png, chunk_limit);
#	endif
#	ifdef PNG_BENIGN_ERRORS_SUPPORTED
		png_set_benign_errors(frame.png, 1);
#	endif
		png_read_info(frame.png, frame.info);
		png_uint_32 iw    = 0;
		png_uint_32 ih    = 0;
		int         depth = 0;
		int         ctype = 0;
		png_get_IHDR(frame.png, frame.info, &iw, &ih, &depth, &ctype, nullptr, nullptr, nullptr);
		if (iw != w || ih != h || ctype != PNG_COLOR_TYPE_PALETTE) {
			// The structure scan has checked this already; libpng must agree.
			out.error = "libpng: IHDR differs from the scanned header";
			return Hires::Png_status::read_error;
		}
		if (depth < 8) {
			png_set_packing(frame.png);
		}
		png_set_interlace_handling(frame.png);
		png_read_update_info(frame.png, frame.info);
		if (png_get_rowbytes(frame.png, frame.info) != w) {
			out.error = "libpng: unexpected row size";
			return Hires::Png_status::read_error;
		}
		png_read_image(frame.png, frame.rows.data());
		png_read_end(frame.png, frame.end_info);
		// From here on only getters, which never longjmp.
		png_colorp palette = nullptr;
		int        npal    = 0;
		if (png_get_PLTE(frame.png, frame.info, &palette, &npal) == 0 || palette == nullptr || npal <= 0) {
			out.error = "palette image without PLTE";
			return Hires::Png_status::read_error;
		}
		out.num_palette = npal;
		out.plte.resize(static_cast<size_t>(npal) * 3);
		for (int i = 0; i < npal; i++) {
			out.plte[3 * i]     = palette[i].red;
			out.plte[3 * i + 1] = palette[i].green;
			out.plte[3 * i + 2] = palette[i].blue;
		}
		for (png_infop src : {frame.info, frame.end_info}) {
			png_textp text  = nullptr;
			int       ntext = 0;
			png_get_text(frame.png, src, &text, &ntext);
			for (int i = 0; i < ntext; i++) {
				if (text[i].key != nullptr) {
					out.text.push_back({text[i].key, text[i].text != nullptr ? text[i].text : ""});
				}
			}
		}
		return Hires::Png_status::ok;
	}

	/*
	 *  All libpng calls of a write, under the same rules as decode_png. Every
	 *  buffer (output, row table, text records and their strings, palette)
	 *  is set up before setjmp.
	 */
	struct Write_input {
		std::vector<png_color>         palette;
		std::vector<png_byte>          trns;
		std::vector<std::vector<char>> strings;    // NUL-terminated keys and values.
		std::vector<png_text>          before;
		std::vector<png_text>          after;
		std::vector<uint8_t>           pixels;
	};

	Hires::Png_status encode_png(
			Png_frame& frame, Write_input& in, int width, int height, const Hires::Png_write_options& options) {
		frame.writing = true;
		frame.png     = png_create_write_struct_2(
                PNG_LIBPNG_VER_STRING, &frame, png_error_cb, png_warning_cb, &frame, png_malloc_cb, png_free_cb);
		if (frame.png == nullptr) {
			return Hires::Png_status::write_error;
		}
		frame.info = png_create_info_struct(frame.png);
		if (frame.info == nullptr) {
			return Hires::Png_status::write_error;
		}
		if (setjmp(png_jmpbuf(frame.png))) {
			return Hires::Png_status::write_error;    // frame.message has libpng's reason.
		}
		png_set_write_fn(frame.png, &frame, png_write_cb, png_flush_cb);
		png_set_IHDR(
				frame.png, frame.info, static_cast<png_uint_32>(width), static_cast<png_uint_32>(height), options.bit_depth,
				PNG_COLOR_TYPE_PALETTE, options.interlace ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
				PNG_FILTER_TYPE_DEFAULT);
		png_set_PLTE(frame.png, frame.info, in.palette.data(), static_cast<int>(in.palette.size()));
		if (!in.trns.empty()) {
			png_set_tRNS(frame.png, frame.info, in.trns.data(), static_cast<int>(in.trns.size()), nullptr);
		}
		if (!in.before.empty()) {
			png_set_text(frame.png, frame.info, in.before.data(), static_cast<int>(in.before.size()));
		}
		png_set_compression_level(frame.png, 9);
		png_set_filter(frame.png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);
		png_write_info(frame.png, frame.info);
		if (options.bit_depth < 8) {
			png_set_packing(frame.png);
		}
		png_set_interlace_handling(frame.png);
		png_write_image(frame.png, frame.rows.data());
		if (!in.after.empty()) {
			// Text set after png_write_info is written by png_write_end, after IDAT.
			png_set_text(frame.png, frame.info, in.after.data(), static_cast<int>(in.after.size()));
		}
		png_write_end(frame.png, frame.info);
		return Hires::Png_status::ok;
	}
#endif
}    // namespace

namespace Hires {
	const char* png_status_name(Png_status status) {
		switch (status) {
		case Png_status::ok:
			return "ok";
		case Png_status::no_png_support:
			return "no PNG support";
		case Png_status::open_error:
			return "cannot open";
		case Png_status::read_error:
			return "unreadable";
		case Png_status::wrong_size:
			return "wrong size";
		case Png_status::not_palette:
			return "not a palette image";
		case Png_status::write_error:
			return "write error";
		}
		return "?";
	}

	bool have_png_support() {
#ifdef HAVE_PNG_H
		return true;
#else
		return false;
#endif
	}

	const std::string* Indexed_png::find_text(const char* key) const {
		for (const auto& t : text) {
			if (t.key == key) {
				return &t.value;
			}
		}
		return nullptr;
	}

	Png_alloc_stats png_alloc_stats() {
		return alloc_stats;
	}

	void reset_png_alloc_stats() {
		alloc_stats = Png_alloc_stats();
	}

	Png_status read_indexed_png(const uint8_t* data, size_t len, int expect_w, int expect_h, Indexed_png& out) {
		out = Indexed_png();
#ifdef HAVE_PNG_H
		Png_structure st;
		if (!scan_structure(data, len, true, st, out.error)) {
			return Png_status::read_error;
		}
		const Png_status status = check_header(st, expect_w, expect_h, out);
		if (status != Png_status::ok) {
			return status;
		}
		if (!st.has_plte) {
			out.error = "palette image without PLTE";
			return Png_status::read_error;
		}
		out.has_trns = st.has_trns;
		// libpng reads the file without the chunks it does not need.
		std::vector<uint8_t> stripped;
		const bool           strip = strip_chunks(data, len, stripped);
		Png_frame            frame;
		frame.data               = strip ? stripped.data() : data;
		frame.size               = strip ? stripped.size() : len;
		const Png_status decoded = decode_png(frame, out);
		if (decoded != Png_status::ok) {
			if (out.error.empty()) {
				out.error = std::string("libpng: ") + frame.message;
			}
			out.pixels.clear();
			out.plte.clear();
			out.num_palette = 0;
			out.text.clear();
		}
		return decoded;
#else
		ignore_unused_variable_warning(data, len, expect_w, expect_h);
		out.error = "built without PNG support";
		return Png_status::no_png_support;
#endif
	}

	Png_status read_indexed_png(const std::filesystem::path& file, int expect_w, int expect_h, Indexed_png& out) {
		out = Indexed_png();
#ifdef HAVE_PNG_H
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			out.error = "cannot open";
			return Png_status::open_error;
		}
		in.seekg(0, std::ios::end);
		const std::streamoff size = in.tellg();
		in.seekg(0, std::ios::beg);
		if (size < 0 || !in) {
			out.error = "cannot read";
			return Png_status::open_error;
		}
		// The signature and IHDR come first: a stray image of the wrong size or
		// colour type is rejected (F3, F1) without reading the rest of it.
		uint8_t      head[33] = {};
		const size_t n        = static_cast<size_t>(std::min<std::streamoff>(size, sizeof(head)));
		in.read(reinterpret_cast<char*>(head), static_cast<std::streamsize>(n));
		if (!in) {
			out.error = "cannot read";
			return Png_status::open_error;
		}
		Png_structure st;
		const bool    head_ok = scan_structure(head, n, false, st, out.error);
		if (head_ok) {
			const Png_status status = check_header(st, expect_w, expect_h, out);
			if (status != Png_status::ok) {
				return status;
			}
		}
		if (static_cast<uint64_t>(size) > max_file_size(expect_w, expect_h)) {
			if (head_ok) {
				out.error = "file too large (" + std::to_string(size) + " bytes)";
			}
			return Png_status::read_error;
		}
		// A bad head is reported by the full structure scan, with its message.
		in.seekg(0, std::ios::beg);
		note_alloc(static_cast<size_t>(size));
		std::vector<uint8_t> data(static_cast<size_t>(size));
		if (!data.empty()) {
			in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
		}
		if (!in) {
			out.error = "cannot read";
			return Png_status::open_error;
		}
		return read_indexed_png(data.data(), data.size(), expect_w, expect_h, out);
#else
		ignore_unused_variable_warning(file, expect_w, expect_h);
		out.error = "built without PNG support";
		return Png_status::no_png_support;
#endif
	}

	Png_status encode_indexed_png(
			int width, int height, const uint8_t* pixels, const uint8_t* plte, int num_palette, const Png_write_options& options,
			std::vector<uint8_t>& out) {
		out.clear();
#ifdef HAVE_PNG_H
		const int depth = options.bit_depth;
		if (width < 1 || height < 1 || width > (1 << 16) || height > (1 << 16) || pixels == nullptr || plte == nullptr
			|| num_palette < 1 || num_palette > 256 || (depth != 1 && depth != 2 && depth != 4 && depth != 8)
			|| options.trns.size() > static_cast<size_t>(num_palette)) {
			return Png_status::write_error;
		}
		const size_t npix = static_cast<size_t>(width) * height;
		if (depth < 8
			&& std::any_of(
					pixels, pixels + npix,
					[depth](uint8_t v) {
						return static_cast<unsigned>(v) >= (1U << depth);
					})) {
			return Png_status::write_error;
		}
		Write_input in;
		in.palette.resize(static_cast<size_t>(num_palette));
		for (int i = 0; i < num_palette; i++) {
			in.palette[i] = png_color{plte[3 * i], plte[3 * i + 1], plte[3 * i + 2]};
		}
		in.trns.assign(options.trns.begin(), options.trns.end());
		in.pixels.assign(pixels, pixels + npix);
		size_t text_bytes = 0;

		auto add_text = [&](const Png_text& t, std::vector<png_text>& dest) {
			if (t.key.empty() || t.key.size() > 79 || t.key.find('\0') != std::string::npos
				|| t.value.find('\0') != std::string::npos) {
				return false;
			}
			in.strings.emplace_back(t.key.begin(), t.key.end());
			in.strings.back().push_back('\0');
			char* key = in.strings.back().data();
			in.strings.emplace_back(t.value.begin(), t.value.end());
			in.strings.back().push_back('\0');
			png_text rec{};
			rec.compression = PNG_TEXT_COMPRESSION_NONE;
			rec.key         = key;
			rec.text        = in.strings.back().data();
			rec.text_length = t.value.size();
			dest.push_back(rec);
			text_bytes += t.key.size() + t.value.size() + 16;
			return true;
		};
		// Reserve first: the records keep pointers into 'strings'.
		in.strings.reserve(2 * (options.text_before.size() + options.text_after.size()));
		for (const auto& t : options.text_before) {
			if (!add_text(t, in.before)) {
				return Png_status::write_error;
			}
		}
		for (const auto& t : options.text_after) {
			if (!add_text(t, in.after)) {
				return Png_status::write_error;
			}
		}
		Png_frame frame;
		// Worst case: stored deflate blocks plus chunk overhead, PLTE, tRNS and texts.
		frame.out.resize(2 * static_cast<size_t>(height) * (static_cast<size_t>(width) + 1) + text_bytes + 65536);
		frame.rows.resize(static_cast<size_t>(height));
		for (int y = 0; y < height; y++) {
			frame.rows[y] = in.pixels.data() + static_cast<size_t>(y) * width;
		}
		const Png_status status = encode_png(frame, in, width, height, options);
		if (status != Png_status::ok) {
			return status;
		}
		out.assign(frame.out.begin(), frame.out.begin() + static_cast<std::ptrdiff_t>(frame.used));
		return Png_status::ok;
#else
		ignore_unused_variable_warning(width, height, pixels, plte, num_palette, options);
		return Png_status::no_png_support;
#endif
	}

	Png_status write_indexed_png(
			const std::filesystem::path& file, int width, int height, const uint8_t* pixels, const uint8_t* plte, int num_palette,
			const Png_write_options& options) {
		std::vector<uint8_t> bytes;
		const Png_status     status = encode_indexed_png(width, height, pixels, plte, num_palette, options, bytes);
		if (status != Png_status::ok) {
			return status;
		}
		std::ofstream outf(file, std::ios::binary | std::ios::trunc);
		if (!outf) {
			return Png_status::write_error;
		}
		outf.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		outf.close();
		return outf ? Png_status::ok : Png_status::write_error;
	}
}    // namespace Hires
