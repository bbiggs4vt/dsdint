// tetrapol_bit_source.cpp
//
// The GNU-Radio-replacement bridge for TETRAPOL — the analog of
// tetra_bit_source for the GMSK chain. Reads complex baseband IQ, runs the
// in-tree GMSK demodulator (src/tetrapol_demod.*), and writes the demodulated
// bitstream as ONE UNPACKED BIT PER BYTE (0x00/0x01) to stdout — exactly the
// format tetrapol-kit's `tetrapol_dump` reads on stdin. So the whole TETRAPOL
// lower-layer + TSDU stack can be driven WITHOUT GNU Radio:
//
//     tetrapol_bit_source capture.cf32 | tetrapol_dump
//
// The point of this tool is that we supply the physical-layer demod GNU Radio
// otherwise provides, and hand the resulting bits to the existing decoder for
// everything above the modem. It is also the validation path: point it at a
// real TETRAPOL IQ capture, pipe into a real tetrapol_dump, and see whether the
// TSDU decode comes out — the confirmation the parser/polarity convention are
// right that the tree can't yet do (no capture in the tree).
//
// Input: interleaved little-endian float32 I/Q samples (I0,Q0,I1,Q1,…) — the
// same complex-baseband wire format the server uses — from a file argument, or
// from stdin when the argument is "-". The IQ must be at
// samples_per_symbol × 8000 Hz (default 2 → 16 kHz) and tuned near zero IF (the
// demod cancels residual CFO via per-sample DC removal on its discriminator
// output).
//
// Output: the raw bitstream on stdout. Diagnostics on stderr, including a
// frame-sync lock check (src/tetrapol_frame_sync.*) as a quick "does this look
// like TETRAPOL?" indicator — a lock means the demod is producing
// TETRAPOL-shaped bits (the 160-bit frame grid), before tetrapol_dump is even
// involved, and reports the detected bit polarity.
//
// Scope: block-oriented — reads the whole input, demodulates, and emits the
// bits. That covers decoding a captured file (the validation path); true
// streaming with state carried across reads is a later refinement.

#include "tetrapol_demod.hpp"
#include "tetrapol_frame_sync.hpp"

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
    int sps = 2;
    bool invert = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sps" && i + 1 < argc) sps = std::atoi(argv[++i]);
        else if (a == "--invert") invert = true;
        else if (a == "-h" || a == "--help") {
            std::fprintf(stderr,
                "usage: %s [--sps N] [--invert] <iq.cf32 | ->\n"
                "  Reads interleaved float32 I/Q (sps*8000 Hz), writes the TETRAPOL\n"
                "  GMSK bitstream (1 byte per bit) to stdout for piping into\n"
                "  tetrapol_dump. --invert flips the bit polarity (spectral\n"
                "  inversion): tetrapol_dump's raw frame-sync search is polarity-\n"
                "  sensitive, so if a known-good capture won't lock, try --invert.\n"
                "  stderr reports a frame-grid lock check and the detected polarity.\n",
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

    TetrapolDemodConfig cfg;
    cfg.samples_per_symbol = sps;
    cfg.invert = invert;
    TetrapolGmskDemod demod(cfg);
    std::vector<unsigned char> bits = demod.demodulate(iq.data(), iq.size());
    // Whole-file (non-streaming) use: drain the trailing samples the streaming
    // demod holds back for filter context, so no symbols are lost.
    {
        std::vector<unsigned char> tail = demod.flush();
        bits.insert(bits.end(), tail.begin(), tail.end());
    }

    // Emit the bitstream: one byte per bit, as tetrapol_dump expects.
    if (!bits.empty())
        std::fwrite(bits.data(), 1, bits.size(), stdout);
    std::fflush(stdout);

    // Diagnostics + a "looks like TETRAPOL?" frame-grid lock check. The sync
    // searches both polarities, so a lock also tells us whether the raw stream
    // is inverted relative to the canonical frame-sync word -- i.e. whether
    // tetrapol_dump will need the bits flipped (rerun with --invert).
    TetrapolFrameSync sync;
    TetrapolLock lk = sync.synchronize(bits.data(), bits.size());
    std::fprintf(stderr,
        "tetrapol_bit_source: %zu IQ samples -> %zu bits; polarity=%s; %s",
        nsamp, bits.size(), invert ? "inverted" : "normal",
        lk.locked ? "TETRAPOL frame grid LOCKED" : "no frame-grid lock");
    if (lk.locked) {
        std::fprintf(stderr, " (phase %ld, %d frames, raw-stream %s)",
                     lk.phase, lk.confirmations,
                     lk.inverted ? "INVERTED -> add --invert for tetrapol_dump"
                                  : "canonical");
    }
    std::fprintf(stderr, "\n");
    return 0;
}
