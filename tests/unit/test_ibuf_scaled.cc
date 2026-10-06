/*
 *  test_ibuf_scaled.cc - The scaled storage of Image_buffer8 (DESIGN.md
 *  section 3.1, oracle O1 of section 6.2).
 *
 *  O1: the op stream of ibuf_ops.h runs on a scale-1 reference and on buffers
 *  of pixel scale 2, 3 and 6 (5 seeds each, owned buffers and views with
 *  logical offsets of both signs and padded pitches). After every op the
 *  scaled buffer must hold the nearest-neighbour upscale of the reference,
 *  the reads and clips must agree, nothing outside a view's storage may change
 *  (canaries), and every changed pixel must lie in the rectangle the write
 *  tracker reports (tracker_complete).
 *
 *  The other cases check what has no scale-1 reference, each against a model:
 *  get, put and blit for every pair of scales (storage extent of views with
 *  offsets, physical rows between equal scales, top-left sample and
 *  replication across scales), put_phys clipping, get_pixel8 sampling,
 *  fill_static's use of std::rand(), create_another, copy() with rectangles
 *  out of range, and RLE scans of 4096 px and more and with malformed runs
 *  (rle_writer.h), all under ASan in build-asan.
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
#include "ibuf8.h"
#include "ibuf_ops.h"
#include "rle_writer.h"
#include "test_support.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using hires_test::decode_rle;
using hires_test::Ibuf_fixtures;
using hires_test::Ibuf_geometry;
using hires_test::Ibuf_op;
using hires_test::Ibuf_op_kind;
using hires_test::Ibuf_reads;
using hires_test::Rle_policy;
using hires_test::Rle_writer;
using hires_test::Rng;

namespace {
	using Tracker = Image_buffer::Write_tracker;

	constexpr int test_scales[] = {2, 3, 6};
	constexpr int all_scales[]  = {1, 2, 3, 6};

	/*
	 *  A buffer under test and its storage. A view lives in `backing` with
	 *  canary bytes around and between its rows (padded pitch); an owned
	 *  buffer is checked by ASan. Coordinates named s* are storage-relative
	 *  logical (0 to w - 1); physical coordinates are storage-relative too.
	 */
	struct Tbuf {
		int                            w      = 0;
		int                            h      = 0;
		int                            off_x  = 0;
		int                            off_y  = 0;
		int                            scale  = 1;
		int                            pitch  = 0;
		bool                           view   = false;
		size_t                         origin = 0;
		std::vector<unsigned char>     backing;
		std::vector<unsigned char>     canary;
		std::unique_ptr<Image_buffer8> buf;

		unsigned char* storage() {
			return view ? backing.data() + origin : buf->get_bits();
		}

		int phys_w() const {
			return w * scale;
		}

		int phys_h() const {
			return h * scale;
		}

		unsigned char& phys(int px, int py) {
			return storage()[static_cast<size_t>(py) * pitch + px];
		}

		// The top-left physical pixel of logical (x, y).
		unsigned char sample(int x, int y) {
			return phys((x + off_x) * scale, (y + off_y) * scale);
		}

		// The storage as a packed phys_w() x phys_h() image.
		std::vector<unsigned char> snapshot() {
			std::vector<unsigned char> out(static_cast<size_t>(phys_w()) * phys_h());
			for (int py = 0; py < phys_h(); py++) {
				std::memcpy(out.data() + static_cast<size_t>(py) * phys_w(), &phys(0, py), phys_w());
			}
			return out;
		}

		// Whether every byte of the backing outside the storage is unchanged.
		bool canaries_intact() const {
			if (!view) {
				return true;
			}
			const size_t row_bytes = static_cast<size_t>(w) * scale;
			const size_t rows      = static_cast<size_t>(h) * scale;
			if (std::memcmp(backing.data(), canary.data(), origin) != 0) {
				return false;
			}
			for (size_t r = 0; r < rows; r++) {
				const size_t pad = origin + r * pitch + row_bytes;
				if (std::memcmp(backing.data() + pad, canary.data() + pad, pitch - row_bytes) != 0) {
					return false;
				}
			}
			const size_t tail = origin + rows * pitch;
			return std::memcmp(backing.data() + tail, canary.data() + tail, backing.size() - tail) == 0;
		}
	};

	// An owned buffer (offsets must be 0) or a view with canaries. The
	// storage is zero for owned buffers and random for views.
	Tbuf make_tbuf(int w, int h, int scale, bool view, int off_x, int off_y, Rng& rng) {
		Tbuf t;
		t.w     = w;
		t.h     = h;
		t.scale = scale;
		t.view  = view;
		t.off_x = off_x;
		t.off_y = off_y;
		if (!view) {
			REQUIRE((off_x == 0 && off_y == 0));
			t.buf   = std::make_unique<Image_buffer8>(w, h, scale);
			t.pitch = static_cast<int>(t.buf->get_line_width());
			return t;
		}
		t.pitch         = w * scale + rng.range(0, 7);
		const int guard = rng.range(1, 3);
		// Room before the storage for a negative offset: bits, the address of
		// logical (0, 0), stays inside the allocation.
		const size_t front = static_cast<size_t>(guard) * t.pitch + guard
							 + static_cast<size_t>(std::max(0, -off_y)) * scale * t.pitch
							 + static_cast<size_t>(std::max(0, -off_x)) * scale;
		const size_t tail = static_cast<size_t>(guard) * t.pitch + guard
							+ static_cast<size_t>(std::max(0, off_y - h)) * scale * t.pitch
							+ static_cast<size_t>(std::max(0, off_x)) * scale;
		t.origin = front;
		t.backing.resize(front + static_cast<size_t>(h) * scale * t.pitch + tail);
		for (auto& b : t.backing) {
			b = rng.byte();
		}
		t.canary = t.backing;
		t.buf    = std::make_unique<Image_buffer8>(t.backing.data() + t.origin, t.pitch, w, h, off_x, off_y, scale);
		return t;
	}

	void fill_phys_random(Tbuf& t, Rng& rng) {
		for (int py = 0; py < t.phys_h(); py++) {
			for (int px = 0; px < t.phys_w(); px++) {
				t.phys(px, py) = rng.byte();
			}
		}
	}

	// Stores the nearest-neighbour upscale of the scale-1 buffer ref into t.
	void store_nn(Tbuf& t, Tbuf& ref) {
		for (int py = 0; py < t.phys_h(); py++) {
			for (int px = 0; px < t.phys_w(); px++) {
				t.phys(px, py) = ref.phys(px / t.scale, py / t.scale);
			}
		}
	}

	// Whether t holds the nearest-neighbour upscale of the scale-1 buffer
	// ref; if not, where the first difference is.
	bool equals_nn(Tbuf& t, Tbuf& ref, std::string& where) {
		std::vector<unsigned char> row(t.phys_w());
		for (int sy = 0; sy < t.h; sy++) {
			const unsigned char* from = &ref.phys(0, sy);
			for (int sx = 0; sx < t.w; sx++) {
				std::memset(row.data() + static_cast<size_t>(sx) * t.scale, from[sx], t.scale);
			}
			for (int k = 0; k < t.scale; k++) {
				const unsigned char* got = &t.phys(0, sy * t.scale + k);
				if (std::memcmp(got, row.data(), row.size()) != 0) {
					int px = 0;
					while (got[px] == row[px]) {
						px++;
					}
					std::ostringstream out;
					out << "physical (" << px << ", " << sy * t.scale + k << ") = " << int(got[px]) << ", expected " << int(row[px])
						<< " (logical " << px / t.scale - t.off_x << ", " << sy - t.off_y << ")";
					where = out.str();
					return false;
				}
			}
		}
		return true;
	}

	// Whether the storage equals the packed image `expected`.
	bool equals_image(Tbuf& t, const std::vector<unsigned char>& expected, std::string& where) {
		for (int py = 0; py < t.phys_h(); py++) {
			const unsigned char* got  = &t.phys(0, py);
			const unsigned char* want = expected.data() + static_cast<size_t>(py) * t.phys_w();
			if (std::memcmp(got, want, t.phys_w()) != 0) {
				int px = 0;
				while (got[px] == want[px]) {
					px++;
				}
				std::ostringstream out;
				out << "physical (" << px << ", " << py << ") = " << int(got[px]) << ", expected " << int(want[px]) << " (logical "
					<< px / t.scale - t.off_x << ", " << py / t.scale - t.off_y << ")";
				where = out.str();
				return false;
			}
		}
		return true;
	}

	std::string rect_text(const TileRect& r) {
		std::ostringstream out;
		out << '(' << r.x << ", " << r.y << ", " << r.w << ", " << r.h << ')';
		return out.str();
	}

	// Whether every pixel that differs between the packed images before and
	// after (storage of t) lies in the logical rectangle `tracked` times the
	// scale.
	bool changes_tracked(
			Tbuf& t, const std::vector<unsigned char>& before, const std::vector<unsigned char>& after, const TileRect& tracked,
			std::string& where) {
		for (int py = 0; py < t.phys_h(); py++) {
			for (int px = 0; px < t.phys_w(); px++) {
				const size_t i = static_cast<size_t>(py) * t.phys_w() + px;
				if (before[i] == after[i]) {
					continue;
				}
				const int x = px / t.scale - t.off_x;
				const int y = py / t.scale - t.off_y;
				if (x < tracked.x || x >= tracked.x + tracked.w || y < tracked.y || y >= tracked.y + tracked.h) {
					std::ostringstream out;
					out << "logical (" << x << ", " << y << ") changed outside the tracked rectangle " << rect_text(tracked);
					where = out.str();
					return false;
				}
			}
		}
		return true;
	}

	TileRect clip_of(Image_buffer& buf) {
		TileRect r;
		buf.get_clip(r.x, r.y, r.w, r.h);
		return r;
	}

	bool same_rect(const TileRect& a, const TileRect& b) {
		return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
	}

	// A random clip that overlaps the buffer (sometimes the whole buffer).
	void random_clip(Tbuf& t, Rng& rng) {
		if (rng.chance(30)) {
			t.buf->clear_clip();
			return;
		}
		const int cw = rng.range(1, t.w + 6);
		const int ch = rng.range(1, t.h + 6);
		t.buf->set_clip(-t.off_x + rng.range(1 - cw, t.w - 1), -t.off_y + rng.range(1 - ch, t.h - 1), cw, ch);
	}

	// Logical offsets of a view: mostly positive (the logical extent starts
	// at a negative coordinate, as in the engine's main buffer), sometimes
	// negative, sometimes zero.
	int random_offset(Rng& rng, int size) {
		const int r = rng.range(0, 9);
		return r < 2 ? 0 : r < 4 ? -rng.range(1, 9) : rng.range(1, size - 1 > 0 ? size - 1 : 1);
	}

	// Writes the physical block of logical (x, y) of t into the packed image
	// img: same scale copies the source block, otherwise its top-left sample.
	void model_block(Tbuf& dst, std::vector<unsigned char>& img, int dsx, int dsy, Tbuf& src, int ssx, int ssy) {
		for (int b = 0; b < dst.scale; b++) {
			for (int a = 0; a < dst.scale; a++) {
				const unsigned char pix = dst.scale == src.scale ? src.phys(ssx * src.scale + a, ssy * src.scale + b)
																 : src.phys(ssx * src.scale, ssy * src.scale);
				img[static_cast<size_t>(dsy * dst.scale + b) * dst.phys_w() + dsx * dst.scale + a] = pix;
			}
		}
	}

	void model_fill(Tbuf& dst, std::vector<unsigned char>& img, int dsx, int dsy, unsigned char pix) {
		for (int b = 0; b < dst.scale; b++) {
			std::memset(img.data() + static_cast<size_t>(dsy * dst.scale + b) * dst.phys_w() + dsx * dst.scale, pix, dst.scale);
		}
	}

	bool in_rect(int x, int y, const TileRect& r) {
		return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
	}

	/*
	 *  O1: one block of the op stream on a reference and a scaled buffer.
	 */
	struct O1_block {
		uint64_t seed;
		int      scale;
		bool     view;
		int      aux_scale;
		int      ops;
	};

	struct O1_result {
		int              ops_run = 0;
		std::string      failure;    // Empty: passed.
		std::vector<int> kind_counts = std::vector<int>(hires_test::num_ibuf_op_kinds, 0);
	};

	O1_result run_o1_block(const O1_block& blk, const Ibuf_fixtures& fx) {
		O1_result result;
		Rng       rng(blk.seed);
		const int w     = rng.range(8, 96);
		const int h     = rng.range(8, 64);
		const int off_x = blk.view ? random_offset(rng, w) : 0;
		const int off_y = blk.view ? random_offset(rng, h) : 0;
		Tbuf      ref   = make_tbuf(w, h, 1, blk.view, off_x, off_y, rng);
		Tbuf      sc    = make_tbuf(w, h, blk.scale, blk.view, off_x, off_y, rng);
		fill_phys_random(ref, rng);
		store_nn(sc, ref);

		const int aw      = rng.range(1, 40);
		const int ah      = rng.range(1, 40);
		Tbuf      ref_aux = make_tbuf(aw, ah, 1, false, 0, 0, rng);
		Tbuf      sc_aux  = make_tbuf(aw, ah, blk.aux_scale, false, 0, 0, rng);
		fill_phys_random(ref_aux, rng);
		store_nn(sc_aux, ref_aux);

		Tracker tracker;
		Tracker aux_tracker;
		sc.buf->set_tracker(&tracker);
		sc_aux.buf->set_tracker(&aux_tracker);

		const Ibuf_geometry geom{w, h, aw, ah, off_x, off_y};
		Ibuf_reads          ref_reads;
		Ibuf_reads          sc_reads;
		for (int i = 0; i < blk.ops; i++) {
			const Ibuf_op op = hires_test::make_ibuf_op(rng, geom, fx);
			result.kind_counts[static_cast<int>(op.kind)]++;
			const std::vector<unsigned char> ref_before     = ref.snapshot();
			const std::vector<unsigned char> ref_aux_before = ref_aux.snapshot();

			hires_test::apply_ibuf_op(op, *ref.buf, *ref_aux.buf, fx, ref_reads);
			if (op.kind == Ibuf_op_kind::fill_all) {
				// Upstream's fill8(pix) also fills the padding at the end of
				// every row (line_width * height bytes); the scaled one writes
				// the storage only.
				ref.canary = ref.backing;
			}
			const int ref_rand = op.kind == Ibuf_op_kind::fill_static ? std::rand() : 0;
			hires_test::apply_ibuf_op(op, *sc.buf, *sc_aux.buf, fx, sc_reads);
			const int sc_rand = op.kind == Ibuf_op_kind::fill_static ? std::rand() : 0;
			result.ops_run++;

			std::ostringstream fail;
			std::string        where;
			const TileRect     tracked     = tracker.take();
			const TileRect     aux_tracked = aux_tracker.take();
			if (!equals_nn(sc, ref, where)) {
				fail << "target is not NN(reference): " << where;
			} else if (!equals_nn(sc_aux, ref_aux, where)) {
				fail << "second buffer is not NN(reference): " << where;
			} else if (ref_reads.hash != sc_reads.hash) {
				fail << "the values read back differ";
			} else if (!same_rect(clip_of(*ref.buf), clip_of(*sc.buf))) {
				fail << "clip " << rect_text(clip_of(*sc.buf)) << ", reference " << rect_text(clip_of(*ref.buf));
			} else if (ref_rand != sc_rand) {
				fail << "fill_static used std::rand() differently";
			} else if (!sc.canaries_intact()) {
				fail << "a write outside the storage of the view";
			} else if (!ref.canaries_intact()) {
				fail << "the reference wrote outside its storage (the op stream left upstream's contract)";
			} else if (!changes_tracked(ref, ref_before, ref.snapshot(), tracked, where)) {
				fail << "tracker_complete: " << where;
			} else if (!changes_tracked(ref_aux, ref_aux_before, ref_aux.snapshot(), aux_tracked, where)) {
				fail << "tracker_complete (second buffer): " << where;
			}
			if (!fail.str().empty()) {
				std::ostringstream out;
				out << "seed 0x" << std::hex << blk.seed << std::dec << " scale " << blk.scale << (blk.view ? " view" : " owned")
					<< ' ' << w << 'x' << h << " offset (" << off_x << ", " << off_y << ") aux scale " << blk.aux_scale << ", op "
					<< i << ' ' << op.describe() << ": " << fail.str();
				result.failure = out.str();
				return result;
			}
		}
		return result;
	}
}    // namespace

TEST_CASE("ibuf scaled: new buffers and the scale-1 constructors") {
	Image_buffer8 plain(5, 4);
	CHECK(plain.get_pixel_scale() == 1);
	CHECK(plain.get_tracker() == nullptr);
	std::vector<unsigned char> mem(20 * 20);
	Image_buffer8              layer(20, 20, 20, mem.data(), 2);
	CHECK(layer.get_pixel_scale() == 1);

	for (const int s : all_scales) {
		CAPTURE(s);
		Image_buffer8 owned(7, 3, s);
		CHECK(owned.get_pixel_scale() == s);
		CHECK(owned.get_width() == 7u);
		CHECK(owned.get_height() == 3u);
		CHECK(owned.get_line_width() == static_cast<unsigned>(7 * s));
		const unsigned char* bits = owned.get_bits();
		CHECK(std::all_of(bits, bits + 7 * 3 * s * s, [](unsigned char b) {
			return b == 0;
		}));
		int cx;
		int cy;
		int cw;
		int ch;
		owned.get_clip(cx, cy, cw, ch);
		CHECK((cx == 0 && cy == 0 && cw == 7 && ch == 3));

		std::vector<unsigned char> store(static_cast<size_t>(40) * 10 * s, 0);
		Image_buffer8              view(store.data(), 40, 6, 4, 2, -3, s);
		CHECK(view.get_pixel_scale() == s);
		CHECK(view.get_line_width() == 40u);
		view.get_clip(cx, cy, cw, ch);
		CHECK((cx == -2 && cy == 3 && cw == 6 && ch == 4));
		// Logical (0, 0) is two blocks right of the storage origin.
		view.put_pixel8(9, -2, 3);
		CHECK(store[0] == 9);
		CHECK(store[static_cast<size_t>(s - 1) * 40 + s - 1] == 9);
		view.put_pixel8(8, 0, 3);
		CHECK(store[2 * s] == 8);
		// bits is the address of logical (0, 0), here before the storage.
		CHECK(reinterpret_cast<std::intptr_t>(view.get_bits()) - reinterpret_cast<std::intptr_t>(store.data())
			  == 2 * s - 3 * s * 40);
	}
}

TEST_CASE("ibuf scaled: Write_tracker") {
	Tracker t;
	CHECK(t.empty());
	TileRect r = t.take();
	CHECK((r.x == 0 && r.y == 0 && r.w == 0 && r.h == 0));
	t.add(-17, -3, 4, 2);
	t.add(5, 10, 0, 7);     // Empty: ignored.
	t.add(5, 10, 3, -1);    // Empty: ignored.
	CHECK(!t.empty());
	t.add(0, 0, 1, 1);
	r = t.take();
	CHECK((r.x == -17 && r.y == -3 && r.w == 18 && r.h == 4));
	CHECK(t.empty());
	// mark_all replaces whatever was there, also a larger rectangle.
	t.add(-100, -100, 1000, 1000);
	t.mark_all(-17, 0, 355, 200);
	r = t.take();
	CHECK((r.x == -17 && r.y == 0 && r.w == 355 && r.h == 200));
	t.add(1, 2, 3, 4);
	t.reset();
	CHECK(t.empty());
}

TEST_CASE("ibuf scaled: O1, the op stream gives NN(scale 1) at every scale, with tracker_complete") {
	const Ibuf_fixtures fx(0x5ca1'ed00'0001ULL);
	std::vector<int>    kind_counts(hires_test::num_ibuf_op_kinds, 0);
	int                 blocks = 0;
	for (const int s : test_scales) {
		for (int seed = 0; seed < 5; seed++) {
			for (const bool view : {false, true}) {
				// The second buffer (get, put) at scale 1, at the same scale, or
				// at another one.
				const int      other     = s == 2 ? 3 : 2;
				const int      aux_scale = seed % 3 == 0 ? 1 : seed % 3 == 1 ? s : other;
				const O1_block blk{
						0x01'0000'0000ULL * static_cast<uint64_t>(s) + 0x100ULL * static_cast<uint64_t>(seed) + (view ? 1 : 0), s,
						view, aux_scale, 600};
				const O1_result result = run_o1_block(blk, fx);
				for (int k = 0; k < hires_test::num_ibuf_op_kinds; k++) {
					kind_counts[k] += result.kind_counts[k];
				}
				blocks++;
				CHECK_MESSAGE(result.failure.empty(), result.failure);
				CHECK(result.ops_run == blk.ops);
			}
		}
	}
	CHECK(blocks == 30);
	for (int k = 0; k < hires_test::num_ibuf_op_kinds; k++) {
		INFO("op kind " << std::string(hires_test::ibuf_op_name(static_cast<Ibuf_op_kind>(k))));
		CHECK(kind_counts[k] >= 30);
	}
}

TEST_CASE("ibuf scaled: get, put and blit for every pair of scales") {
	Rng rng(0x6e7'0000'0001ULL);
	for (const int ss : all_scales) {
		for (const int ds : all_scales) {
			for (int trial = 0; trial < 60; trial++) {
				// 0: dst.put(src), 1: dst.blit(src), 2: src.get(dst), where
				// src is the buffer read and dst the one written.
				const int  kind     = trial % 3;
				const bool both_one = ss == 1 && ds == 1;
				// Upstream's scale-1 put and get ignore the offsets of the
				// second buffer, so that pair gets them only through blit.
				const bool src_offsets = !(both_one && kind == 0) && rng.chance(60);
				const bool dst_offsets = !(both_one && kind == 2) && rng.chance(60);
				const int  sw          = rng.range(1, 24);
				const int  sh          = rng.range(1, 24);
				const int  dw          = rng.range(4, 40);
				const int  dh          = rng.range(4, 40);
				// Upstream's scale-1 put also reads the source with its width
				// as the pitch.
				const bool src_view = (src_offsets || rng.chance(50)) && !(both_one && kind == 0);
				const bool dst_view = dst_offsets || rng.chance(50);
				Tbuf       src      = make_tbuf(
                        sw, sh, ss, src_view, src_offsets ? random_offset(rng, sw) : 0, src_offsets ? random_offset(rng, sh) : 0,
                        rng);
				Tbuf dst = make_tbuf(
						dw, dh, ds, dst_view, dst_offsets ? random_offset(rng, dw) : 0, dst_offsets ? random_offset(rng, dh) : 0,
						rng);
				fill_phys_random(src, rng);
				fill_phys_random(dst, rng);
				random_clip(src, rng);
				random_clip(dst, rng);
				Tracker src_tracker;
				Tracker dst_tracker;
				src.buf->set_tracker(&src_tracker);
				dst.buf->set_tracker(&dst_tracker);

				const std::vector<unsigned char> src_before = src.snapshot();
				const std::vector<unsigned char> dst_before = dst.snapshot();
				std::vector<unsigned char>       expected   = dst_before;
				std::ostringstream               what;
				what
						<< (kind == 0   ? "put"
							: kind == 1 ? "blit"
										: "get")
						<< " scale " << ss << " -> " << ds << " src " << sw << 'x' << sh << " off (" << src.off_x << ", "
						<< src.off_y << ") dst " << dw << 'x' << dh << " off (" << dst.off_x << ", " << dst.off_y << ") clips "
						<< rect_text(clip_of(*src.buf)) << ' ' << rect_text(clip_of(*dst.buf));
				if (kind == 2) {
					// dst's logical (u, v) takes src's logical (x + u, y + v),
					// over dst's storage extent, inside src's clip.
					const int      x    = rng.range(-src.off_x - dw - 2, sw - src.off_x + 2);
					const int      y    = rng.range(-src.off_y - dh - 2, sh - src.off_y + 2);
					const TileRect clip = clip_of(*src.buf);
					what << " at (" << x << ", " << y << ')';
					for (int dsy = 0; dsy < dh; dsy++) {
						for (int dsx = 0; dsx < dw; dsx++) {
							const int lx = x + dsx - dst.off_x;
							const int ly = y + dsy - dst.off_y;
							if (in_rect(lx, ly, clip)) {
								model_block(dst, expected, dsx, dsy, src, lx + src.off_x, ly + src.off_y);
							}
						}
					}
					src.buf->get(dst.buf.get(), x, y);
				} else {
					// src's logical (u, v), over its storage extent, goes to
					// dst's logical (x + u, y + v), inside dst's clip.
					const int      x    = rng.range(-dst.off_x - sw - 2, dw - dst.off_x + 2);
					const int      y    = rng.range(-dst.off_y - sh - 2, dh - dst.off_y + 2);
					const TileRect clip = clip_of(*dst.buf);
					what << " at (" << x << ", " << y << ')';
					for (int ssy = 0; ssy < sh; ssy++) {
						for (int ssx = 0; ssx < sw; ssx++) {
							const int lx = x + ssx - src.off_x;
							const int ly = y + ssy - src.off_y;
							if (in_rect(lx, ly, clip)) {
								model_block(dst, expected, lx + dst.off_x, ly + dst.off_y, src, ssx, ssy);
							}
						}
					}
					if (kind == 0) {
						dst.buf->put(src.buf.get(), x, y);
					} else {
						dst.buf->blit(*src.buf, x, y);
					}
				}
				INFO(what.str());
				std::string where;
				CHECK_MESSAGE(equals_image(dst, expected, where), where);
				CHECK_MESSAGE(equals_image(src, src_before, where), "the source changed: " << where);
				CHECK(dst.canaries_intact());
				CHECK(src.canaries_intact());
				CHECK(src_tracker.empty());
				// Scale-1 put and get run upstream's code, which has no tracker
				// (the engine sets one only on a scaled main buffer).
				if (!both_one || kind == 1) {
					const TileRect tracked = dst_tracker.take();
					CHECK_MESSAGE(changes_tracked(dst, dst_before, dst.snapshot(), tracked, where), where);
				}
			}
		}
	}
}

TEST_CASE("ibuf scaled: a get/put round trip through create_another keeps the physical detail") {
	Rng rng(0xc4ea'7e00'0001ULL);
	for (const int s : all_scales) {
		for (int trial = 0; trial < 20; trial++) {
			const int  w    = rng.range(4, 40);
			const int  h    = rng.range(4, 40);
			const bool view = rng.chance(50);
			Tbuf       t    = make_tbuf(w, h, s, view, view ? random_offset(rng, w) : 0, view ? random_offset(rng, h) : 0, rng);
			fill_phys_random(t, rng);
			const std::vector<unsigned char> before = t.snapshot();

			const int bw    = rng.range(1, w);
			const int bh    = rng.range(1, h);
			auto      saved = t.buf->create_another(bw, bh);
			CAPTURE(s);
			CHECK(saved->get_pixel_scale() == s);
			CHECK(saved->get_width() == static_cast<unsigned>(bw));
			CHECK(saved->get_height() == static_cast<unsigned>(bh));
			CHECK(saved->get_line_width() == static_cast<unsigned>(bw * s));
			const int x = -t.off_x + rng.range(0, w - bw);
			const int y = -t.off_y + rng.range(0, h - bh);
			t.buf->get(saved.get(), x, y);
			t.buf->fill8(rng.byte(), bw, bh, x, y);
			t.buf->put(saved.get(), x, y);
			std::string where;
			CHECK_MESSAGE(equals_image(t, before, where), where);
			CHECK(t.canaries_intact());
		}
	}
}

TEST_CASE("ibuf scaled: put_phys clips against the clip times the scale and the source size") {
	Rng rng(0x9b75'0000'0001ULL);
	for (const int s : all_scales) {
		for (int trial = 0; trial < 150; trial++) {
			const int  w    = rng.range(2, 30);
			const int  h    = rng.range(2, 30);
			const bool view = rng.chance(50);
			Tbuf       t    = make_tbuf(w, h, s, view, view ? random_offset(rng, w) : 0, view ? random_offset(rng, h) : 0, rng);
			fill_phys_random(t, rng);
			random_clip(t, rng);
			Tracker tracker;
			t.buf->set_tracker(&tracker);

			const int                  pw    = rng.range(0, 3 * s + 40);
			const int                  ph    = rng.range(0, 3 * s + 40);
			const int                  pitch = pw + rng.range(0, 5);
			std::vector<unsigned char> src(static_cast<size_t>(pitch) * std::max(ph, 1));
			for (auto& b : src) {
				b = rng.byte();
			}
			// Physical position relative to logical (0, 0).
			const int px = rng.range(-t.off_x * s - pw - 4, (w - t.off_x) * s + 4);
			const int py = rng.range(-t.off_y * s - ph - 4, (h - t.off_y) * s + 4);

			const TileRect                   clip     = clip_of(*t.buf);
			const std::vector<unsigned char> before   = t.snapshot();
			std::vector<unsigned char>       expected = before;
			int                              lx0      = INT_MAX;
			int                              ly0      = INT_MAX;
			int                              lx1      = INT_MIN;
			int                              ly1      = INT_MIN;
			for (int j = 0; j < ph; j++) {
				for (int i = 0; i < pw; i++) {
					const int x = px + i;
					const int y = py + j;
					if (x < clip.x * s || x >= (clip.x + clip.w) * s || y < clip.y * s || y >= (clip.y + clip.h) * s) {
						continue;
					}
					expected[static_cast<size_t>(y + t.off_y * s) * t.phys_w() + x + t.off_x * s]
							= src[static_cast<size_t>(j) * pitch + i];
					// Logical block of the pixel (floor division).
					const int bx = (x + t.off_x * s) / s - t.off_x;
					const int by = (y + t.off_y * s) / s - t.off_y;
					lx0          = std::min(lx0, bx);
					ly0          = std::min(ly0, by);
					lx1          = std::max(lx1, bx + 1);
					ly1          = std::max(ly1, by + 1);
				}
			}
			t.buf->put_phys(src.data(), pw, ph, pitch, px, py);
			INFO("scale " << s << ' ' << w << 'x' << h << " off (" << t.off_x << ", " << t.off_y << ") clip " << rect_text(clip)
						  << " src " << pw << 'x' << ph << " at (" << px << ", " << py << ')');
			std::string where;
			CHECK_MESSAGE(equals_image(t, expected, where), where);
			CHECK(t.canaries_intact());
			const TileRect tracked = tracker.take();
			if (lx0 == INT_MAX) {
				CHECK(tracked.w == 0);
			} else {
				// Exactly the logical blocks touched: floor at the start,
				// ceiling at the end.
				CHECK((tracked.x == lx0 && tracked.y == ly0 && tracked.w == lx1 - lx0 && tracked.h == ly1 - ly0));
			}
		}
	}
}

TEST_CASE("ibuf scaled: get_pixel8 reads the top-left physical sample") {
	Rng rng(0x9e7'0000'0001ULL);
	for (const int s : all_scales) {
		for (int trial = 0; trial < 20; trial++) {
			const int  w    = rng.range(1, 30);
			const int  h    = rng.range(1, 30);
			const bool view = rng.chance(50);
			Tbuf       t    = make_tbuf(w, h, s, view, view ? random_offset(rng, w) : 0, view ? random_offset(rng, h) : 0, rng);
			fill_phys_random(t, rng);
			for (int y = -t.off_y; y < h - t.off_y; y++) {
				for (int x = -t.off_x; x < w - t.off_x; x++) {
					if (t.buf->get_pixel8(x, y) != t.sample(x, y)) {
						FAIL_CHECK("scale " << s << ": get_pixel8(" << x << ", " << y << ")");
						break;
					}
				}
			}
		}
	}
}

TEST_CASE("ibuf scaled: fill_static draws one std::rand() per logical pixel, as at scale 1") {
	Rng rng(0xf5'0000'0001ULL);
	for (const int s : test_scales) {
		for (int trial = 0; trial < 6; trial++) {
			const int  w     = rng.range(1, 80);
			const int  h     = rng.range(1, 50);
			const bool view  = trial % 2 == 1;
			const int  off_x = view ? random_offset(rng, w) : 0;
			const int  off_y = view ? random_offset(rng, h) : 0;
			Tbuf       ref   = make_tbuf(w, h, 1, view, off_x, off_y, rng);
			Tbuf       sc    = make_tbuf(w, h, s, view, off_x, off_y, rng);
			fill_phys_random(ref, rng);
			fill_phys_random(sc, rng);
			const auto seed = rng.next32();
			std::srand(seed);
			ref.buf->fill_static(1, 2, 3);
			const int ref_next = std::rand();
			std::srand(seed);
			sc.buf->fill_static(1, 2, 3);
			const int sc_next = std::rand();
			CAPTURE(s);
			CHECK(ref_next == sc_next);
			std::string where;
			CHECK_MESSAGE(equals_nn(sc, ref, where), where);
			CHECK(sc.canaries_intact());
		}
	}
}

TEST_CASE("ibuf scaled: copy() clips rectangles out of range (no scale-1 reference)") {
	Rng rng(0xc0b7'0000'0001ULL);
	for (const int s : test_scales) {
		for (int trial = 0; trial < 300; trial++) {
			const int  w    = rng.range(1, 40);
			const int  h    = rng.range(1, 30);
			const bool view = rng.chance(60);
			Tbuf       t    = make_tbuf(w, h, s, view, view ? random_offset(rng, w) : 0, view ? random_offset(rng, h) : 0, rng);
			fill_phys_random(t, rng);
			random_clip(t, rng);    // copy() ignores the clip.
			Tracker tracker;
			t.buf->set_tracker(&tracker);

			const int x0 = -t.off_x;
			const int y0 = -t.off_y;
			int       a[6];
			switch (rng.range(0, 3)) {
			case 0:    // Near the buffer, partly outside.
				a[2] = rng.range(-2, w + 8);
				a[3] = rng.range(-2, h + 8);
				a[0] = x0 + rng.range(-w - 4, w + 4);
				a[1] = y0 + rng.range(-h - 4, h + 4);
				a[4] = x0 + rng.range(-w - 4, w + 4);
				a[5] = y0 + rng.range(-h - 4, h + 4);
				break;
			case 1:    // A scroll by a few pixels over the whole buffer.
				a[0] = x0;
				a[1] = y0;
				a[2] = w;
				a[3] = h;
				a[4] = x0 + rng.range(-4, 4);
				a[5] = y0 + rng.range(-4, 4);
				break;
			case 2:    // Far away or huge.
				a[2] = rng.chance(50) ? rng.range(0, 1 << 30) : rng.range(-5, 5);
				a[3] = rng.chance(50) ? rng.range(0, 1 << 30) : rng.range(-5, 5);
				a[0] = rng.range(-(1 << 30), 1 << 30);
				a[1] = rng.range(-(1 << 30), 1 << 30);
				a[4] = rng.chance(50) ? x0 + rng.range(-3, 3) : rng.range(-(1 << 30), 1 << 30);
				a[5] = rng.chance(50) ? y0 + rng.range(-3, 3) : rng.range(-(1 << 30), 1 << 30);
				break;
			default:    // In range, as in the O1 stream.
				a[2] = rng.range(1, w);
				a[3] = rng.range(1, h);
				a[0] = x0 + rng.range(0, w - a[2]);
				a[1] = y0 + rng.range(0, h - a[3]);
				a[4] = x0 + rng.range(0, w - a[2]);
				a[5] = y0 + rng.range(0, h - a[3]);
				break;
			}
			const std::vector<unsigned char> before   = t.snapshot();
			std::vector<unsigned char>       expected = before;
			// Every (i, j) of the rectangle whose source and destination both
			// lie in the logical extent is copied, from the old contents.
			auto inside = [&](long long x, long long y) {
				return x >= x0 && x < x0 + w && y >= y0 && y < y0 + h;
			};
			if (a[2] > 0 && a[3] > 0) {
				for (int sy = 0; sy < h; sy++) {
					for (int sx = 0; sx < w; sx++) {
						// Destination logical (x0 + sx, y0 + sy) <- source
						// (x0 + sx - destx + srcx, ...).
						const long long i = static_cast<long long>(x0) + sx - a[4];
						const long long j = static_cast<long long>(y0) + sy - a[5];
						if (i < 0 || j < 0 || i >= a[2] || j >= a[3]) {
							continue;
						}
						const long long fx = a[0] + i;
						const long long fy = a[1] + j;
						if (!inside(fx, fy)) {
							continue;
						}
						for (int b = 0; b < s; b++) {
							std::memcpy(
									expected.data() + static_cast<size_t>(sy * s + b) * t.phys_w() + sx * s,
									before.data() + static_cast<size_t>((fy - y0) * s + b) * t.phys_w() + (fx - x0) * s, s);
						}
					}
				}
			}
			t.buf->copy(a[0], a[1], a[2], a[3], a[4], a[5]);
			INFO("scale " << s << ' ' << w << 'x' << h << " off (" << t.off_x << ", " << t.off_y << ") copy(" << a[0] << ", "
						  << a[1] << ", " << a[2] << ", " << a[3] << ", " << a[4] << ", " << a[5] << ')');
			std::string where;
			CHECK_MESSAGE(equals_image(t, expected, where), where);
			CHECK(t.canaries_intact());
			const TileRect tracked = tracker.take();
			CHECK_MESSAGE(changes_tracked(t, before, t.snapshot(), tracked, where), where);
		}
	}
}

TEST_CASE("ibuf scaled: RLE scans of 4096 px and more and malformed runs (no scale-1 reference)") {
	Rng                        rng(0x41e'0000'0001ULL);
	std::vector<unsigned char> remap(256);
	for (int i = 0; i < 256; i++) {
		remap[i] = static_cast<unsigned char>((i * 7 + 3) & 0xff);
	}
	int wide_scans      = 0;
	int malformed_scans = 0;
	for (const int s : test_scales) {
		for (int trial = 0; trial < 24; trial++) {
			const int  w    = trial % 2 == 0 ? rng.range(20, 60) : rng.range(500, 900);
			const int  h    = rng.range(3, 9);
			const bool view = rng.chance(60);
			Tbuf       t    = make_tbuf(w, h, s, view, view ? random_offset(rng, w) : 0, view ? random_offset(rng, h) : 0, rng);
			fill_phys_random(t, rng);
			random_clip(t, rng);
			Tracker tracker;
			t.buf->set_tracker(&tracker);

			// Scans relative to the paint origin (xoff, yoff).
			const int  xoff = -t.off_x + rng.range(-20, 20);
			const int  yoff = -t.off_y + rng.range(-2, 2);
			Rle_writer rle;
			const int  scans = rng.range(4, 14);
			for (int n = 0; n < scans; n++) {
				const int kind = rng.range(0, 5);
				int       len;
				if (kind <= 1) {
					len = rng.range(4096, 32767);
					wide_scans++;
				} else {
					len = rng.range(1, 2 * w);
				}
				// Somewhere around the buffer, often cut on both sides.
				const int                  x = std::max(-32768, std::min(32767, rng.range(-len - 8, w + 8) - xoff - t.off_x));
				const int                  y = rng.range(-2, h + 1) - yoff - t.off_y;
				std::vector<unsigned char> pixels(len);
				unsigned char              pix = rng.byte();
				for (auto& p : pixels) {
					if (rng.chance(30)) {
						pix = rng.byte();
					}
					p = pix;
				}
				if (kind == 0 || kind == 2) {
					rle.raw(x, y, pixels);
				} else if (kind == 1 || kind == 3) {
					rle.encoded(x, y, pixels);
				} else {
					// Malformed: the runs add up to more than the header says,
					// with a zero-length run now and then.
					rle.begin_encoded(x, y, len);
					int covered = 0;
					while (covered < len) {
						const int count = rng.chance(5) ? 0 : rng.range(1, Rle_writer::max_run);
						if (rng.chance(50)) {
							rle.repeat_run(count, rng.byte());
						} else {
							std::vector<unsigned char> lit(count);
							for (auto& p : lit) {
								p = rng.byte();
							}
							rle.literal_run(lit);
						}
						covered += count;
					}
					if (covered > len) {
						malformed_scans++;
					}
				}
			}
			const std::vector<unsigned char> data     = rle.finish();
			const bool                       remapped = rng.chance(50);
			const TileRect                   clip     = clip_of(*t.buf);
			const std::vector<unsigned char> before   = t.snapshot();
			std::vector<unsigned char>       expected = before;
			const bool parsed_ok = decode_rle(data, xoff, yoff, Rle_policy::clamp, [&](int x, int y, unsigned char p) {
				if (in_rect(x, y, clip)) {
					model_fill(t, expected, x + t.off_x, y + t.off_y, remapped ? remap[p] : p);
				}
			});
			REQUIRE(parsed_ok);
			if (remapped) {
				const unsigned char* trans = remap.data();
				t.buf->paint_rle_remapped(xoff, yoff, data.data(), trans);
			} else {
				t.buf->paint_rle(xoff, yoff, data.data());
			}
			INFO("scale " << s << ' ' << w << 'x' << h << " off (" << t.off_x << ", " << t.off_y << ") clip " << rect_text(clip)
						  << " at (" << xoff << ", " << yoff << ")" << (remapped ? " remapped" : ""));
			std::string where;
			CHECK_MESSAGE(equals_image(t, expected, where), where);
			CHECK(t.canaries_intact());
			const TileRect tracked = tracker.take();
			CHECK_MESSAGE(changes_tracked(t, before, t.snapshot(), tracked, where), where);
		}
	}
	CHECK(wide_scans >= 20);
	CHECK(malformed_scans >= 10);
}
