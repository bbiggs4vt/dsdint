// tetra_bit_source.cpp
//
// The GNU-Radio-replacement bridge for TETRA: reads complex baseband IQ,
// runs the in-tree π/4-DQPSK demodulator (src/tetra_demod.*, with CFO
// correction), and writes the demodulated bitstream as ONE UNPACKED BIT
// PER BYTE (0x00/0x01) to stdout — exactly the format osmo-tetra's
// `tetra-rx` reads (its input is documented as "<file_with_1_byte_per_bit>").
// So the whole TETRA lower-MAC + voice stack can be driven WITHOUT GNU
// Radio:
//
//     tetra_bit_source capture.cf32 | tetra-rx -
//
// (tetra-kit's bit input works the same way.) The point of this tool is
// that we supply the physical-layer demod GNU Radio otherwise provides,
// and hand the resulting bits to the existing decoders for everything
// above the modem.
//
// Input: interleaved little-endian float32 I/Q samples (I0,Q0,I1,Q1,…) —
// the same complex-baseband wire format the server uses — from a file
// argument, or from stdin when the argument is "-". The IQ must be at
// samples_per_symbol × 18000 Hz (default 4 → 72 kHz) and tuned near zero
// IF (the demod's CFO corrector pulls in the residual ±Rs/8).
//
// Output: the raw bitstream on stdout. Diagnostics on stderr, including a
// burst/frame-sync lock check (src/tetra_burst_sync.*) as a quick "does
// this look like TETRA?" indicator — a lock means the demod is producing
// TETRA-shaped bits, before tetra-rx is even involved.
//
// Scope: block-oriented — it reads the whole input, demodulates, and emits
// the bits. That covers decoding a captured file (the validation path);
// true streaming with state carried across reads is a later refinement.

#include "tetra_demod.hpp"
#include "tetra_burst_sync.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <complex>
#include <string>
#include <vector>

using namespace dsdsrv;

namespace {

// Slurp an entire stream of bytes (file or stdin).
std::vector<uint8_t> read_all(std::FILE* f) {
    std::vector<uint8_t> data;
    uint8_t buf[1 << 16];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        data.insert(data.end(), buf, buf + n);
    return data;
}

} // namespace

int main(int argc, char** argv) {
    const char* input = nullptr;
    int sps = 4;
    bool correct_cfo = true;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sps" && i + 1 < argc) sps = std::atoi(argv[++i]);
        else if (a == "--no-cfo") correct_cfo = false;
        else if (a == "-h" || a == "--help") {
            std::fprintf(stderr,
                "usage: %s [--sps N] [--no-cfo] <iq.cf32 | ->\n"
                "  Reads interleaved float32 I/Q (sps*18000 Hz), writes the TETRA\n"
                "  bitstream (1 byte per bit) to stdout for piping into tetra-rx.\n",
                argv[0]);
            return 0;
        } else if (a[0] != '-' || a == "-") {
            input = argv[i];
        } else {
            std::fprintf(stderr, "%s: unknown option '%s'\n", argv[0], a.c_str());
            return 2;
        }
    }
    if (!input) {
        std::fprintf(stderr, "%s: no input (give an IQ file or '-' for stdin; -h for help)\n", argv[0]);
        return 2;
    }

    std::FILE* in = (std::strcmp(input, "-") == 0) ? stdin : std::fopen(input, "rb");
    if (!in) { std::fprintf(stderr, "%s: cannot open %s\n", argv[0], input); return 1; }
    std::vector<uint8_t> raw = read_all(in);
    if (in != stdin) std::fclose(in);

    // Reinterpret as interleaved float32 I/Q -> complex samples.
    const std::size_t nfloat = raw.size() / sizeof(float);
    const std::size_t nsamp = nfloat / 2; // 2 floats per complex sample
    std::vector<std::complex<float>> iq(nsamp);
    std::memcpy(iq.data(), raw.data(), nsamp * 2 * sizeof(float));

    TetraDemodConfig cfg;
    cfg.samples_per_symbol = sps;
    cfg.correct_cfo = correct_cfo;
    TetraDpqskDemod demod(cfg);
    std::vector<unsigned char> bits = demod.demodulate(iq.data(), iq.size());
    // Whole-file (non-streaming) use: drain the trailing samples the streaming
    // demod holds back for matched-filter context, so no symbols are lost.
    {
        std::vector<unsigned char> tail = demod.flush();
        bits.insert(bits.end(), tail.begin(), tail.end());
    }

    // Emit the bitstream: one byte per bit, as tetra-rx expects.
    if (!bits.empty())
        std::fwrite(bits.data(), 1, bits.size(), stdout);
    std::fflush(stdout);

    // Diagnostics + a "looks like TETRA?" lock check.
    TetraBurstSync sync;
    TetraLock lk = sync.synchronize(bits.data(), bits.size());
    std::fprintf(stderr,
        "tetra_bit_source: %zu IQ samples -> %zu bits; CFO est %.0f Hz; %s",
        nsamp, bits.size(), demod.last_cfo_hz(),
        lk.locked ? "TETRA burst grid LOCKED" : "no burst-grid lock");
    if (lk.locked)
        std::fprintf(stderr, " (phase %ld, %d bursts)", lk.phase, lk.confirmations);
    std::fprintf(stderr, "\n");
    return 0;
}
