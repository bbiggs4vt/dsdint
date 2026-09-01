// test_tetrapol_bit_source.cpp
//
// End-to-end test of the tetrapol_bit_source bridge tool: build a framed
// TETRAPOL bitstream (the 7-bit frame-sync word planted on the 160-bit grid),
// GMSK-modulate it to IQ, write that IQ to a file, run the built
// tetrapol_bit_source binary on it as a subprocess, and confirm the bitstream
// it emits on stdout locks the TETRAPOL frame grid — i.e. the tool produces
// exactly the 1-byte-per-bit stream tetrapol_dump would consume.
//
// The tool binary path is passed as argv[1] (CMake supplies it). Links the
// frame synchroniser and demod for the in-test checks; the GMSK modulator is
// inline (matching src/tetrapol_demod's convention, same as the demod's own
// test).

#include "tetrapol_frame_sync.hpp"

#include <algorithm>
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
const uint8_t DS[7] = { 1, 0, 1, 0, 0, 1, 1 };
constexpr int FR = 160, HDR = 1;

// Build `frames` back-to-back 160-bit frames, random-filled, each carrying the
// sync word at offset HDR.
std::vector<uint8_t> build_stream(int frames, std::mt19937& rng) {
    std::uniform_int_distribution<int> d(0, 1);
    std::vector<uint8_t> s(static_cast<std::size_t>(frames) * FR);
    for (auto& b : s) b = static_cast<uint8_t>(d(rng));
    for (int f = 0; f < frames; ++f)
        for (int i = 0; i < 7; ++i) s[f * FR + HDR + i] = DS[i];
    return s;
}

// GMSK modulator (matches tetrapol_demod / test_tetrapol_frame_sync).
std::vector<cf> modulate(const std::vector<uint8_t>& bits, int sps, float bt) {
    const int span = 3, half = span * sps;
    const float sigma = std::sqrt(std::log(2.0f)) / (2.0f * kPi * bt);
    std::vector<float> g(2 * half + 1); float gs = 0;
    for (int i = -half; i <= half; ++i) { float t = (float)i / sps; g[i + half] = std::exp(-t * t / (2 * sigma * sigma)); gs += g[i + half]; }
    for (float& v : g) v /= gs;
    std::vector<float> nrz(bits.size() * sps);
    for (std::size_t k = 0; k < bits.size(); ++k) { float a = bits[k] ? 1.f : -1.f; for (int j = 0; j < sps; ++j) nrz[k * sps + j] = a; }
    const int Nn = (int)nrz.size(), ng = (int)g.size();
    std::vector<float> ff(Nn, 0);
    for (int i = 0; i < Nn; ++i) { float ac = 0; for (int t = 0; t < ng; ++t) { int idx = i + t - half; ac += g[t] * nrz[std::clamp(idx, 0, Nn - 1)]; } ff[i] = ac; }
    std::vector<cf> iq(Nn); float ph = 0, kp = (kPi / 2) / sps;
    for (int i = 0; i < Nn; ++i) { ph += kp * ff[i]; iq[i] = std::polar(1.f, ph); }
    return iq;
}
} // namespace

int main(int argc, char** argv) {
    std::printf("test_tetrapol_bit_source\n");
    if (argc < 2) { std::printf("SKIPPED: no tetrapol_bit_source binary path given\n"); return 0; }
    const std::string tool = argv[1];

    // Generate a framed TETRAPOL GMSK IQ file (sps=2, the tool's default).
    const int sps = 2;
    std::mt19937 rng(31337);
    auto stream = build_stream(80, rng);
    auto iq = modulate(stream, sps, 0.3f);
    const std::string iqfile = "tetrapol_bitsrc_test.cf32";
    {
        std::FILE* f = std::fopen(iqfile.c_str(), "wb");
        if (!f) { std::printf("  FAIL: cannot write %s\n", iqfile.c_str()); return 1; }
        std::fwrite(iq.data(), sizeof(cf), iq.size(), f); // interleaved float32 I/Q
        std::fclose(f);
    }

    // Run the tool (optionally with extra flags) and return its stdout bits.
    auto run_tool = [&](const std::string& flags) {
        const std::string cmd = "'" + tool + "' " + flags + "'" + iqfile + "'";
        std::FILE* p = popen(cmd.c_str(), "r");
        std::vector<uint8_t> bits;
        if (p) {
            uint8_t buf[4096]; std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) bits.insert(bits.end(), buf, buf + n);
            pclose(p);
        }
        return bits;
    };
    auto verify = [&](const std::vector<uint8_t>& bits, const std::string& label) {
        std::printf("  (%s: emitted %zu bits)\n", label.c_str(), bits.size());
        check(bits.size() > 12000, label + ": full bitstream (~1 bit/symbol over 80 frames)");
        bool binary_ok = true;
        for (uint8_t b : bits) if (b > 1) { binary_ok = false; break; }
        check(binary_ok, label + ": every emitted byte is an unpacked bit (0x00/0x01)");
        TetrapolFrameSync sync;
        TetrapolLock lk = sync.synchronize(bits.data(), bits.size());
        std::printf("    (grid lock: %s, phase %ld, %d frames, inverted=%d)\n",
                    lk.locked ? "yes" : "no", lk.phase, lk.confirmations, (int)lk.inverted);
        check(lk.locked, label + ": bitstream locks the TETRAPOL frame grid (ready for tetrapol_dump)");
        check(lk.confirmations >= 70, label + ": most frames confirm the grid");
    };

    // Default polarity.
    verify(run_tool(""), "default");
    // --invert flips the demod polarity; the frame grid still locks, now as the
    // inverted raw stream (the sync searches both polarities).
    {
        auto bits = run_tool("--invert ");
        std::printf("  (invert: emitted %zu bits)\n", bits.size());
        TetrapolFrameSync sync;
        TetrapolLock lk = sync.synchronize(bits.data(), bits.size());
        std::printf("    (grid lock: %s, inverted=%d)\n", lk.locked ? "yes" : "no", (int)lk.inverted);
        check(lk.locked, "invert: still locks the frame grid");
        check(lk.inverted, "invert: locks as the inverted raw stream");
    }

    std::remove(iqfile.c_str());

    if (g_failures == 0) { std::printf("\nALL TETRAPOL BIT SOURCE TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
