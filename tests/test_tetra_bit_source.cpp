// test_tetra_bit_source.cpp
//
// End-to-end test of the tetra_bit_source bridge tool: generate a TETRA
// burst stream, π/4-DQPSK-modulate it to IQ, write that IQ to a file, run
// the built tetra_bit_source binary on it as a subprocess, and confirm the
// bitstream it emits on stdout locks the TETRA burst grid — i.e. the tool
// produces exactly the 1-byte-per-bit stream tetra-rx would consume.
//
// The tool binary path is passed as argv[1] (CMake supplies it). Links the
// burst synchroniser and demod for the in-test checks; the modulator is
// inline.

#include "tetra_burst_sync.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
std::vector<uint8_t> build_stream(int slots, std::mt19937& rng) {
    std::uniform_int_distribution<int> d(0, 1);
    std::vector<uint8_t> s(static_cast<std::size_t>(slots) * TS);
    for (auto& b : s) b = static_cast<uint8_t>(d(rng));
    for (int slot = 0; slot < slots; ++slot) {
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
    float e = 0;
    for (float t : h) e += t * t;
    const float sc = 1.0f / std::sqrt(e);
    for (float& t : h) t *= sc;
    return h;
}
std::vector<cf> modulate(const std::vector<uint8_t>& bits, int sps, float a, int span) {
    std::vector<cf> syms; float ph = 0; syms.push_back(std::polar(1.0f, ph));
    for (std::size_t i = 0; i + 1 < bits.size(); i += 2) {
        const float sign = bits[i] ? -1.0f : 1.0f;
        const float mag = bits[i + 1] ? (3 * kPi / 4) : (kPi / 4);
        ph += sign * mag; syms.push_back(std::polar(1.0f, ph));
    }
    const auto rrc = make_rrc(sps, a, span);
    std::vector<cf> up(syms.size() * sps, cf(0, 0));
    for (std::size_t k = 0; k < syms.size(); ++k) up[k * sps] = syms[k];
    const int nt = static_cast<int>(rrc.size()), dl = nt / 2;
    std::vector<cf> iq(up.size());
    for (std::size_t i = 0; i < up.size(); ++i) {
        cf ac(0, 0);
        for (int t = 0; t < nt; ++t) { const long idx = static_cast<long>(i) + t - dl;
            if (idx >= 0 && idx < static_cast<long>(up.size())) ac += rrc[t] * up[idx]; }
        iq[i] = ac;
    }
    return iq;
}
} // namespace

int main(int argc, char** argv) {
    std::printf("test_tetra_bit_source\n");
    if (argc < 2) { std::printf("SKIPPED: no tetra_bit_source binary path given\n"); return 0; }
    const std::string tool = argv[1];

    // Generate a TETRA burst IQ file.
    std::mt19937 rng(77);
    auto stream = build_stream(20, rng);
    auto iq = modulate(stream, 4, 0.35f, 8);
    const std::string iqfile = "tetra_bitsrc_test.cf32";
    {
        std::FILE* f = std::fopen(iqfile.c_str(), "wb");
        if (!f) { std::printf("  FAIL: cannot write %s\n", iqfile.c_str()); return 1; }
        std::fwrite(iq.data(), sizeof(cf), iq.size(), f); // interleaved float32 I/Q
        std::fclose(f);
    }

    // Run the tool and capture its stdout (the bitstream).
    const std::string cmd = "'" + tool + "' '" + iqfile + "'";
    std::FILE* p = popen(cmd.c_str(), "r");
    check(p != nullptr, "tetra_bit_source runs");
    std::vector<uint8_t> bits;
    if (p) {
        uint8_t buf[4096]; std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) bits.insert(bits.end(), buf, buf + n);
        pclose(p);
    }
    std::remove(iqfile.c_str());

    std::printf("  (tool emitted %zu bits)\n", bits.size());
    check(bits.size() > 9000, "tool emits a full bitstream (~2 bits/symbol over 20 slots)");
    bool binary_ok = true;
    for (uint8_t b : bits) if (b > 1) { binary_ok = false; break; }
    check(binary_ok, "every emitted byte is an unpacked bit (0x00/0x01)");

    // The emitted bits must lock the TETRA burst grid.
    TetraBurstSync sync;
    TetraLock lk = sync.synchronize(bits.data(), bits.size());
    std::printf("  (grid lock: %s, phase %ld, %d bursts)\n",
                lk.locked ? "yes" : "no", lk.phase, lk.confirmations);
    check(lk.locked, "emitted bitstream locks the TETRA burst grid (ready for tetra-rx)");
    check(lk.confirmations >= 18, "nearly all bursts confirm the grid");

    if (g_failures == 0) { std::printf("\nALL TETRA BIT SOURCE TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
