/*
 *  test_ibuf_golden.cc - Pins the scale-1 behaviour of Image_buffer8.
 *
 *  A fixed-seed stream of 24,000 ops over every primitive (ibuf_ops.h) runs on
 *  scale-1 buffers: owned buffers, layer-constructor buffers with a guard band
 *  and a padded pitch, and layer buffers with a logical offset like the
 *  engine's main buffer. After every block of 1,000 ops the FNV-1a-64 digest of
 *  the buffers' whole memory (guard band and padding included), the second
 *  buffer, the clip rectangles and everything read back must equal the digest
 *  in tests/data/ibuf_golden.txt. WP-01 recorded that file on unmodified
 *  upstream imagewin/ibuf8.cc and imagebuf.h (8b6ab6b43).
 *
 *  fill_static() draws from std::rand(), whose sequence differs between C
 *  libraries, so the digests cannot cover its output. The test checks it
 *  against a model of upstream's loop instead (one rand() % 5 per pixel, row
 *  by row: 0-1 black, 2-3 gray, 4 white) and then restores the buffer.
 *
 *  Recording is a separate test case that never runs by default:
 *    HIRES_IBUF_GOLDEN_RECORD=<file> hires_unit -tc='ibuf golden: record*' --no-skip
 *  writes the digests to <file>. The comparing test ignores that variable.
 *  Record only on unmodified upstream image-buffer code (tests/README).
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

#include "doctest.h"
#include "ibuf_ops.h"
#include "test_support.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using hires_test::fnv1a64;
using hires_test::fnv1a64_value;
using hires_test::fnv_offset;
using hires_test::Ibuf_fixtures;
using hires_test::Ibuf_geometry;
using hires_test::Ibuf_op;
using hires_test::Ibuf_op_kind;
using hires_test::Ibuf_reads;
using hires_test::Rng;

namespace {
	// Changing any of these, the generator in ibuf_ops.cc or the fixtures
	// changes the digests, and the file must then be recorded again on
	// unmodified upstream code (tests/README). Version 2 added blocks 20-23
	// (logical offsets); every block has its own generator, so the digests of
	// blocks 0-19 are those of version 1.
	constexpr int      golden_version       = 2;
	constexpr uint64_t golden_seed          = 0x1b0f'901d'e000'0001ULL;
	constexpr int      golden_blocks        = 24;
	constexpr int      golden_ops_per_block = 1000;
	const char* const  golden_file          = "ibuf_golden.txt";

	/*
	 *  A layer buffer whose logical origin lies inside its storage, as
	 *  Image_window::create_surface sets up the main buffer when the full area
	 *  is larger than the game area (a 320x200 game area in a 355x200 full
	 *  area gives offset_x = 17). The logical extent is then
	 *  [-offset_x, width - offset_x) x [-offset_y, height - offset_y), and
	 *  fill8(pix), fill_static, clear_clip and set_clip use the offsets.
	 */
	class Offset_buffer8 : public Image_buffer8 {
	public:
		Offset_buffer8(unsigned int w, unsigned int h, unsigned int pitch, unsigned char* mem, int guard_band, int off_x, int off_y)
				: Image_buffer8(w, h, pitch, mem, guard_band) {
			offset_x = off_x;
			offset_y = off_y;
			bits += off_x + off_y * line_width;
			clear_clip();
		}
	};

	/*
	 *  The target buffer of one block, and the memory its digest covers.
	 */
	struct Target {
		std::vector<unsigned char>     backing;    // Layer buffers: the whole allocation.
		std::unique_ptr<Image_buffer8> buf;
		unsigned char*                 mem      = nullptr;    // Digest covers mem[0, mem_size).
		size_t                         mem_size = 0;
		size_t                         origin   = 0;    // Storage origin in mem: logical (-offset_x, -offset_y).
		int                            pitch    = 0;
		int                            width    = 0;
		int                            height   = 0;
		int                            offset_x = 0;
		int                            offset_y = 0;
		bool                           layer    = false;
	};

	void fill_random(unsigned char* mem, size_t size, Rng& rng) {
		for (size_t i = 0; i < size; i++) {
			mem[i] = rng.byte();
		}
	}

	// Blocks 0-3 use fixed shapes (the classic 320x200 view, a 355x200 layer
	// with a 4 px guard band like the engine's, and two 1 px thin buffers);
	// blocks 4-19 alternate between owned and layer buffers of random size,
	// guard band and pitch padding. Blocks 20-23 are layer buffers with a
	// logical offset (Offset_buffer8), the case of the engine's main buffer.
	Target make_target(int block, Rng& rng) {
		Target t;
		int    guard = 0;
		int    pad   = 0;
		switch (block) {
		case 0:
			t.width  = 320;
			t.height = 200;
			break;
		case 1:
			t.layer  = true;
			t.width  = 355;
			t.height = 200;
			guard    = 4;
			pad      = 1;
			break;
		case 2:
			t.width  = 37;
			t.height = 1;
			break;
		case 3:
			t.layer  = true;
			t.width  = 1;
			t.height = 29;
			guard    = 2;
			pad      = 3;
			break;
		case 20:
			// A 320x200 game area centred in a 355x200 full area, with the
			// engine's 4 px guard band: offset_x = (355 - 320) / 2.
			t.layer    = true;
			t.width    = 355;
			t.height   = 200;
			t.offset_x = 17;
			guard      = 4;
			pad        = 1;
			break;
		case 21:
			// A 320x200 game area in a 320x240 full area: offset_y only.
			t.layer    = true;
			t.width    = 320;
			t.height   = 240;
			t.offset_y = 20;
			guard      = 4;
			break;
		case 22:
			// Both offsets, odd sizes and a padded pitch.
			t.layer    = true;
			t.width    = 401;
			t.height   = 263;
			t.offset_x = 40;
			t.offset_y = 31;
			guard      = 2;
			pad        = 5;
			break;
		case 23:
			// Random size, guard band, padding and offsets; an offset may reach
			// the whole size, so most of the logical extent can be negative.
			t.layer    = true;
			t.width    = rng.range(8, 400);
			t.height   = rng.range(8, 300);
			t.offset_x = rng.range(0, t.width - 1);
			t.offset_y = rng.range(0, t.height - 1);
			guard      = rng.range(0, 6);
			pad        = rng.range(0, 12);
			break;
		default:
			t.layer  = block % 2 == 1;
			t.width  = rng.range(8, 400);
			t.height = rng.range(8, 300);
			guard    = rng.range(0, 6);
			pad      = rng.range(0, 12);
			break;
		}
		if (t.layer) {
			// Image_buffer8(w, h, pitch, bits, guard_band), as Image_window
			// creates the layer buffers over an SDL surface.
			const int surf_w = t.width + 2 * guard;
			const int surf_h = t.height + 2 * guard;
			t.pitch          = surf_w + pad;
			t.backing.resize(static_cast<size_t>(t.pitch) * surf_h);
			fill_random(t.backing.data(), t.backing.size(), rng);
			if (t.offset_x != 0 || t.offset_y != 0) {
				t.buf = std::make_unique<Offset_buffer8>(surf_w, surf_h, t.pitch, t.backing.data(), guard, t.offset_x, t.offset_y);
			} else {
				t.buf = std::make_unique<Image_buffer8>(surf_w, surf_h, t.pitch, t.backing.data(), guard);
			}
			t.mem      = t.backing.data();
			t.mem_size = t.backing.size();
			t.origin   = static_cast<size_t>(guard) * t.pitch + guard;
		} else {
			t.buf      = std::make_unique<Image_buffer8>(t.width, t.height);
			t.pitch    = t.width;
			t.mem      = t.buf->get_bits();
			t.mem_size = static_cast<size_t>(t.width) * t.height;
			t.origin   = 0;
			fill_random(t.mem, t.mem_size, rng);
		}
		return t;
	}

	// Runs fill_static as the op says and compares the result with the model of
	// upstream's loop (every row of the storage, from the storage origin, also
	// when the buffer has a logical offset); then restores the memory, so that
	// the digests do not depend on the C library's rand().
	bool check_fill_static(const Ibuf_op& op, Target& t) {
		const std::vector<unsigned char> before(t.mem, t.mem + t.mem_size);
		const auto                       seed  = static_cast<unsigned int>(op.arg[0]);
		const auto                       black = static_cast<unsigned char>(op.arg[1]);
		const auto                       gray  = static_cast<unsigned char>(op.arg[2]);
		const auto                       white = static_cast<unsigned char>(op.arg[3]);
		std::srand(seed);
		t.buf->fill_static(op.arg[1], op.arg[2], op.arg[3]);

		std::vector<unsigned char> expected = before;
		std::srand(seed);
		for (int y = 0; y < t.height; y++) {
			for (int x = 0; x < t.width; x++) {
				const int    r   = std::rand() % 5;
				const size_t pos = t.origin + static_cast<size_t>(y) * t.pitch + x;
				expected[pos]    = r < 2 ? black : r < 4 ? gray : white;
			}
		}
		const bool ok = std::memcmp(expected.data(), t.mem, t.mem_size) == 0;
		std::memcpy(t.mem, before.data(), t.mem_size);
		return ok;
	}

	uint64_t hash_clip(Image_buffer& buf, uint64_t hash) {
		int x;
		int y;
		int w;
		int h;
		buf.get_clip(x, y, w, h);
		for (const int v : {x, y, w, h}) {
			hash = fnv1a64_value(static_cast<uint64_t>(static_cast<uint32_t>(v)), hash);
		}
		return hash;
	}

	struct Golden_result {
		std::vector<uint64_t>    digests;
		int                      fill_static_mismatches = 0;
		std::vector<int>         kind_counts            = std::vector<int>(hires_test::num_ibuf_op_kinds, 0);
		std::vector<std::string> first_ops;    // First op of each block, for messages.
	};

	Golden_result run_golden_stream() {
		const Ibuf_fixtures fx(golden_seed);
		Golden_result       result;
		for (int block = 0; block < golden_blocks; block++) {
			Rng    rng(golden_seed ^ (0x9e3779b97f4a7c15ULL * static_cast<uint64_t>(block + 1)));
			Target t = make_target(block, rng);

			const int     aux_w = rng.range(1, 48);
			const int     aux_h = rng.range(1, 48);
			Image_buffer8 aux(aux_w, aux_h);
			fill_random(aux.get_bits(), static_cast<size_t>(aux_w) * aux_h, rng);

			const Ibuf_geometry geom{t.width, t.height, aux_w, aux_h, t.offset_x, t.offset_y};
			Ibuf_reads          reads;
			for (int i = 0; i < golden_ops_per_block; i++) {
				const Ibuf_op op = hires_test::make_ibuf_op(rng, geom, fx);
				result.kind_counts[static_cast<int>(op.kind)]++;
				if (i == 0) {
					result.first_ops.push_back(op.describe());
				}
				if (op.kind == Ibuf_op_kind::fill_static) {
					if (!check_fill_static(op, t)) {
						result.fill_static_mismatches++;
					}
					continue;
				}
				hires_test::apply_ibuf_op(op, *t.buf, aux, fx, reads);
			}

			uint64_t hash = fnv1a64_value(static_cast<uint64_t>(block), fnv_offset);
			hash          = fnv1a64(t.mem, t.mem_size, hash);
			hash          = fnv1a64(aux.get_bits(), static_cast<size_t>(aux_w) * aux_h, hash);
			hash          = fnv1a64_value(reads.hash, hash);
			hash          = hash_clip(*t.buf, hash);
			hash          = hash_clip(aux, hash);
			result.digests.push_back(hash);
		}
		return result;
	}

	std::string hex64(uint64_t value) {
		char text[17];
		std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
		return text;
	}

	std::string header_line() {
		std::ostringstream out;
		out << "ibuf_golden v" << golden_version << " seed=0x" << hex64(golden_seed) << " blocks=" << golden_blocks
			<< " ops_per_block=" << golden_ops_per_block;
		return out.str();
	}

	bool write_golden(const std::string& path, const std::vector<uint64_t>& digests) {
		std::ofstream out(path, std::ios::binary);
		out << "# tests/data/ibuf_golden.txt: digests of test_ibuf_golden (tests/unit/test_ibuf_golden.cc).\n"
			   "# FNV-1a-64 after each block of ops on scale-1 Image_buffer8 buffers. Recorded on\n"
			   "# unmodified upstream imagewin/ibuf8.cc and imagebuf.h; record again only on unmodified\n"
			   "# upstream code and with a written reason (tests/README).\n"
			<< header_line() << '\n';
		for (size_t i = 0; i < digests.size(); i++) {
			out << "block " << i << ' ' << hex64(digests[i]) << '\n';
		}
		return static_cast<bool>(out.flush());
	}

	// Reads the recorded digests; returns an error message, empty on success.
	std::string read_golden(const std::string& path, std::vector<uint64_t>& digests) {
		std::ifstream in(path, std::ios::binary);
		if (!in) {
			return "cannot open " + path;
		}
		std::string line;
		bool        header = false;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.empty() || line[0] == '#') {
				continue;
			}
			if (!header) {
				if (line != header_line()) {
					return "header '" + line + "' does not match the test ('" + header_line() + "')";
				}
				header = true;
				continue;
			}
			std::istringstream fields(line);
			std::string        word;
			size_t             index;
			std::string        digest;
			if (!(fields >> word >> index >> digest) || word != "block" || index != digests.size() || digest.size() != 16) {
				return "malformed line '" + line + "'";
			}
			digests.push_back(std::strtoull(digest.c_str(), nullptr, 16));
		}
		if (!header) {
			return "no header line in " + path;
		}
		return std::string();
	}

	// The checks that do not need the recorded file: fill_static follows
	// upstream's loop (one std::rand() per pixel), and every primitive is
	// exercised (the stream is fixed, so the counts are too; the bound only
	// guards against a generator change that drops a kind). Returns whether
	// all of them passed.
	bool check_stream(const Golden_result& result) {
		bool ok = result.fill_static_mismatches == 0;
		CHECK(result.fill_static_mismatches == 0);
		for (int k = 0; k < hires_test::num_ibuf_op_kinds; k++) {
			// doctest prints a char* as a pointer, so the name goes in as a string.
			INFO("op kind " << std::string(hires_test::ibuf_op_name(static_cast<Ibuf_op_kind>(k))));
			CHECK(result.kind_counts[k] >= 50);
			ok = ok && result.kind_counts[k] >= 50;
		}
		return ok;
	}
}    // namespace

TEST_CASE("ibuf golden: scale-1 primitives match the digests recorded on upstream ibuf8.cc") {
	const Golden_result result = run_golden_stream();
	REQUIRE(result.digests.size() == static_cast<size_t>(golden_blocks));
	check_stream(result);

	const std::string     path = hires_test::data_path(golden_file);
	std::vector<uint64_t> expected;
	const std::string     error = read_golden(path, expected);
	INFO("golden file: " << path);
	REQUIRE_MESSAGE(error.empty(), error);
	REQUIRE(expected.size() == result.digests.size());
	for (int block = 0; block < golden_blocks; block++) {
		CHECK_MESSAGE(
				result.digests[block] == expected[block], "block " << block << ": digest " << hex64(result.digests[block])
																   << ", recorded " << hex64(expected[block])
																   << " (first op: " << result.first_ops[block] << ")");
	}
}

TEST_CASE("ibuf golden: the op stream is deterministic") {
	// Two runs in one process give the same digests. This guards the test
	// itself: an uninitialised read would make the recorded file meaningless.
	const Golden_result first  = run_golden_stream();
	const Golden_result second = run_golden_stream();
	CHECK(first.digests == second.digests);
}

// Writes the digests of the code under test to $HIRES_IBUF_GOLDEN_RECORD. It
// runs only with --no-skip (tests/README selects it with -tc) and fails without
// the variable, so recording never happens by accident and never passes in
// place of the comparison.
TEST_CASE("ibuf golden: record the digests to $HIRES_IBUF_GOLDEN_RECORD" * doctest::skip()) {
	const char* const record = std::getenv("HIRES_IBUF_GOLDEN_RECORD");
	REQUIRE_MESSAGE((record != nullptr && *record != '\0'), "HIRES_IBUF_GOLDEN_RECORD names no file to write");
	const Golden_result result = run_golden_stream();
	REQUIRE(result.digests.size() == static_cast<size_t>(golden_blocks));
	const bool stream_ok = check_stream(result);
	REQUIRE_MESSAGE(stream_ok, "the op stream fails its own checks: nothing recorded");
	REQUIRE(write_golden(record, result.digests));
	MESSAGE("recorded " << result.digests.size() << " digests to " << std::string(record));
}
