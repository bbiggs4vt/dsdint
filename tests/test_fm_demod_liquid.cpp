// Standalone sanity test for FmDemodulatorLiquid, mirroring
// test_fm_demod.cpp: synthesize an FM-modulated tone in IQ, run it
// through the liquid-dsp demod chain, and check the recovered signal's
// magnitude lands in a plausible range. NOT compiled/run in the
// environment this was written in (no liquid-dsp available) — this is
// the first thing to build and run on your target platform, before
// wiring the liquid backend into the full server. See README.md.
#include "fm_demod_liquid.hpp"
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

using namespace dsdsrv;

int main() {
    const double fs_in = 2'000'000.0;
    const double fs_out = 48'000.0;
    const double tone_hz = 1000.0;
    const double deviation_hz = 2000.0; // matches assumed_deviation_hz in fm_demod_liquid.cpp
    const double duration_s = 0.05;

    FmDemodConfig cfg;
    cfg.input_sample_rate_hz = fs_in;
    cfg.output_sample_rate_hz = fs_out;
    cfg.channel_bandwidth_hz = 12500.0;
    cfg.disc_gain = 30000.0f; // freqdem output is ~normalized, so this should land near full scale at peak deviation

    FmDemodulatorLiquid demod(cfg);

    const std::size_t n_in = static_cast<std::size_t>(fs_in * duration_s);
    std::vector<cf32> iq(n_in);
    double phase = 0.0;
    for (std::size_t i = 0; i < n_in; ++i) {
        const double t = i / fs_in;
        const double inst_freq = deviation_hz * std::sin(2.0 * M_PI * tone_hz * t);
        phase += 2.0 * M_PI * inst_freq / fs_in;
        iq[i] = cf32{static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
    }

    std::vector<int16_t> out;
    const std::size_t chunk = 4096;
    for (std::size_t off = 0; off < iq.size(); off += chunk) {
        const std::size_t n = std::min(chunk, iq.size() - off);
        demod.process(iq.data() + off, n, out);
    }

    printf("input samples: %zu, output samples: %zu\n", n_in, out.size());

    if (out.empty()) {
        printf("FAIL: no output produced\n");
        return 1;
    }

    float peak = 0.0f;
    double sum_sq = 0.0;
    std::size_t clipped = 0;
    for (int16_t v : out) {
        float f = static_cast<float>(v);
        peak = std::max(peak, std::fabs(f));
        sum_sq += static_cast<double>(f) * f;
        if (v == 32767 || v == -32768) ++clipped;
    }
    double rms = std::sqrt(sum_sq / out.size());
    double clip_pct = 100.0 * static_cast<double>(clipped) / out.size();
    printf("peak=%.1f rms=%.1f clipped=%zu/%zu (%.1f%%) (int16 scale, disc_gain=%.0f)\n",
           peak, rms, clipped, out.size(), clip_pct, cfg.disc_gain);

    // A few clipped samples right at stream start (resampler settling
    // transient) is normal. Sustained clipping across a large fraction
    // of the signal would mean disc_gain is genuinely too hot for your
    // actual deviation -- that's the real failure mode this guards
    // against, not the exact peak value.
    if (clip_pct > 5.0) {
        printf("WARN: %.1f%% of samples clipped -- that's more than a brief "
               "startup transient. Try lowering disc_gain, or check whether "
               "assumed_deviation_hz in fm_demod_liquid.cpp's constructor "
               "actually matches your signal's real peak deviation.\n", clip_pct);
        return 2;
    }
    if (peak < cfg.disc_gain * 0.2f) {
        printf("WARN: peak output much lower than expected -- check the "
               "freqdem_demodulate_block call name/signature first (see the "
               "comment block at the top of fm_demod_liquid.cpp), then check "
               "assumed_deviation_hz vs the deviation_hz used in this test\n");
        return 2;
    }

    printf("OK: liquid-dsp demod pipeline runs end-to-end and output magnitude "
           "is in a plausible range\n");
    return 0;
}
