// tetra_demod.hpp
//
// π/4-DQPSK demodulator for TETRA (ETSI EN 300 392-2): the physical-layer
// front end that turns complex baseband IQ into the 36 kbit/s demodulated
// bitstream. TETRA is linear π/4-shifted differential QPSK at 18000
// symbols/s (2 bits/symbol) on 25 kHz channels, root-raised-cosine shaped
// with roll-off α = 0.35 — a completely different modulation from the FM
// discriminator path the rest of this server uses for C4FM/FDMA voice
// (DMR/NXDN/P25/dPMR/D-STAR/YSF), so it needs its own coherent front end
// rather than the fm_demod chain.
//
// Pipeline:
//   1. Root-raised-cosine matched filter (α = 0.35). Cascaded with the
//      transmit RRC this is a raised-cosine response, zero-ISI at the
//      symbol instants.
//   2. Gardner symbol-timing recovery (non-data-aided; a cubic-Farrow
//      interpolator steered by a PI loop). Gardner needs no carrier phase,
//      which suits the differential detector below.
//   3. Differential detection: the angle of s[k]·conj(s[k-1]) is the
//      π/4-DQPSK phase transition, decoded straight to the symbol's two
//      bits. Because detection is differential, an arbitrary *static*
//      carrier phase cancels out — the bits are recovered regardless of
//      absolute phase (there is no 90°/180° ambiguity to resolve).
//
// TETRA phase-transition mapping (EN 300 392-2, clause 5.5), with
// B(2k-1) = the sign bit, B(2k) = the magnitude bit:
//   (0,0) -> +π/4    (0,1) -> +3π/4    (1,0) -> -π/4    (1,1) -> -3π/4
//
// SCOPE / WHAT THIS IS NOT (yet):
//   - No carrier-frequency-offset (CFO) correction. Differential detection
//     tolerates a static phase and only a *small* residual CFO — a fixed
//     offset f rotates every transition by 2π·f/Rs and biases the slicer,
//     so anything beyond a few hundred Hz needs an AFC/CFO estimator
//     (TETRA's training sequence is the usual source). That is a separate
//     step; feed this a signal already near zero IF.
//   - No burst/frame synchronisation, descrambling, de-interleaving or
//     channel decoding. Those are the TETRA lower-MAC's job and consume
//     the continuous bitstream this produces. This module is only the
//     modem.
//
// Verification: tests/test_tetra_demod.cpp carries a matching π/4-DQPSK
// modulator (random bits -> RRC-shaped IQ) as ground truth and asserts the
// demod round-trips the bits exactly (zero BER) with perfect timing, under
// an arbitrary static phase rotation, and after a fractional-symbol timing
// offset the Gardner loop must pull in; plus a graceful BER-vs-noise check
// under AWGN. That validates the modem against the TETRA phase mapping
// encoded above; confirming the bits are correct for *real* TETRA is a
// later step that needs a real capture or a cross-check against osmo-tetra.

#pragma once

#include <cstddef>
#include <complex>
#include <vector>

namespace dsdsrv {

struct TetraDemodConfig {
    // Samples per symbol of the input IQ. Input sample rate must therefore
    // be samples_per_symbol * 18000 Hz (e.g. 4 -> 72 kHz). The timing
    // recovery and matched filter are designed around this.
    int samples_per_symbol = 4;

    // Root-raised-cosine matched filter. α = 0.35 is TETRA's shaping
    // roll-off; the span is the one-sided filter length in symbols
    // (total taps = 2*span*sps + 1).
    float rrc_alpha = 0.35f;
    int rrc_span_symbols = 8;

    // Normalised bandwidth of the Gardner timing loop (fraction of the
    // symbol rate). Smaller = steadier once locked but slower to pull in;
    // the default trades a quick lock against jitter for a clean signal.
    float timing_loop_bandwidth = 0.02f;
};

class TetraDpqskDemod {
public:
    explicit TetraDpqskDemod(const TetraDemodConfig& cfg = TetraDemodConfig{});

    // Demodulate a block of complex baseband IQ into hard bits, two per
    // recovered symbol in transmission order (B(2k-1) then B(2k)). Values
    // are 0/1. Block-oriented: each call demodulates the buffer as a whole
    // (streaming across calls is a later refinement).
    std::vector<unsigned char> demodulate(const std::complex<float>* iq, std::size_t n);

    // The symbol-instant samples recovered by the timing loop for the last
    // demodulate() call — exposed for tests/diagnostics (constellation).
    const std::vector<std::complex<float>>& last_symbols() const { return symbols_; }

    const TetraDemodConfig& config() const { return cfg_; }

private:
    std::vector<std::complex<float>> matched_filter(const std::complex<float>* iq, std::size_t n) const;
    void recover_symbols(const std::vector<std::complex<float>>& mf);

    TetraDemodConfig cfg_;
    std::vector<float> rrc_taps_;
    std::vector<std::complex<float>> symbols_;
};

} // namespace dsdsrv
