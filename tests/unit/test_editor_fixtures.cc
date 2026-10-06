/*
 *  test_editor_fixtures.cc - PNGs laid out like real Aseprite and GIMP
 *  exports (synthetic palette and content, tests/data/hires/editor) through
 *  the engine's tile rules (DESIGN.md section 6.2, test_editor_fixtures).
 *  WP-15 adds the user's own exports under editor/real/ with their own
 *  expected.txt; this test runs that directory too when it exists.
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
#include "hires_png.h"
#include "hires_store.h"
#include "hires_test_util.h"
#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef HAVE_PNG_H

namespace {
	struct Outcome {
		Hires::File_check check;
		std::string       rule;
		std::string       severity;
	};

	// Runs every file of <dir>/expected.txt through the tile rules at S = 6.
	std::map<std::string, Outcome> run_dir(const std::string& dir) {
		const auto&                    game = hires_test::synth_game();
		std::map<std::string, Outcome> out;
		std::ifstream                  in(hires_test::data_path("hires/" + dir + "/expected.txt"));
		std::string                    line;
		while (std::getline(in, line)) {
			if (line.empty() || line[0] == '#') {
				continue;
			}
			std::istringstream fields(line);
			std::string        file;
			Outcome            o;
			if (!(fields >> file >> o.rule >> o.severity)) {
				continue;    // A blank line (or a lone '\r' in a CRLF checkout).
			}
			o.check   = Hires::check_tile_file(hires_test::data_path("hires/" + dir + "/" + file), 6, game.pal, game.provider());
			out[file] = std::move(o);
		}
		return out;
	}

	void check_outcomes(const std::map<std::string, Outcome>& outcomes) {
		for (const auto& entry : outcomes) {
			const Outcome& o = entry.second;
			INFO(entry.first << ": expected " << o.rule << " " << o.severity << ", got " << Hires::rule_name(o.check.rule) << " "
							 << o.check.detail);
			std::string warnings;
			for (const auto& f : o.check.warnings) {
				warnings += Hires::rule_name(f.rule);
			}
			if (o.severity == "reject") {
				CHECK(Hires::rule_name(o.check.rule) == o.rule);
			} else {
				CHECK(o.check.rule == Hires::Rule::none);
				CHECK(warnings == (o.severity == "warning" ? o.rule : std::string()));
			}
		}
	}
}    // namespace

TEST_CASE("hires editor fixtures: Aseprite and GIMP exports") {
	const auto outcomes = run_dir("editor");
	REQUIRE(outcomes.size() >= 9);
	check_outcomes(outcomes);

	const auto& game = hires_test::synth_game();

	const auto nn = [&](int shape, int frame) {
		return hires_test::nn_tile(game.provider()(shape, frame), 6);
	};
	// Accepted exports give the template's raw indices back, interlaced or not.
	CHECK(outcomes.at("aseprite_indexed/0004_00.png").check.pixels == nn(4, 0));
	CHECK(outcomes.at("aseprite_transparent/0004_00.png").check.pixels == nn(4, 0));
	CHECK(outcomes.at("gimp_full_palette/0004_00.png").check.pixels == nn(4, 0));
	CHECK(outcomes.at("gimp_interlaced/0004_00.png").check.pixels == nn(4, 0));
	// GIMP's "remove unused colors" renumbers the colormap: F2 names index 0.
	const auto& unused = outcomes.at("gimp_remove_unused/0004_00.png").check;
	CHECK(unused.detail.find("PLTE[0] = ") == 0);
	CHECK(unused.detail.find("(load palette/pal0.gpl, keep indices)") != std::string::npos);
	// An editor that drops tEXt: accepted, unguarded; the guard after IDAT counts.
	CHECK_FALSE(outcomes.at("text_dropped/0004_01.png").check.guarded);
	CHECK(outcomes.at("text_after_idat/0004_02.png").check.guarded);
	CHECK(outcomes.at("gimp_rgb/0004_00.png").check.detail.find("colour type 2") == 0);
	CHECK(outcomes.at("gimp_rgba/0004_00.png").check.detail.find("colour type 6") == 0);
}

TEST_CASE("hires editor fixtures: the user's own exports (editor/real, WP-15)") {
	std::error_code ec;
	if (!std::filesystem::exists(hires_test::data_path("hires/editor/real/expected.txt"), ec)) {
		MESSAGE("skipped: tests/data/hires/editor/real is added in WP-15");
		return;
	}
	check_outcomes(run_dir("editor/real"));
}

#endif
