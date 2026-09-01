// test_tetrapol_frame_sync.cpp
//
// Validates TetrapolFrameSync (src/tetrapol_frame_sync.*): finding the 7-bit
// TETRAPOL frame-sync word on the raw bitstream and locking the 160-bit frame
// grid. A generator plants the sync word on the grid; cases:
//   1. clean stream — sync found on the grid, lock at the right phase;
//   2. tolerance — one injected bit error per sync still locks;
//   3. stream-wide offset — the grid phase tracks it;
//   4. random data does NOT lock (the weak 7-bit word needs the run test);
//   5. inverted raw stream locks with inverted=true;
//   6. end-to-end — a framed bitstream through the GMSK modulator + demod locks.
//
// Links the demod for case 6; the modulator is inline.

#include "tetrapol_frame_sync.hpp"
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
const uint8_t DS[7] = { 1, 0, 1, 0, 0, 1, 1 };
constexpr int FR = 160, HDR = 1; // plant the 7-bit dsync at frame offset HDR

// Build `frames` back-to-back 160-bit frames, random-filled, each carrying the
// sync word at offset HDR. Optionally invert every bit.
std::vector<uint8_t> build_stream(int frames, std::mt19937& rng, bool invert = false) {
    std::uniform_int_distribution<int> d(0, 1);
    std::vector<uint8_t> s(static_cast<std::size_t>(frames) * FR);
    for (auto& b : s) b = static_cast<uint8_t>(d(rng));
    for (int f = 0; f < frames; ++f)
        for (int i = 0; i < 7; ++i) s[f * FR + HDR + i] = DS[i];
    if (invert) for (auto& b : s) b ^= 1u;
    return s;
}

// --- GMSK modulator (matches tetrapol_demod), for case 6 ---
std::vector<cf> modulate(const std::vector<uint8_t>& bits, int sps, float bt) {
    const int span = 3, half = span * sps;
    const float sigma = std::sqrt(std::log(2.0f)) / (2.0f * kPi * bt);
    std::vector<float> g(2 * half + 1); float gs = 0;
    for (int i = -half; i <= half; ++i) { float t = (float)i / sps; g[i + half] = std::exp(-t * t / (2 * sigma * sigma)); gs += g[i + half]; }
    for (float& v : g) v /= gs;
    std::vector<float> nrz(bits.size() * sps);
    for (std::size_t k = 0; k < bits.size(); ++k) { float a = bits[k] ? 1.f : -1.f; for (int j = 0; j < sps; ++j) nrz[k * sps + j] = a; }
    const int N = (int)nrz.size(), ng = (int)g.size();
    std::vector<float> f(N, 0);
    for (int i = 0; i < N; ++i) { float ac = 0; for (int t = 0; t < ng; ++t) { int idx = i + t - half; ac += g[t] * nrz[std::clamp(idx, 0, N - 1)]; } f[i] = ac; }
    std::vector<cf> iq(N); float ph = 0, kp = (kPi / 2) / sps;
    for (int i = 0; i < N; ++i) { ph += kp * f[i]; iq[i] = std::polar(1.f, ph); }
    return iq;
}
} // namespace

int main() {
    std::printf("test_tetrapol_frame_sync\n");
    std::mt19937 rng(4242);

    // 1. Clean stream.
    {
        auto s = build_stream(40, rng);
        TetrapolFrameSync sync;
        auto ms = sync.find_all(s.data(), s.size());
        int on_grid = 0;
        for (const auto& m : ms) if (!m.inverted && m.errors == 0 && m.pos % FR == HDR) ++on_grid;
        check(on_grid >= 38, "sync word found on the 160-bit grid in most frames");
        auto lk = sync.find_lock(ms);
        check(lk.locked, "frame grid locks on a clean stream");
        check(lk.phase == HDR, "locked phase is the planted sync offset");
        check(!lk.inverted, "locked polarity is canonical (not inverted)");
        check(lk.confirmations >= 38, "nearly all frames confirm the grid");
    }

    // 2. One injected bit error per sync — still locks (tolerance = 1).
    {
        auto s = build_stream(40, rng);
        for (int f = 0; f < 40; ++f) s[f * FR + HDR + (f % 7)] ^= 1u; // flip one sync bit
        TetrapolFrameSync sync;
        auto lk = sync.synchronize(s.data(), s.size());
        std::printf("  (1-error: locked=%d, run=%d)\n", (int)lk.locked, lk.confirmations);
        check(lk.locked && lk.phase == HDR, "locks with one bit error per sync word");
    }

    // 3. Stream-wide offset — grid phase tracks it.
    {
        auto base = build_stream(40, rng);
        const int pre = 53;
        std::vector<uint8_t> s(pre, 0);
        s.insert(s.end(), base.begin(), base.end());
        TetrapolFrameSync sync;
        auto lk = sync.synchronize(s.data(), s.size());
        check(lk.locked, "locks after a stream-wide bit offset");
        check(lk.phase == (HDR + pre) % FR, "phase reflects the offset");
    }

    // 4. Random data must NOT lock.
    {
        std::uniform_int_distribution<int> d(0, 1);
        std::vector<uint8_t> r(40 * FR);
        for (auto& b : r) b = static_cast<uint8_t>(d(rng));
        TetrapolFrameSync sync;
        auto lk = sync.synchronize(r.data(), r.size());
        std::printf("  (random: locked=%d, longest run=%d)\n", (int)lk.locked, lk.confirmations);
        check(!lk.locked, "random data does not lock (the run test rejects it)");
    }

    // 5. Inverted raw stream — locks with inverted=true.
    {
        auto s = build_stream(40, rng, /*invert=*/true);
        TetrapolFrameSync sync;
        auto lk = sync.synchronize(s.data(), s.size());
        check(lk.locked, "inverted raw stream still locks");
        check(lk.inverted, "reports inverted polarity");
    }

    // 6. End-to-end through the GMSK modulator + demodulator.
    {
        const int sps = 4;
        std::mt19937 r2(77);
        auto s = build_stream(80, r2);
        auto iq = modulate(s, sps, 0.3f);
        TetrapolDemodConfig dc; dc.samples_per_symbol = sps;
        TetrapolGmskDemod demod(dc);
        auto bits = demod.demodulate(iq.data(), iq.size());
        { auto t = demod.flush(); bits.insert(bits.end(), t.begin(), t.end()); }
        TetrapolFrameSync sync;
        auto lk = sync.synchronize(bits.data(), bits.size());
        std::printf("  (end-to-end: %zu bits, locked=%d, run=%d, phase=%ld, inv=%d)\n",
                    bits.size(), (int)lk.locked, lk.confirmations, lk.phase, (int)lk.inverted);
        check(lk.locked, "locks end-to-end through the GMSK modem");
        check(lk.confirmations >= 70, "most frames confirm the grid end-to-end");
    }

    if (g_failures == 0) { std::printf("\nALL TETRAPOL FRAME SYNC TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
