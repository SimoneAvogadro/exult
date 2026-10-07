// Gap #4: world rasterization baseline harness. NOT part of exult-hires.
// Enabled with EXULT_PAINT_BENCH=1; runs after Game_window::setup_game() and exits.
#include "bench_hooks.h"

#include "actors.h"
#include "chunks.h"
#include "gamemap.h"
#include "gamewin.h"
#include "ibuf8.h"
#include "iwin8.h"
#include "objs.h"
#include "shapeinf.h"
#include "ucmachine.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

Image_buffer8* Bench_make_hibuf(int fw, int fh, int offx, int offy, int S);
void           Bench_clear_caches();

namespace {
using clk = std::chrono::steady_clock;

double ms_since(clk::time_point t0) {
	return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

struct Stats {
	double mean = 0, med = 0, p90 = 0, mn = 0, mx = 0;
};

Stats stats(std::vector<double> v) {
	Stats s;
	if (v.empty()) {
		return s;
	}
	std::sort(v.begin(), v.end());
	double sum = 0;
	for (double d : v) {
		sum += d;
	}
	s.mean = sum / v.size();
	s.med  = v[v.size() / 2];
	s.p90  = v[(v.size() * 9) / 10];
	s.mn   = v.front();
	s.mx   = v.back();
	return s;
}

FILE* out = nullptr;

void emit(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void emit(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	if (out) {
		va_start(ap, fmt);
		vfprintf(out, fmt, ap);
		va_end(ap);
	}
	fflush(stdout);
}

template <typename F>
std::vector<double> time_n(int n, F&& f) {
	std::vector<double> v;
	v.reserve(n);
	for (int i = 0; i < n; i++) {
		auto t0 = clk::now();
		f();
		v.push_back(ms_since(t0));
	}
	return v;
}

void dump_ppm(const char* path, unsigned char* bits0, int lw, int w, int h, const unsigned char* pal) {
	FILE* f = fopen(path, "wb");
	if (!f) {
		return;
	}
	int maxv = 0;
	for (int i = 0; i < 768; i++) {
		maxv = std::max<int>(maxv, pal[i]);
	}
	const int mul = maxv <= 63 ? 4 : 1;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	std::vector<unsigned char> row(w * 3);
	for (int y = 0; y < h; y++) {
		const unsigned char* p = bits0 + size_t(y) * lw;
		for (int x = 0; x < w; x++) {
			row[x * 3 + 0] = static_cast<unsigned char>(std::min(255, pal[p[x] * 3 + 0] * mul));
			row[x * 3 + 1] = static_cast<unsigned char>(std::min(255, pal[p[x] * 3 + 1] * mul));
			row[x * 3 + 2] = static_cast<unsigned char>(std::min(255, pal[p[x] * 3 + 2] * mul));
		}
		fwrite(row.data(), 1, row.size(), f);
	}
	fclose(f);
}

struct Scene {
	std::string name;
	int         tx, ty, tz;
	int         xlu;    // 1 = add translucent stress (invisible avatar + translucent objects)
};

std::vector<Scene> parse_scenes(const char* spec) {
	// "name:tx,ty,tz[,xlu];name2:..."
	std::vector<Scene> v;
	std::string        s(spec);
	size_t             pos = 0;
	while (pos < s.size()) {
		size_t end = s.find(';', pos);
		if (end == std::string::npos) {
			end = s.size();
		}
		std::string item = s.substr(pos, end - pos);
		size_t      c    = item.find(':');
		if (c != std::string::npos) {
			Scene sc{item.substr(0, c), 0, 0, 0, 0};
			std::sscanf(item.c_str() + c + 1, "%d,%d,%d,%d", &sc.tx, &sc.ty, &sc.tz, &sc.xlu);
			v.push_back(sc);
		}
		pos = end + 1;
	}
	return v;
}

std::vector<Game_object_shared> spawned;

void add_translucent_stress(Game_window* gwin, const Tile_coord& c) {
	// Find translucent shapes and scatter some around the avatar.
	Shapes_vga_file& shapes = Shape_manager::get_instance()->get_shapes();
	std::vector<int> xl;
	for (int sh = 150; sh < shapes.get_num_shapes() && xl.size() < 64; sh++) {
		const Shape_info& info = shapes.get_info(sh);
		if (info.has_translucency()) {
			ShapeID      id(sh, 0);
			Shape_frame* fr = id.get_shape();
			if (fr && fr->is_rle() && fr->get_width() >= 8 && fr->get_width() <= 64 && fr->get_height() <= 64) {
				xl.push_back(sh);
			}
		}
	}
	emit("#   translucent shapes found: %zu (first: %d)\n", xl.size(), xl.empty() ? -1 : xl[0]);
	if (xl.empty()) {
		return;
	}
	Game_map* gmap = gwin->get_map();
	int       k    = 0;
	for (int dy = -10; dy <= 10; dy += 3) {
		for (int dx = -16; dx <= 16; dx += 4) {
			const int          sh  = xl[k++ % xl.size()];
			const Shape_info&  inf = shapes.get_info(sh);
			Game_object_shared obj = gmap->create_ireg_object(inf, sh, 0, 0, 0, 0);
			obj->move(c.tx + dx, c.ty + dy, c.tz);
			spawned.push_back(obj);
		}
	}
	// Make the avatar invisible -> paint_rle_transformed
	gwin->get_main_actor()->set_flag(Obj_flags::invisible);
}

void remove_translucent_stress(Game_window* gwin) {
	for (auto& o : spawned) {
		o->remove_this();
	}
	spawned.clear();
	gwin->get_main_actor()->clear_flag(Obj_flags::invisible);
}

}    // namespace

void Run_paint_bench() {
	Game_window*   gwin = Game_window::get_instance();
	Image_window8* win  = gwin->get_win();
	Image_buffer8* ib   = win->get_ib8();

	const char* outpath = std::getenv("EXULT_BENCH_OUT");
	out                 = outpath ? std::fopen(outpath, "a") : nullptr;
	const int   N       = std::getenv("EXULT_BENCH_N") ? std::atoi(std::getenv("EXULT_BENCH_N")) : 200;
	const char* tag     = std::getenv("EXULT_BENCH_TAG") ? std::getenv("EXULT_BENCH_TAG") : "run";
	const char* shotdir = std::getenv("EXULT_BENCH_SHOTS");
	const char* sspec   = std::getenv("EXULT_BENCH_SCENES");
	const char* svals   = std::getenv("EXULT_BENCH_S") ? std::getenv("EXULT_BENCH_S") : "2,3,4,6";
	const bool  hires   = !std::getenv("EXULT_BENCH_NOHIRES");

	// Game area geometry (incl. border bands).
	int cx, cy, cw, ch;
	ib->get_clip(cx, cy, cw, ch);
	ib->clear_clip();
	int fx, fy, fw, fh;
	ib->get_clip(fx, fy, fw, fh);    // fx = -offset_x, fw = full width
	ib->set_clip(cx, cy, cw, ch);
	const int gw = gwin->get_width();
	const int gh = gwin->get_height();
	emit("# tag=%s game=%dx%d full=%dx%d off=%d,%d N=%d\n", tag, gw, gh, fw, fh, -fx, -fy, N);

	std::vector<Scene> scenes;
	const Tile_coord   start = gwin->get_main_actor()->get_tile();
	scenes.push_back({"start", start.tx, start.ty, start.tz, 0});
	if (sspec) {
		auto more = parse_scenes(sspec);
		scenes.insert(scenes.end(), more.begin(), more.end());
	}
	g_bench.main = ib;

	for (const Scene& sc : scenes) {
		const Tile_coord t(sc.tx, sc.ty, sc.tz);
		gwin->teleport_party(t, true);
		if (sc.xlu) {
			add_translucent_stress(gwin, t);
		}
		// warm-up: loads chunks, builds flats cache
		for (int i = 0; i < 30; i++) {
			gwin->paint();
		}
		// one stats pass
		g_bench.reset_counters();
		g_phase.reset();
		gwin->paint();
		emit("SCENE %s %s tile=%d,%d,%d in_dungeon=%d chunks=%llu objs=%llu flatrle=%llu lights=%llu rle_calls=%llu remap=%llu "
			 "px_flat=%llu px_rle=%llu px_xlu=%llu px_fill=%llu xlu_calls=%llu copy8=%llu\n",
			 tag, sc.name.c_str(), sc.tx, sc.ty, sc.tz, gwin->is_in_dungeon(), (unsigned long long)g_phase.n_chunks,
			 (unsigned long long)g_phase.n_objs, (unsigned long long)g_phase.n_flat_objs, (unsigned long long)g_phase.n_lights,
			 (unsigned long long)g_bench.n_rle, (unsigned long long)g_bench.n_remap, (unsigned long long)g_bench.px_flat,
			 (unsigned long long)g_bench.px_rle, (unsigned long long)g_bench.px_xlu, (unsigned long long)g_bench.px_fill,
			 (unsigned long long)g_bench.n_xlu_calls, (unsigned long long)g_bench.n_copy8);
		if (shotdir) {
			char path[512];
			std::snprintf(path, sizeof(path), "%s/%s_%s_1x.ppm", shotdir, tag, sc.name.c_str());
			dump_ppm(path, ib->get_bits() + fy * int(ib->get_line_width()) + fx, ib->get_line_width(), fw, fh, win->get_palette());
		}

		// --- full paint at 1x ---
		g_phase.reset();
		auto v  = time_n(N, [&] {
            gwin->paint();
        });
		auto st = stats(v);
		const double n = g_phase.n_paints ? double(g_phase.n_paints) : 1.0;
		emit("PAINT %s %s S=1 mode=0 mean=%.4f med=%.4f p90=%.4f min=%.4f max=%.4f | map=%.4f flats=%.4f flatrle=%.4f objs=%.4f "
			 "black=%.4f eff=%.4f border=%.4f gumps=%.4f lights=%.4f\n",
			 tag, sc.name.c_str(), st.mean, st.med, st.p90, st.mn, st.mx, g_phase.t_paint_map / n, g_phase.t_flats / n,
			 g_phase.t_flat_rles / n, g_phase.t_objects / n, g_phase.t_blackness / n, g_phase.t_effects / n, g_phase.t_border / n,
			 g_phase.t_gumps / n, g_phase.t_lights / n);

		// --- paint_dirty path as the main loop uses it (set_all_dirty + paint_dirty) ---
		v  = time_n(N, [&] {
            gwin->set_all_dirty();
            gwin->paint_dirty();
        });
		st = stats(v);
		emit("DIRTYALL %s %s mean=%.4f med=%.4f p90=%.4f\n", tag, sc.name.c_str(), st.mean, st.med, st.p90);

		// --- partial paints: 1-tile column strip (view_right) and an NPC-sized rect ---
		v  = time_n(N, [&] {
            gwin->paint(gw - 8, 0, 8, gh);
        });
		st = stats(v);
		emit("STRIP %s %s mean=%.4f med=%.4f p90=%.4f\n", tag, sc.name.c_str(), st.mean, st.med, st.p90);
		v  = time_n(N, [&] {
            win->copy(8, 0, gw - 8, gh, 0, 0);
        });
		st = stats(v);
		emit("SHIFTCOPY %s %s mean=%.4f med=%.4f p90=%.4f\n", tag, sc.name.c_str(), st.mean, st.med, st.p90);
		v  = time_n(N, [&] {
            gwin->paint(gw / 2 - 24, gh / 2 - 40, 48, 64);
        });
		st = stats(v);
		emit("NPCRECT %s %s mean=%.4f med=%.4f p90=%.4f\n", tag, sc.name.c_str(), st.mean, st.med, st.p90);

		// --- lerped paint (smooth scrolling) ---
		gwin->lerp_reset();
		gwin->lerp_reset();
		const int stx = gwin->get_scrolltx(), sty = gwin->get_scrollty();
		gwin->set_scrolls(stx + 1, sty);
		gwin->lerp_reset();
		v  = time_n(N, [&] {
            gwin->paint_lerped(0x8000);
        });
		st = stats(v);
		emit("LERP %s %s mean=%.4f med=%.4f p90=%.4f\n", tag, sc.name.c_str(), st.mean, st.med, st.p90);
		gwin->set_scrolls(stx, sty);
		gwin->lerp_reset();
		gwin->lerp_reset();

		// --- show() (present) at the configured scaler, for reference ---
		v  = time_n(std::min(N, 100), [&] {
            gwin->set_painted();
            gwin->show(true);
        });
		st = stats(v);
		emit("SHOW %s %s mean=%.4f med=%.4f p90=%.4f\n", tag, sc.name.c_str(), st.mean, st.med, st.p90);

		if (hires) {
			// --- simulated hi-res world rendering ---
			std::vector<int> Svals;
			{
				std::string ss(svals);
				size_t      p = 0;
				while (p < ss.size()) {
					size_t e = ss.find(',', p);
					if (e == std::string::npos) {
						e = ss.size();
					}
					Svals.push_back(std::atoi(ss.substr(p, e - p).c_str()));
					p = e + 1;
				}
			}
			for (int S : Svals) {
				Image_buffer8* hb = Bench_make_hibuf(fw, fh, -fx, -fy, S);
				g_bench.hi        = hb;
				g_bench.S         = S;
				for (int mode = 1; mode <= 2; mode++) {
					g_bench.mode = mode;
					Bench_clear_caches();
					auto   t0   = clk::now();
					gwin->paint();    // cold (mode 2 builds caches)
					double cold = ms_since(t0);
					for (int i = 0; i < 3; i++) {
						gwin->paint();
					}
					g_phase.reset();
					auto vh  = time_n(std::max(10, N / (S >= 4 ? 4 : 2)), [&] {
                        gwin->paint();
                    });
					auto sh  = stats(vh);
					double nn = g_phase.n_paints ? double(g_phase.n_paints) : 1.0;
					emit("PAINT %s %s S=%d mode=%d mean=%.4f med=%.4f p90=%.4f min=%.4f max=%.4f | map=%.4f flats=%.4f flatrle=%.4f "
						 "objs=%.4f black=%.4f eff=%.4f border=%.4f gumps=%.4f lights=%.4f cold=%.3f cacheMB=%.2f entries=%llu\n",
						 tag, sc.name.c_str(), S, mode, sh.mean, sh.med, sh.p90, sh.mn, sh.mx, g_phase.t_paint_map / nn,
						 g_phase.t_flats / nn, g_phase.t_flat_rles / nn, g_phase.t_objects / nn, g_phase.t_blackness / nn,
						 g_phase.t_effects / nn, g_phase.t_border / nn, g_phase.t_gumps / nn, g_phase.t_lights / nn, cold,
						 g_bench.cache_bytes / 1048576.0, (unsigned long long)g_bench.cache_entries);
					if (shotdir && S == 6) {
						char path[512];
						std::snprintf(path, sizeof(path), "%s/%s_%s_S%d_m%d.ppm", shotdir, tag, sc.name.c_str(), S, mode);
						dump_ppm(path, hb->get_bits() + (fy * S) * int(hb->get_line_width()) + fx * S, hb->get_line_width(), fw * S,
								 fh * S, win->get_palette());
					}
				}
				g_bench.mode = 0;
				g_bench.hi   = nullptr;
				Bench_clear_caches();
				delete hb;
			}
		}
		if (sc.xlu) {
			remove_translucent_stress(gwin);
		}
	}
	emit("# done %s\n", tag);
	if (out) {
		std::fclose(out);
	}
	std::exit(0);
}
