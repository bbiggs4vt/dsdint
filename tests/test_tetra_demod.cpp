// test_tetra_demod.cpp
//
// Validates the π/4-DQPSK demodulator (src/tetra_demod.cpp) against a
// matching modulator carried here as ground truth: random bits -> π/4-DQPSK
// symbols -> RRC-shaped complex baseband IQ. Because the modulator uses the
// same TETRA phase mapping the demod decodes, a correct DSP chain
// round-trips the bits exactly. Cases:
//   1. Clean round trip, symbol-aligned  -> zero BER.
//   2. Arbitrary static carrier phase     -> zero BER (differential immunity).
//   3. Fractional-symbol timing offset     -> zero BER (Gardner pulls in).
//   4. AWGN                                 -> low BER at good SNR, clean at high SNR.
//
// Pure DSP, no external deps (links only libm). See tetra_demod.hpp for
// what this modem does and does not (yet) cover.

#include "tetra_demod.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace dsdsrv;
using cf = std::complex<float>;

namespace {
int g_failures = 0;
void check(bool c, const std::string& what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what.c_str());
    if (!c) ++g_failures;
}

constexpr float kPi = 3.14159265358979323846f;

// RRC taps identical in form to the demod's (transmit half of the pair).
std::vector<float> make_rrc(int sps, float alpha, int span) {
    const int half = span * sps;
    std::vector<float> h(2 * half + 1);
    const float a = alpha;
    for (int i = -half; i <= half; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(sps);
        float v;
        if (i == 0) {
            v = 1.0f + a * (4.0f / kPi - 1.0f);
        } else if (a > 0.0f && std::fabs(std::fabs(4.0f * a * x) - 1.0f) < 1e-4f) {
            v = (a / std::sqrt(2.0f)) *
                ((1.0f + 2.0f / kPi) * std::sin(kPi / (4.0f * a)) +
                 (1.0f - 2.0f / kPi) * std::cos(kPi / (4.0f * a)));
        } else {
            const float num = std::sin(kPi * x * (1.0f - a)) +
                              4.0f * a * x * std::cos(kPi * x * (1.0f + a));
            const float den = kPi * x * (1.0f - (4.0f * a * x) * (4.0f * a * x));
            v = num / den;
        }
        h[i + half] = v;
    }
    float energy = 0.0f;
    for (float t : h) energy += t * t;
    const float scale = 1.0f / std::sqrt(energy);
    for (float& t : h) t *= scale;
    return h;
}

// π/4-DQPSK modulate `bits` (2 per symbol) into IQ at `sps` samples/symbol.
// Prepends a reference symbol so the first data pair is recoverable by the
// differential detector (a real TETRA burst's training sequence serves the
// same role). TETRA mapping: sign bit B(2k-1) (0->+,1->-), magnitude bit
// B(2k) (0->π/4, 1->3π/4).
std::vector<cf> modulate(const std::vector<unsigned char>& bits, int sps,
                         float alpha, int span) {
    std::vector<cf> syms;
    syms.reserve(bits.size() / 2 + 1);
    float phase = 0.0f;
    syms.push_back(std::polar(1.0f, phase)); // reference symbol
    for (std::size_t i = 0; i + 1 < bits.size(); i += 2) {
        const float sign = bits[i] ? -1.0f : 1.0f;
        const float mag = bits[i + 1] ? (3.0f * kPi / 4.0f) : (kPi / 4.0f);
        phase += sign * mag;
        syms.push_back(std::polar(1.0f, phase));
    }
    // Upsample and pulse-shape with the transmit RRC.
    const std::vector<float> rrc = make_rrc(sps, alpha, span);
    std::vector<cf> up(syms.size() * sps, cf(0.0f, 0.0f));
    for (std::size_t k = 0; k < syms.size(); ++k) up[k * sps] = syms[k];
    const int ntaps = static_cast<int>(rrc.size());
    const int delay = ntaps / 2;
    std::vector<cf> iq(up.size());
    for (std::size_t i = 0; i < up.size(); ++i) {
        cf acc(0.0f, 0.0f);
        for (int t = 0; t < ntaps; ++t) {
            const long idx = static_cast<long>(i) + t - delay;
            if (idx >= 0 && idx < static_cast<long>(up.size()))
                acc += rrc[t] * up[idx];
        }
        iq[i] = acc;
    }
    return iq;
}

// Fractional-sample delay via cubic interpolation (to inject a timing phase
// the Gardner loop has to track). frac in [0,1).
std::vector<cf> fractional_delay(const std::vector<cf>& in, float frac) {
    std::vector<cf> out(in.size());
    auto at = [&](long i) -> cf { return (i >= 0 && i < (long)in.size()) ? in[i] : cf(0, 0); };
    for (long i = 0; i < (long)in.size(); ++i) {
        const cf x0 = at(i - 1), x1 = at(i), x2 = at(i + 1), x3 = at(i + 2);
        const cf c0 = x1;
        const cf c1 = 0.5f * (x2 - x0);
        const cf c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
        const cf c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
        out[i] = ((c3 * frac + c2) * frac + c1) * frac + c0;
    }
    return out;
}

// Best-alignment BER of demod bits against the source bits. The RRC group
// delay and the differential detector's warm-up introduce an unknown fixed
// bit offset that can point either way (the demod discards a couple of
// start symbols, so its output can even *lead* the source), so the search
// sweeps a symmetric shift window and returns the minimum bit-error rate.
double best_ber(const std::vector<unsigned char>& src, const std::vector<unsigned char>& demod,
                int max_shift = 200, std::size_t win = 4000) {
    const std::size_t base = static_cast<std::size_t>(max_shift) + 8;
    // Shrink the window if the sequences are short.
    const std::size_t cap = std::min(src.size(), demod.size());
    if (cap < base + static_cast<std::size_t>(max_shift) + 16) return 1.0;
    win = std::min(win, cap - base - static_cast<std::size_t>(max_shift) - 8);
    double best = 1.0;
    for (int s = -max_shift; s <= max_shift; ++s) {
        const std::size_t si = base;
        const std::size_t di = static_cast<std::size_t>(static_cast<long>(base) + s);
        std::size_t err = 0;
        for (std::size_t j = 0; j < win; ++j)
            if (src[si + j] != demod[di + j]) ++err;
        best = std::min(best, static_cast<double>(err) / win);
    }
    return best;
}

std::vector<unsigned char> random_bits(std::size_t n, std::mt19937& rng) {
    std::vector<unsigned char> b(n);
    std::uniform_int_distribution<int> d(0, 1);
    for (auto& x : b) x = static_cast<unsigned char>(d(rng));
    return b;
}
} // namespace

int main() {
    std::printf("test_tetra_demod: π/4-DQPSK modulator round-trip\n");

    const int sps = 4;
    const float alpha = 0.35f;
    const int span = 8;
    std::mt19937 rng(12345);

    const std::vector<unsigned char> bits = random_bits(16000, rng);
    const std::vector<cf> iq = modulate(bits, sps, alpha, span);

    TetraDemodConfig cfg;
    cfg.samples_per_symbol = sps;
    cfg.rrc_alpha = alpha;
    cfg.rrc_span_symbols = span;

    // 1. Clean round trip.
    {
        TetraDpqskDemod demod(cfg);
        auto out = demod.demodulate(iq.data(), iq.size());
        double ber = best_ber(bits, out);
        std::printf("  clean BER = %.5f (%zu symbols recovered)\n", ber, demod.last_symbols().size());
        check(ber == 0.0, "clean signal demodulates with zero bit errors");
    }

    // 2. Arbitrary static carrier phase — differential detection must be immune.
    {
        const float theta = 1.1f; // radians, arbitrary
        std::vector<cf> rot(iq.size());
        const cf r = std::polar(1.0f, theta);
        for (std::size_t i = 0; i < iq.size(); ++i) rot[i] = iq[i] * r;
        TetraDpqskDemod demod(cfg);
        auto out = demod.demodulate(rot.data(), rot.size());
        double ber = best_ber(bits, out);
        std::printf("  phase-rotated BER = %.5f\n", ber);
        check(ber == 0.0, "static carrier phase leaves BER at zero (differential immunity)");
    }

    // 3. Fractional-symbol timing offset — the Gardner loop must pull in.
    {
        std::vector<cf> shifted = fractional_delay(iq, 0.6f);
        TetraDpqskDemod demod(cfg);
        auto out = demod.demodulate(shifted.data(), shifted.size());
        double ber = best_ber(bits, out);
        std::printf("  timing-offset BER = %.5f\n", ber);
        check(ber == 0.0, "fractional timing offset is recovered (zero BER)");
    }

    // 4. AWGN. Add complex Gaussian noise at a chosen Eb/N0; assert a clean
    //    decode at high SNR and a low BER at a moderate SNR. Signal power is
    //    ~1 per sample after shaping; Eb = Es/2 with Es ≈ sps * (per-sample
    //    power) over a symbol. We size noise from a measured signal power to
    //    stay robust to the exact pulse-shaping gain.
    auto add_awgn = [&](const std::vector<cf>& in, double ebno_db, std::mt19937& r) {
        double sp = 0.0;
        for (const auto& s : in) sp += std::norm(s);
        sp /= in.size();                          // mean per-sample power
        const double es = sp * sps;               // energy per symbol
        const double eb = es / 2.0;               // 2 bits/symbol
        const double ebno = std::pow(10.0, ebno_db / 10.0);
        const double n0 = eb / ebno;
        const double sigma = std::sqrt(n0 / 2.0); // per I/Q component
        std::normal_distribution<double> g(0.0, sigma);
        std::vector<cf> out(in.size());
        for (std::size_t i = 0; i < in.size(); ++i)
            out[i] = in[i] + cf(static_cast<float>(g(r)), static_cast<float>(g(r)));
        return out;
    };
    auto ber_at = [&](double ebno_db, unsigned seed) {
        std::mt19937 nr(seed);
        auto noisy = add_awgn(iq, ebno_db, nr);
        TetraDpqskDemod demod(cfg);
        auto out = demod.demodulate(noisy.data(), noisy.size());
        double ber = best_ber(bits, out, 200, 12000);
        std::printf("  AWGN Eb/N0=%.0fdB BER = %.5f\n", ebno_db, ber);
        return ber;
    };
    // High SNR: a clean decode. Moderate SNR: a low but nonzero BER
    // consistent with π/4-DQPSK differential detection (≈1e-2 at 7 dB).
    // Low SNR: errors clearly rise yet stay far below the 0.5 of random
    // guessing — which also guards against the noise being scaled too weak.
    check(ber_at(15.0, 999) == 0.0, "high-SNR AWGN (15 dB) decodes cleanly");
    check(ber_at(7.0, 2024) < 3e-2, "moderate-SNR AWGN (7 dB) BER stays low (< 3e-2)");
    {
        double ber0 = ber_at(0.0, 4242);
        check(ber0 > 0.05 && ber0 < 0.30,
              "low-SNR AWGN (0 dB) degrades sensibly (0.05 < BER < 0.30)");
    }

    // 5. Carrier-frequency offset. Rotate the IQ by a fixed CFO and confirm
    //    the demod estimates it and corrects it back to zero BER, that it is
    //    genuinely needed (a large offset wrecks the bits with correction
    //    off), and that estimation holds across the ±Rs/8 acquisition range.
    const double fs = sps * 18000.0;
    auto apply_cfo = [&](const std::vector<cf>& in, double f_hz) {
        std::vector<cf> out(in.size());
        const double w = 2.0 * M_PI * f_hz / fs;
        for (std::size_t i = 0; i < in.size(); ++i)
            out[i] = in[i] * cf(static_cast<float>(std::cos(w * i)),
                                static_cast<float>(std::sin(w * i)));
        return out;
    };
    for (double f : {500.0, -1500.0, 2000.0}) {
        auto shifted = apply_cfo(iq, f);
        TetraDpqskDemod demod(cfg); // correct_cfo defaults on
        auto out = demod.demodulate(shifted.data(), shifted.size());
        double ber = best_ber(bits, out);
        std::printf("  CFO %+.0f Hz: est=%.0f Hz, BER=%.5f\n", f, demod.last_cfo_hz(), ber);
        check(ber == 0.0, std::string("CFO ") + std::to_string((int)f) + " Hz corrected to zero BER");
        check(std::fabs(demod.last_cfo_hz() - f) < 50.0,
              std::string("CFO ") + std::to_string((int)f) + " Hz estimated within 50 Hz");
    }
    {
        // A static CFO within ±Rs/8 doesn't by itself cross the slicer's
        // ±45° decision margin, so differential detection alone rides it out
        // noise-free. The correction earns its keep by reclaiming that
        // margin under noise: a large offset (1800 Hz -> 36° bias, leaving
        // only 9°) plus moderate AWGN is far worse uncorrected than
        // corrected.
        std::mt19937 nr(7777);
        auto noisy_cfo = add_awgn(apply_cfo(iq, 1800.0), 7.0, nr);

        TetraDemodConfig off = cfg; off.correct_cfo = false;
        TetraDpqskDemod dem_off(off);
        double ber_off = best_ber(bits, dem_off.demodulate(noisy_cfo.data(), noisy_cfo.size()), 200, 12000);

        TetraDpqskDemod dem_on(cfg); // correction on
        double ber_on = best_ber(bits, dem_on.demodulate(noisy_cfo.data(), noisy_cfo.size()), 200, 12000);

        std::printf("  CFO 1800 Hz + 7 dB AWGN: BER off=%.5f on=%.5f\n", ber_off, ber_on);
        check(ber_on < 3e-2, "CFO+noise: corrected BER stays low (< 3e-2)");
        check(ber_on < ber_off * 0.5, "CFO+noise: correction roughly halves the BER or better");
    }

    if (g_failures == 0) { std::printf("\nALL TETRA DEMOD TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
