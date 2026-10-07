p='/home/simonea/ultima7_exult/tmp/gap4/exult-src/imagewin/ibuf8.cc'
s=open(p).read()
def rep(old,new,count=1):
    global s
    n=s.count(old)
    assert n==count, (old[:70], n)
    s=s.replace(old,new)

HELPERS = r'''
// ===================== gap4 bench hooks =====================
#include "bench_hooks.h"
#include <unordered_map>
#include <vector>
BenchHooks g_bench;

namespace {
struct HiBuf : public Image_buffer8 {
	unsigned char* base;
	HiBuf(int fw, int fh, int offx, int offy, int S, int M) : Image_buffer8(fw * S + 2 * M, fh * S + 2 * M) {
		base     = bits;
		width    = fw * S;
		height   = fh * S;
		offset_x = offx * S;
		offset_y = offy * S;
		bits     = base + M + M * line_width + offset_x + offset_y * line_width;
		clear_clip();
	}
	~HiBuf() {
		bits = base;
	}
	void sync_clip(int cx, int cy, int cw, int ch, int S) {
		clipx = cx * S;
		clipy = cy * S;
		clipw = cw * S;
		cliph = ch * S;
	}
	bool row_visible(int y0, int S) const {
		return !(y0 + S <= clipy || y0 >= clipy + cliph);
	}
	static unsigned char* expand(const unsigned char* src, int n, int S, const unsigned char* trans = nullptr) {
		static std::vector<unsigned char> line;
		if (line.size() < size_t(n * S)) {
			line.resize(n * S);
		}
		unsigned char* d = line.data();
		for (int i = 0; i < n; i++) {
			const unsigned char c = trans ? trans[src[i]] : src[i];
			for (int k = 0; k < S; k++) {
				*d++ = c;
			}
		}
		return line.data();
	}
	// ---- mode 1: nearest-neighbour expansion on the fly ----
	void nn_copy8(const unsigned char* src, int w, int h, int x, int y, int S) {
		for (int r = 0; r < h; r++) {
			const int y0 = (y + r) * S;
			if (!row_visible(y0, S)) {
				continue;
			}
			unsigned char* line = expand(src + r * w, w, S);
			for (int k = 0; k < S; k++) {
				Image_buffer8::copy_hline8(line, w * S, x * S, y0 + k);
			}
		}
	}
	void nn_paint_rle(int xoff, int yoff, const unsigned char* data, int S, const unsigned char* trans) {
		const uint8* in = data;
		int          scanlen;
		while ((scanlen = little_endian::Read2(in)) != 0) {
			const int encoded = scanlen & 1;
			scanlen           = scanlen >> 1;
			const int sx      = xoff + static_cast<sint16>(little_endian::Read2(in));
			const int sy      = yoff + static_cast<sint16>(little_endian::Read2(in));
			const int y0      = sy * S;
			const bool vis    = row_visible(y0, S);
			if (!encoded) {
				if (vis) {
					unsigned char* line = expand(in, scanlen, S, trans);
					for (int k = 0; k < S; k++) {
						Image_buffer8::copy_hline8(line, scanlen * S, sx * S, y0 + k);
					}
				}
				in += scanlen;
				continue;
			}
			for (int b = 0; b < scanlen;) {
				unsigned char bcnt   = *in++;
				const int     repeat = bcnt & 1;
				bcnt                 = bcnt >> 1;
				if (repeat) {
					const unsigned char col = trans ? trans[*in] : *in;
					in++;
					if (vis) {
						for (int k = 0; k < S; k++) {
							Image_buffer8::fill_hline8(col, bcnt * S, (sx + b) * S, y0 + k);
						}
					}
				} else {
					if (vis) {
						unsigned char* line = expand(in, bcnt, S, trans);
						for (int k = 0; k < S; k++) {
							Image_buffer8::copy_hline8(line, bcnt * S, (sx + b) * S, y0 + k);
						}
					}
					in += bcnt;
				}
				b += bcnt;
			}
		}
	}
};

// ---- mode 2: caches of pre-upscaled data ----
std::unordered_map<const unsigned char*, std::vector<unsigned char>> up_rle_cache;
std::unordered_map<const unsigned char*, std::vector<unsigned char>> up_flat_cache;

void put2(std::vector<unsigned char>& o, int v) {
	o.push_back(static_cast<unsigned char>(v & 0xff));
	o.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
}
void put_runs(std::vector<unsigned char>& o, bool repeat, int n, const unsigned char* px) {
	// n output pixels; repeat => single colour *px ; raw => px[0..n)
	int done = 0;
	while (done < n) {
		const int c = std::min(127, n - done);
		o.push_back(static_cast<unsigned char>((c << 1) | (repeat ? 1 : 0)));
		if (repeat) {
			o.push_back(*px);
		} else {
			o.insert(o.end(), px + done, px + done + c);
		}
		done += c;
	}
}
const std::vector<unsigned char>& get_up_rle(const unsigned char* data, int S) {
	auto it = up_rle_cache.find(data);
	if (it != up_rle_cache.end()) {
		return it->second;
	}
	std::vector<unsigned char> o;
	std::vector<unsigned char> tmp;
	const uint8*               in = data;
	int                        scanlen;
	while ((scanlen = little_endian::Read2(in)) != 0) {
		const int encoded = scanlen & 1;
		scanlen           = scanlen >> 1;
		const int sx      = static_cast<sint16>(little_endian::Read2(in));
		const int sy      = static_cast<sint16>(little_endian::Read2(in));
		const uint8* payload = in;
		// advance `in` past this scanline
		if (!encoded) {
			in += scanlen;
		} else {
			for (int b = 0; b < scanlen;) {
				unsigned char bcnt = *in++;
				const int     rep  = bcnt & 1;
				bcnt >>= 1;
				in += rep ? 1 : bcnt;
				b += bcnt;
			}
		}
		for (int k = 0; k < S; k++) {
			put2(o, ((scanlen * S) << 1) | encoded);
			put2(o, sx * S);
			put2(o, sy * S + k);
			if (!encoded) {
				for (int i = 0; i < scanlen; i++) {
					for (int j = 0; j < S; j++) {
						o.push_back(payload[i]);
					}
				}
			} else {
				const uint8* p = payload;
				for (int b = 0; b < scanlen;) {
					unsigned char bcnt = *p++;
					const int     rep  = bcnt & 1;
					bcnt >>= 1;
					if (rep) {
						put_runs(o, true, bcnt * S, p);
						p++;
					} else {
						tmp.clear();
						for (int i = 0; i < bcnt; i++) {
							for (int j = 0; j < S; j++) {
								tmp.push_back(p[i]);
							}
						}
						put_runs(o, false, bcnt * S, tmp.data());
						p += bcnt;
					}
					b += bcnt;
				}
			}
		}
	}
	put2(o, 0);
	g_bench.cache_bytes += o.size();
	g_bench.cache_entries++;
	return up_rle_cache.emplace(data, std::move(o)).first->second;
}
const std::vector<unsigned char>& get_up_flat(const unsigned char* src, int w, int h, int S) {
	auto it = up_flat_cache.find(src);
	if (it != up_flat_cache.end()) {
		return it->second;
	}
	std::vector<unsigned char> o(size_t(w) * S * h * S);
	for (int y = 0; y < h * S; y++) {
		for (int x = 0; x < w * S; x++) {
			o[size_t(y) * w * S + x] = src[(y / S) * w + x / S];
		}
	}
	g_bench.cache_bytes += o.size();
	g_bench.cache_entries++;
	return up_flat_cache.emplace(src, std::move(o)).first->second;
}
}    // namespace

Image_buffer8* Bench_make_hibuf(int fw, int fh, int offx, int offy, int S) {
	return new HiBuf(fw, fh, offx, offy, S, 16 * S);
}
void Bench_clear_caches() {
	up_rle_cache.clear();
	up_flat_cache.clear();
	g_bench.cache_bytes = g_bench.cache_entries = 0;
}

#define HI_REDIRECT (g_bench.mode != 0 && this == g_bench.main)
#define HI (static_cast<HiBuf*>(g_bench.hi))
#define HI_SYNC() HI->sync_clip(clipx, clipy, clipw, cliph, g_bench.S)
#define SS (g_bench.S)
// ============================================================
'''
rep('using std::endl;\n', 'using std::endl;\n' + HELPERS)

# --- prologues (insert after opening brace of each function) ---
def after(sig, code):
    global s
    n=s.count(sig)
    assert n==1,(sig,n)
    s=s.replace(sig, sig+code)

after('''void Image_buffer8::copy(
		int srcx, int srcy,     // Where to start.
		int srcw, int srch,     // Dimensions to copy.
		int destx, int desty    // Where to copy to.
) {
''','''	if (HI_REDIRECT) {
		HI->copy(srcx * SS, srcy * SS, srcw * SS, srch * SS, destx * SS, desty * SS);
		return;
	}
''')
after('''void Image_buffer8::fill8(unsigned char pix) {
''','''	if (HI_REDIRECT) {
		HI->fill8(pix);
		return;
	}
	if (this == g_bench.main) {
		g_bench.px_fill += uint64_t(line_width) * height;
		g_bench.n_fill++;
	}
''')
after('''void Image_buffer8::fill8(unsigned char pix, int srcw, int srch, int destx, int desty) {
''','''	if (HI_REDIRECT) {
		HI_SYNC();
		HI->fill8(pix, srcw * SS, srch * SS, destx * SS, desty * SS);
		return;
	}
''')
rep('''	unsigned char* pixels  = bits + desty * line_width + destx;
	const int      to_next = line_width - srcw;    // # pixels to next line.
	while (srch--) {                               // Do each line.
		for (int cnt = srcw; cnt; cnt--) {
			Write1(pixels, pix);
		}''','''	if (this == g_bench.main) {
		g_bench.px_fill += uint64_t(srcw) * srch;
		g_bench.n_fill++;
	}
	unsigned char* pixels  = bits + desty * line_width + destx;
	const int      to_next = line_width - srcw;    // # pixels to next line.
	while (srch--) {                               // Do each line.
		for (int cnt = srcw; cnt; cnt--) {
			Write1(pixels, pix);
		}''')
after('''void Image_buffer8::fill_hline8(unsigned char pix, int srcw, int destx, int desty) {
''','''	if (HI_REDIRECT) {
		HI_SYNC();
		for (int k = 0; k < SS; k++) {
			HI->fill_hline8(pix, srcw * SS, destx * SS, desty * SS + k);
		}
		return;
	}
''')
rep('''	unsigned char* pixels = bits + desty * line_width + destx;
	std::memset(pixels, pix, srcw);''','''	if (this == g_bench.main) {
		g_bench.px_fill += srcw;
	}
	unsigned char* pixels = bits + desty * line_width + destx;
	std::memset(pixels, pix, srcw);''')
after('''void Image_buffer8::copy8(
		const unsigned char* src_pixels,    // Source rectangle pixels.
		int srcw, int srch,                 // Dimensions of source.
		int destx, int desty) {
''','''	if (HI_REDIRECT && src_pixels) {
		HI_SYNC();
		if (g_bench.mode == 1) {
			HI->nn_copy8(src_pixels, srcw, srch, destx, desty, SS);
		} else {
			const auto& up = get_up_flat(src_pixels, srcw, srch, SS);
			HI->copy8(up.data(), srcw * SS, srch * SS, destx * SS, desty * SS);
		}
		return;
	}
''')
rep('''	uint8*       to   = bits + desty * line_width + destx;
	const uint8* from = src_pixels + srcy * src_width + srcx;
	while (srch--) {
		std::memcpy(to, from, srcw);''','''	if (this == g_bench.main) {
		g_bench.px_flat += uint64_t(srcw) * srch;
		g_bench.n_copy8++;
	}
	uint8*       to   = bits + desty * line_width + destx;
	const uint8* from = src_pixels + srcy * src_width + srcx;
	while (srch--) {
		std::memcpy(to, from, srcw);''')
after('''void Image_buffer8::copy_hline8(
		const unsigned char* src_pixels,    // Source rectangle pixels.
		int                  srcw,          // Width to copy.
		int destx, int desty) {
''','''	if (HI_REDIRECT) {
		HI_SYNC();
		unsigned char* line = HiBuf::expand(src_pixels, srcw, SS);
		for (int k = 0; k < SS; k++) {
			HI->copy_hline8(line, srcw * SS, destx * SS, desty * SS + k);
		}
		return;
	}
''')
after('''		const Xform_palette* xforms                // Transformers.  Need same # as
												   //   (last_translucent -
												   //    first_translucent + 1).
) {
''','''	if (HI_REDIRECT) {
		HI_SYNC();
		unsigned char* line = HiBuf::expand(src_pixels, srcw, SS);
		for (int k = 0; k < SS; k++) {
			HI->copy_hline_translucent8(line, srcw * SS, destx * SS, desty * SS + k, first_translucent, last_translucent, xforms);
		}
		return;
	}
	if (this == g_bench.main) {
		g_bench.n_xlu_calls++;
		g_bench.px_xlu += srcw;
	}
''')
after('''void Image_buffer8::fill_hline_translucent8(
		unsigned char val,    // Ignored for this method.
		int srcw, int destx, int desty,
		const Xform_palette& xform    // Transform table.
) {
''','''	if (HI_REDIRECT) {
		HI_SYNC();
		for (int k = 0; k < SS; k++) {
			HI->fill_hline_translucent8(val, srcw * SS, destx * SS, desty * SS + k, xform);
		}
		return;
	}
	if (this == g_bench.main) {
		g_bench.n_xlu_calls++;
		g_bench.px_xlu += srcw;
	}
''')
after('''void Image_buffer8::fill_translucent8(
		unsigned char /* val */,    // Not used.
		int srcw, int srch, int destx, int desty,
		const Xform_palette& xform    // Transform table.
) {
''','''	if (HI_REDIRECT) {
		HI_SYNC();
		HI->fill_translucent8(0, srcw * SS, srch * SS, destx * SS, desty * SS, xform);
		return;
	}
	if (this == g_bench.main) {
		g_bench.n_xlu_calls++;
		g_bench.px_xlu += uint64_t(srcw) * srch;
	}
''')
after('''void Image_buffer8::copy_transparent8(
		const unsigned char* src_pixels,    // Source rectangle pixels.
		int srcw, int srch,                 // Dimensions of source.
		int destx, int desty) {
''','''	if (HI_REDIRECT) {
		HI_SYNC();
		for (int r = 0; r < srch; r++) {
			unsigned char* line = HiBuf::expand(src_pixels + r * srcw, srcw, SS);
			for (int k = 0; k < SS; k++) {
				HI->copy_transparent8(line, srcw * SS, 1, destx * SS, (desty + r) * SS + k);
			}
		}
		return;
	}
''')
after('''void Image_buffer8::paint_rle(int xoff, int yoff, const unsigned char* inptr) {
''','''	if (HI_REDIRECT) {
		HI_SYNC();
		if (g_bench.mode == 1) {
			HI->nn_paint_rle(xoff, yoff, inptr, SS, nullptr);
		} else {
			HI->paint_rle(xoff * SS, yoff * SS, get_up_rle(inptr, SS).data());
		}
		return;
	}
	const bool cntme = (this == g_bench.main);
	if (cntme) {
		g_bench.n_rle++;
	}
''')
after('''void Image_buffer8::paint_rle_remapped(int xoff, int yoff, const unsigned char* inptr, const unsigned char*& trans) {
''','''	if (HI_REDIRECT) {
		HI_SYNC();
		if (g_bench.mode == 1) {
			HI->nn_paint_rle(xoff, yoff, inptr, SS, trans);
		} else {
			HI->paint_rle_remapped(xoff * SS, yoff * SS, get_up_rle(inptr, SS).data(), trans);
		}
		return;
	}
	const bool cntme = (this == g_bench.main);
	if (cntme) {
		g_bench.n_rle++;
		g_bench.n_remap++;
	}
''')
rep('''const unsigned char* end  = in + scanlen - skip;''','''const unsigned char* end  = in + scanlen - skip;
					if (cntme) {
						g_bench.px_rle += scanlen - skip;
					}''',2)
for pat in ['unsigned char* end = dest + bcnt - skip;', 'unsigned char*      end = dest + bcnt - skip;']:
    rep(pat, pat + '''
							if (cntme) {
								g_bench.px_rle += bcnt - skip;
							}''', 2)
open(p,'w').write(s)
print('ok')
