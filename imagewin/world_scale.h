/*
 *  world_scale.h - Hi-res render scale: the scale policy, the present filter
 *  ladder and the tracker-to-texture conversion (DESIGN.md sections 3.2.2 and
 *  3.2.4). Header-only and free of SDL and engine types, so that the unit
 *  tests check it without a window.
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

#ifndef WORLD_SCALE_H
#define WORLD_SCALE_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

/*
 *  Scale policy (config/video/hires/render_scale, --render-scale).
 */
enum class World_policy {
	Off,     // S = 1: the upstream pipeline.
	Art,     // The largest divisor of S_art within the caps.
	Auto,    // The smallest divisor of S_art that covers the output, capped.
	Force    // N (2..8); only the texture limit applies.
};

struct World_scale_request {
	World_policy policy  = World_policy::Off;
	int          force_n = 0;    // Force only.
};

// Parses "off" (or "1"), "art", "auto" or "force:N" (N in 2..8), ignoring
// case and surrounding blanks. Returns false (and leaves req unchanged) for
// anything else.
inline bool parse_world_policy(const std::string& text, World_scale_request& req) {
	std::string s;
	for (const char c : text) {
		if (!std::isspace(static_cast<unsigned char>(c))) {
			s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}
	if (s == "off" || s == "1") {
		req = {World_policy::Off, 0};
		return true;
	}
	if (s == "art") {
		req = {World_policy::Art, 0};
		return true;
	}
	if (s == "auto") {
		req = {World_policy::Auto, 0};
		return true;
	}
	if (s.compare(0, 6, "force:") == 0 && s.size() == 7 && s[6] >= '2' && s[6] <= '8') {
		req = {World_policy::Force, s[6] - '0'};
		return true;
	}
	return false;
}

inline const char* world_policy_name(World_policy policy) {
	switch (policy) {
	case World_policy::Off:
		return "off";
	case World_policy::Art:
		return "art";
	case World_policy::Auto:
		return "auto";
	case World_policy::Force:
		return "force";
	}
	return "?";
}

struct World_scale_in {
	World_policy policy        = World_policy::Off;
	int          force_n       = 0;
	int          full_w        = 0;    // The main buffer in game px (the full area).
	int          full_h        = 0;
	double       aspect_y      = 1.0;    // 1.2 for the aspect-correct fill modes.
	int          out_w         = 0;      // Letterbox rect in output pixels (Auto only).
	int          out_h         = 0;
	int          s_art         = 6;
	int          max_tex       = 0;     // 0: 16384.
	double       max_world_mpx = 40;    // Pixel budget for full_w * full_h * S^2.
};

constexpr int world_default_max_texture = 16384;

namespace World_scale_detail {
	inline bool fits_texture(const World_scale_in& in, int d) {
		const int64_t max_tex = in.max_tex > 0 ? in.max_tex : world_default_max_texture;
		return int64_t(in.full_w) * d <= max_tex && int64_t(in.full_h) * d <= max_tex;
	}

	inline bool fits_budget(const World_scale_in& in, int d) {
		const double px = double(in.full_w) * double(in.full_h) * d * d;
		return px <= in.max_world_mpx * 1e6;
	}

	// The largest divisor of s_art that is <= limit and passes both caps.
	inline int largest_divisor_within(const World_scale_in& in, int s_art, int limit) {
		for (int d = std::min(s_art, limit); d > 1; d--) {
			if (s_art % d == 0 && fits_texture(in, d) && fits_budget(in, d)) {
				return d;
			}
		}
		return 1;
	}
}    // namespace World_scale_detail

// The effective world scale S for a policy. S never steps through a value
// that is not a divisor of S_art (6 -> 3 -> 2 -> 1), except under Force.
inline int compute_world_scale(const World_scale_in& in) {
	using namespace World_scale_detail;
	if (in.full_w <= 0 || in.full_h <= 0) {
		return 1;
	}
	const int s_art = std::clamp(in.s_art, 1, 8);
	switch (in.policy) {
	case World_policy::Off:
		return 1;
	case World_policy::Art:
		return largest_divisor_within(in, s_art, s_art);
	case World_policy::Auto: {
		const double aspect = in.aspect_y > 0 ? in.aspect_y : 1.0;
		const double p      = std::max(double(in.out_w) / in.full_w, double(in.out_h) / (in.full_h * aspect));
		// Snap ceil(p) up to a divisor of S_art (S_art itself above it).
		int need = std::max(1, static_cast<int>(p - 1e-9) + 1);
		if (p <= 1) {
			need = 1;
		}
		int snapped = s_art;
		for (int d = need; d <= s_art; d++) {
			if (s_art % d == 0) {
				snapped = d;
				break;
			}
		}
		return largest_divisor_within(in, s_art, snapped);
	}
	case World_policy::Force: {
		for (int n = std::clamp(in.force_n, 1, 8); n > 1; n--) {
			if (fits_texture(in, n)) {
				return n;
			}
		}
		return 1;
	}
	}
	return 1;
}

/*
 *  Present filter ladder (section 3.2.4), chosen for every frame from the
 *  letterbox rect L in output pixels and the world texture size.
 */
enum class World_filter {
	Skip,        // L is empty: draw no world this frame.
	Nearest,     // Equal integer ratios: exact.
	Pixelart,    // Fractional upscale on a shader renderer (SDL >= 3.4).
	Linear,      // Fractional upscale elsewhere, and 0.5 <= r < 1.
	Halving      // r < 0.5: exact 2:1 LINEAR passes, then LINEAR.
};

// present_filter: Auto follows the ladder, the others force one direct pass.
enum class World_filter_override {
	Auto,
	Nearest,
	Linear,
	Pixelart
};

inline bool parse_world_filter_override(const std::string& text, World_filter_override& out) {
	std::string s;
	for (const char c : text) {
		if (!std::isspace(static_cast<unsigned char>(c))) {
			s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}
	if (s == "auto") {
		out = World_filter_override::Auto;
	} else if (s == "nearest") {
		out = World_filter_override::Nearest;
	} else if (s == "linear") {
		out = World_filter_override::Linear;
	} else if (s == "pixelart") {
		out = World_filter_override::Pixelart;
	} else {
		return false;
	}
	return true;
}

inline const char* world_filter_name(World_filter filter) {
	switch (filter) {
	case World_filter::Skip:
		return "skip";
	case World_filter::Nearest:
		return "nearest";
	case World_filter::Pixelart:
		return "pixelart";
	case World_filter::Linear:
		return "linear";
	case World_filter::Halving:
		return "halving";
	}
	return "?";
}

struct World_filter_choice {
	World_filter filter   = World_filter::Skip;
	int          halvings = 0;    // Halving only: 1..world_max_halvings.
};

constexpr int world_max_halvings = 6;

inline World_filter_choice choose_world_filter(
		int l_w, int l_h, int tex_w, int tex_h, bool pixelart_ok, World_filter_override force = World_filter_override::Auto) {
	if (l_w < 1 || l_h < 1 || tex_w < 1 || tex_h < 1) {
		return {World_filter::Skip, 0};
	}
	switch (force) {
	case World_filter_override::Nearest:
		return {World_filter::Nearest, 0};
	case World_filter_override::Linear:
		return {World_filter::Linear, 0};
	case World_filter_override::Pixelart:
		return {pixelart_ok ? World_filter::Pixelart : World_filter::Linear, 0};
	case World_filter_override::Auto:
		break;
	}
	// Exact integer ratio, equal on both axes.
	if (l_w % tex_w == 0 && l_h % tex_h == 0 && l_w / tex_w == l_h / tex_h) {
		return {World_filter::Nearest, 0};
	}
	const double r = std::min(double(l_w) / tex_w, double(l_h) / tex_h);
	if (r >= 1) {
		return {pixelart_ok ? World_filter::Pixelart : World_filter::Linear, 0};
	}
	if (r >= 0.5) {
		return {World_filter::Linear, 0};
	}
	int    k  = 0;
	double rk = r;
	while (rk < 0.5 && k < world_max_halvings) {
		rk *= 2;
		k++;
	}
	return {World_filter::Halving, k};
}

/*
 *  Tracker to texture: the write tracker holds logical rects relative to the
 *  logical origin (often negative); the world texture starts at logical
 *  (-off_x, -off_y). Returns the physical rect in texture space, clamped to
 *  [0, full_w * S) x [0, full_h * S); empty (all zero) if nothing is left.
 *  The source pointer for it is
 *  pixels + (guard + y) * pitch + guard + x.
 */
struct World_phys_rect {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;

	bool empty() const {
		return w <= 0 || h <= 0;
	}
};

inline World_phys_rect tracked_to_phys(int rx, int ry, int rw, int rh, int off_x, int off_y, int S, int full_w, int full_h) {
	if (rw <= 0 || rh <= 0 || S < 1) {
		return {};
	}
	int64_t x0 = (int64_t(rx) + off_x) * S;
	int64_t y0 = (int64_t(ry) + off_y) * S;
	int64_t x1 = x0 + int64_t(rw) * S;
	int64_t y1 = y0 + int64_t(rh) * S;
	x0         = std::max<int64_t>(x0, 0);
	y0         = std::max<int64_t>(y0, 0);
	x1         = std::min<int64_t>(x1, int64_t(full_w) * S);
	y1         = std::min<int64_t>(y1, int64_t(full_h) * S);
	if (x1 <= x0 || y1 <= y0) {
		return {};
	}
	return {int(x0), int(y0), int(x1 - x0), int(y1 - y0)};
}

// Clamps a physical rect to [0, tex_w) x [0, tex_h).
inline World_phys_rect clamp_phys(World_phys_rect r, int tex_w, int tex_h) {
	const int64_t x0 = std::max<int64_t>(r.x, 0);
	const int64_t y0 = std::max<int64_t>(r.y, 0);
	const int64_t x1 = std::min<int64_t>(int64_t(r.x) + r.w, tex_w);
	const int64_t y1 = std::min<int64_t>(int64_t(r.y) + r.h, tex_h);
	if (r.w <= 0 || r.h <= 0 || x1 <= x0 || y1 <= y0) {
		return {};
	}
	return {int(x0), int(y0), int(x1 - x0), int(y1 - y0)};
}

#endif
