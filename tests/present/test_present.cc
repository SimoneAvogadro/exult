/*
 *  test_present.cc - hires_present, the data-free present tests of the hi-res
 *  render path.
 *
 *  They run SDL with the offscreen video driver and the software renderer
 *  (make check sets SDL_VIDEO_DRIVER=offscreen and SDL_RENDER_DRIVER=software)
 *  and exit with 77 (skipped) when either is unavailable. WP-01 provides the
 *  harness and checks the read-back path the world presenter tests rely on;
 *  WP-05 adds the presenter cases.
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

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#ifdef __GNUC__
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wold-style-cast"
#	pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#	if !defined(__llvm__) && !defined(__clang__)
#		pragma GCC diagnostic ignored "-Wuseless-cast"
#	endif
#endif    // __GNUC__
#include <SDL3/SDL.h>
#ifdef __GNUC__
#	pragma GCC diagnostic pop
#endif    // __GNUC__

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
	// Exit code for "skipped" in the automake test harness.
	constexpr int exit_skip = 77;

	SDL_Window*   window   = nullptr;
	SDL_Renderer* renderer = nullptr;

	// Reads `rect` of the current render target back as ARGB8888 pixels.
	bool read_back(const SDL_Rect& rect, std::vector<uint32_t>& pixels) {
		SDL_Surface* raw = SDL_RenderReadPixels(renderer, &rect);
		if (raw == nullptr) {
			return false;
		}
		SDL_Surface* argb = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888);
		SDL_DestroySurface(raw);
		if (argb == nullptr) {
			return false;
		}
		pixels.assign(static_cast<size_t>(rect.w) * rect.h, 0);
		const auto* bytes = static_cast<const unsigned char*>(argb->pixels);
		for (int y = 0; y < rect.h; y++) {
			std::memcpy(&pixels[static_cast<size_t>(y) * rect.w], bytes + static_cast<ptrdiff_t>(y) * argb->pitch, rect.w * 4);
		}
		SDL_DestroySurface(argb);
		return true;
	}

	void shut_down() {
		if (renderer != nullptr) {
			SDL_DestroyRenderer(renderer);
			renderer = nullptr;
		}
		if (window != nullptr) {
			SDL_DestroyWindow(window);
			window = nullptr;
		}
		SDL_Quit();
	}
}    // namespace

TEST_CASE("present harness: the SDL library matches the headers") {
	// A lane built against one SDL release must run with it: build-sdl32
	// compiles against 3.2.14, and env.sh puts 3.4.18 (same soname) on
	// LD_LIBRARY_PATH, which only its DT_RPATH overrides.
	const int linked = SDL_GetVersion();
	INFO("headers " << SDL_MAJOR_VERSION << "." << SDL_MINOR_VERSION << "." << SDL_MICRO_VERSION << ", library "
					<< SDL_VERSIONNUM_MAJOR(linked) << "." << SDL_VERSIONNUM_MINOR(linked) << "." << SDL_VERSIONNUM_MICRO(linked));
	CHECK(SDL_VERSIONNUM_MAJOR(linked) == SDL_MAJOR_VERSION);
	CHECK(SDL_VERSIONNUM_MINOR(linked) == SDL_MINOR_VERSION);
}

TEST_CASE("present harness: an ARGB8888 texture reads back exactly at 1:1") {
	constexpr int         w = 7;
	constexpr int         h = 5;
	std::vector<uint32_t> source(static_cast<size_t>(w) * h);
	for (int i = 0; i < w * h; i++) {
		// Opaque, distinct colours with every channel varying.
		source[i] = 0xff000000u | (static_cast<uint32_t>(i * 37 % 256) << 16) | (static_cast<uint32_t>(255 - i * 11) << 8)
					| static_cast<uint32_t>(i * 7);
	}
	SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
	REQUIRE(texture != nullptr);
	CHECK(SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE));
	CHECK(SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST));
	REQUIRE(SDL_UpdateTexture(texture, nullptr, source.data(), w * 4));

	// Draw over a non-black backbuffer, at an offset.
	CHECK(SDL_SetRenderDrawColor(renderer, 40, 80, 120, 255));
	CHECK(SDL_RenderClear(renderer));
	const SDL_FRect dst{3, 2, w, h};
	CHECK(SDL_RenderTexture(renderer, texture, nullptr, &dst));

	std::vector<uint32_t> pixels;
	REQUIRE(read_back(SDL_Rect{3, 2, w, h}, pixels));
	CHECK(pixels == source);

	// Outside the drawn rectangle the clear colour is untouched.
	REQUIRE(read_back(SDL_Rect{0, 0, 3, 1}, pixels));
	CHECK(pixels == std::vector<uint32_t>(3, 0xff285078u));
	SDL_DestroyTexture(texture);
}

int main(int argc, char** argv) {
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		std::cerr << "SKIP: SDL_Init(SDL_INIT_VIDEO) failed: " << SDL_GetError() << '\n';
		SDL_Quit();
		return exit_skip;
	}
	window = SDL_CreateWindow("hires_present", 64, 48, SDL_WINDOW_HIDDEN);
	if (window != nullptr) {
		renderer = SDL_CreateRenderer(window, "software");
	}
	if (renderer == nullptr) {
		std::cerr << "SKIP: no window with a software renderer (video driver '"
				  << (SDL_GetCurrentVideoDriver() != nullptr ? SDL_GetCurrentVideoDriver() : "none") << "'): " << SDL_GetError()
				  << '\n';
		shut_down();
		return exit_skip;
	}

	doctest::Context context(argc, argv);
	const int        result = context.run();
	shut_down();
	return result;
}
