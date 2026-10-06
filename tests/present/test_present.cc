/*
 *  test_present.cc - hires_present, the data-free present tests of the hi-res
 *  render path (DESIGN.md section 6.3).
 *
 *  They run SDL with the offscreen video driver and the software renderer
 *  (make check sets SDL_VIDEO_DRIVER=offscreen and SDL_RENDER_DRIVER=software)
 *  and exit with 77 (skipped) when either is unavailable. The harness cases
 *  check the read-back path; the World_presenter cases check the hi-res
 *  present path of section 6.3 for ARGB and, with SDL 3.4 or newer, INDEX8:
 *  exact 1:1 read-back, palette changes, partial and clamped uploads, the
 *  2x2 box at r = 0.5, the halving chain at r = 0.25 (with direct LINEAR as
 *  a negative control), palette alpha 0, letterbox bars, a window resized
 *  after create, and the reset events.
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

#include "test_support.h"
#include "world_present.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
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

/*
 *  World_presenter (DESIGN.md sections 3.2.4, 3.2.5 and 6.3).
 */

namespace {
	using Format = World_presenter::Format;

	// The draw surface's guard band, as in Image_window.
	constexpr int guard = 4;

	std::vector<Format> formats_to_test() {
		std::vector<Format> formats{Format::Argb};
#if SDL_VERSION_ATLEAST(3, 4, 0)
		formats.push_back(Format::Index8);
#endif
		return formats;
	}

	const char* format_name(Format format) {
		return format == Format::Index8 ? "INDEX8" : "ARGB";
	}

	// A hidden window with a software renderer and Exult's logical
	// presentation (LETTERBOX).
	struct Target {
		SDL_Window*   win = nullptr;
		SDL_Renderer* ren = nullptr;

		Target(int w, int h, int logical_w, int logical_h) {
			win = SDL_CreateWindow("hires_present", w, h, SDL_WINDOW_HIDDEN);
			if (win != nullptr) {
				ren = SDL_CreateRenderer(win, "software");
			}
			if (ren != nullptr) {
				SDL_SetRenderLogicalPresentation(ren, logical_w, logical_h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
			}
		}

		~Target() {
			if (ren != nullptr) {
				SDL_DestroyRenderer(ren);
			}
			if (win != nullptr) {
				SDL_DestroyWindow(win);
			}
		}

		Target(const Target&)            = delete;
		Target& operator=(const Target&) = delete;

		bool ok() const {
			return ren != nullptr;
		}

		// The window size changes; the logical presentation stays, so the
		// letterbox rect follows the new output size.
		void resize(int w, int h) const {
			SDL_SetWindowSize(win, w, h);
			SDL_SyncWindow(win);
			SDL_PumpEvents();
			// SDL 3.2's software renderer picks up a larger window surface
			// only at the next present.
			SDL_RenderPresent(ren);
		}
	};

	// The scaled 8-bit main buffer: a texture area of tw x th physical
	// pixels inside a guard band, with its own palette.
	struct World8 {
		SDL_Surface* surface = nullptr;
		int          tw;
		int          th;

		World8(int w, int h) : tw(w), th(h) {
			surface = SDL_CreateSurface(w + 2 * guard, h + 2 * guard, SDL_PIXELFORMAT_INDEX8);
			if (surface != nullptr && !SDL_CreateSurfacePalette(surface)) {
				SDL_DestroySurface(surface);
				surface = nullptr;
			}
		}

		~World8() {
			if (surface != nullptr) {
				SDL_DestroySurface(surface);
			}
		}

		World8(const World8&)            = delete;
		World8& operator=(const World8&) = delete;

		// Texture space: (0, 0) is the surface's (guard, guard).
		unsigned char& at(int x, int y) {
			return static_cast<unsigned char*>(surface->pixels)[(y + guard) * surface->pitch + x + guard];
		}

		// A synthetic palette (no game data).
		void set_palette(int seed, Uint8 alpha) {
			SDL_Color colors[256];
			for (int i = 0; i < 256; i++) {
				colors[i].r = static_cast<Uint8>(i * 67 + seed * 31 + 13);
				colors[i].g = static_cast<Uint8>(i * 151 + seed * 7 + 101);
				colors[i].b = static_cast<Uint8>(i * 29 + seed * 53 + 200);
				colors[i].a = alpha;
			}
			SDL_SetPaletteColors(SDL_GetSurfacePalette(surface), colors, 0, 256);
		}

		// Random bytes everywhere (guard band included), then the texture
		// area as random blocks of block x block pixels.
		void fill(hires_test::Rng& rng, int block) {
			auto* pixels = static_cast<unsigned char*>(surface->pixels);
			for (int i = 0; i < surface->pitch * surface->h; i++) {
				pixels[i] = rng.byte();
			}
			for (int y = 0; y < th; y += block) {
				for (int x = 0; x < tw; x += block) {
					const unsigned char v = rng.byte();
					for (int dy = 0; dy < block && y + dy < th; dy++) {
						for (int dx = 0; dx < block && x + dx < tw; dx++) {
							at(x + dx, y + dy) = v;
						}
					}
				}
			}
		}

		uint32_t rgb(int x, int y) {
			const SDL_Color& c = SDL_GetSurfacePalette(surface)->colors[at(x, y)];
			return (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | c.b;
		}

		// LUT(indices) of the texture area, RGB only.
		std::vector<uint32_t> lut_image() {
			std::vector<uint32_t> image(size_t(tw) * th);
			for (int y = 0; y < th; y++) {
				for (int x = 0; x < tw; x++) {
					image[size_t(y) * tw + x] = rgb(x, y);
				}
			}
			return image;
		}
	};

	// Reads the whole output back as RGB, with the logical presentation
	// switched off for the read.
	bool read_output(SDL_Renderer* ren, std::vector<uint32_t>& out, int& w, int& h) {
		SDL_FlushRenderer(ren);
		int                             lw   = 0;
		int                             lh   = 0;
		SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
		SDL_GetRenderLogicalPresentation(ren, &lw, &lh, &mode);
		SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
		SDL_Surface* raw = SDL_RenderReadPixels(ren, nullptr);
		SDL_SetRenderLogicalPresentation(ren, lw, lh, mode);
		if (raw == nullptr) {
			return false;
		}
		SDL_Surface* argb = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888);
		SDL_DestroySurface(raw);
		if (argb == nullptr) {
			return false;
		}
		w = argb->w;
		h = argb->h;
		out.assign(size_t(w) * h, 0);
		for (int y = 0; y < h; y++) {
			std::memcpy(&out[size_t(y) * w], static_cast<const unsigned char*>(argb->pixels) + y * argb->pitch, size_t(w) * 4);
		}
		for (uint32_t& pixel : out) {
			pixel &= 0xffffffu;
		}
		SDL_DestroySurface(argb);
		return true;
	}

	std::vector<uint32_t> read_output(SDL_Renderer* ren) {
		std::vector<uint32_t> out;
		int                   w = 0;
		int                   h = 0;
		if (!read_output(ren, out, w, h)) {
			out.clear();
		}
		return out;
	}

	// The largest difference of one colour channel (-1 if the sizes differ).
	int max_channel_diff(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
		if (a.size() != b.size() || a.empty()) {
			return -1;
		}
		int worst = 0;
		for (size_t i = 0; i < a.size(); i++) {
			for (int shift = 0; shift <= 16; shift += 8) {
				const int ca = int((a[i] >> shift) & 255);
				const int cb = int((b[i] >> shift) & 255);
				worst        = std::max(worst, std::abs(ca - cb));
			}
		}
		return worst;
	}

	// One 2:1 pass: the 2x2 box average of each channel, truncated.
	std::vector<uint32_t> box2(const std::vector<uint32_t>& src, int w, int h) {
		const int             ow = w / 2;
		const int             oh = h / 2;
		std::vector<uint32_t> out(size_t(ow) * oh);
		for (int y = 0; y < oh; y++) {
			for (int x = 0; x < ow; x++) {
				uint32_t pixel = 0;
				for (int shift = 0; shift <= 16; shift += 8) {
					int sum = 0;
					for (int dy = 0; dy < 2; dy++) {
						for (int dx = 0; dx < 2; dx++) {
							sum += int((src[size_t(2 * y + dy) * w + 2 * x + dx] >> shift) & 255);
						}
					}
					pixel |= uint32_t(sum / 4) << shift;
				}
				out[size_t(y) * ow + x] = pixel;
			}
		}
		return out;
	}

	// Nearest-neighbour upscale by an integer factor.
	std::vector<uint32_t> nn(const std::vector<uint32_t>& src, int w, int h, int f) {
		std::vector<uint32_t> out(size_t(w) * f * h * f);
		for (int y = 0; y < h * f; y++) {
			for (int x = 0; x < w * f; x++) {
				out[size_t(y) * w * f + x] = src[size_t(y / f) * w + x / f];
			}
		}
		return out;
	}

	// One frame as Image_window::present_world_frame draws it (no present).
	bool frame(World_presenter& presenter, World_filter_override force = World_filter_override::Auto) {
		presenter.clear_window();
		return presenter.draw(force);
	}

	// Creates the presenter; for INDEX8 it reports when the renderer refused it.
	bool create(World_presenter& presenter, const Target& target, World8& world, Format format) {
		if (!presenter.create(target.ren, world.surface, world.tw, world.th, format)) {
			return false;
		}
		if (presenter.format() != format) {
			MESSAGE("the software renderer refused " << format_name(format) << "; testing ARGB instead");
		}
		return true;
	}

	size_t bytes_per_pixel(const World_presenter& presenter) {
		return presenter.format() == Format::Index8 ? 1 : 4;
	}

	void push_render_event(Uint32 type, SDL_WindowID window_id) {
		SDL_Event event;
		SDL_zero(event);
		event.type            = type;
		event.render.windowID = window_id;
		SDL_PushEvent(&event);
	}
}    // namespace

TEST_CASE("world presenter: 1:1 read-back equals LUT(NN(indices))") {
	for (const int S : {2, 6}) {
		for (const Format format : formats_to_test()) {
			CAPTURE(S);
			INFO("format " << format_name(format));
			const int tw = 40 * S;
			const int th = 25 * S;
			Target    target(tw, th, tw, th);
			REQUIRE(target.ok());
			World8 world(tw, th);
			REQUIRE(world.surface != nullptr);
			hires_test::Rng rng(1000 + S);
			world.set_palette(1, 255);
			world.fill(rng, S);
			World_presenter presenter;
			REQUIRE(create(presenter, target, world, format));
			presenter.upload(world.surface, guard, World_phys_rect{0, 0, tw, th});
			CHECK(presenter.stats().upload_bytes == size_t(tw) * th * bytes_per_pixel(presenter));
			CHECK_FALSE(presenter.stats().clamped);
			REQUIRE(frame(presenter));
			CHECK(presenter.stats().filter.filter == World_filter::Nearest);
			CHECK(presenter.stats().l_w == tw);
			CHECK(presenter.stats().l_h == th);
			CHECK(read_output(target.ren) == world.lut_image());
			// NEAREST 1:1 needs neither a resolve nor halving targets.
			CHECK_FALSE(presenter.has_rgb_target());
			CHECK(presenter.halving_targets() == 0);
		}
	}
}

TEST_CASE("world presenter: a palette change without writes") {
	for (const Format format : formats_to_test()) {
		INFO("format " << format_name(format));
		const int tw = 120;
		const int th = 75;
		Target    target(tw, th, tw, th);
		REQUIRE(target.ok());
		World8          world(tw, th);
		hires_test::Rng rng(7);
		world.set_palette(1, 255);
		world.fill(rng, 3);
		World_presenter presenter;
		REQUIRE(create(presenter, target, world, format));
		presenter.upload(world.surface, guard, World_phys_rect{0, 0, tw, th});
		REQUIRE(frame(presenter));
		// No writes, no palette change: nothing to upload.
		presenter.upload(world.surface, guard, World_phys_rect{});
		CHECK(presenter.stats().upload_bytes == 0);
		// A palette tick: ARGB converts the whole texture, INDEX8 uploads 0 bytes.
		world.set_palette(2, 255);
		presenter.upload(world.surface, guard, World_phys_rect{});
		if (presenter.format() == Format::Index8) {
			CHECK(presenter.stats().upload_bytes == 0);
			CHECK_FALSE(presenter.stats().full_convert);
		} else {
			CHECK(presenter.stats().upload_bytes == size_t(tw) * th * 4);
			CHECK(presenter.stats().full_convert);
		}
		REQUIRE(frame(presenter));
		CHECK(read_output(target.ren) == world.lut_image());
	}
}

TEST_CASE("world presenter: partial and clamped uploads") {
	for (const Format format : formats_to_test()) {
		INFO("format " << format_name(format));
		const int tw = 120;
		const int th = 75;
		Target    target(tw, th, tw, th);
		REQUIRE(target.ok());
		World8          world(tw, th);
		hires_test::Rng rng(11);
		world.set_palette(3, 255);
		world.fill(rng, 3);
		World_presenter presenter;
		REQUIRE(create(presenter, target, world, format));
		presenter.upload(world.surface, guard, World_phys_rect{0, 0, tw, th});
		const std::vector<uint32_t> before = world.lut_image();

		// Change every index, upload only one rect: only that rect changes.
		world.fill(rng, 3);
		const std::vector<uint32_t> after = world.lut_image();
		const World_phys_rect       rect{21, 9, 33, 17};
		presenter.upload(world.surface, guard, rect);
		CHECK(presenter.stats().upload_bytes == size_t(rect.w) * rect.h * bytes_per_pixel(presenter));
		CHECK_FALSE(presenter.stats().clamped);
		REQUIRE(frame(presenter));
		std::vector<uint32_t> expected = before;
		for (int y = rect.y; y < rect.y + rect.h; y++) {
			for (int x = rect.x; x < rect.x + rect.w; x++) {
				expected[size_t(y) * tw + x] = after[size_t(y) * tw + x];
			}
		}
		CHECK(read_output(target.ren) == expected);

		// A rect partly outside the texture is clamped (ASan watches the lock).
		const World_phys_rect outside{tw - 7, -3, 20, 10};
		presenter.upload(world.surface, guard, outside);
		CHECK(presenter.stats().clamped);
		CHECK(presenter.stats().upload_bytes == size_t(7) * 7 * bytes_per_pixel(presenter));
		REQUIRE(frame(presenter));
		for (int y = 0; y < 7; y++) {
			for (int x = tw - 7; x < tw; x++) {
				expected[size_t(y) * tw + x] = after[size_t(y) * tw + x];
			}
		}
		CHECK(read_output(target.ren) == expected);
		// Entirely outside: nothing.
		presenter.upload(world.surface, guard, World_phys_rect{tw, th, 5, 5});
		CHECK(presenter.stats().upload_bytes == 0);
		presenter.upload(world.surface, guard, World_phys_rect{-50, -50, 10, 10});
		CHECK(presenter.stats().upload_bytes == 0);
	}
}

TEST_CASE("world presenter: r = 0.5 is the 2x2 box") {
	for (const Format format : formats_to_test()) {
		INFO("format " << format_name(format));
		const int tw = 160;
		const int th = 100;
		Target    target(tw / 2, th / 2, tw / 2, th / 2);
		REQUIRE(target.ok());
		World8          world(tw, th);
		hires_test::Rng rng(21);
		world.set_palette(4, 255);
		world.fill(rng, 1);    // Physical detail.
		World_presenter presenter;
		REQUIRE(create(presenter, target, world, format));
		presenter.upload(world.surface, guard, World_phys_rect{0, 0, tw, th});
		REQUIRE(frame(presenter));
		CHECK(presenter.stats().filter.filter == World_filter::Linear);
		CHECK(max_channel_diff(read_output(target.ren), box2(world.lut_image(), tw, th)) <= 1);
		CHECK(presenter.has_rgb_target() == (presenter.format() == Format::Index8));
		CHECK(presenter.halving_targets() == 0);
	}
}

TEST_CASE("world presenter: r = 0.25 runs the halving chain") {
	for (const Format format : formats_to_test()) {
		INFO("format " << format_name(format));
		const int tw = 320;
		const int th = 200;
		Target    target(tw / 4, th / 4, tw / 4, th / 4);
		REQUIRE(target.ok());
		World8          world(tw, th);
		hires_test::Rng rng(31);
		world.set_palette(5, 255);
		world.fill(rng, 1);
		World_presenter presenter;
		REQUIRE(create(presenter, target, world, format));
		presenter.upload(world.surface, guard, World_phys_rect{0, 0, tw, th});
		REQUIRE(frame(presenter));
		CHECK(presenter.stats().filter.filter == World_filter::Halving);
		CHECK(presenter.stats().filter.halvings == 1);
		CHECK(presenter.halving_targets() == 1);
		// Reference: the cascaded per-pass 2x2 boxes, truncating.
		const std::vector<uint32_t> cascaded = box2(box2(world.lut_image(), tw, th), tw / 2, th / 2);
		const int                   chain    = max_channel_diff(read_output(target.ren), cascaded);
		CHECK(chain >= 0);
		CHECK(chain <= 1);
		// Negative control: a direct LINEAR draw at 0.25 aliases.
		REQUIRE(frame(presenter, World_filter_override::Linear));
		CHECK(presenter.stats().filter.filter == World_filter::Linear);
		const int direct = max_channel_diff(read_output(target.ren), cascaded);
		MESSAGE("halving chain max error " << chain << ", direct LINEAR max error " << direct);
		CHECK(direct > 32);
	}
}

TEST_CASE("world presenter: palette alpha 0 changes nothing") {
	struct Geometry {
		int tw, th, ww, wh;
	};

	// r = 0.667 (LINEAR, an INDEX8 resolve) and r = 0.25 (halving).
	static const Geometry geometries[] = {
			{120,  75, 80, 50},
            {320, 200, 80, 50}
    };
	for (const Geometry& g : geometries) {
		for (const Format format : formats_to_test()) {
			CAPTURE(g.tw);
			INFO("format " << format_name(format));
			std::vector<uint32_t> results[2];
			for (int pass = 0; pass < 2; pass++) {
				Target target(g.ww, g.wh, g.ww, g.wh);
				REQUIRE(target.ok());
				World8          world(g.tw, g.th);
				hires_test::Rng rng(41);
				world.set_palette(6, pass == 0 ? 255 : 0);
				world.fill(rng, 1);
				World_presenter presenter;
				REQUIRE(create(presenter, target, world, format));
				presenter.upload(world.surface, guard, World_phys_rect{0, 0, g.tw, g.th});
				REQUIRE(frame(presenter));
				CHECK(presenter.stats().filter.filter != World_filter::Nearest);
				results[pass] = read_output(target.ren);
			}
			REQUIRE_FALSE(results[0].empty());
			CHECK(results[0] == results[1]);
			// And the picture is not black.
			CHECK(std::count(results[1].begin(), results[1].end(), 0u) < std::ptrdiff_t(results[1].size() / 4));
		}
	}
}

TEST_CASE("world presenter: the letterbox bars are black") {
	for (const Format format : formats_to_test()) {
		INFO("format " << format_name(format));
		// A 50x25 logical presentation in a 100x100 window: the world is
		// 100x50 at y = 25, with bars above and below.
		const int tw = 50;
		const int th = 25;
		Target    target(100, 100, tw, th);
		REQUIRE(target.ok());
		World8          world(tw, th);
		hires_test::Rng rng(51);
		world.set_palette(7, 255);
		world.fill(rng, 1);
		World_presenter presenter;
		REQUIRE(create(presenter, target, world, format));
		presenter.upload(world.surface, guard, World_phys_rect{0, 0, tw, th});
		// A non-black backbuffer first.
		SDL_SetRenderDrawColor(target.ren, 255, 0, 0, 255);
		SDL_RenderClear(target.ren);
		SDL_RenderPresent(target.ren);
		SDL_RenderClear(target.ren);
		REQUIRE(frame(presenter));
		CHECK(presenter.stats().filter.filter == World_filter::Nearest);
		CHECK(presenter.stats().l_w == 100);
		CHECK(presenter.stats().l_h == 50);
		std::vector<uint32_t> out;
		int                   w = 0;
		int                   h = 0;
		REQUIRE(read_output(target.ren, out, w, h));
		REQUIRE(w == 100);
		REQUIRE(h == 100);
		const std::vector<uint32_t> world2 = nn(world.lut_image(), tw, th, 2);
		bool                        bars   = true;
		bool                        middle = true;
		for (int y = 0; y < 100; y++) {
			for (int x = 0; x < 100; x++) {
				const uint32_t p = out[size_t(y) * 100 + x];
				if (y < 25 || y >= 75) {
					bars = bars && p == 0;
				} else {
					middle = middle && p == world2[size_t(y - 25) * 100 + x];
				}
			}
		}
		CHECK(bars);
		CHECK(middle);
	}
}

TEST_CASE("world presenter: a window resized after create re-chooses the filter") {
	for (const Format format : formats_to_test()) {
		INFO("format " << format_name(format));
		const int tw = 160;
		const int th = 100;
		Target    target(tw, th, tw, th);
		REQUIRE(target.ok());
		World8          world(tw, th);
		hires_test::Rng rng(61);
		world.set_palette(8, 255);
		world.fill(rng, 2);
		World_presenter presenter;
		REQUIRE(create(presenter, target, world, format));
		presenter.upload(world.surface, guard, World_phys_rect{0, 0, tw, th});
		REQUIRE(frame(presenter));
		CHECK(presenter.stats().filter.filter == World_filter::Nearest);
		// No surface rebuild: only the window (and so the letterbox rect) changes.
		target.resize(tw / 2, th / 2);
		REQUIRE(frame(presenter));
		CHECK(presenter.stats().l_w == tw / 2);
		CHECK(presenter.stats().filter.filter == World_filter::Linear);
		CHECK(max_channel_diff(read_output(target.ren), box2(world.lut_image(), tw, th)) <= 1);
		target.resize(tw / 4, th / 4);
		REQUIRE(frame(presenter));
		CHECK(presenter.stats().filter.filter == World_filter::Halving);
		CHECK(presenter.halving_targets() == 1);
		target.resize(tw, th);
		REQUIRE(frame(presenter));
		CHECK(presenter.stats().filter.filter == World_filter::Nearest);
		CHECK(read_output(target.ren) == world.lut_image());
	}
}

TEST_CASE("world presenter: reset events reach the event watch") {
	for (const Format format : formats_to_test()) {
		INFO("format " << format_name(format));
		const int tw = 120;
		const int th = 75;
		Target    target(tw, th, tw, th);
		REQUIRE(target.ok());
		World8          world(tw, th);
		hires_test::Rng rng(71);
		world.set_palette(9, 255);
		world.fill(rng, 3);
		World_presenter presenter;
		REQUIRE(create(presenter, target, world, format));
		presenter.upload(world.surface, guard, World_phys_rect{0, 0, tw, th});
		REQUIRE(frame(presenter));
		const std::vector<uint32_t> reference = read_output(target.ren);
		REQUIRE(reference == world.lut_image());
		const SDL_WindowID      id     = SDL_GetWindowID(target.win);
		World_presenter::Resets resets = presenter.consume_resets();
		CHECK_FALSE((resets.targets || resets.device || resets.lost));

		push_render_event(SDL_EVENT_RENDER_TARGETS_RESET, id);
		resets = presenter.consume_resets();
		CHECK(resets.targets);
		CHECK_FALSE(resets.device);
		CHECK_FALSE(resets.lost);
		CHECK_FALSE(presenter.consume_resets().targets);    // Taken.
		// The consumer forces a full upload; the read-back is identical.
		presenter.upload(world.surface, guard, World_phys_rect{}, true);
		REQUIRE(frame(presenter));
		CHECK(read_output(target.ren) == reference);

		push_render_event(SDL_EVENT_RENDER_DEVICE_RESET, id);
		resets = presenter.consume_resets();
		CHECK(resets.device);
		CHECK_FALSE(resets.targets);
		// The consumer recreates the textures and uploads everything.
		const Format created = presenter.format();
		REQUIRE(presenter.recreate_textures());
		CHECK(presenter.format() == created);
		presenter.upload(world.surface, guard, World_phys_rect{}, true);
		REQUIRE(frame(presenter));
		CHECK(read_output(target.ren) == reference);

		push_render_event(SDL_EVENT_RENDER_DEVICE_LOST, 0);    // 0: any window
		CHECK(presenter.consume_resets().lost);

		// Another window's renderer is not ours.
		push_render_event(SDL_EVENT_RENDER_TARGETS_RESET, id + 1000);
		CHECK_FALSE(presenter.consume_resets().targets);
		SDL_FlushEvents(SDL_EVENT_RENDER_TARGETS_RESET, SDL_EVENT_RENDER_DEVICE_LOST);
	}
}

TEST_CASE("world presenter: destroy removes the event watch") {
	Target target(64, 40, 64, 40);
	REQUIRE(target.ok());
	World8 world(64, 40);
	world.set_palette(10, 255);
	auto presenter = std::make_unique<World_presenter>();
	REQUIRE(presenter->create(target.ren, world.surface, 64, 40, Format::Argb));
	presenter->destroy();
	CHECK_FALSE(presenter->is_created());
	push_render_event(SDL_EVENT_RENDER_TARGETS_RESET, 0);
	CHECK_FALSE(presenter->consume_resets().targets);
	// Destroyed and freed: an event must not reach it (ASan would report).
	REQUIRE(presenter->create(target.ren, world.surface, 64, 40, Format::Argb));
	presenter.reset();
	push_render_event(SDL_EVENT_RENDER_DEVICE_RESET, 0);
	SDL_FlushEvents(SDL_EVENT_RENDER_TARGETS_RESET, SDL_EVENT_RENDER_DEVICE_LOST);
	// A presenter without a texture draws nothing.
	World_presenter empty;
	CHECK_FALSE(empty.draw());
	empty.upload(world.surface, guard, World_phys_rect{0, 0, 64, 40});
	CHECK(empty.stats().upload_bytes == 0);
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
