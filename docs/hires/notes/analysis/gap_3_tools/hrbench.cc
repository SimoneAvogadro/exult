// hrbench.cc - empirical memory/time costs of S x hi-res override frames.
// Simulates "AI-upscaled art re-quantised to the U7 palette" by bilinear RGB upscaling
// + per-pixel noise + nearest-palette quantisation, then measures:
//   * exact Exult encode_rle size (copied algorithm) and max runs per scanline,
//   * RLE paint (decode) time into a 1920x1200 8-bit buffer,
//   * indexed PNG size and libpng decode time,
//   * the same for a plain nearest-neighbour upscale.
// Build: g++ -O2 -std=c++17 hrbench.cc -I root/usr/include -L root/usr/lib/x86_64-linux-gnu -lpng16 -lz
#include <png.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;
static double ms(clk::time_point a, clk::time_point b) {
	return std::chrono::duration<double, std::milli>(b - a).count();
}
static std::vector<uint8_t> slurp(const char* p) {
	std::ifstream f(p, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}
static uint32_t rd4(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
static int      rd2s(const uint8_t* p) { return int16_t(p[0] | p[1] << 8); }
static int      rd2(const uint8_t* p) { return p[0] | p[1] << 8; }

// ---- Exult encode_rle (vgafile.cc:135-347), with a dynamic runs buffer ----
static int Skip_transparent(const uint8_t*& pixels, int x, int w) {
	while (x < w && *pixels == 255) { x++; pixels++; }
	return x;
}
static int Find_runs(std::vector<unsigned short>& runs, int& runcnt, const uint8_t* pixels, int x, int w) {
	runcnt = 0;
	runs[0] = runs[1] = 0;
	while (x < w && *pixels != 255) {
		int run = 0;
		while (x < w - 1 && pixels[0] == pixels[1]) { x++; pixels++; run++; }
		if (run) { run = ((run + 1) << 1) | 1; x++; pixels++; }
		else {
			do { x++; pixels++; run += 2; } while (x < w && *pixels != 255 && (x == w - 1 || pixels[0] != pixels[1]));
		}
		runs[runcnt++] = run;
	}
	runs[runcnt] = 0;
	return x;
}
static std::vector<uint8_t> encode_rle(const uint8_t* pixels, int w, int h, int xoff, int yoff, int& maxruns) {
	std::vector<uint8_t> buf(size_t(w) * h * 2 + 16 * h + 2);
	uint8_t* out = buf.data();
	std::vector<unsigned short> runs(w + 2);
	auto W2 = [&](int v) { *out++ = v & 0xff; *out++ = (v >> 8) & 0xff; };
	int newx;
	for (int y = 0; y < h; y++) {
		for (int x = 0; (x = Skip_transparent(pixels, x, w)) < w; x = newx) {
			int rc;
			newx = Find_runs(runs, rc, pixels, x, w);
			maxruns = std::max(maxruns, rc);
			if (!runs[1] && !(runs[0] & 1)) {
				int len = runs[0] >> 1;
				W2(runs[0]); W2(x - xoff); W2(y - yoff);
				out = std::copy_n(pixels, len, out); pixels += len;
				continue;
			}
			W2(((newx - x) << 1) | 1); W2(x - xoff); W2(y - yoff);
			for (int i = 0; runs[i]; i++) {
				int len = runs[i] >> 1;
				if (runs[i] & 1) {
					while (len) { int c = len > 127 ? 127 : len; *out++ = (c << 1) | 1; *out++ = *pixels; pixels += c; len -= c; }
				} else {
					while (len > 0) { int c = len > 127 ? 127 : len; *out++ = c << 1; out = std::copy_n(pixels, c, out); pixels += c; len -= c; }
				}
			}
		}
	}
	W2(0);
	buf.resize(out - buf.data());
	return buf;
}
// ---- Image_buffer8::paint_rle (ibuf8.cc:516+), unclipped fast path ----
static void paint_rle(uint8_t* bits, int line_width, int xoff, int yoff, const uint8_t* in) {
	int scanlen;
	while ((scanlen = rd2(in)) != 0) {
		in += 2;
		int enc = scanlen & 1; scanlen >>= 1;
		int sx = xoff + rd2s(in); in += 2;
		int sy = yoff + rd2s(in); in += 2;
		uint8_t* d = bits + sy * line_width + sx;
		if (!enc) { std::memcpy(d, in, scanlen); in += scanlen; continue; }
		while (scanlen) {
			uint8_t b = *in++; int rep = b & 1; b >>= 1;
			if (rep) { std::memset(d, *in++, b); } else { std::memcpy(d, in, b); in += b; }
			d += b; scanlen -= b;
		}
	}
}

struct Frame { int shape, frame, w, h, xl, ya; std::vector<uint8_t> px; };

static void decode_frame(const uint8_t* p, Frame& f) {
	int xr = rd2s(p), xl = rd2s(p + 2), ya = rd2s(p + 4), yb = rd2s(p + 6);
	f.w = xl + xr + 1; f.h = ya + yb + 1; f.xl = xl; f.ya = ya;
	f.px.assign(size_t(f.w) * f.h, 255);
	p += 8;
	int sl;
	while ((sl = rd2(p)) != 0) {
		p += 2;
		int enc = sl & 1, L = sl >> 1;
		int sx = rd2s(p) + xl, sy = rd2s(p + 2) + ya; p += 4;
		uint8_t* d = &f.px[size_t(sy) * f.w + sx];
		if (!enc) { std::memcpy(d, p, L); p += L; continue; }
		while (L > 0) {
			uint8_t b = *p++; int c = b >> 1;
			if (b & 1) { std::memset(d, *p++, c); } else { std::memcpy(d, p, c); p += c; }
			d += c; L -= c;
		}
	}
}

static uint8_t pal[256][3];
static uint8_t lut[64 * 64 * 64];
static float   noise_tab[65536];

int main(int argc, char** argv) {
	if (argc < 5) { std::fprintf(stderr, "usage: %s shapes.vga palettes.flx S sigma [maxframes] [pngframes]\n", argv[0]); return 1; }
	const int   S     = std::atoi(argv[3]);
	const float sigma = std::atof(argv[4]);
	const int   maxfr = argc > 5 ? std::atoi(argv[5]) : 1 << 30;
	const int   pngfr = argc > 6 ? std::atoi(argv[6]) : 400;    // PNG only for the N largest frames (slow).
	auto vga = slurp(argv[1]);
	auto pf  = slurp(argv[2]);
	{   // palette 0 from palettes.flx
		uint32_t off = rd4(&pf[0x80]);
		for (int i = 0; i < 256; i++) for (int c = 0; c < 3; c++) pal[i][c] = pf[off + 3 * i + c] & 63;
	}
	for (int r = 0; r < 64; r++) for (int g = 0; g < 64; g++) for (int b = 0; b < 64; b++) {
		int best = 0, bd = 1 << 30;
		for (int i = 0; i < 0xE0; i++) {
			int dr = r - pal[i][0], dg = g - pal[i][1], db = b - pal[i][2];
			int d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
			if (d < bd) { bd = d; best = i; }
		}
		lut[(r << 12) | (g << 6) | b] = best;
	}
	{ std::mt19937 rng(7); std::normal_distribution<float> nd(0, sigma); for (auto& v : noise_tab) v = nd(rng); }

	// Collect RLE frames.
	std::vector<Frame> frames;
	uint32_t nshapes = rd4(&vga[0x54]);
	for (uint32_t s = 0; s < nshapes; s++) {
		uint32_t off = rd4(&vga[0x80 + 8 * s]), ln = rd4(&vga[0x84 + 8 * s]);
		if (!off || !ln) continue;
		uint32_t dlen = rd4(&vga[off]);
		if (!(dlen == ln || (ln % 2 == 0 && dlen == ln - 1))) continue;    // flat
		uint32_t hdr = rd4(&vga[off + 4]);
		int nf = (hdr - 4) / 4;
		for (int f = 0; f < nf; f++) {
			Frame fr; fr.shape = s; fr.frame = f;
			decode_frame(&vga[off + rd4(&vga[off + 4 + 4 * f])], fr);
			frames.push_back(std::move(fr));
		}
	}
	std::sort(frames.begin(), frames.end(), [](const Frame& a, const Frame& b) { return a.w * a.h > b.w * b.h; });
	if (int(frames.size()) > maxfr) frames.resize(maxfr);

	std::vector<uint8_t> screen(size_t(1920 + 4000) * (1200 + 4000));
	const int            lw = 1920 + 4000;
	double tot_opq = 0, tot_bbox = 0, tot_rle_noisy = 0, tot_rle_nn = 0, tot_png_noisy = 0, tot_png_bytes_px = 0;
	double t_gen = 0, t_enc = 0, t_paint = 0, t_png_dec = 0, t_png_enc = 0, tot_png_frames_px = 0;
	int    maxruns_noisy = 0, maxruns_nn = 0, lines_over_200 = 0, frames_over_200 = 0;
	std::vector<std::string> bigrows;
	uint32_t hsh = 1;
	for (size_t fi = 0; fi < frames.size(); fi++) {
		const Frame& f = frames[fi];
		const int    W = f.w * S, H = f.h * S;
		std::vector<uint8_t> hi(size_t(W) * H, 255), nn(size_t(W) * H, 255);
		auto t0 = clk::now();
		long opq = 0;
		for (int y = 0; y < H; y++) {
			for (int x = 0; x < W; x++) {
				int lx = x / S, ly = y / S;
				uint8_t c0 = f.px[size_t(ly) * f.w + lx];
				if (c0 == 255) continue;
				opq++;
				nn[size_t(y) * W + x] = c0;
				if (c0 >= 0xE0) { hi[size_t(y) * W + x] = c0; continue; }    // keep cycling/translucent indices
				// bilinear over opaque neighbours (pixel centres)
				float fx = (x + 0.5f) / S - 0.5f, fy = (y + 0.5f) / S - 0.5f;
				int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
				float ax = fx - x0, ay = fy - y0, acc[3] = {0, 0, 0}, wsum = 0;
				for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
					int sx = x0 + i, sy = y0 + j;
					if (sx < 0 || sy < 0 || sx >= f.w || sy >= f.h) continue;
					uint8_t c = f.px[size_t(sy) * f.w + sx];
					if (c == 255 || c >= 0xE0) continue;
					float wgt = (i ? ax : 1 - ax) * (j ? ay : 1 - ay);
					for (int k = 0; k < 3; k++) acc[k] += wgt * pal[c][k];
					wsum += wgt;
				}
				int rgb[3];
				for (int k = 0; k < 3; k++) {
					hsh = hsh * 1664525u + 1013904223u;
					float v = (wsum > 0 ? acc[k] / wsum : pal[c0][k]) + noise_tab[hsh >> 16] / 4.0f;    // sigma in 8-bit units -> 6-bit
					rgb[k] = std::clamp(int(std::lround(v)), 0, 63);
				}
				hi[size_t(y) * W + x] = lut[(rgb[0] << 12) | (rgb[1] << 6) | rgb[2]];
			}
		}
		auto t1 = clk::now();
		int  mr_noisy = 0, mr_nn = 0;
		auto rle_noisy = encode_rle(hi.data(), W, H, f.xl * S, f.ya * S, mr_noisy);
		auto t2 = clk::now();
		auto rle_nn = encode_rle(nn.data(), W, H, f.xl * S, f.ya * S, mr_nn);
		auto t3 = clk::now();
		paint_rle(screen.data(), lw, f.xl * S + 10, f.ya * S + 10, rle_noisy.data());
		auto t4 = clk::now();
		t_gen += ms(t0, t1); t_enc += ms(t1, t2); t_paint += ms(t3, t4);
		maxruns_noisy = std::max(maxruns_noisy, mr_noisy); maxruns_nn = std::max(maxruns_nn, mr_nn);
		if (mr_noisy >= 200) frames_over_200++;
		tot_opq += opq; tot_bbox += double(W) * H; tot_rle_noisy += rle_noisy.size(); tot_rle_nn += rle_nn.size();
		double png_dec_ms = -1, png_size = -1;
		if (int(fi) < pngfr) {
			png_image img; std::memset(&img, 0, sizeof img);
			img.version = PNG_IMAGE_VERSION; img.width = W; img.height = H;
			img.format = PNG_FORMAT_RGBA_COLORMAP; img.colormap_entries = 256;
			uint8_t cmap[256 * 4];
			for (int i = 0; i < 256; i++) { cmap[4 * i] = pal[i][0] << 2; cmap[4 * i + 1] = pal[i][1] << 2; cmap[4 * i + 2] = pal[i][2] << 2; cmap[4 * i + 3] = i == 255 ? 0 : 255; }
			png_alloc_size_t sz = 0;
			auto te0 = clk::now();
			png_image_write_to_memory(&img, nullptr, &sz, 0, hi.data(), W, cmap);
			std::vector<uint8_t> mem(sz);
			if (!png_image_write_to_memory(&img, mem.data(), &sz, 0, hi.data(), W, cmap)) { std::fprintf(stderr, "png write failed %s\n", img.message); }
			auto te1 = clk::now();
			png_image_free(&img);
			png_image rd; std::memset(&rd, 0, sizeof rd); rd.version = PNG_IMAGE_VERSION;
			auto td0 = clk::now();
			png_image_begin_read_from_memory(&rd, mem.data(), sz);
			rd.format = PNG_FORMAT_RGBA_COLORMAP;
			std::vector<uint8_t> out(PNG_IMAGE_SIZE(rd)), ocmap(PNG_IMAGE_COLORMAP_SIZE(rd));
			png_image_finish_read(&rd, nullptr, out.data(), 0, ocmap.data());
			auto td1 = clk::now();
			png_image_free(&rd);
			png_dec_ms = ms(td0, td1); png_size = sz;
			t_png_dec += png_dec_ms; t_png_enc += ms(te0, te1); tot_png_noisy += sz; tot_png_frames_px += opq;
		}
		if (fi < 10) {
			char line[512];
			std::snprintf(line, sizeof line,
					"shape %4d fr %2d  %4dx%-4d -> %5dx%-5d opaque %8ld  rle_noisy %8.2f MB  rle_nn %6.2f MB  png %6.2f MB  encode %6.1f ms  paint %5.2f ms  png_dec %6.1f ms  maxruns %d",
					f.shape, f.frame, f.w, f.h, W, H, opq, rle_noisy.size() / 1e6, rle_nn.size() / 1e6, png_size / 1e6, ms(t1, t2), ms(t3, t4), png_dec_ms, mr_noisy);
			bigrows.push_back(line);
		}
	}
	std::printf("file=%s S=%d sigma=%.1f frames=%zu\n", argv[1], S, sigma, frames.size());
	for (auto& r : bigrows) std::printf("  %s\n", r.c_str());
	std::printf("totals: opaque_px=%.0f  bbox_px=%.0f  opaque*1B=%.1f MB  bbox*1B=%.1f MB\n", tot_opq, tot_bbox, tot_opq / 1e6, tot_bbox / 1e6);
	std::printf("        rle_noisy=%.1f MB (%.3f B/opaque px)  rle_nn=%.1f MB (%.3f B/opaque px)\n", tot_rle_noisy / 1e6, tot_rle_noisy / tot_opq, tot_rle_nn / 1e6, tot_rle_nn / tot_opq);
	std::printf("        png(%d largest)=%.1f MB for %.1f Mpx opaque (%.3f B/opaque px)\n", std::min<int>(pngfr, frames.size()), tot_png_noisy / 1e6, tot_png_frames_px / 1e6, tot_png_noisy / std::max(1.0, tot_png_frames_px));
	std::printf("        maxruns/scanline noisy=%d nn=%d  frames with >=200 runs in a scanline (overflow of runs[200]) = %d\n", maxruns_noisy, maxruns_nn, frames_over_200);
	std::printf("time:   gen=%.0f ms  encode_rle=%.0f ms (%.1f MB/s out)  paint_rle=%.0f ms (%.0f MB/s)  png_enc=%.0f ms  png_dec=%.0f ms (%.0f Mpx/s)\n",
			t_gen, t_enc, tot_rle_noisy / 1e3 / std::max(1.0, t_enc), t_paint, tot_rle_noisy / 1e3 / std::max(1.0, t_paint), t_png_enc, t_png_dec,
			tot_png_frames_px / 1e3 / std::max(1.0, t_png_dec));
	return 0;
}
