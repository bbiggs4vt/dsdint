// test_tetrapol_demod.cpp
//
// Validates the TETRAPOL GMSK demodulator (src/tetrapol_demod.cpp) against a
// matching GMSK modulator carried here as ground truth: random bits -> NRZ ->
// Gaussian frequency pulse -> phase-accumulated IQ. A correct FM/quadrature
// demod + timing recovery round-trips the bits. Cases:
//   1. Clean round trip                    -> zero BER.
//   2. Arbitrary static carrier phase       -> zero BER (FM discr. is phase-blind).
//   3. Carrier-frequency offset             -> zero BER (DC removal cancels it).
//   4. Fractional-symbol timing offset       -> zero BER (Gardner pulls in).
//   5. AWGN                                  -> clean at high SNR, low BER moderate.
//   6. Streaming continuity                  -> chunked == one-shot.
//
// Pure DSP, links only libm. See tetrapol_demod.hpp for scope.

#include "tetrapol_demod.hpp"

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

// GMSK modulate `bits` at `sps` samples/symbol, Gaussian BT product `bt`.
// NRZ (±1) -> upsample (rect) -> Gaussian frequency pulse -> phase accumulate
// (modulation index h=0.5: each symbol adds ±π/2) -> unit-magnitude IQ.
std::vector<cf> modulate(const std::vector<unsigned char>& bits, int sps, float bt) {
    // Gaussian pulse (unit-gain), span 3 symbols.
    const int span = 3;
    const int half = span * sps;
    const float sigma = std::sqrt(std::log(2.0f)) / (2.0f * kPi * bt); // in symbols
    std::vector<float> g(2 * half + 1);
    float gs = 0.0f;
    for (int i = -half; i <= half; ++i) {
        const float t = static_cast<float>(i) / sps; // symbols
        g[i + half] = std::exp(-(t * t) / (2.0f * sigma * sigma));
        gs += g[i + half];
    }
    for (float& v : g) v /= gs; // unit DC gain

    // Upsample NRZ (rectangular hold).
    std::vector<float> nrz(bits.size() * sps);
    for (std::size_t k = 0; k < bits.size(); ++k) {
        const float a = bits[k] ? 1.0f : -1.0f;
        for (int j = 0; j < sps; ++j) nrz[k * sps + j] = a;
    }
    // Gaussian-filtered frequency pulse.
    const int N = static_cast<int>(nrz.size());
    std::vector<float> f(N, 0.0f);
    const int ng = static_cast<int>(g.size());
    for (int i = 0; i < N; ++i) {
        float acc = 0.0f;
        for (int t = 0; t < ng; ++t) {
            const int idx = i + t - half;
            acc += g[t] * (idx >= 0 && idx < N ? nrz[idx] : nrz[std::clamp(idx, 0, N - 1)]);
        }
        f[i] = acc;
    }
    // Accumulate phase (π/2 per symbol for a steady bit) and form IQ.
    std::vector<cf> iq(N);
    float phase = 0.0f;
    const float kphi = (kPi / 2.0f) / sps;
    for (int i = 0; i < N; ++i) {
        phase += kphi * f[i];
        iq[i] = std::polar(1.0f, phase);
    }
    return iq;
}

// Best-alignment BER over a symmetric shift window (the demod discards a couple
// of warm-up symbols, so its output can lead or lag the source).
double best_ber(const std::vector<unsigned char>& src, const std::vector<unsigned char>& dm,
                int ms = 100, std::size_t win = 4000) {
    const std::size_t base = static_cast<std::size_t>(ms) + 8;
    const std::size_t cap = std::min(src.size(), dm.size());
    if (cap < base + static_cast<std::size_t>(ms) + 16) return 1.0;
    win = std::min(win, cap - base - static_cast<std::size_t>(ms) - 8);
    double best = 1.0;
    for (int s = -ms; s <= ms; ++s) {
        std::size_t err = 0;
        for (std::size_t j = 0; j < win; ++j)
            if (src[base + j] != dm[static_cast<std::size_t>(static_cast<long>(base) + s) + j]) ++err;
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
    std::printf("test_tetrapol_demod: GMSK modulator round-trip\n");
    const int sps = 4; // 32 kHz at 8000 baud
    const float bt = 0.3f;
    std::mt19937 rng(9001);

    const std::vector<unsigned char> bits = random_bits(16000, rng);
    const std::vector<cf> iq = modulate(bits, sps, bt);

    TetrapolDemodConfig cfg;
    cfg.samples_per_symbol = sps;

    // 1. Clean round trip.
    {
        TetrapolGmskDemod d(cfg);
        auto out = d.demodulate(iq.data(), iq.size());
        { auto t = d.flush(); out.insert(out.end(), t.begin(), t.end()); }
        double ber = best_ber(bits, out);
        std::printf("  clean BER = %.5f (%zu symbols)\n", ber, out.size());
        check(ber == 0.0, "clean signal demodulates with zero bit errors");
    }

    // 2. Static carrier phase — FM discrimination is phase-blind.
    {
        const cf r = std::polar(1.0f, 2.0f);
        std::vector<cf> rot(iq.size());
        for (std::size_t i = 0; i < iq.size(); ++i) rot[i] = iq[i] * r;
        TetrapolGmskDemod d(cfg);
        auto out = d.demodulate(rot.data(), rot.size());
        { auto t = d.flush(); out.insert(out.end(), t.begin(), t.end()); }
        check(best_ber(bits, out) == 0.0, "static carrier phase leaves BER at zero");
    }

    // 3. Carrier-frequency offset — the DC removal must cancel the discriminator
    //    bias it introduces.
    {
        const double fs = sps * 8000.0;
        for (double fhz : {600.0, -1200.0}) {
            std::vector<cf> sh(iq.size());
            const double w = 2.0 * kPi * fhz / fs;
            for (std::size_t i = 0; i < iq.size(); ++i)
                sh[i] = iq[i] * cf(std::cos(w * i), std::sin(w * i));
            TetrapolGmskDemod d(cfg);
            auto out = d.demodulate(sh.data(), sh.size());
            { auto t = d.flush(); out.insert(out.end(), t.begin(), t.end()); }
            double ber = best_ber(bits, out);
            std::printf("  CFO %+.0f Hz: BER = %.5f\n", fhz, ber);
            check(ber == 0.0, std::string("CFO ") + std::to_string((int)fhz) + " Hz corrected to zero BER");
        }
    }

    // 4. Fractional-symbol timing offset — the Gardner loop must pull in.
    {
        // Resample at a fractional phase by cubic-interpolating the IQ.
        auto frac_delay = [&](const std::vector<cf>& in, float frac) {
            std::vector<cf> out(in.size());
            auto at = [&](long i) { return (i >= 0 && i < (long)in.size()) ? in[i] : cf(0, 0); };
            for (long i = 0; i < (long)in.size(); ++i) {
                const cf x0 = at(i - 1), x1 = at(i), x2 = at(i + 1), x3 = at(i + 2);
                const cf c1 = 0.5f * (x2 - x0);
                const cf c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
                const cf c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
                out[i] = ((c3 * frac + c2) * frac + c1) * frac + x1;
            }
            return out;
        };
        auto sh = frac_delay(iq, 0.5f);
        TetrapolGmskDemod d(cfg);
        auto out = d.demodulate(sh.data(), sh.size());
        { auto t = d.flush(); out.insert(out.end(), t.begin(), t.end()); }
        double ber = best_ber(bits, out);
        std::printf("  timing-offset BER = %.5f\n", ber);
        check(ber == 0.0, "fractional timing offset is recovered (zero BER)");
    }

    // 5. AWGN.
    auto add_awgn = [&](const std::vector<cf>& in, double ebno_db, unsigned seed) {
        double sp = 0.0; for (auto& s : in) sp += std::norm(s); sp /= in.size();
        const double eb = sp * sps;               // 1 bit/symbol, per-sample power ~1
        const double n0 = eb / std::pow(10.0, ebno_db / 10.0);
        const double sigma = std::sqrt(n0 / 2.0);
        std::mt19937 r(seed); std::normal_distribution<double> gg(0.0, sigma);
        std::vector<cf> out(in.size());
        for (std::size_t i = 0; i < in.size(); ++i)
            out[i] = in[i] + cf(static_cast<float>(gg(r)), static_cast<float>(gg(r)));
        return out;
    };
    auto ber_at = [&](double ebno_db, unsigned seed) {
        auto noisy = add_awgn(iq, ebno_db, seed);
        TetrapolGmskDemod d(cfg);
        auto out = d.demodulate(noisy.data(), noisy.size());
        { auto t = d.flush(); out.insert(out.end(), t.begin(), t.end()); }
        double ber = best_ber(bits, out, 100, 12000);
        std::printf("  AWGN Eb/N0=%.0fdB BER = %.5f\n", ebno_db, ber);
        return ber;
    };
    // Simple discriminator detection of BT=0.3 GMSK has an ISI/FM-detection
    // floor (the reference gmsk_demod does too; TETRAPOL's FEC handles the
    // residual), so these are realistic BERs, not the coherent bound.
    check(ber_at(16.0, 11) < 5e-3, "high-SNR AWGN (16 dB) BER is low (< 5e-3)");
    check(ber_at(10.0, 22) < 5e-2, "moderate-SNR AWGN (10 dB) BER stays low (< 5e-2)");
    {
        double b0 = ber_at(2.0, 33);
        check(b0 > 0.15 && b0 < 0.47, "low-SNR AWGN (2 dB) degrades sensibly (near floor, not pinned at 0.5)");
    }

    // 6. Streaming continuity: chunked == one-shot; a fresh demod per chunk
    //    loses lock (what the streaming state fixes).
    {
        TetrapolGmskDemod one(cfg);
        std::vector<unsigned char> ref = one.demodulate(iq.data(), iq.size());
        { auto t = one.flush(); ref.insert(ref.end(), t.begin(), t.end()); }

        TetrapolGmskDemod strm(cfg);
        std::vector<unsigned char> streamed;
        const std::size_t chunk = 511;
        for (std::size_t off = 0; off < iq.size(); off += chunk) {
            const std::size_t m = std::min(chunk, iq.size() - off);
            auto b = strm.demodulate(iq.data() + off, m);
            streamed.insert(streamed.end(), b.begin(), b.end());
        }
        { auto t = strm.flush(); streamed.insert(streamed.end(), t.begin(), t.end()); }
        std::printf("  streaming: one-shot %zu bits, chunked %zu bits\n", ref.size(), streamed.size());
        check(streamed == ref, "chunked streaming yields identical bits to one call");
        check(best_ber(bits, streamed) == 0.0, "streamed clean signal decodes with zero BER");

        strm.reset();
        auto after = strm.demodulate(iq.data(), iq.size());
        { auto t = strm.flush(); after.insert(after.end(), t.begin(), t.end()); }
        check(best_ber(bits, after) == 0.0, "reset() clears state for a fresh signal");
    }

    if (g_failures == 0) { std::printf("\nALL TETRAPOL DEMOD TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
