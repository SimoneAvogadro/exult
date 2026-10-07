// C entry point for xBRZ 1.9 so that Python can call it through ctypes (tools/hires/u7hires/scalers.py).
// Built by build_xbrz.sh into libxbrz19.so together with the downloaded xbrz.cpp. Offline tool only:
// xBRZ is GPLv3, so this library is never linked into Exult.
#include "xbrz.h"

extern "C" {
// colfmt: 0 = RGB, 1 = ARGB, 2 = ARGB_UNBUFFERED
void xbrz_scale(int factor, const uint32_t* src, uint32_t* trg, int w, int h, int colfmt,
                double eqTol, double centerBias, double domThr, double steepThr) {
	xbrz::ScalerCfg cfg;
	cfg.equalColorTolerance        = eqTol;
	cfg.centerDirectionBias        = centerBias;
	cfg.dominantDirectionThreshold = domThr;
	cfg.steepDirectionThreshold    = steepThr;
	const xbrz::ColorFormat f = colfmt == 0 ? xbrz::ColorFormat::rgb
	                            : colfmt == 1 ? xbrz::ColorFormat::argb
	                                          : xbrz::ColorFormat::argbUnbuffered;
	xbrz::scale(static_cast<size_t>(factor), src, trg, w, h, f, cfg);
}
}
