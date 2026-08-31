// test_tetra_frontend.cpp
//
// Validates TetraDemodFrontend (src/tetra_frontend.*): the coordinator that
// keeps the default differential demod and adds a coherent (Costas) path whose
// π/4 parity ambiguity is resolved by burst-grid lock (tetra_burst_sync).
//
// A burst generator plants the real TETRA training sequences on the 510-bit
// grid (as in test_tetra_burst_sync), modulates to π/4-DQPSK IQ, and feeds the
// frontend. Checks:
//   1. Differential mode reproduces the bare demod's bits exactly.
//   2. Coherent mode auto-resolves the parity (locks) and decodes to zero BER,
//      block and streamed (chunked).
//   3. Coherent mode still decodes under moderate AWGN.
//   4. A non-TETRA (random) input makes coherent give up and fall back to the
//      differential path rather than emitting a wrong-parity guess forever.

#include "tetra_frontend.hpp"
#include "tetra_demod.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
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

const uint8_t Y[38] = { 1,1,0,0,0,0,0,1,1,0,0,1,1,1,0,0,1,1,1,0,1,0,0,1,1,1,0,0,0,0,0,1,1,0,0,1,1,1 };
const uint8_t N[22] = { 1,1,0,1,0,0,0,0,1,1,1,0,1,0,0,1,1,1,0,1,0,0 };
constexpr int OFF_SYNC = 214, OFF_NORM = 244, TS = 510;

void place(std::vector<uint8_t>& s, std::size_t pos, const uint8_t* seq, int len) {
    for (int i = 0; i < len; ++i) s[pos + i] = seq[i];
}
std::vector<uint8_t> build_stream(int num_slots, std::mt19937& rng) {
    std::uniform_int_distribution<int> d(0, 1);
    std::vector<uint8_t> s(static_cast<std::size_t>(num_slots) * TS);
    for (auto& b : s) b = static_cast<uint8_t>(d(rng));
    for (int slot = 0; slot < num_slots; ++slot) {
        const std::size_t base = static_cast<std::size_t>(slot) * TS;
        if (slot == 0) place(s, base + OFF_SYNC, Y, 38);
        else           place(s, base + OFF_NORM, N, 22);
    }
    return s;
}

std::vector<float> make_rrc(int sps, float a, int span) {
    const int half = span * sps; std::vector<float> h(2 * half + 1);
    for (int i = -half; i <= half; ++i) {
        const float x = static_cast<float>(i) / sps; float v;
        if (i == 0) v = 1 + a * (4 / kPi - 1);
        else if (a > 0 && std::fabs(std::fabs(4 * a * x) - 1) < 1e-4f)
            v = (a / std::sqrt(2.0f)) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) +
                                         (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
        else { const float num = std::sin(kPi * x * (1 - a)) + 4 * a * x * std::cos(kPi * x * (1 + a));
               const float den = kPi * x * (1 - (4 * a * x) * (4 * a * x)); v = num / den; }
        h[i + half] = v;
    }
    float e = 0; for (float t : h) e += t * t;
    const float sc = 1.0f / std::sqrt(e); for (float& t : h) t *= sc; return h;
}
std::vector<cf> modulate(const std::vector<uint8_t>& bits, int sps, float a, int span) {
    std::vector<cf> syms; float ph = 0; syms.push_back(std::polar(1.0f, ph));
    for (std::size_t i = 0; i + 1 < bits.size(); i += 2) {
        const float sign = bits[i] ? -1.0f : 1.0f;
        const float mag = bits[i + 1] ? (3 * kPi / 4) : (kPi / 4);
        ph += sign * mag; syms.push_back(std::polar(1.0f, ph));
    }
    const std::vector<float> rrc = make_rrc(sps, a, span);
    std::vector<cf> up(syms.size() * sps, cf(0, 0));
    for (std::size_t k = 0; k < syms.size(); ++k) up[k * sps] = syms[k];
    const int nt = static_cast<int>(rrc.size()), dl = nt / 2;
    std::vector<cf> iq(up.size());
    for (std::size_t i = 0; i < up.size(); ++i) {
        cf ac(0, 0);
        for (int t = 0; t < nt; ++t) {
            const long idx = static_cast<long>(i) + t - dl;
            if (idx >= 0 && idx < static_cast<long>(up.size())) ac += rrc[t] * up[idx];
        }
        iq[i] = ac;
    }
    return iq;
}

double best_ber(const std::vector<unsigned char>& src, const std::vector<unsigned char>& dm,
                int ms = 400, std::size_t win = 8000) {
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
} // namespace

int main() {
    std::printf("test_tetra_frontend: coherent auto-parity via burst-grid lock\n");
    const int sps = 4; const float alpha = 0.35f; const int span = 8;
    std::mt19937 rng(2024);

    const std::vector<uint8_t> stream = build_stream(48, rng); // 48 bursts
    std::vector<unsigned char> src(stream.begin(), stream.end());
    const std::vector<cf> iq = modulate(stream, sps, alpha, span);

    TetraFrontendConfig fc;
    fc.demod.samples_per_symbol = sps;
    fc.demod.rrc_alpha = alpha;
    fc.demod.rrc_span_symbols = span;

    // 1. Differential mode == bare differential demod, and it locks the grid.
    {
        TetraFrontendConfig d = fc; d.coherent = false;
        TetraDemodFrontend fe(d);
        auto fb = fe.demodulate(iq.data(), iq.size());
        { auto t = fe.flush(); fb.insert(fb.end(), t.begin(), t.end()); }

        TetraDemodConfig bc = fc.demod; // coherent stays false
        TetraDpqskDemod bare(bc);
        auto bb = bare.demodulate(iq.data(), iq.size());
        { auto t = bare.flush(); bb.insert(bb.end(), t.begin(), t.end()); }

        check(fb == bb, "differential mode reproduces the bare demod bits exactly");
        check(!fe.coherent_locked(), "differential mode never enters coherent lock");
        TetraBurstSync sync;
        check(sync.synchronize(fb.data(), fb.size()).locked, "differential output locks the burst grid");
    }

    // 2a. Coherent mode, whole block: auto-resolves parity, locks, zero BER.
    {
        TetraFrontendConfig c = fc; c.coherent = true;
        TetraDemodFrontend fe(c);
        auto out = fe.demodulate(iq.data(), iq.size());
        { auto t = fe.flush(); out.insert(out.end(), t.begin(), t.end()); }
        check(fe.coherent_locked(), "coherent mode resolves parity and locks (block)");
        check(fe.resolved_parity() == 0 || fe.resolved_parity() == 1, "a concrete parity was chosen");
        check(!fe.fell_back(), "coherent did not fall back on a real signal");
        double ber = best_ber(src, out);
        std::printf("  coherent block: parity=%d, BER=%.5f\n", fe.resolved_parity(), ber);
        check(ber == 0.0, "coherent block decodes clean signal with zero BER");
    }

    // 2b. Coherent mode, streamed in chunks: same result across the seam.
    {
        TetraFrontendConfig c = fc; c.coherent = true;
        TetraDemodFrontend fe(c);
        std::vector<unsigned char> out;
        const std::size_t chunk = 997; // not a symbol multiple
        for (std::size_t off = 0; off < iq.size(); off += chunk) {
            const std::size_t m = std::min(chunk, iq.size() - off);
            auto b = fe.demodulate(iq.data() + off, m);
            out.insert(out.end(), b.begin(), b.end());
        }
        { auto t = fe.flush(); out.insert(out.end(), t.begin(), t.end()); }
        check(fe.coherent_locked(), "coherent mode locks when fed in chunks");
        check(best_ber(src, out) == 0.0, "chunked coherent decode is zero BER");
    }

    // 3. Coherent mode under moderate AWGN still locks and decodes.
    {
        auto add_awgn = [&](const std::vector<cf>& in, double ebno_db, unsigned seed) {
            double sp = 0; for (auto& s : in) sp += std::norm(s); sp /= in.size();
            const double eb = sp * sps / 2.0, ebno = std::pow(10.0, ebno_db / 10.0);
            const double sigma = std::sqrt((eb / ebno) / 2.0);
            std::mt19937 r(seed); std::normal_distribution<double> g(0.0, sigma);
            std::vector<cf> o(in.size());
            for (std::size_t i = 0; i < in.size(); ++i)
                o[i] = in[i] + cf(static_cast<float>(g(r)), static_cast<float>(g(r)));
            return o;
        };
        auto noisy = add_awgn(iq, 8.0, 77);
        TetraFrontendConfig c = fc; c.coherent = true;
        TetraDemodFrontend fe(c);
        auto out = fe.demodulate(noisy.data(), noisy.size());
        { auto t = fe.flush(); out.insert(out.end(), t.begin(), t.end()); }
        double ber = best_ber(src, out);
        std::printf("  coherent @8dB AWGN: locked=%d BER=%.5f\n", (int)fe.coherent_locked(), ber);
        check(fe.coherent_locked(), "coherent locks under 8 dB AWGN");
        check(ber < 1e-2, "coherent decode under 8 dB AWGN stays low-BER");
    }

    // 4. Non-TETRA input: coherent must give up and fall back to differential
    //    (rather than emit a wrong-parity guess forever). Differential mode on
    //    the same input just produces bits without locking.
    {
        std::mt19937 nr(555);
        std::normal_distribution<float> g(0.0f, 1.0f);
        std::vector<cf> noise(60000);
        for (auto& s : noise) s = cf(g(nr), g(nr));

        TetraFrontendConfig c = fc; c.coherent = true;
        TetraDemodFrontend fe(c);
        auto out = fe.demodulate(noise.data(), noise.size());
        { auto t = fe.flush(); out.insert(out.end(), t.begin(), t.end()); }
        check(!fe.coherent_locked(), "coherent does not lock on non-TETRA noise");
        check(fe.fell_back(), "coherent falls back to differential on non-TETRA input");

        TetraFrontendConfig d = fc; d.coherent = false;
        TetraDemodFrontend fed(d);
        auto od = fed.demodulate(noise.data(), noise.size());
        { auto t = fed.flush(); od.insert(od.end(), t.begin(), t.end()); }
        check(!od.empty(), "differential mode still produces bits on noise (no crash/stall)");
    }

    if (g_failures == 0) { std::printf("\nALL TETRA FRONTEND TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
