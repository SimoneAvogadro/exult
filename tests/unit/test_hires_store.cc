/*
 *  test_hires_store.cc - The hi-res override store: loading, the rules with
 *  their IDs, groups, roots, reduction, terrain overrides, bundles and the
 *  per-scale store set (DESIGN.md section 6.2, test_hires_store), plus the
 *  shared rule fixtures of tests/data/hires/rules and the engine's source
 *  provider on the synthetic game.
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
#include "hires_bundle.h"
#include "hires_png.h"
#include "hires_rules.h"
#include "hires_store.h"
#include "hires_test_util.h"
#include "hires_vga.h"
#include "test_support.h"
#include "vgafile.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Hires::Rule;
using Hires::Severity;
using hires_test::Fake_flats;
using hires_test::Rng;
using hires_test::Temp_dir;
using hires_test::write_tile;
namespace fs = std::filesystem;

namespace {
	using Flat = std::array<uint8_t, 64>;

	// A flat of static indices 0x10-0x8f, with some E0-E7 glints when asked.
	Flat make_flat(Rng& rng, bool glints = false) {
		Flat f{};
		for (auto& v : f) {
			v = static_cast<uint8_t>(rng.range(0x10, 0x8f));
		}
		if (glints) {
			f[9]  = 0xe2;
			f[46] = 0xe5;
		}
		return f;
	}

	// The world of a test: a palette and flats 0-9 x 0-31 of shapes 0-19 (of 40),
	// shape 30 absent (RLE or missing for the provider).
	struct World {
		Hires::Pal8 pal = hires_test::test_palette();
		Fake_flats  flats{40};
		Rng         rng{0x5703};

		World() {
			for (int s = 0; s < 20; s++) {
				for (int f = 0; f < 32; f++) {
					flats.set(s, f, make_flat(rng, s == 7));
				}
			}
		}

		const uint8_t* flat(int s, int f) const {
			return flats.get(s, f);
		}

		std::vector<uint8_t> nn(int s, int f, int scale) const {
			return hires_test::nn_tile(flat(s, f), scale);
		}

		std::string guard(int s, int f) const {
			return hires_test::guard_of(flat(s, f));
		}

		Hires::Report load(Hires::Store& store, const std::vector<fs::path>& roots, int scale, int s_art = 6) const {
			std::vector<Hires::Root> r;
			for (const auto& p : roots) {
				r.push_back({p.string(), p.filename().string()});
			}
			return store.load(r, scale, s_art, pal, flats.provider());
		}
	};

	[[maybe_unused]] std::string flats_dir(const fs::path& root, int scale = 6) {
		return (root / ("x" + std::to_string(scale)) / "flats").string();
	}

	[[maybe_unused]] bool same_pixels(const Hires::Tile_view& v, const std::vector<uint8_t>& px) {
		return v.px != nullptr && static_cast<size_t>(v.side) * v.side == px.size() && std::equal(px.begin(), px.end(), v.px);
	}

	// The finding about a path (the last one with that rule), or nullptr.
	[[maybe_unused]] const Hires::Finding* finding_for(const Hires::Report& rep, const std::string& path_end, Rule rule) {
		const Hires::Finding* found = nullptr;
		for (const auto& f : rep.findings) {
			if (f.rule == rule && f.path.size() >= path_end.size()
				&& f.path.compare(f.path.size() - path_end.size(), path_end.size(), path_end) == 0) {
				found = &f;
			}
		}
		return found;
	}

	// A copy of 'bytes' with one byte replaced.
	[[maybe_unused]] std::vector<uint8_t> patched(const std::vector<uint8_t>& bytes, size_t offset, uint8_t value) {
		std::vector<uint8_t> out(bytes);
		if (offset < out.size()) {
			out[offset] = value;
		}
		return out;
	}

	[[maybe_unused]] std::string dump(const Hires::Report& rep) {
		std::string s = rep.summary(0);
		for (const auto& l : rep.lines) {
			s += "\n  " + l;
		}
		return s;
	}

	// tests/data/hires/<dir>/expected.txt: "<case>/<file> <RULE|OK> <severity>".
	struct Expectation {
		std::string file;
		std::string rule;
		std::string severity;
	};

	[[maybe_unused]] std::vector<Expectation> read_expected(const std::string& dir) {
		std::ifstream            in(hires_test::data_path("hires/" + dir + "/expected.txt"));
		std::vector<Expectation> out;
		std::string              line;
		while (std::getline(in, line)) {
			if (line.empty() || line[0] == '#') {
				continue;
			}
			std::istringstream fields(line);
			Expectation        e;
			if (fields >> e.file >> e.rule >> e.severity) {    // CRLF checkouts: '\r' is a blank
				out.push_back(e);
			}
		}
		return out;
	}
}    // namespace

TEST_CASE("hires store: key names") {
	int shape = 0;
	int frame = 0;
	CHECK(Hires::parse_tile_name("0123_04.png", shape, frame));
	CHECK(shape == 123);
	CHECK(frame == 4);
	CHECK(Hires::parse_tile_name("9999_99.png", shape, frame));
	CHECK_FALSE(Hires::parse_tile_name("123_04.png", shape, frame));
	CHECK_FALSE(Hires::parse_tile_name("0123_04.PNG", shape, frame));
	CHECK_FALSE(Hires::parse_tile_name("0123-04.png", shape, frame));
	CHECK_FALSE(Hires::parse_tile_name("0123_4a.png", shape, frame));
	CHECK(Hires::tile_name(19, 3) == "0019_03.png");
	uint64_t key = 0;
	CHECK(Hires::parse_terrain_name("9a1c0e44b2d6f001.png", key));
	CHECK(key == 0x9a1c0e44b2d6f001ULL);
	CHECK_FALSE(Hires::parse_terrain_name("9A1C0E44B2D6F001.png", key));
	CHECK_FALSE(Hires::parse_terrain_name("9a1c0e44b2d6f00.png", key));
	CHECK(Hires::terrain_name(0x1fULL) == "000000000000001f.png");
	uint32_t crc = 0;
	CHECK(Hires::parse_crc_text("c9c2c0e7", crc));
	CHECK(crc == 0xc9c2c0e7U);
	CHECK(Hires::parse_crc_text(" 0xC9C2C0E7 ", crc));
	CHECK(crc == 0xc9c2c0e7U);
	CHECK_FALSE(Hires::parse_crc_text("1c9c2c0e7", crc));
	CHECK_FALSE(Hires::parse_crc_text("xyz", crc));
	CHECK_FALSE(Hires::parse_crc_text("", crc));
}

#ifdef HAVE_PNG_H

TEST_CASE("hires store: a valid tile is served at 8 S; frame & 31; explain") {
	World    w;
	Temp_dir tmp;
	write_tile(fs::path(flats_dir(tmp.path())) / "0005_03.png", w.nn(5, 3, 6), 48, w.pal, w.guard(5, 3));
	write_tile(fs::path(flats_dir(tmp.path())) / "0006_03.png", w.nn(6, 3, 6), 48, w.pal, w.guard(6, 3));
	Hires::Store        store;
	const Hires::Report rep = w.load(store, {tmp.path()}, 6);
	INFO(dump(rep));
	CHECK(rep.tiles == 2);
	CHECK(rep.loaded == 2);
	CHECK(rep.rejected == 0);
	CHECK(rep.unguarded == 0);
	CHECK(rep.roots == 1);
	CHECK(store.tile_count() == 2);
	const Hires::Tile_view v = store.flat(5, 3);
	CHECK(v.side == 48);
	CHECK(same_pixels(v, w.nn(5, 3, 6)));
	CHECK(store.flat(5, 3 + 32).px == v.px);    // frame & 31
	const Hires::Tile_view v6 = store.flat(6, 3);
	CHECK(same_pixels(v6, w.nn(6, 3, 6)));
	CHECK(store.flat(6, 3 + 32).px == v6.px);    // (an even shape: bit 5 is not the shape's)
	CHECK(store.flat(6, 3 + 64).px == v6.px);
	CHECK(store.flat(5, 4).px == nullptr);
	CHECK(store.flat(-1, 0).px == nullptr);
	const Hires::Entry_info* e = store.explain_flat(5, 3);
	REQUIRE(e != nullptr);
	CHECK(e->state == Hires::Entry_state::loaded);
	CHECK(e->guarded);
	CHECK_FALSE(e->from_bundle);
	CHECK(e->root == 0);
	CHECK(store.explain_flat(5, 4) == nullptr);
	CHECK(rep.summary(6).find("x6: 2 tiles loaded") == 0);
}

TEST_CASE("hires store: rejects carry the rule IDs F2 F3 P0 P4 G1 N1") {
	World          w;
	Temp_dir       tmp;
	const fs::path d(flats_dir(tmp.path()));
	// F2: one used colour changed in the PLTE.
	{
		Hires::Pal8 bad = w.pal;
		const auto  px  = w.nn(1, 0, 6);
		bad.rgb[3 * px[0]] ^= 0x40;
		write_tile(d / "0001_00.png", px, 48, bad);
	}
	// F3: 40 x 40.
	{
		std::vector<uint8_t> px(40 * 40, 0x20);
		REQUIRE(Hires::write_indexed_png(d / "0001_01.png", 40, 40, px.data(), w.pal.rgb, 256) == Hires::Png_status::ok);
	}
	// P0: one 0xFF.
	{
		auto px = w.nn(1, 2, 6);
		px[77]  = 0xff;
		write_tile(d / "0001_02.png", px, 48, w.pal);
	}
	// P4: 16 E3 pixels above a parent without cycling (0.69 % > 0.5 %).
	{
		auto px = w.nn(1, 3, 6);
		for (int i = 0; i < 16; i++) {
			px[static_cast<size_t>(i) * 48 + 5] = 0xe3;
		}
		write_tile(d / "0001_03.png", px, 48, w.pal);
	}
	// P4 warning only: one pixel.
	{
		auto px = w.nn(1, 4, 6);
		px[5]   = 0xe3;
		write_tile(d / "0001_04.png", px, 48, w.pal);
	}
	// G1: stale and malformed guards.
	write_tile(d / "0001_05.png", w.nn(1, 5, 6), 48, w.pal, "00000000");
	write_tile(d / "0001_06.png", w.nn(1, 6, 6), 48, w.pal, "xyz");
	write_tile(d / "0001_07.png", w.nn(1, 7, 6), 48, w.pal, "0x" + w.guard(1, 7));    // accepted
	// N1: name, frame, shape beyond the provider's range, no flat for the key.
	write_tile(d / "tile.png", w.nn(1, 8, 6), 48, w.pal);
	write_tile(d / "0001_40.png", w.nn(1, 8, 6), 48, w.pal);
	write_tile(d / "0900_00.png", w.nn(1, 8, 6), 48, w.pal);
	write_tile(d / "0030_00.png", w.nn(1, 8, 6), 48, w.pal);
	// F1: RGB and garbage.
	{
		std::vector<uint8_t> rgb(48 * 48 * 3, 0x33);
		hires_test::write_file(
				d / "0001_09.png",
				hires_test::Png_builder().ihdr(48, 48, 8, 2).idat(hires_test::filtered_rows(rgb.data(), 144, 48)).iend().bytes());
		hires_test::write_file(d / "0001_10.png", {1, 2, 3, 4});
	}
	// F4: tRNS, accepted with a warning.
	{
		Hires::Png_write_options opt;
		opt.trns = {0};
		write_tile(d / "0001_11.png", w.nn(1, 11, 6), 48, w.pal, "", opt);
	}

	Hires::Store        store;
	const Hires::Report rep = w.load(store, {tmp.path()}, 6);
	INFO(dump(rep));

	struct Case {
		const char* file;
		Rule        rule;
	};

	const Case rejected[] = {
			{"0001_00.png",    Rule::f2_plte},
            {"0001_01.png",    Rule::f3_size},
            {"0001_02.png",  Rule::p0_border},
			{"0001_03.png", Rule::p4_cycling},
            {"0001_05.png",   Rule::g1_guard},
            {"0001_06.png",   Rule::g1_guard},
			{   "tile.png",    Rule::n1_name},
            {"0001_40.png",    Rule::n1_name},
            {"0900_00.png",    Rule::n1_name},
			{"0030_00.png",    Rule::n1_name},
            {"0001_09.png",  Rule::f1_colour},
            {"0001_10.png",  Rule::f1_colour}
    };
	for (const auto& c : rejected) {
		INFO(c.file);
		const Hires::Finding* f = finding_for(rep, c.file, c.rule);
		REQUIRE(f != nullptr);
		CHECK(f->severity == Severity::reject);
	}
	CHECK(rep.tiles == 15);
	CHECK(rep.rejected == 12);
	CHECK(rep.loaded == 3);    // 0001_04 (P4 warning), 0001_07 (0x guard), 0001_11 (tRNS)
	CHECK(rep.unguarded == 2);
	CHECK(rep.warnings == 2);
	CHECK(finding_for(rep, "0001_04.png", Rule::p4_cycling)->severity == Severity::warning);
	CHECK(finding_for(rep, "0001_11.png", Rule::f4_trns)->severity == Severity::warning);
	CHECK(finding_for(rep, "0001_00.png", Rule::f2_plte)->detail.find("PLTE[") == 0);
	CHECK(finding_for(rep, "0001_05.png", Rule::g1_guard)->detail.find("stale") == 0);
	CHECK(finding_for(rep, "0001_06.png", Rule::g1_guard)->detail.find("malformed") == 0);
	CHECK(store.flat(1, 4).px != nullptr);
	CHECK(store.flat(1, 7).px != nullptr);
	CHECK(store.flat(1, 3).px == nullptr);
	const Hires::Entry_info* e = store.explain_flat(1, 3);
	REQUIRE(e != nullptr);
	CHECK(e->state == Hires::Entry_state::rejected);
	CHECK(e->rule == Rule::p4_cycling);
}

TEST_CASE("hires store: strict groups, disabled and ignored entries") {
	World          w;
	Temp_dir       tmp;
	const fs::path d(flats_dir(tmp.path()));
	// A good group, a group with one bad file, a disabled group.
	for (int f = 0; f < 3; f++) {
		write_tile(d / "0002" / Hires::tile_name(2, f), w.nn(2, f, 6), 48, w.pal);
		write_tile(d / "water" / Hires::tile_name(3, f), w.nn(3, f, 6), 48, w.pal);
		write_tile(d / "0004.off" / Hires::tile_name(4, f), w.nn(4, f, 6), 48, w.pal);
	}
	write_tile(d / "water" / "0003_03.png", w.nn(3, 3, 6), 48, w.pal, "00000000");    // G1 stale
	// Ignored: '_' and '.' directories, non-PNG files, nested directories.
	write_tile(d / "_work" / "0005_00.png", w.nn(5, 0, 6), 48, w.pal);
	write_tile(d / ".git" / "0005_01.png", w.nn(5, 1, 6), 48, w.pal);
	write_tile(d / "0002" / "nested" / "0005_03.png", w.nn(5, 3, 6), 48, w.pal);
	hires_test::write_text(d / "0005_04.json", "{}");
	hires_test::write_text(d / "README", "notes");
	hires_test::write_text(d / "0002" / "0002_00.txt", "notes");
	// Not ignored: a hidden PNG file is a misnamed tile (N1), as in hirescheck.py.
	write_tile(d / "._0005_02.png", w.nn(5, 2, 6), 48, w.pal);

	Hires::Store        store;
	const Hires::Report rep = w.load(store, {tmp.path()}, 6);
	INFO(dump(rep));
	CHECK(rep.tiles == 8);
	CHECK(rep.loaded == 3);
	CHECK(rep.rejected == 5);    // the whole water group and the hidden file
	CHECK(rep.groups_skipped == 1);
	const Hires::Finding* hidden = finding_for(rep, "._0005_02.png", Rule::n1_name);
	REQUIRE(hidden != nullptr);
	CHECK(hidden->severity == Severity::reject);
	for (int f = 0; f < 3; f++) {
		CHECK(store.flat(2, f).px != nullptr);
		CHECK(store.flat(3, f).px == nullptr);
		CHECK(store.flat(4, f).px == nullptr);
	}
	for (int f = 0; f < 4; f++) {
		CHECK(store.flat(5, f).px == nullptr);
	}
	const Hires::Finding* g2 = finding_for(rep, "water", Rule::g2_group);
	REQUIRE(g2 != nullptr);
	CHECK(g2->severity == Severity::group_skipped);
	CHECK(g2->detail.find("0003_03.png failed G1") != std::string::npos);
	const Hires::Finding* off = finding_for(rep, "0004.off", Rule::g2_group);
	REQUIRE(off != nullptr);
	CHECK(off->severity == Severity::info);
	const Hires::Entry_info* e = store.explain_flat(3, 1);
	REQUIRE(e != nullptr);
	CHECK(e->rule == Rule::g2_group);
}

TEST_CASE("hires store: a hidden PNG in a group skips the group, as in hirescheck.py") {
	World          w;
	Temp_dir       tmp;
	const fs::path g = fs::path(flats_dir(tmp.path())) / "0001";
	write_tile(g / "0001_02.png", w.nn(1, 2, 6), 48, w.pal);
	write_tile(g / "0001_03.png", w.nn(1, 3, 6), 48, w.pal);
	write_tile(g / "._0001_02.png", w.nn(1, 2, 6), 48, w.pal);    // e.g. a macOS resource fork

	Hires::Store        store;
	const Hires::Report rep = w.load(store, {tmp.path()}, 6);
	INFO(dump(rep));
	// hirescheck.py on the same root: tiles 3, loaded 0, groups_skipped 1, N1 reject 1.
	CHECK(rep.tiles == 3);
	CHECK(rep.loaded == 0);
	CHECK(rep.rejected == 3);
	CHECK(rep.groups_skipped == 1);
	const Hires::Finding* n1 = finding_for(rep, "._0001_02.png", Rule::n1_name);
	REQUIRE(n1 != nullptr);
	CHECK(n1->severity == Severity::reject);
	const Hires::Finding* g2 = finding_for(rep, "0001", Rule::g2_group);
	REQUIRE(g2 != nullptr);
	CHECK(g2->severity == Severity::group_skipped);
	CHECK(g2->detail.find("._0001_02.png failed N1") != std::string::npos);
	CHECK(store.flat(1, 2).px == nullptr);
	CHECK(store.flat(1, 3).px == nullptr);
}

TEST_CASE("hires store: root precedence (<PATCH>/hires over <HIRES>), duplicates") {
	World          w;
	Temp_dir       tmp;
	const fs::path patch = tmp / "patch_hires";
	const fs::path base  = tmp / "hires";
	// Same key in both roots: the first root wins.
	auto custom = w.nn(6, 0, 6);
	std::fill(custom.begin(), custom.begin() + 48, uint8_t{0x77});
	write_tile(fs::path(flats_dir(patch)) / "0006_00.png", custom, 48, w.pal);
	write_tile(fs::path(flats_dir(base)) / "0006_00.png", w.nn(6, 0, 6), 48, w.pal);
	// Only in the second root.
	write_tile(fs::path(flats_dir(base)) / "0006_01.png", w.nn(6, 1, 6), 48, w.pal);
	// Rejected in the first root: the second root's tile is served.
	write_tile(fs::path(flats_dir(patch)) / "0006_02.png", w.nn(6, 2, 6), 48, w.pal, "00000000");
	write_tile(fs::path(flats_dir(base)) / "0006_02.png", w.nn(6, 2, 6), 48, w.pal);
	// A duplicate inside a root: the later file (the group) wins, with a warning.
	auto dup = w.nn(6, 3, 6);
	dup[0]   = 0x70;
	write_tile(fs::path(flats_dir(base)) / "0006_03.png", w.nn(6, 3, 6), 48, w.pal);
	write_tile(fs::path(flats_dir(base)) / "grp" / "0006_03.png", dup, 48, w.pal);

	Hires::Store        store;
	const Hires::Report rep = w.load(store, {patch, base}, 6);
	INFO(dump(rep));
	CHECK(rep.roots == 2);
	CHECK(rep.loaded == 4);
	CHECK(same_pixels(store.flat(6, 0), custom));
	CHECK(store.explain_flat(6, 0)->root == 0);
	CHECK(same_pixels(store.flat(6, 1), w.nn(6, 1, 6)));
	CHECK(store.explain_flat(6, 1)->root == 1);
	CHECK(same_pixels(store.flat(6, 2), w.nn(6, 2, 6)));
	CHECK(store.explain_flat(6, 2)->root == 1);
	CHECK(same_pixels(store.flat(6, 3), dup));
	const Hires::Finding* f = finding_for(rep, "grp/0006_03.png", Rule::n1_name);
	REQUIRE(f != nullptr);
	CHECK(f->severity == Severity::warning);
	CHECK(f->detail.find("duplicate key, also ") == 0);
	// Paths use '/' on every platform (Windows too), in findings and for the inspector.
	for (const auto& finding : rep.findings) {
		INFO(finding.format());
		CHECK(finding.path.find('\\') == std::string::npos);
	}
	CHECK(store.explain_flat(6, 3)->path.find('\\') == std::string::npos);
	CHECK(store.explain_flat(6, 3)->path.find("/grp/0006_03.png") != std::string::npos);
	// Swapping the roots swaps the winner.
	Hires::Store        store2;
	const Hires::Report rep2 = w.load(store2, {base, patch}, 6);
	CHECK(same_pixels(store2.flat(6, 0), w.nn(6, 0, 6)));
	CHECK(rep2.loaded == 4);
}

TEST_CASE("hires store: x<S> is preferred over reduction; reduction from x6 otherwise") {
	World          w;
	Temp_dir       tmp;
	const fs::path root = tmp / "root";
	// x6: NN6 of (8, 0) and a free-form tile (8, 1) with E0-E7 pixels over the glints of shape 7.
	write_tile(fs::path(flats_dir(root)) / "0008_00.png", w.nn(8, 0, 6), 48, w.pal);
	auto art = w.nn(7, 1, 6);
	for (size_t i = 0; i < art.size(); i += 7) {
		art[i] = static_cast<uint8_t>(0x10 + (i % 0x70));
	}
	REQUIRE(Hires::p4_violations(art.data(), 6, w.flat(7, 1), 8, 8) == 0);
	write_tile(fs::path(flats_dir(root)) / "0007_01.png", art, 48, w.pal);
	// x3: hand-made art for (8, 0) only.
	std::vector<uint8_t> x3(24 * 24, 0x42);
	write_tile(fs::path(flats_dir(root, 3)) / "0008_00.png", x3, 24, w.pal);

	Hires::Store        s3;
	const Hires::Report r3 = w.load(s3, {root}, 3);
	INFO(dump(r3));
	CHECK(r3.loaded == 1);    // x3 exists: x6 is not used
	CHECK(same_pixels(s3.flat(8, 0), x3));
	CHECK(s3.flat(7, 1).px == nullptr);
	CHECK_FALSE(s3.explain_flat(8, 0)->reduced);

	for (const int k : {2, 1}) {
		Hires::Store        sk;
		const Hires::Report rk = w.load(sk, {root}, k);
		INFO("scale " << k << " " << dump(rk));
		CHECK(rk.loaded == 2);
		CHECK(same_pixels(sk.flat(8, 0), w.nn(8, 0, k)));    // reduce(NN6(x)) == NN_k(x)
		std::vector<uint8_t> expect(static_cast<size_t>(64) * k * k);
		REQUIRE(Hires::reduce_mode(art.data(), 6, k, w.flat(7, 1), 8, 8, expect.data()));
		CHECK(same_pixels(sk.flat(7, 1), expect));
		CHECK(sk.explain_flat(7, 1)->reduced);
		CHECK(sk.flat(7, 1).side == 8 * k);
	}
	// 4 does not divide 6: no art without an x4 folder.
	Hires::Store        s4;
	const Hires::Report r4 = w.load(s4, {root}, 4);
	CHECK(r4.loaded == 0);
	CHECK(s4.flat(8, 0).px == nullptr);
}

TEST_CASE("hires store: Tile_view.side is 8 S for every scale") {
	World          w;
	Temp_dir       tmp;
	const fs::path root = tmp / "root";
	write_tile(fs::path(flats_dir(root)) / "0009_09.png", w.nn(9, 9, 6), 48, w.pal);
	std::vector<uint8_t> x4(32 * 32, 0x31);
	write_tile(fs::path(flats_dir(root, 4)) / "0009_09.png", x4, 32, w.pal);
	for (const int s : {1, 2, 3, 4, 6}) {
		Hires::Store store;
		w.load(store, {root}, s);
		const Hires::Tile_view v = store.flat(9, 9);
		INFO("scale " << s);
		REQUIRE(v.px != nullptr);
		CHECK(v.side == 8 * s);
		CHECK(store.get_scale() == s);
	}
	Hires::Store        bad;
	const Hires::Report rep = w.load(bad, {root}, 0);
	CHECK(rep.loaded == 0);
	CHECK(rep.findings.at(0).severity == Severity::error);
}

TEST_CASE("hires store: an exception while checking rejects that file or entry alone") {
	World w;
	// The provider fails for shape 15, and for shape 17 on its second call for a
	// key: the reduction to S < 6 after a successful check.
	std::map<std::pair<int, int>, int> calls;
	const Hires::Src_provider          base = w.flats.provider();
	const Hires::Src_provider          src  = [&](int s, int f) -> const uint8_t* {
        if (s == 15 || (s == 17 && ++calls[{s, f}] > 1)) {
            throw std::runtime_error("provider failed");
        }
        return base(s, f);
	};
	Temp_dir       tmp;
	const fs::path root = tmp / "a";
	const fs::path d(flats_dir(root));
	write_tile(d / "0014_00.png", w.nn(14, 0, 6), 48, w.pal);
	write_tile(d / "0015_00.png", w.nn(15, 0, 6), 48, w.pal);
	write_tile(d / "0017_00.png", w.nn(17, 0, 6), 48, w.pal);
	write_tile(d / "grp" / "0015_01.png", w.nn(15, 1, 6), 48, w.pal);
	write_tile(d / "grp" / "0016_00.png", w.nn(16, 0, 6), 48, w.pal);
	std::vector<Hires::Bundle_source> entries(2);
	entries[0].shape  = 14;
	entries[0].frame  = 1;
	entries[0].pixels = w.nn(14, 1, 6);
	entries[1].shape  = 15;
	entries[1].frame  = 2;
	entries[1].pixels = w.nn(15, 2, 6);
	hires_test::write_file(root / "x6" / "flats.bundle", Hires::build_bundle(6, Hires::palette_crc32(w.pal), entries));
	const fs::path other = tmp / "b";
	write_tile(fs::path(flats_dir(other)) / "0018_00.png", w.nn(18, 0, 6), 48, w.pal);
	const auto load = [&](Hires::Store& store, int scale) {
		calls.clear();
		return store.load(
				{
						{ root.string(), "a"},
                        {other.string(), "b"}
        },
				scale, 6, w.pal, src);
	};

	Hires::Store        store;
	const Hires::Report rep = load(store, 6);
	INFO(dump(rep));
	CHECK(rep.roots_disabled == 0);
	CHECK(rep.tiles == 8);
	CHECK(rep.loaded == 4);    // 0014_00, 0017_00, the bundle's (14, 1), root b's 0018_00
	CHECK(rep.rejected == 4);
	CHECK(rep.groups_skipped == 1);
	CHECK(store.flat(14, 0).px != nullptr);
	CHECK(store.flat(17, 0).px != nullptr);
	CHECK(store.flat(14, 1).px != nullptr);
	CHECK(store.flat(18, 0).px != nullptr);
	CHECK(store.flat(16, 0).px == nullptr);
	const Hires::Finding* f = finding_for(rep, "0015_00.png", Rule::none);
	REQUIRE(f != nullptr);
	CHECK(f->severity == Severity::error);
	CHECK(f->detail == "cannot check: provider failed");
	const Hires::Finding* g2 = finding_for(rep, "grp", Rule::g2_group);
	REQUIRE(g2 != nullptr);
	CHECK(g2->detail.find("0015_01.png could not be checked") != std::string::npos);
	const Hires::Finding* entry = finding_for(rep, "flats.bundle#0015_02", Rule::none);
	REQUIRE(entry != nullptr);
	CHECK(entry->severity == Severity::error);
	const Hires::Entry_info* e = store.explain_flat(15, 0);
	REQUIRE(e != nullptr);
	CHECK(e->state == Hires::Entry_state::rejected);
	CHECK(e->detail == "cannot check: provider failed");
	CHECK(store.explain_flat(16, 0)->rule == Rule::g2_group);

	// At S = 3 the reduction of 0017_00 fails: that file alone is not served.
	Hires::Store        s3;
	const Hires::Report rep3 = load(s3, 3);
	INFO(dump(rep3));
	CHECK(rep3.roots_disabled == 0);
	CHECK(rep3.loaded == 3);
	CHECK(s3.flat(17, 0).px == nullptr);
	CHECK(s3.flat(14, 0).side == 24);
	CHECK(s3.flat(18, 0).side == 24);
	const Hires::Finding* r17 = finding_for(rep3, "0017_00.png", Rule::none);
	REQUIRE(r17 != nullptr);
	CHECK(r17->severity == Severity::error);
}

TEST_CASE("hires store: pack.txt palette_crc32 (R1)") {
	World          w;
	Temp_dir       tmp;
	const uint32_t crc      = Hires::palette_crc32(w.pal);
	const fs::path ok_lower = tmp / "a", ok_upper = tmp / "b", wrong = tmp / "c", malformed = tmp / "d", none = tmp / "e";
	const std::vector<std::pair<fs::path, std::string>> roots = {
			{ ok_lower,						   "\xEF\xBB\xBFpalette_crc32=" + hires_test::hex8(crc) + "\n"},
			{ ok_upper, "# comment\ngame=TEST\n palette_crc32 = 0X" + hires_test::hex8(crc) + " \nnoequals\n"},
			{    wrong,															"palette_crc32=c9c2c0e7\n"},
			{malformed,																  "palette_crc32=zz\n"},
			{     none,																"game=TEST\nscale=6\n"}
    };
	int frame = 0;
	for (const auto& r : roots) {
		hires_test::write_text(r.first / "pack.txt", r.second);
		write_tile(fs::path(flats_dir(r.first)) / Hires::tile_name(10, frame), w.nn(10, frame, 6), 48, w.pal);
		frame++;
	}
	Hires::Store        store;
	const Hires::Report rep = w.load(store, {ok_lower, ok_upper, wrong, malformed, none}, 6);
	INFO(dump(rep));
	CHECK(rep.roots == 5);
	CHECK(rep.roots_disabled == 2);
	CHECK(store.flat(10, 0).px != nullptr);
	CHECK(store.flat(10, 1).px != nullptr);
	CHECK(store.flat(10, 2).px == nullptr);
	CHECK(store.flat(10, 3).px == nullptr);
	CHECK(store.flat(10, 4).px != nullptr);
	const Hires::Finding* r1 = finding_for(rep, "c/pack.txt", Rule::r1_palette);
	REQUIRE(r1 != nullptr);
	CHECK(r1->severity == Severity::root_disabled);
	CHECK(r1->detail.find("wrong game?") != std::string::npos);
	CHECK(finding_for(rep, "d/pack.txt", Rule::r1_palette) != nullptr);
}

TEST_CASE("hires store: terrain overrides are indexed, decoded on demand and checked") {
	World    w;
	Temp_dir tmp;
	Rng      rng(0x7e44);
	// A 128x128 1x layer with some E8-EF pixels.
	std::vector<uint8_t> layer(128 * 128);
	for (auto& v : layer) {
		v = static_cast<uint8_t>(rng.chance(3) ? rng.range(0xe8, 0xef) : rng.range(0x10, 0xd0));
	}
	std::vector<uint8_t> art6(768 * 768);
	Hires::nn_upscale(layer.data(), 128, 128, 6, art6.data());
	const uint64_t key_ok = 0x0123456789abcdefULL, key_p0 = 0x1111111111111111ULL, key_p4 = 0x2222222222222222ULL,
				   key_text = 0x3333333333333333ULL, key_textok = 0x4444444444444444ULL;
	const fs::path tdir = tmp / "root" / "x6" / "terrain";
	write_tile(tdir / Hires::terrain_name(key_ok), art6, 768, w.pal);
	auto p0  = art6;
	p0[1000] = 0xff;
	write_tile(tdir / Hires::terrain_name(key_p0), p0, 768, w.pal);
	// P4: F0-F3 pixels over a static layer (the file is checked against the
	// layer it is decoded for).
	std::vector<uint8_t> static_layer(128 * 128, 0x20);
	std::vector<uint8_t> p4(768 * 768, 0x20);
	std::fill(p4.begin(), p4.begin() + 4000, uint8_t{0xf1});
	write_tile(tdir / Hires::terrain_name(key_p4), p4, 768, w.pal);
	Hires::Png_write_options opt;
	opt.text_after = {
			{"Exult-Terrain-Key", "0000000000000000"}
    };
	write_tile(tdir / Hires::terrain_name(key_text), art6, 768, w.pal, "", opt);
	opt.text_after = {
			{"Exult-Terrain-Key", "0x" + Hires::terrain_name(key_textok).substr(0, 16)}
    };
	write_tile(tdir / Hires::terrain_name(key_textok), art6, 768, w.pal, "", opt);
	write_tile(tdir / "ABCDEF0123456789.png", art6, 768, w.pal);    // N1: upper case

	Hires::Store        store;
	const Hires::Report rep = w.load(store, {tmp / "root"}, 6);
	INFO(dump(rep));
	CHECK(rep.terrains == 5);
	CHECK(rep.rejected == 1);
	CHECK(finding_for(rep, "ABCDEF0123456789.png", Rule::n1_name) != nullptr);
	CHECK(store.explain_terrain(key_ok)->state == Hires::Entry_state::indexed);

	std::vector<uint8_t> dst(800 * 768, 0x5a);
	// A good file decodes into the destination pitch.
	CHECK(store.terrain(key_ok, layer.data(), dst.data(), 768, 768, 800));
	bool rows_ok = true;
	for (int y = 0; y < 768; y++) {
		rows_ok = rows_ok && std::equal(art6.begin() + y * 768, art6.begin() + (y + 1) * 768, dst.begin() + y * 800);
		rows_ok = rows_ok && dst[static_cast<size_t>(y) * 800 + 790] == 0x5a;    // the padding is untouched
	}
	CHECK(rows_ok);
	CHECK(store.take_findings().empty());
	// The wrong destination size: false, nothing written.
	std::vector<uint8_t> small(384 * 384, 0x5a);
	CHECK_FALSE(store.terrain(key_ok, layer.data(), small.data(), 384, 384, 384));
	CHECK(std::all_of(small.begin(), small.end(), [](uint8_t v) {
		return v == 0x5a;
	}));
	CHECK_FALSE(store.terrain(key_ok, nullptr, dst.data(), 768, 768, 768));
	CHECK_FALSE(store.terrain(0x9999ULL, layer.data(), dst.data(), 768, 768, 768));
	// P0: rejected, reported once, latched.
	CHECK_FALSE(store.terrain(key_p0, layer.data(), dst.data(), 768, 768, 800));
	auto found = store.take_findings();
	REQUIRE(found.size() == 1);
	CHECK(found[0].rule == Rule::p0_border);
	CHECK_FALSE(store.terrain(key_p0, layer.data(), dst.data(), 768, 768, 800));
	CHECK(store.take_findings().empty());
	CHECK(store.explain_terrain(key_p0)->state == Hires::Entry_state::rejected);
	// P4 depends on the layer: rejected and reported once, but not latched.
	CHECK_FALSE(store.terrain(key_p4, static_layer.data(), dst.data(), 768, 768, 800));
	found = store.take_findings();
	REQUIRE(found.size() == 1);
	CHECK(found[0].rule == Rule::p4_cycling);
	CHECK(store.explain_terrain(key_p4)->state == Hires::Entry_state::indexed);
	CHECK(store.explain_terrain(key_p4)->rule == Rule::p4_cycling);
	CHECK_FALSE(store.terrain(key_p4, static_layer.data(), dst.data(), 768, 768, 800));
	CHECK(store.take_findings().empty());              // reported once
	std::vector<uint8_t> f0_layer(128 * 128, 0xf0);    // every parent pixel in F0-F3
	CHECK(store.terrain(key_p4, f0_layer.data(), dst.data(), 768, 768, 800));
	CHECK(dst[0] == 0xf1);
	CHECK(dst[static_cast<size_t>(767) * 800 + 767] == 0x20);
	CHECK(store.explain_terrain(key_p4)->rule == Rule::none);
	// The Exult-Terrain-Key text must match the name.
	CHECK_FALSE(store.terrain(key_text, layer.data(), dst.data(), 768, 768, 800));
	found = store.take_findings();
	REQUIRE(found.size() == 1);
	CHECK(found[0].rule == Rule::g1_guard);
	CHECK(store.terrain(key_textok, layer.data(), dst.data(), 768, 768, 800));

	// Reduction to S = 3 with the layer as the parent.
	Hires::Store s3;
	w.load(s3, {tmp / "root"}, 3);
	std::vector<uint8_t> dst3(384 * 384);
	CHECK(s3.terrain(key_ok, layer.data(), dst3.data(), 384, 384, 384));
	std::vector<uint8_t> nn3(384 * 384);
	Hires::nn_upscale(layer.data(), 128, 128, 3, nn3.data());
	CHECK(dst3 == nn3);
	CHECK(s3.explain_terrain(key_ok)->reduced);
	CHECK_FALSE(s3.terrain(key_ok, layer.data(), dst.data(), 768, 768, 800));    // 768 is not 128 * 3
}

TEST_CASE("hires store: bundles (B0, independent entries, loose override)") {
	World          w;
	Temp_dir       tmp;
	const uint32_t crc = Hires::palette_crc32(w.pal);

	auto entry = [&](int s, int f, bool guarded) {
		Hires::Bundle_source e;
		e.shape   = s;
		e.frame   = f;
		e.guarded = guarded;
		e.guard   = guarded ? Hires::crc32(w.flat(s, f), 64) : 0;
		e.pixels  = w.nn(s, f, 6);
		return e;
	};
	std::vector<Hires::Bundle_source> entries = {entry(11, 0, true), entry(11, 1, false), entry(12, 5, true)};
	// One bad entry: P0.
	Hires::Bundle_source bad = entry(12, 6, true);
	bad.pixels[3]            = 0xff;
	entries.push_back(bad);
	// One stale guard and one frame >= 32.
	Hires::Bundle_source stale = entry(12, 7, true);
	stale.guard ^= 1;
	entries.push_back(stale);
	Hires::Bundle_source frame40 = entry(12, 8, false);
	frame40.frame                = 40;
	entries.push_back(frame40);
	const auto bytes = Hires::build_bundle(6, crc, entries);
	REQUIRE(bytes.size() == Hires::bundle_header_size + 6 * Hires::bundle_entry_size(6));

	SUBCASE("valid; one bad entry is rejected alone; a loose file overrides an entry") {
		const fs::path root = tmp / "valid";
		hires_test::write_file(root / "x6" / "flats.bundle", bytes);
		auto loose = w.nn(11, 1, 6);
		loose[0]   = 0x7f;
		write_tile(fs::path(flats_dir(root)) / "0011_01.png", loose, 48, w.pal);
		Hires::Store        store;
		const Hires::Report rep = w.load(store, {root}, 6);
		INFO(dump(rep));
		CHECK(rep.tiles == 7);
		CHECK(rep.loaded == 3);
		CHECK(rep.bundled == 2);
		CHECK(rep.rejected == 3);
		CHECK(rep.groups_skipped == 0);
		CHECK(same_pixels(store.flat(11, 0), w.nn(11, 0, 6)));
		CHECK(same_pixels(store.flat(11, 1), loose));
		CHECK_FALSE(store.explain_flat(11, 1)->from_bundle);
		CHECK(store.explain_flat(12, 5)->from_bundle);
		CHECK(store.flat(12, 6).px == nullptr);
		CHECK(finding_for(rep, "flats.bundle#0012_06", Rule::p0_border) != nullptr);
		CHECK(finding_for(rep, "flats.bundle#0012_07", Rule::g1_guard) != nullptr);
		CHECK(finding_for(rep, "flats.bundle#0012_40", Rule::n1_name) != nullptr);
		CHECK(store.explain_flat(12, 6)->rule == Rule::p0_border);
		// At S = 3 the x6 bundle is reduced.
		Hires::Store s3;
		w.load(s3, {root}, 3);
		CHECK(same_pixels(s3.flat(11, 0), w.nn(11, 0, 3)));
		CHECK(s3.explain_flat(11, 0)->reduced);
	}
	SUBCASE("B0: truncated, palette CRC, scale, version, magic; loose files still load") {
		struct Broken {
			const char*          name;
			std::vector<uint8_t> data;
			const char*          detail;
		};

		std::vector<Broken> broken;
		broken.push_back({"truncated", std::vector<uint8_t>(bytes.begin(), bytes.end() - 1), "size "});
		auto trailing = bytes;
		trailing.push_back(0);
		broken.push_back({"trailing", trailing, "size "});
		broken.push_back({"short header", std::vector<uint8_t>(bytes.begin(), bytes.begin() + 10), "truncated header"});
		broken.push_back({"palette", Hires::build_bundle(6, crc ^ 1U, entries), "palette_crc32 "});
		broken.push_back({"scale", Hires::build_bundle(3, crc, {}), "scale 3 != 6"});
		broken.push_back({"version", patched(bytes, 4, 2), "version 2"});
		broken.push_back({"magic", patched(bytes, 0, 'X'), "bad magic"});
		for (const auto& b : broken) {
			INFO(b.name);
			const fs::path root = tmp / (std::string("b_") + b.name);
			hires_test::write_file(root / "x6" / "flats.bundle", b.data);
			write_tile(fs::path(flats_dir(root)) / "0013_00.png", w.nn(13, 0, 6), 48, w.pal);
			Hires::Store        store;
			const Hires::Report rep = w.load(store, {root}, 6);
			INFO(dump(rep));
			const Hires::Finding* f = finding_for(rep, "flats.bundle", Rule::b0_bundle);
			REQUIRE(f != nullptr);
			CHECK(f->severity == Severity::bundle_disabled);
			CHECK(f->detail.find(b.detail) == 0);
			CHECK(rep.loaded == 1);
			CHECK(store.flat(13, 0).px != nullptr);
			CHECK(store.flat(11, 0).px == nullptr);
		}
	}
	SUBCASE("entries in any order; of two with one key the later accepted one wins (as pack.py reads them)") {
		// The first two entries swapped: not sorted, still no B0.
		auto         unsorted = bytes;
		const size_t es       = Hires::bundle_entry_size(6);
		std::swap_ranges(
				unsorted.begin() + Hires::bundle_header_size, unsorted.begin() + Hires::bundle_header_size + es,
				unsorted.begin() + Hires::bundle_header_size + es);
		REQUIRE(unsorted != bytes);
		const fs::path a = tmp / "unsorted";
		hires_test::write_file(a / "x6" / "flats.bundle", unsorted);
		Hires::Store        store;
		const Hires::Report rep = w.load(store, {a}, 6);
		INFO(dump(rep));
		CHECK(finding_for(rep, "flats.bundle", Rule::b0_bundle) == nullptr);
		CHECK(rep.tiles == 6);
		CHECK(rep.loaded == 3);
		CHECK(rep.rejected == 3);
		CHECK(same_pixels(store.flat(11, 0), w.nn(11, 0, 6)));
		CHECK(same_pixels(store.flat(11, 1), w.nn(11, 1, 6)));
		CHECK(same_pixels(store.flat(12, 5), w.nn(12, 5, 6)));

		// Duplicate keys (build_bundle keeps their order): (11, 0) twice, the later
		// entry served; (11, 1) good, then rejected (P0): the good one stays.
		Hires::Bundle_source later  = entry(11, 0, true);
		later.pixels[0]             = 0x7e;
		Hires::Bundle_source bad_p0 = entry(11, 1, true);
		bad_p0.pixels[5]            = 0xff;
		const fs::path b            = tmp / "duplicate";
		hires_test::write_file(
				b / "x6" / "flats.bundle", Hires::build_bundle(6, crc, {entry(11, 0, true), later, entry(11, 1, false), bad_p0}));
		Hires::Store        store2;
		const Hires::Report rep2 = w.load(store2, {b}, 6);
		INFO(dump(rep2));
		CHECK(finding_for(rep2, "flats.bundle", Rule::b0_bundle) == nullptr);
		CHECK(rep2.tiles == 4);
		CHECK(rep2.loaded == 2);
		CHECK(rep2.rejected == 1);
		CHECK(same_pixels(store2.flat(11, 0), later.pixels));
		CHECK(same_pixels(store2.flat(11, 1), w.nn(11, 1, 6)));
		CHECK(store2.explain_flat(11, 1)->state == Hires::Entry_state::loaded);
		CHECK(finding_for(rep2, "flats.bundle#0011_01", Rule::p0_border) != nullptr);
	}
	SUBCASE("the reader on bytes") {
		Hires::Bundle b;
		std::string   error;
		REQUIRE(b.parse(bytes, 6, crc, error));
		CHECK(b.size() == 6);
		CHECK(b.header().count == 6);
		CHECK(b.header().scale == 6);
		const Hires::Bundle_entry e = b.entry(0);
		CHECK(e.shape == 11);
		CHECK(e.frame == 0);
		CHECK(e.guarded);
		CHECK(e.guard == Hires::crc32(w.flat(11, 0), 64));
		CHECK(std::equal(e.pixels, e.pixels + 48 * 48, w.nn(11, 0, 6).begin()));
		CHECK(b.entry(6).pixels == nullptr);
		CHECK_FALSE(b.parse(bytes, 3, crc, error));
		CHECK(b.size() == 0);
		CHECK(Hires::build_bundle(6, crc, {entry(1, 0, false), Hires::Bundle_source{}}).empty());
	}
}

TEST_CASE("hires store: reads the bundle written by mkpack (tests/data/hires/bundle)") {
	const auto&         game = hires_test::synth_game();
	Hires::Store        store;
	const Hires::Report rep = store.load(
			{
					{hires_test::data_path("hires/bundle"), "mkpack"}
    },
			6, 6, game.pal, game.provider());
	INFO(dump(rep));
	CHECK(rep.roots_disabled == 0);    // its pack.txt has the palette's CRC
	CHECK(rep.loaded == 3);
	CHECK(rep.bundled == 3);
	CHECK(rep.unguarded == 1);
	CHECK(rep.rejected == 0);
	for (const auto& key : std::vector<std::pair<int, int>>{
				 {1, 2},
                 {1, 3},
                 {2, 0}
    }) {
		const uint8_t* src = game.provider()(key.first, key.second);
		REQUIRE(src != nullptr);
		CHECK(same_pixels(store.flat(key.first, key.second), hires_test::nn_tile(src, 6)));
	}
	CHECK_FALSE(store.explain_flat(1, 3)->guarded);
	CHECK(store.explain_flat(2, 0)->guarded);
}

TEST_CASE("hires store: the shared rule fixtures (tests/data/hires/rules)") {
	const auto& game  = hires_test::synth_game();
	const auto  cases = read_expected("rules");
	REQUIRE(cases.size() >= 18);
	for (const auto& c : cases) {
		INFO(c.file << " expected " << c.rule << " " << c.severity);
		const Hires::File_check r
				= Hires::check_tile_file(hires_test::data_path("hires/rules/" + c.file), 6, game.pal, game.provider());
		INFO("got " << Hires::rule_name(r.rule) << " " << r.detail);
		if (c.severity == "reject") {
			CHECK(Hires::rule_name(r.rule) == c.rule);
			if (c.file == "f2_short/0001_06.png") {
				// The largest used index equals the PLTE size: the bound, not a colour.
				CHECK(r.detail.find("index ") == 0);
				CHECK(r.detail.find(" >= PLTE size ") != std::string::npos);
			}
		} else {
			// OK, warnings, and the offline rule P2 (not an engine rule): accepted.
			CHECK(r.rule == Rule::none);
			CHECK(r.pixels.size() == 48 * 48);
			std::string warnings;
			for (const auto& f : r.warnings) {
				warnings += Hires::rule_name(f.rule);
			}
			CHECK(warnings == (c.severity == "warning" ? c.rule : std::string()));
		}
	}
}

#endif

TEST_CASE("hires store: the engine's provider on the synthetic shapes.vga") {
	auto&                     game = hires_test::synth_game();
	const Hires::Src_provider src  = game.provider();
	REQUIRE(game.vga->get_num_shapes() == 16);
	// Shape 1: 32 flat frames; the flx data is 64 bytes per frame.
	std::ifstream        in(hires_test::data_path("hires/rules/game/shapes.vga"), std::ios::binary);
	std::vector<uint8_t> flx((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	REQUIRE(flx.size() > 0x80 + 16);
	const auto off = static_cast<size_t>(flx[0x80 + 8]) | (static_cast<size_t>(flx[0x80 + 9]) << 8)
					 | (static_cast<size_t>(flx[0x80 + 10]) << 16) | (static_cast<size_t>(flx[0x80 + 11]) << 24);
	const uint8_t* f12 = src(1, 2);
	REQUIRE(f12 != nullptr);
	CHECK(std::equal(f12, f12 + 64, flx.begin() + static_cast<long>(off + 2 * 64)));
	CHECK(src(1, 31) != nullptr);
	CHECK(src(1, 32) == nullptr);    // frames are 0-31
	CHECK(src(7, 0) == nullptr);     // RLE
	CHECK(src(12, 3) != nullptr);    // 4 frames
	CHECK(src(12, 4) == nullptr);    // beyond the frame count
	CHECK(src(6, 0) == nullptr);     // empty
	CHECK(src(16, 0) == nullptr);    // out of range
	CHECK(src(9999, 0) == nullptr);
	CHECK(src(-1, 0) == nullptr);
	CHECK(src(1, -1) == nullptr);
	// A Vga_file without shapes.
	Vga_file empty;
	CHECK(Hires::flat_from_vga(empty, 0, 0) == nullptr);
}

TEST_CASE("hires store: Store_set loads lazily per scale; generation, toggle, fail latch") {
	World                    w;
	Temp_dir                 tmp;
	int                      calls   = 0;
	bool                     fail    = false;
	int                      reports = 0;
	std::vector<std::string> texts;
	Hires::Store_set         set(
            [&]() {
                calls++;
                if (fail) {
                    throw std::runtime_error("palette missing");
                }
                Hires::Store_set::Inputs in;
                in.roots = {
                        {tmp.path().string(), "root"}
                };
                in.pal   = w.pal;
                in.src   = w.flats.provider();
                in.s_art = 6;
                return in;
            },
            [&](int, const Hires::Report&) {
                reports++;
            },
            [&](const std::string& text) {
                texts.push_back(text);
            });
	auto write_bundle = [&](const std::vector<int>& frames) {
		std::vector<Hires::Bundle_source> entries;
		for (const int f : frames) {
			Hires::Bundle_source e;
			e.shape  = 14;
			e.frame  = f;
			e.pixels = w.nn(14, f, 6);
			entries.push_back(e);
		}
		hires_test::write_file(tmp / "x6" / "flats.bundle", Hires::build_bundle(6, Hires::palette_crc32(w.pal), entries));
	};
	write_bundle({0});
	const uint32_t g0 = set.generation();
	CHECK(set.enabled());
	CHECK(set.flat(14, 0, 1).px == nullptr);    // scale 1: never loads
	CHECK(calls == 0);
	CHECK(set.flat(14, 0, 6).side == 48);
	CHECK(set.flat(14, 32, 6).side == 48);
	CHECK(calls == 1);
	CHECK(reports == 1);
	CHECK(set.flat(14, 0, 3).side == 24);    // its own store, reduced
	CHECK(calls == 2);
	REQUIRE(set.report(6) != nullptr);
	CHECK(set.report(6)->loaded == 1);
	CHECK(set.report(2) == nullptr);
	CHECK(set.generation() == g0);    // loading does not change answers already given

	set.set_enabled(false);
	CHECK(set.generation() == g0 + 1);
	CHECK(set.flat(14, 0, 6).px == nullptr);
	set.set_enabled(false);
	CHECK(set.generation() == g0 + 1);
	set.set_enabled(true);
	CHECK(set.generation() == g0 + 2);
	CHECK(set.flat(14, 0, 6).px != nullptr);
	CHECK(calls == 2);

	// A reload (invalidate) rescans: a new entry shows up, generation changes.
	write_bundle({0, 1});
	CHECK(set.flat(14, 1, 6).px == nullptr);
	set.invalidate();
	CHECK(set.generation() == g0 + 3);
	CHECK(set.flat(14, 1, 6).px != nullptr);
	CHECK(calls == 3);

	// A failing load is latched until the next invalidate().
	fail = true;
	set.invalidate();
	const int before = calls;
	CHECK(set.flat(14, 0, 6).px == nullptr);
	CHECK(set.flat(14, 0, 6).px == nullptr);
	CHECK(set.store(6) == nullptr);
	CHECK(calls == before + 1);
	REQUIRE_FALSE(texts.empty());
	CHECK(texts.back().find("x6: overrides disabled: palette missing") == 0);
	fail = false;
	set.invalidate();
	CHECK(set.flat(14, 0, 6).px != nullptr);
	std::vector<uint8_t> dst(768 * 768);
	std::vector<uint8_t> layer(128 * 128);
	CHECK_FALSE(set.terrain(1, 6, layer.data(), dst.data(), 768, 768, 768));    // no terrain overrides
	CHECK_FALSE(set.terrain(1, 1, layer.data(), dst.data(), 128, 128, 128));
}

TEST_CASE("hires store: Store_set survives report and text functions that throw") {
	World            w;
	Temp_dir         tmp;
	bool             fail_inputs = false;
	int              reports     = 0;
	int              texts       = 0;
	Hires::Store_set set(
			[&]() {
				if (fail_inputs) {
					throw std::runtime_error("palette missing");
				}
				Hires::Store_set::Inputs in;
				in.roots = {
						{tmp.path().string(), "root"}
                };
				in.pal   = w.pal;
				in.src   = w.flats.provider();
				in.s_art = 6;
				return in;
			},
			[&](int, const Hires::Report&) {
				reports++;
				throw std::runtime_error("report sink failed");
			},
			[&](const std::string&) {
				texts++;
				throw std::runtime_error("log sink failed");
			});
	Hires::Bundle_source e;
	e.shape  = 14;
	e.frame  = 0;
	e.pixels = w.nn(14, 0, 6);
	hires_test::write_file(tmp / "x6" / "flats.bundle", Hires::build_bundle(6, Hires::palette_crc32(w.pal), {e}));
	hires_test::write_file(tmp / "x6" / "terrain" / "0123456789abcdef.png", {1, 2, 3});    // F1 when decoded
	// The report is lost, the store stands.
	CHECK(set.flat(14, 0, 6).side == 48);
	CHECK(reports == 1);
	CHECK(set.store(6) != nullptr);
	// A terrain reject goes to the text function: lost, nothing escapes.
	std::vector<uint8_t> dst(768 * 768);
	std::vector<uint8_t> layer(128 * 128);
	CHECK_FALSE(set.terrain(0x0123456789abcdefULL, 6, layer.data(), dst.data(), 768, 768, 768));
	CHECK(texts == 1);
	// A failing load is latched; its message is lost.
	fail_inputs = true;
	set.invalidate();
	CHECK(set.flat(14, 0, 6).px == nullptr);
	CHECK(set.store(6) == nullptr);
	CHECK(texts == 2);
	CHECK(set.report(6) != nullptr);
}
