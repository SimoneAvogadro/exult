/**
 ** Iwin8.h - 8-bit image window.
 **
 ** Written: 8/13/98 - JSF
 **/

/*
Copyright (C) 1998 Jeffrey S. Freedman
Copyright (C) 2000-2022 The Exult Team

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with this library; if not, write to the
Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA  02111-1307, USA.
*/

#ifndef INCL_IWIN8
#define INCL_IWIN8 1

#include "ibuf8.h"
#include "imagewin.h"

#include <array>
#include <memory>

template <class T>
class GammaTable;

/*
 *  Here's an 8-bit color-depth window (faster than the generic).
 */
class Image_window8 : public Image_window {
	unsigned char  colors[768];    // Palette.
	Image_buffer8* ib8;            // Cast to 8-bit buffer.

	static GammaTable<unsigned char> GammaRed;
	static GammaTable<unsigned char> GammaGreen;
	static GammaTable<unsigned char> GammaBlue;

public:
	Image_window8(
			unsigned int w, unsigned int h, unsigned int gw, unsigned int gh, int scl = 1, bool fs = false, int sclr = point,
			Image_window::FillMode fillmode = CentreAspectCorrect, unsigned int fillsclr = point);

	Image_buffer8* get_ib8() const {
		return ib8;
	}

	// Redirect drawing (shapes/text/fills) to the given buffer, e.g. an
	// overlay layer, and return the previous buffer so it can be restored.
	// Callers must also point Shape_frame::set_to_render at the same buffer;
	// use Game_window::push_render_target / pop_render_target which do both.
	Image_buffer8* set_render_buffer(Image_buffer8* buf) {
		Image_buffer8* prev = ib8;
		ib8                 = buf;
		ibuf                = buf;
		return prev;
	}

	Image_buffer8* get_render_buffer() const {
		return ib8;
	}

	// The window's own buffer, whatever target is pushed (--render-test).
	Image_buffer8* get_main_ib8() const {
		return static_cast<Image_buffer8*>(main_ibuf);
	}

	// Set palette.
	void set_palette(const unsigned char* rgbs, int maxval, int brightness = 100) override;

	// Gamma-correct a raw 768-byte RGB palette into 'out' the same way
	// set_palette builds the live 'colors' table, without disturbing it. Used
	// to build a fixed-palette override for an overlay layer.
	void apply_gamma_palette(const unsigned char* rgbs, int maxval, int brightness, std::array<unsigned char, 768>& out) const;

	// Get palette.
	virtual const unsigned char* get_palette() const {
		return colors;
	}

	// Rotate palette colors.
	void rotate_colors(int first, int num, int upd) override;

	/*
	 *  8-bit color methods:
	 */
	// Fill with given (8-bit) value.
	void fill8(unsigned char val) {
		ib8->Image_buffer8::fill8(val);
	}

	// Fill rect. wth pixel.
	void fill8(unsigned char val, int srcw, int srch, int destx, int desty) {
		ib8->Image_buffer8::fill8(val, srcw, srch, destx, desty);
	}

	// Fill line with pixel.
	void fill_hline8(unsigned char val, int srcw, int destx, int desty) {
		ib8->Image_buffer8::fill_hline8(val, srcw, destx, desty);
	}

	// Draw an arbitrary line from any point to any point inclusive
	// If xform is not null then the line is drawn using xform otherwise val is
	// used;
	void draw_line8(unsigned char val, int startx, int starty, int endx, int endy, const Xform_palette* xform = nullptr) {
		ib8->draw_line8(val, startx, starty, endx, endy, xform);
	}

	// Copy rectangle into here.
	void copy8(const unsigned char* src_pixels, int srcw, int srch, int destx, int desty) {
		ib8->Image_buffer8::copy8(src_pixels, srcw, srch, destx, desty);
	}

	// Copy line to here.
	void copy_hline8(const unsigned char* src_pixels, int srcw, int destx, int desty) {
		ib8->Image_buffer8::copy_hline8(src_pixels, srcw, destx, desty);
	}

	// Copy with translucency table.
	void copy_hline_translucent8(
			const unsigned char* src_pixels, int srcw, int destx, int desty, int first_translucent, int last_translucent,
			const Xform_palette* xforms) {
		ib8->Image_buffer8::copy_hline_translucent8(src_pixels, srcw, destx, desty, first_translucent, last_translucent, xforms);
	}

	// Apply translucency to a line.
	void fill_hline_translucent8(unsigned char val, int srcw, int destx, int desty, const Xform_palette& xform) {
		ib8->Image_buffer8::fill_hline_translucent8(val, srcw, destx, desty, xform);
	}

	// Copy rect. with transp. color.
	void copy_transparent8(const unsigned char* src_pixels, int srcw, int srch, int destx, int desty) {
		ib8->Image_buffer8::copy_transparent8(src_pixels, srcw, srch, destx, desty);
	}

	// Get/put a single pixel.
	unsigned char get_pixel8(int x, int y) {
		return ib8->Image_buffer8::get_pixel8(x, y);
	}

	void put_pixel8(unsigned char pix, int x, int y) {
		ib8->Image_buffer8::put_pixel8(pix, x, y);
	}

	static void get_gamma(double& r, double& g, double& b);
	static void set_gamma(double r, double g, double b);

	std::unique_ptr<unsigned char[]> mini_screenshot();

protected:
	// Convert an overlay layer's 8-bit paletted pixels to its texture, using
	// the current palette; the transparent index becomes fully transparent.
	void refresh_layer(Layer& layer) override;

private:
	// ARGB for one palette index within a layer (transparent index -> 0,
	// translucent override -> its ARGB, otherwise the opaque palette colour).
	uint32 layer_argb_pixel(const Layer& layer, unsigned char p) const;
	// Pre-scale a layer with the current (member) game scaler by factor N and
	// upload it, preserving transparency/translucency. Returns false if the
	// scaler could not be applied.
	bool refresh_layer_scaled(Layer& layer, int factor);
};

#endif
