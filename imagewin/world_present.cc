/*
 *  world_present.cc - Hi-res render scale: the world presenter (DESIGN.md
 *  section 3.2.4).
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

#include "world_present.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

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

namespace {
	// Creates an ARGB8888 texture with an explicit blend and scale mode: SDL
	// defaults ARGB textures to BLEND and every texture to LINEAR.
	SDL_Texture* create_argb(SDL_Renderer* renderer, SDL_TextureAccess access, int w, int h) {
		SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, access, w, h);
		if (texture == nullptr) {
			return nullptr;
		}
		if (!SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE) || !SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST)) {
			SDL_DestroyTexture(texture);
			return nullptr;
		}
		return texture;
	}

#if SDL_VERSION_ATLEAST(3, 4, 0)
	bool renderer_supports(SDL_Renderer* renderer, SDL_PixelFormat format) {
		const auto* formats = static_cast<const SDL_PixelFormat*>(
				SDL_GetPointerProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, nullptr));
		if (formats == nullptr) {
			return false;
		}
		for (; *formats != SDL_PIXELFORMAT_UNKNOWN; ++formats) {
			if (*formats == format) {
				return true;
			}
		}
		return false;
	}
#endif
}    // namespace

World_presenter::~World_presenter() {
	destroy();
}

std::string World_presenter::capabilities() {
	std::string text = "Hi-res present: ARGB8888";
#if SDL_VERSION_ATLEAST(3, 4, 0)
	text += " and INDEX8 world textures (INDEX8 where the renderer accepts it), PIXELART on shader renderers";
#else
	text += " world textures only, LINEAR filtering (built with SDL older than 3.4)";
#endif
	return text;
}

bool World_presenter::renderer_pixelart_ok(SDL_Renderer* renderer) {
#if SDL_VERSION_ATLEAST(3, 4, 0)
	// The software renderer and direct3d (D3D9) map PIXELART to NEAREST.
	static const char* const shader_renderers[] = {"direct3d11", "direct3d12", "vulkan", "opengl", "opengles2", "gpu", "metal"};
	const char*              name               = renderer != nullptr ? SDL_GetRendererName(renderer) : nullptr;
	if (name == nullptr) {
		return false;
	}
	for (const char* candidate : shader_renderers) {
		if (std::strcmp(name, candidate) == 0) {
			return true;
		}
	}
#else
	(void)renderer;
#endif
	return false;
}

bool World_presenter::create(SDL_Renderer* r, SDL_Surface* draw8, int pw, int ph, Format want) {
	destroy();
	if (r == nullptr || draw8 == nullptr || pw < 1 || ph < 1) {
		return false;
	}
	renderer      = r;
	palette_owner = draw8;
	tex_w         = pw;
	tex_h         = ph;
	wanted        = want;
	pixelart_ok   = renderer_pixelart_ok(r);
	if (!create_world_texture(want)) {
		destroy();
		return false;
	}
	SDL_Window* window = SDL_GetRenderWindow(r);
	window_id          = window != nullptr ? SDL_GetWindowID(window) : 0;
	reset_targets      = false;
	reset_device       = false;
	device_lost        = false;
	if (SDL_AddEventWatch(event_watch, this)) {
		watching = true;
	}
	return true;
}

bool World_presenter::create_world_texture(Format want) {
	lut_valid = false;
#if SDL_VERSION_ATLEAST(3, 4, 0)
	if (want == Format::Index8) {
		SDL_Palette* palette = SDL_GetSurfacePalette(palette_owner);
		if (palette == nullptr) {
			std::cerr << "[hires] INDEX8 present: the draw surface has no palette; using ARGB" << std::endl;
		} else if (!renderer_supports(renderer, SDL_PIXELFORMAT_INDEX8)) {
			std::cerr << "[hires] INDEX8 present: renderer '" << SDL_GetRendererName(renderer)
					  << "' has no INDEX8 textures; using ARGB" << std::endl;
		} else {
			SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_INDEX8, SDL_TEXTUREACCESS_STREAMING, tex_w, tex_h);
			// An INDEX8 texture is only ever sampled with NEAREST.
			if (texture != nullptr && SDL_SetTexturePalette(texture, palette)
				&& SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE)
				&& SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST)) {
				world_texture = texture;
				fmt           = Format::Index8;
				return true;
			}
			std::cerr << "[hires] INDEX8 present: " << SDL_GetError() << "; using ARGB" << std::endl;
			if (texture != nullptr) {
				SDL_DestroyTexture(texture);
			}
		}
	}
#else
	(void)want;
#endif
	world_texture = create_argb(renderer, SDL_TEXTUREACCESS_STREAMING, tex_w, tex_h);
	fmt           = Format::Argb;
	return world_texture != nullptr;
}

void World_presenter::destroy_lazy_targets() {
	if (world_rgb != nullptr) {
		SDL_DestroyTexture(world_rgb);
		world_rgb = nullptr;
	}
	for (SDL_Texture* texture : halving) {
		SDL_DestroyTexture(texture);
	}
	halving.clear();
}

void World_presenter::destroy() {
	if (watching) {
		SDL_RemoveEventWatch(event_watch, this);
		watching = false;
	}
	destroy_lazy_targets();
	if (world_texture != nullptr) {
		SDL_DestroyTexture(world_texture);
		world_texture = nullptr;
	}
	renderer      = nullptr;
	palette_owner = nullptr;
	tex_w = tex_h = 0;
	fmt           = Format::Argb;
	lut_valid     = false;
	last          = Stats();
}

bool World_presenter::recreate_textures() {
	if (renderer == nullptr) {
		return false;
	}
	destroy_lazy_targets();
	if (world_texture != nullptr) {
		SDL_DestroyTexture(world_texture);
		world_texture = nullptr;
	}
	return create_world_texture(wanted);
}

bool World_presenter::refresh_lut(SDL_Surface* draw8) {
	// Compare the RGB only: the alpha bytes and SDL_Palette::version are not
	// part of the contract.
	std::array<unsigned char, 768> rgb{};
	const SDL_Palette*             palette = SDL_GetSurfacePalette(draw8);
	if (palette != nullptr) {
		const int n = std::min(palette->ncolors, 256);
		for (int i = 0; i < n; i++) {
			rgb[3 * i]     = palette->colors[i].r;
			rgb[3 * i + 1] = palette->colors[i].g;
			rgb[3 * i + 2] = palette->colors[i].b;
		}
	}
	if (lut_valid && rgb == lut_rgb) {
		return false;
	}
	lut_rgb = rgb;
	for (int i = 0; i < 256; i++) {
		// Alpha 0xFF whatever the palette holds.
		lut[i] = 0xff000000u | (uint32_t(rgb[3 * i]) << 16) | (uint32_t(rgb[3 * i + 1]) << 8) | uint32_t(rgb[3 * i + 2]);
	}
	lut_valid = true;
	return true;
}

void World_presenter::upload(SDL_Surface* draw8, int guard, World_phys_rect phys, bool force_full) {
	last.upload_bytes = 0;
	last.full_convert = false;
	last.clamped      = false;
	if (world_texture == nullptr || draw8 == nullptr || draw8->pixels == nullptr) {
		return;
	}
	// The surface must hold the texture plus the guard band on every side.
	if (draw8->w < tex_w + 2 * guard || draw8->h < tex_h + 2 * guard) {
		return;
	}
	const World_phys_rect full{0, 0, tex_w, tex_h};
	if (force_full) {
		phys = full;
	}
	World_phys_rect rect = clamp_phys(phys, tex_w, tex_h);
	// SDL_LockTexture does not clip: the clamp above is the only guard.
	last.clamped = !phys.empty() && (rect.x != phys.x || rect.y != phys.y || rect.w != phys.w || rect.h != phys.h);
	if (fmt == Format::Argb && refresh_lut(draw8)) {
		rect              = full;
		last.full_convert = true;
	}
	if (rect.empty()) {
		return;
	}
	const auto*    src_base = static_cast<const unsigned char*>(draw8->pixels);
	const SDL_Rect area{rect.x, rect.y, rect.w, rect.h};
	const auto     source_row = [&](int row) {
        return src_base + static_cast<ptrdiff_t>(guard + rect.y + row) * draw8->pitch + guard + rect.x;
	};
	if (fmt == Format::Index8) {
		if (SDL_UpdateTexture(world_texture, &area, source_row(0), draw8->pitch)) {
			last.upload_bytes = size_t(rect.w) * rect.h;
		}
		return;
	}
	void* pixels = nullptr;
	int   pitch  = 0;
	if (!SDL_LockTexture(world_texture, &area, &pixels, &pitch)) {
		return;
	}
	for (int row = 0; row < rect.h; row++) {
		const unsigned char* src = source_row(row);
		// SDL aligns the rows of a locked ARGB texture.
		auto* dst = static_cast<uint32_t*>(
				static_cast<void*>(static_cast<unsigned char*>(pixels) + static_cast<ptrdiff_t>(row) * pitch));
		for (int x = 0; x < rect.w; x++) {
			dst[x] = lut[src[x]];
		}
	}
	SDL_UnlockTexture(world_texture);
	last.upload_bytes = size_t(rect.w) * rect.h * 4;
}

bool World_presenter::ensure_rgb_target() {
	if (world_rgb == nullptr) {
		world_rgb = create_argb(renderer, SDL_TEXTUREACCESS_TARGET, tex_w, tex_h);
	}
	return world_rgb != nullptr;
}

bool World_presenter::ensure_halving_targets(int k) {
	// Pass i halves the previous size, rounding up.
	int  w     = tex_w;
	int  h     = tex_h;
	bool match = halving.size() == size_t(k);
	for (int i = 0; match && i < k; i++) {
		w = (w + 1) / 2;
		h = (h + 1) / 2;
		float tw;
		float th;
		match = SDL_GetTextureSize(halving[i], &tw, &th) && int(tw) == w && int(th) == h;
	}
	if (match) {
		return true;
	}
	for (SDL_Texture* texture : halving) {
		SDL_DestroyTexture(texture);
	}
	halving.clear();
	w = tex_w;
	h = tex_h;
	for (int i = 0; i < k; i++) {
		w                    = (w + 1) / 2;
		h                    = (h + 1) / 2;
		SDL_Texture* texture = create_argb(renderer, SDL_TEXTUREACCESS_TARGET, w, h);
		if (texture == nullptr) {
			for (SDL_Texture* t : halving) {
				SDL_DestroyTexture(t);
			}
			halving.clear();
			return false;
		}
		halving.push_back(texture);
	}
	return true;
}

void World_presenter::clear_window() {
	if (renderer == nullptr) {
		return;
	}
	SDL_SetRenderTarget(renderer, nullptr);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
}

bool World_presenter::draw(World_filter_override force) {
	if (world_texture == nullptr) {
		return false;
	}
	// The letterbox rect in output pixels. It reads 0x0 while a texture is the
	// render target, so the caller must have set the window target.
	SDL_FRect lf{0, 0, 0, 0};
	if (!SDL_GetRenderLogicalPresentationRect(renderer, &lf)) {
		lf = SDL_FRect{0, 0, 0, 0};
	}
	last.l_w    = static_cast<int>(std::lround(lf.w));
	last.l_h    = static_cast<int>(std::lround(lf.h));
	last.filter = choose_world_filter(last.l_w, last.l_h, tex_w, tex_h, pixelart_ok, force);
	if (last.filter.filter == World_filter::Skip) {
		return false;
	}
	if (last.filter.filter == World_filter::Nearest) {
		SDL_SetTextureScaleMode(world_texture, SDL_SCALEMODE_NEAREST);
		return SDL_RenderTexture(renderer, world_texture, nullptr, nullptr);
	}
	SDL_Texture* src = world_texture;
	if (fmt == Format::Index8) {
		// Resolve 1:1 with NEAREST into an ARGB target, then filter that.
		if (!ensure_rgb_target()) {
			return false;
		}
		SDL_SetTextureScaleMode(world_texture, SDL_SCALEMODE_NEAREST);
		const bool ok = SDL_SetRenderTarget(renderer, world_rgb) && SDL_RenderTexture(renderer, world_texture, nullptr, nullptr);
		SDL_SetRenderTarget(renderer, nullptr);
		if (!ok) {
			return false;
		}
		src = world_rgb;
	}
	if (last.filter.filter == World_filter::Halving) {
		if (!ensure_halving_targets(last.filter.halvings)) {
			return false;
		}
		bool ok = true;
		for (SDL_Texture* target : halving) {
			// An exact 2:1 LINEAR pass.
			SDL_SetTextureScaleMode(src, SDL_SCALEMODE_LINEAR);
			ok  = ok && SDL_SetRenderTarget(renderer, target) && SDL_RenderTexture(renderer, src, nullptr, nullptr);
			src = target;
		}
		SDL_SetRenderTarget(renderer, nullptr);
		if (!ok) {
			return false;
		}
	}
	SDL_ScaleMode mode = SDL_SCALEMODE_LINEAR;
#if SDL_VERSION_ATLEAST(3, 4, 0)
	if (last.filter.filter == World_filter::Pixelart) {
		mode = SDL_SCALEMODE_PIXELART;
	}
#endif
	SDL_SetTextureScaleMode(src, mode);
	return SDL_RenderTexture(renderer, src, nullptr, nullptr);
}

World_presenter::Resets World_presenter::consume_resets() {
	Resets resets;
	resets.targets = reset_targets.exchange(false);
	resets.device  = reset_device.exchange(false);
	resets.lost    = device_lost.exchange(false);
	return resets;
}

bool World_presenter::event_watch(void* userdata, SDL_Event* event) {
	auto* self = static_cast<World_presenter*>(userdata);
	if (self == nullptr || event == nullptr) {
		return true;
	}
	switch (event->type) {
	case SDL_EVENT_RENDER_TARGETS_RESET:
	case SDL_EVENT_RENDER_DEVICE_RESET:
	case SDL_EVENT_RENDER_DEVICE_LOST:
		break;
	default:
		return true;
	}
	if (self->window_id != 0 && event->render.windowID != 0 && event->render.windowID != self->window_id) {
		return true;    // Another window's renderer.
	}
	if (event->type == SDL_EVENT_RENDER_TARGETS_RESET) {
		self->reset_targets = true;
	} else if (event->type == SDL_EVENT_RENDER_DEVICE_RESET) {
		self->reset_device = true;
	} else {
		self->device_lost = true;
	}
	// Keep the event in the queue for the other watchers and loops.
	return true;
}
