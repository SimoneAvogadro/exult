#include "xbrz.h"
extern "C" {
// colfmt: 0=RGB, 1=ARGB, 2=ARGB_UNBUFFERED
void xbrz_scale(int factor, const uint32_t* src, uint32_t* trg, int w, int h, int colfmt,
                double eqTol, double centerBias, double domThr, double steepThr) {
    xbrz::ScalerCfg cfg; cfg.equalColorTolerance = eqTol; cfg.centerDirectionBias = centerBias;
    cfg.dominantDirectionThreshold = domThr; cfg.steepDirectionThreshold = steepThr;
    xbrz::ColorFormat f = colfmt==0 ? xbrz::ColorFormat::rgb : (colfmt==1 ? xbrz::ColorFormat::argb : xbrz::ColorFormat::argbUnbuffered);
    xbrz::scale((size_t)factor, src, trg, w, h, f, cfg);
}
}
