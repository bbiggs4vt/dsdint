// test_tetra_burst_sync.cpp
//
// Validates the TETRA burst/frame synchroniser (src/tetra_burst_sync.cpp).
// A burst generator here independently plants the real training sequences
// at the real within-burst offsets inside 510-bit timeslots (ground truth
// kept separate from the implementation's copy), then checks:
//   1. every planted training sequence is found at the right position/type;
//   2. the 510-bit grid locks with the expected phase;
//   3. a stream-wide bit offset just shifts the locked phase;
//   4. it still locks with injected bit errors (tolerant correlation);
//   5. pure random data does NOT lock (grid consistency rejects noise);
//   6. end-to-end: a burst stream pushed through the π/4-DQPSK modulator
//      and demodulator still locks.
//
// Pure DSP/logic, links only the demod for case 6; libm otherwise.

#include "tetra_burst_sync.hpp"
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

// Training sequences, redeclared independently of the implementation (this
// is the test's own ground truth). Verbatim from ETSI EN 300 392-2 /
// osmo-tetra.
const uint8_t Y[38] = { 1,1,0,0,0,0,0,1,1,0,0,1,1,1,0,0,1,1,1,0,1,0,0,1,1,1,0,0,0,0,0,1,1,0,0,1,1,1 };
const uint8_t N[22] = { 1,1,0,1,0,0,0,0,1,1,1,0,1,0,0,1,1,1,0,1,0,0 };
const uint8_t P[22] = { 0,1,1,1,1,0,1,0,0,1,0,0,0,0,1,1,0,1,1,1,1,0 };
constexpr int OFF_SYNC = 214, OFF_NORM = 244, TS = 510;

void place(std::vector<uint8_t>& s, std::size_t pos, const uint8_t* seq, int len) {
    for (int i = 0; i < len; ++i) s[pos + i] = seq[i];
}

// Build `num_slots` back-to-back 510-bit bursts filled with random bits, the
// first a sync burst (Y at 214) and the rest normal bursts (N at 244).
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

bool has_match(const std::vector<TrainMatch>& ms, std::size_t pos, TetraTrain type) {
    for (const auto& m : ms)
        if (m.pos == pos && m.type == type) return true;
    return false;
}

// --- minimal π/4-DQPSK modulator (matches tetra_demod), for case 6 ---
std::vector<float> make_rrc(int sps, float a, int span) {
    const int half = span * sps;
    std::vector<float> h(2 * half + 1);
    for (int i = -half; i <= half; ++i) {
        const float x = static_cast<float>(i) / sps;
        float v;
        if (i == 0) v = 1 + a * (4 / kPi - 1);
        else if (a > 0 && std::fabs(std::fabs(4 * a * x) - 1) < 1e-4f)
            v = (a / std::sqrt(2.0f)) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) +
                                         (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
        else {
            const float num = std::sin(kPi * x * (1 - a)) + 4 * a * x * std::cos(kPi * x * (1 + a));
            const float den = kPi * x * (1 - (4 * a * x) * (4 * a * x));
            v = num / den;
        }
        h[i + half] = v;
    }
    float e = 0; for (float t : h) e += t * t;
    const float sc = 1.0f / std::sqrt(e); for (float& t : h) t *= sc;
    return h;
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
} // namespace

int main() {
    std::printf("test_tetra_burst_sync\n");
    TetraBurstSync sync;
    const int slots = 20;

    // 1 & 2: clean stream — find the planted sequences and lock the grid.
    {
        std::mt19937 rng(101);
        auto s = build_stream(slots, rng);
        auto ms = sync.find_all(s.data(), s.size());
        check(has_match(ms, OFF_SYNC, TetraTrain::Sync), "sync training seq found at offset 214");
        bool all_norm = true;
        for (int slot = 1; slot < slots; ++slot)
            if (!has_match(ms, static_cast<std::size_t>(slot) * TS + OFF_NORM, TetraTrain::Norm1))
                all_norm = false;
        check(all_norm, "every normal training seq found at slot*510 + 244");

        auto lk = sync.find_lock(ms);
        check(lk.locked, "grid locks");
        check(lk.phase == 0, "locked phase is 0 (bursts start at 0, 510, 1020, …)");
        check(lk.confirmations >= slots, "all bursts confirm the grid");
    }

    // 3: a stream-wide offset shifts the phase but still locks.
    {
        std::mt19937 rng(202);
        auto s = build_stream(slots, rng);
        const int pre = 137;
        std::vector<uint8_t> shifted(pre, 0);
        std::mt19937 pr(5); std::uniform_int_distribution<int> d(0, 1);
        for (auto& b : shifted) b = static_cast<uint8_t>(d(pr));
        shifted.insert(shifted.end(), s.begin(), s.end());
        auto lk = sync.synchronize(shifted.data(), shifted.size());
        check(lk.locked, "still locks with a stream-wide offset");
        check(lk.phase == pre % TS, "locked phase reflects the offset (137)");
    }

    // 4: injected bit errors — tolerant correlation still locks.
    {
        std::mt19937 rng(303);
        auto s = build_stream(slots, rng);
        std::mt19937 er(9);
        std::uniform_real_distribution<double> u(0, 1);
        int flipped = 0;
        for (auto& b : s) if (u(er) < 0.02) { b ^= 1; ++flipped; } // ~2% BER
        auto lk = sync.synchronize(s.data(), s.size());
        std::printf("  (flipped %d bits, ~2%%)\n", flipped);
        check(lk.locked, "locks despite ~2% bit errors");
        check(lk.phase == 0, "phase still correct under errors");
    }

    // 5: pure random data must NOT lock (no consistent burst grid).
    {
        std::mt19937 rng(404);
        std::uniform_int_distribution<int> d(0, 1);
        std::vector<uint8_t> r(slots * TS);
        for (auto& b : r) b = static_cast<uint8_t>(d(rng));
        auto lk = sync.synchronize(r.data(), r.size());
        std::printf("  (random-data best phase had %d confirmations)\n", lk.confirmations);
        check(!lk.locked, "random data does not lock");
    }

    // 6: end-to-end — modulate the burst stream, demodulate, then lock.
    {
        std::mt19937 rng(505);
        auto s = build_stream(slots, rng);
        const int sps = 4;
        auto iq = modulate(s, sps, 0.35f, 8);
        TetraDemodConfig cfg; cfg.samples_per_symbol = sps;
        TetraDpqskDemod demod(cfg);
        auto bits = demod.demodulate(iq.data(), iq.size());
        auto lk = sync.synchronize(bits.data(), bits.size());
        std::printf("  (end-to-end: %zu bits out, phase=%ld, conf=%d)\n",
                    bits.size(), lk.phase, lk.confirmations);
        check(lk.locked, "locks end-to-end through the modulator + demodulator");
        check(lk.confirmations >= slots - 2, "most bursts confirm the grid end-to-end");
    }

    if (g_failures == 0) { std::printf("\nALL TETRA BURST SYNC TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
