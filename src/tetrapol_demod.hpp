// tetrapol_demod.hpp
//
// GMSK demodulator for TETRAPOL (the European PMR standard used by France's
// Rubis, Spain's SIRDEE, the Czech/Swiss Polycom networks, ...). It is the
// physical-layer front end that turns complex baseband IQ into the 8 kbit/s
// demodulated bitstream the TETRAPOL lower layers / tetrapol_dump consume.
//
// TETRAPOL is GMSK (Gaussian Minimum Shift Keying): binary (1 bit/symbol) at
// 8000 symbols/s on 12.5 kHz channels. Unlike the π/4-DQPSK TETRA modem
// (tetra_demod.*), GMSK is a *constant-envelope FM* modulation, so it is
// demodulated the classic, robust way — as FM:
//
// Pipeline:
//   1. Quadrature (FM) discriminator: d[n] = arg(s[n]·conj(s[n-1])) is the
//      instantaneous frequency, i.e. the Gaussian-shaped ±deviation carrying
//      the data. This is phase-blind (an arbitrary static carrier phase
//      cancels in the conjugate product), so there is no carrier loop.
//   2. DC removal: a carrier-frequency offset adds a *constant* to the
//      discriminator output (a CFO of f Hz biases d by 2π·f/fs), which would
//      skew the binary slicer. A slow running-mean subtraction removes it, so
//      the modem tolerates residual SDR tuning error the way the TETRA modem's
//      CFO estimator does.
//   3. Gardner symbol-timing recovery (non-data-aided; a cubic-Farrow
//      interpolator steered by a PI loop) on the real discriminator signal.
//   4. Binary slice at the symbol instants: bit = (sample > 0). GNU Radio's
//      gmsk_demod (the tetrapol-kit reference front end) does the same
//      quadrature-demod → clock-recovery → slice with no differential decode,
//      emitting raw sliced bits; tetrapol_dump handles everything above the
//      modem. We match that, so our bits drop into tetrapol_dump unchanged
//      (the exact bit polarity is a 1-bit convention pinned against a real
//      capture / tetrapol_dump, exposed as `invert`).
//
// SCOPE / WHAT THIS IS NOT (yet):
//   - No frame synchronisation, descrambling, or channel decoding. Those are
//     the TETRAPOL lower-MAC's job (tetrapol_frame_sync.* / tetrapol_dump)
//     and consume the continuous bitstream this produces.
//
// STREAMING: demodulate() may be called repeatedly with successive blocks of
// one continuous signal; all modem state (discriminator history, DC-removal
// mean, the Gardner timing loop, and the AGC) carries across calls, so the
// bitstream is continuous with no per-block restart. flush() drains the few
// trailing samples at end-of-stream; reset() returns to the initial state.
//
// Verification: tests/test_tetrapol_demod.cpp carries a matching GMSK
// modulator (random bits -> Gaussian-shaped IQ) as ground truth and asserts
// the demod round-trips the bits exactly at good SNR, is immune to a static
// carrier phase and tolerant of a carrier-frequency offset, pulls in a
// fractional-symbol timing error, and degrades gracefully under AWGN. That
// validates the modem against its own modulator; confirming the bit
// *convention* against real TETRAPOL is a later step (needs a capture / a
// cross-check against tetrapol_dump), exactly as the TETRA modem was.

#pragma once

#include <cstddef>
#include <complex>
#include <vector>

namespace dsdsrv {

struct TetrapolDemodConfig {
    // Samples per symbol of the input IQ. Input sample rate must therefore be
    // samples_per_symbol * 8000 Hz (e.g. 2 -> 16 kHz, matching tetrapol-kit's
    // reference demod; 4 -> 32 kHz). Timing recovery is designed around this.
    int samples_per_symbol = 2;

    // Pre-detection channel low-pass, as a fraction of the 8000 Hz symbol
    // rate (the FIR's single-sided cutoff). FM detection amplifies any noise
    // reaching the discriminator, so band-limiting the IQ to the GMSK channel
    // first is what makes it work under noise. ~0.6·Rb is the sweet spot for a
    // BT=0.3 signal (wider lets in noise, narrower adds ISI); 0 disables it.
    float channel_lowpass_symbols = 0.6f;
    int channel_lowpass_span = 4; // FIR half-length in symbols (taps = 2*span*sps+1)

    // Normalised bandwidth of the Gardner timing loop (fraction of the symbol
    // rate). Smaller = steadier once locked but slower to pull in.
    float timing_loop_bandwidth = 0.01f;

    // Time constant (in symbols) of the running-mean DC removal that cancels a
    // residual carrier-frequency offset in the discriminator output. Long
    // enough not to droop on a run of same-value bits, short enough to track
    // slow drift. 0 disables DC removal (feed an already zero-IF signal).
    float dc_removal_symbols = 64.0f;

    // Bit polarity. GMSK's sign convention (which frequency deviation is a 1)
    // depends on the tx/rx chain; the correct value for tetrapol_dump is a
    // 1-bit unknown pinned against a real capture. false = bit is (d>0).
    bool invert = false;
};

class TetrapolGmskDemod {
public:
    explicit TetrapolGmskDemod(const TetrapolDemodConfig& cfg = TetrapolDemodConfig{});

    // Demodulate the next block of complex baseband IQ into hard bits, one per
    // recovered symbol (0/1). Streaming: modem state carries across calls, so
    // feeding a signal in blocks is equivalent to one call. A block too short
    // to yield a symbol returns no bits and is buffered for the next call.
    std::vector<unsigned char> demodulate(const std::complex<float>* iq, std::size_t n);

    // Drain the trailing samples held back for interpolator context at
    // end-of-stream and return their bits. Not needed between streaming blocks.
    std::vector<unsigned char> flush();

    // Discard all streaming state so the next demodulate() starts a fresh,
    // unrelated signal.
    void reset();

    // The symbol-instant discriminator samples recovered by the timing loop on
    // the last call -- for tests/diagnostics (eye/soft values).
    const std::vector<float>& last_symbols() const { return symbols_; }

    const TetrapolDemodConfig& config() const { return cfg_; }

private:
    std::vector<unsigned char> run(const std::complex<float>* iq, std::size_t n, bool final_flush);

    TetrapolDemodConfig cfg_;
    std::vector<float> symbols_; // last call's recovered symbol samples (diagnostics)
    std::vector<float> lp_taps_;   // pre-detection channel low-pass (empty = off)

    // ---- streaming state (persists across demodulate() calls) ----
    std::vector<std::complex<float>> iq_hist_; // channel-filter FIR history (raw IQ)
    std::complex<float> prev_iq_{0.0f, 0.0f}; // last filtered IQ sample (discriminator history)
    bool have_prev_iq_ = false;
    std::vector<float> disc_tail_;            // carried discriminator samples not yet consumed
    double pos_ = 0.0;                         // fractional read position into the work buffer
    float integ_ = 0.0f;                       // Gardner PI-loop integrator
    float prev_on_ = 0.0f;                     // last on-time sample (TED history)
    bool timing_started_ = false;
    float dc_ = 0.0f;                           // running-mean DC estimate (CFO bias)
    bool dc_init_ = false;
    float agc_ = 1.0f;                          // running RMS for amplitude normalisation
    bool agc_init_ = false;
};

} // namespace dsdsrv
