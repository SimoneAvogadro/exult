/*
 *  main.cc - Entry point of hires_unit, the SDL-free unit tests of the hi-res
 *  render path (DESIGN.md section 6.2).
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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "test_support.h"

#include <cstdlib>

#ifndef HIRES_TEST_DATA_DIR
#	define HIRES_TEST_DATA_DIR "tests/data"
#endif

std::string hires_test::data_path(const std::string& name) {
	const char* dir = std::getenv("HIRES_TEST_DATA");
	if (dir == nullptr || *dir == '\0') {
		dir = HIRES_TEST_DATA_DIR;
	}
	return std::string(dir) + "/" + name;
}

std::string hires_test::scratch_path(const std::string& name) {
	const char* dir = std::getenv("HIRES_TEST_TMP");
	if (dir == nullptr || *dir == '\0') {
		dir = ".";
	}
	return std::string(dir) + "/hires_unit_" + name;
}
