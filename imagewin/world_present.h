/*
 *  world_present.h - Hi-res render scale: the world presenter (DESIGN.md
 *  section 3.2.4). It owns the world texture of an Image_window whose world
 *  scale S is above 1, uploads the scaled 8-bit main buffer into it and
 *  draws it into the window with the filter ladder of world_scale.h. It
 *  depends on SDL only, so the data-free present test links it without the
 *  engine.
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

#ifndef WORLD_PRESENT_H
#define WORLD_PRESENT_H

#include "world_scale.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Surface;
union SDL_Event;

class World_presenter {
public:
	enum class Format {
		Argb,     // ARGB8888 STREAMING, converted through a palette LUT (SDL 3.2 API).
		Index8    // INDEX8 STREAMING sharing the draw surface's palette (SDL >= 3.4).
	};

	// Flags set by the event watch (any thread), taken by consume_resets().
	struct Resets {
		bool targets = false;    // SDL_EVENT_RENDER_TARGETS_RESET
		bool device  = false;    // SDL_EVENT_RENDER_DEVICE_RESET
		bool lost    = false;    // SDL_EVENT_RENDER_DEVICE_LOST
	};

	// What the last upload() and draw() did (logs and tests).
	struct Stats {
		size_t              upload_bytes = 0;        // Bytes written by the last upload().
		bool                full_convert = false;    // ARGB: the last upload converted the whole texture.
		bool                clamped      = false;    // The last upload rect reached outside the texture.
		World_filter_choice filter;                  // Chosen by the last draw().
		int                 l_w = 0;                 // Letterbox rect of the last draw(), output px.
		int                 l_h = 0;
	};

	World_presenter() = default;
	~World_presenter();
	World_presenter(const World_presenter&)            = delete;
	World_presenter& operator=(const World_presenter&) = delete;

	// Creates the world texture of pw x ph physical pixels on renderer for
	// the 8-bit surface draw8 (whose palette INDEX8 shares). Index8 falls
	// back to Argb when it is not compiled in (SDL < 3.4) or the renderer
	// refuses it. Registers the event watch. Returns false (with nothing
	// left allocated) on failure.
	bool create(SDL_Renderer* renderer, SDL_Surface* draw8, int pw, int ph, Format want);
	// Releases every texture and removes the event watch. Call it before the
	// renderer is destroyed (SDL_DestroyRenderer frees the textures).
	void destroy();
	// Device reset: the textures are lost. Creates them again (same sizes).
	bool recreate_textures();

	bool is_created() const {
		return world_texture != nullptr;
	}

	Format format() const {
		return fmt;
	}

	int texture_width() const {
		return tex_w;
	}

	int texture_height() const {
		return tex_h;
	}

	SDL_Renderer* get_renderer() const {
		return renderer;
	}

	// Copies the physical rect phys (texture space) of the draw surface into
	// the world texture; the texture's (0, 0) is the surface's
	// (guard, guard). phys is clamped to the texture first (stats().clamped
	// tells whether that was needed). ARGB: when the
	// palette's RGB changed since the last conversion, the whole texture is
	// converted instead. force_full uploads the whole texture.
	void upload(SDL_Surface* draw8, int guard, World_phys_rect phys, bool force_full = false);

	// Makes the window the render target and clears it to black: SDL leaves
	// the backbuffer undefined after a present, and the world covers only
	// the letterbox rect.
	void clear_window();

	// Draws the world texture into the logical presentation rect of the
	// window. The window must be the current render target. The filter is
	// chosen from the current letterbox rect. Returns false when nothing was
	// drawn (empty letterbox rect, or an SDL error).
	bool draw(World_filter_override force = World_filter_override::Auto);

	// Takes and clears the reset flags.
	Resets consume_resets();

	// What this build can present (exult --version).
	static std::string capabilities();

	// True if the renderer implements PIXELART in a shader (section 3.2.4).
	static bool renderer_pixelart_ok(SDL_Renderer* renderer);

	const Stats& stats() const {
		return last;
	}

	// For tests: the lazily created targets.
	size_t halving_targets() const {
		return halving.size();
	}

	bool has_rgb_target() const {
		return world_rgb != nullptr;
	}

private:
	SDL_Renderer*             renderer      = nullptr;
	SDL_Surface*              palette_owner = nullptr;    // The draw surface (INDEX8 palette).
	SDL_Texture*              world_texture = nullptr;
	SDL_Texture*              world_rgb     = nullptr;    // INDEX8 resolve target (lazy).
	std::vector<SDL_Texture*> halving;                    // Halving targets (lazy).
	int                       tex_w       = 0;
	int                       tex_h       = 0;
	Format                    fmt         = Format::Argb;
	Format                    wanted      = Format::Argb;
	bool                      pixelart_ok = false;
	bool                      watching    = false;
	uint32_t                  window_id   = 0;

	// ARGB: the LUT and the RGB it was built from.
	std::array<uint32_t, 256>      lut{};
	std::array<unsigned char, 768> lut_rgb{};
	bool                           lut_valid = false;

	std::atomic<bool> reset_targets{false};
	std::atomic<bool> reset_device{false};
	std::atomic<bool> device_lost{false};

	Stats last;

	bool        create_world_texture(Format want);
	bool        ensure_rgb_target();
	bool        ensure_halving_targets(int k);
	void        destroy_lazy_targets();
	bool        refresh_lut(SDL_Surface* draw8);
	static bool event_watch(void* userdata, SDL_Event* event);
};

#endif
