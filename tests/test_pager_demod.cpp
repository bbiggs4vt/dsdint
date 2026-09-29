// PagerFmDemodulator sanity: a synthetic 2-FSK signal (POCSAG-like, +/-4.5 kHz)
// placed off-center in IQ must come out as PCM at exactly 22050 Hz whose sign
// follows the bits and whose level is set by deviation, not by IQ rate.

#include "pager_demod.hpp"
#include <cstdio>

static int g_failures = 0;
#define CHECK_MSG(cond, ...) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: FAIL: %s -- ", __FILE__, __LINE__, #cond); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); ++g_failures; } } while (0)
#define CHECK(cond) CHECK_MSG(cond, "%s", "")

#include <cmath>
#include <random>
#include <vector>

using namespace dsdsrv;

namespace {
constexpr double kPi = 3.14159265358979323846;

struct Result {
    std::vector<int16_t> pcm;
    std::vector<int> bits;
};

// FSK at `baud`, deviation +/-dev_hz, sitting at +offset_hz in IQ sampled at fs.
Result run(double fs, double offset_hz, bool invert, double afc_error_hz = 0.0, bool afc = false,
           double seconds = 1.0) {
    const double baud = 1200.0, dev = 4500.0;
    std::mt19937 rng(1234);
    std::bernoulli_distribution coin(0.5);
    const std::size_t nsym = static_cast<std::size_t>(seconds * baud);
    Result r;
    for (std::size_t i = 0; i < nsym; ++i) r.bits.push_back(coin(rng) ? 1 : 0);

    const std::size_t n = static_cast<std::size_t>(seconds * fs);
    std::vector<cf32> iq(n);
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t sym = std::min(nsym - 1, static_cast<std::size_t>(i * baud / fs));
        const double f = offset_hz + afc_error_hz + (r.bits[sym] ? dev : -dev);
        phase += 2.0 * kPi * f / fs;
        iq[i] = cf32(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
    }

    PagerDemodConfig cfg;
    cfg.input_sample_rate_hz = fs;
    cfg.freq_offset_hz = offset_hz;
    cfg.invert = invert;
    cfg.afc_enabled = afc;
    PagerFmDemodulator d(cfg);
    // Odd, varying block sizes exercise the history/decimation-phase carry.
    std::size_t pos = 0, blk = 997;
    while (pos < n) {
        const std::size_t m = std::min(blk, n - pos);
        d.process(iq.data() + pos, m, r.pcm);
        pos += m;
        blk = blk == 997 ? 4099 : 997;
    }
    return r;
}

// Fraction of symbols (sampled mid-symbol, skipping filter delay slop) whose
// PCM sign matches the bit; also returns mean |PCM| at those points.
double bit_agreement(const Result& r, double& mean_abs, double delay_s) {
    const double baud = 1200.0, fs_out = 22050.0;
    std::size_t good = 0, total = 0;
    double acc = 0.0;
    for (std::size_t s = 4; s + 4 < r.bits.size(); ++s) {
        const double t = (s + 0.5) / baud + delay_s;
        const std::size_t idx = static_cast<std::size_t>(t * fs_out);
        if (idx >= r.pcm.size()) break;
        const int v = r.pcm[idx];
        good += ((v > 0) == (r.bits[s] == 1));
        acc += std::abs(v);
        ++total;
    }
    mean_abs = total ? acc / total : 0.0;
    return total ? static_cast<double>(good) / total : 0.0;
}

double best_agreement(const Result& r, double& mean_abs) {
    // The filter group delay depends on the rate; search a small window.
    double best = 0.0;
    for (double d = 0.0; d < 0.0015; d += 0.00005) {
        double m;
        double a = bit_agreement(r, m, d);
        if (a > best) { best = a; mean_abs = m; }
    }
    return best;
}
} // namespace

int main() {
    // 250 kHz IQ, channel at +30 kHz.
    {
        Result r = run(250'000.0, 30'000.0, false);
        CHECK_MSG(std::abs(static_cast<long>(r.pcm.size()) - 22050) < 60, "pcm samples: %zu", r.pcm.size());
        double m = 0.0;
        double a = best_agreement(r, m);
        CHECK_MSG(a > 0.99, "bit agreement %.3f", a);
        // 4.5 kHz / 5 kHz full-scale * 16384 = ~14746 at symbol centers.
        CHECK_MSG(m > 12000 && m < 17000, "mean |pcm| %.0f", m);
    }
    // Inversion flips polarity.
    {
        Result n = run(250'000.0, 30'000.0, false);
        Result r = run(250'000.0, 30'000.0, true);
        CHECK(n.pcm.size() == r.pcm.size());
        std::size_t mirrored = 0;
        for (std::size_t i = 0; i < std::min(n.pcm.size(), r.pcm.size()); ++i)
            mirrored += std::abs(n.pcm[i] + r.pcm[i]) <= 1;
        CHECK_MSG(mirrored == n.pcm.size(), "inverted PCM mirrors normal at %zu/%zu samples", mirrored,
                  n.pcm.size());
    }
    // Low-rate IQ (48 kHz, no decimation): same level -- scaling is in Hz.
    {
        Result r = run(48'000.0, 0.0, false);
        CHECK_MSG(std::abs(static_cast<long>(r.pcm.size()) - 22050) < 60, "pcm samples: %zu", r.pcm.size());
        double m = 0.0;
        double a = best_agreement(r, m);
        CHECK_MSG(a > 0.99, "48k bit agreement %.3f", a);
        CHECK_MSG(m > 12000 && m < 17000, "48k mean |pcm| %.0f", m);
    }
    // A 2 kHz tuning error: without AFC the DC offset biases the slicer; with
    // AFC it is pulled out and the level at symbol centers recovers.
    {
        Result r = run(250'000.0, 30'000.0, false, 2000.0, true, 3.0);
        // Check only the last second (after lock).
        Result tail;
        tail.bits.assign(r.bits.end() - 1200, r.bits.end());
        tail.pcm.assign(r.pcm.end() - 22050, r.pcm.end());
        double m = 0.0;
        double a = best_agreement(tail, m);
        CHECK_MSG(a > 0.99, "AFC tail agreement %.3f", a);
        // Mean over symbols ~ zero once locked.
        double mean = 0.0;
        for (int16_t v : tail.pcm) mean += v;
        mean /= static_cast<double>(tail.pcm.size());
        CHECK_MSG(std::abs(mean) < 1500, "residual DC after AFC %.0f counts", mean);
    }
    if (g_failures) { std::fprintf(stderr, "test_pager_demod: %d failure(s)\n", g_failures); return 1; }
    std::printf("test_pager_demod: all checks passed\n");
    return 0;
}
