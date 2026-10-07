// Gap #4 measurement hooks (NOT part of exult-hires; lives only in tmp/gap4 copy).
#ifndef BENCH_HOOKS_H
#define BENCH_HOOKS_H
#include <chrono>
#include <cstdint>

class Image_buffer8;

struct BenchHooks {
	// Hi-res redirect of drawing primitives that target `main`.
	Image_buffer8* main = nullptr;    // game window ibuf
	Image_buffer8* hi   = nullptr;    // S x buffer
	int            S    = 1;
	int            mode = 0;    // 0 = off, 1 = NN expand on the fly, 2 = cached pre-upscaled RLE/flats
	// pixel / call counters for the 1x path (bytes written into `main`)
	uint64_t px_flat = 0, px_rle = 0, px_xlu = 0, px_fill = 0, px_other = 0;
	uint64_t n_rle = 0, n_copy8 = 0, n_xlu_calls = 0, n_fill = 0, n_remap = 0;
	// cache stats for mode 2
	uint64_t cache_bytes = 0, cache_entries = 0;
	void     reset_counters() {
        px_flat = px_rle = px_xlu = px_fill = px_other = 0;
        n_rle = n_copy8 = n_xlu_calls = n_fill = n_remap = 0;
	}
};
extern BenchHooks g_bench;

struct RendPhases {
	using clk = std::chrono::steady_clock;
	double   t_flats = 0, t_flat_rles = 0, t_objects = 0, t_blackness = 0, t_select = 0;
	double   t_paint_map = 0, t_effects = 0, t_border = 0, t_gumps = 0, t_lights = 0, t_total = 0;
	uint64_t n_chunks = 0, n_objs = 0, n_flat_objs = 0, n_lights = 0, n_paints = 0;
	void     reset() {
        *this = RendPhases();
	}
	static double ms(clk::time_point a, clk::time_point b) {
		return std::chrono::duration<double, std::milli>(b - a).count();
	}
};
extern RendPhases g_phase;

void Run_paint_bench();
#endif
