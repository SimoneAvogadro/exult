/*
 *  test_support.h - Shared helpers for the hi-res unit tests (hires_unit).
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

#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

namespace hires_test {
	/*
	 *  Deterministic PRNG (SplitMix64). The tests never use std::rand() or the
	 *  standard distributions for their own choices: those sequences differ
	 *  between C libraries and compilers, and the recorded digests are shared
	 *  by every platform.
	 */
	class Rng {
		uint64_t state;

	public:
		explicit Rng(uint64_t seed) : state(seed) {}

		uint64_t next64() {
			uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
			z          = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
			z          = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
			return z ^ (z >> 31);
		}

		uint32_t next32() {
			return static_cast<uint32_t>(next64() >> 32);
		}

		// An integer in [lo, hi], both inclusive (the modulo bias does not matter here).
		int range(int lo, int hi) {
			assert(lo <= hi);
			const auto span = static_cast<uint64_t>(static_cast<int64_t>(hi) - lo + 1);
			return static_cast<int>(lo + static_cast<int64_t>(next64() % span));
		}

		// True with the given probability in percent.
		bool chance(int percent) {
			return range(0, 99) < percent;
		}

		unsigned char byte() {
			return static_cast<unsigned char>(next32() >> 24);
		}
	};

	/*
	 *  FNV-1a, 64 bit.
	 */
	constexpr uint64_t fnv_offset = 0xcbf29ce484222325ULL;
	constexpr uint64_t fnv_prime  = 0x100000001b3ULL;

	inline uint64_t fnv1a64(const void* data, size_t len, uint64_t hash = fnv_offset) {
		const auto* bytes = static_cast<const unsigned char*>(data);
		for (size_t i = 0; i < len; i++) {
			hash ^= bytes[i];
			hash *= fnv_prime;
		}
		return hash;
	}

	// Hashes a value as 8 little-endian bytes, independent of the host's byte order.
	inline uint64_t fnv1a64_value(uint64_t value, uint64_t hash) {
		for (int i = 0; i < 8; i++) {
			hash ^= (value >> (8 * i)) & 0xff;
			hash *= fnv_prime;
		}
		return hash;
	}

	// Path of a file in tests/data: $HIRES_TEST_DATA if set, else the source
	// directory compiled in (HIRES_TEST_DATA_DIR). Defined in main.cc.
	std::string data_path(const std::string& name);

	// Path of a scratch file "hires_unit_<name>" that a test writes and
	// removes: in $HIRES_TEST_TMP if set, else in the current directory (the
	// build tree under make check). Defined in main.cc.
	std::string scratch_path(const std::string& name);
}    // namespace hires_test

#endif
