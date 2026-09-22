// fm_demod.hpp
//
// Performance-oriented FM discriminator chain for narrowband digital voice
// (DMR / P25 / NXDN-style) signals, intended to feed the DSD / DSD-FME
// decoder's raw discriminator-audio input.
//
// Pipeline per IQ block:
//   1. (optional) frequency shift via NCO, if the channel of interest is
//      not centered at 0 Hz in the supplied IQ stream.
//   2. Channel low-pass FIR filter (windowed-sinc) to reject adjacent
//      channels / out-of-band noise before demodulation.
//   3. Integer decimation to bring the sample rate down to something close
//      to a small multiple of the target output rate (48000 Hz, the rate
//      DSD-FME expects on its raw discriminator input).
//   4. Quadrature FM demodulation: angle(x[n] * conj(x[n-1])), which is
//      proportional to instantaneous frequency and avoids the cost of a
//      per-sample atan() on the raw IQ (only one atan2 per output sample).
//   5. Linear resample from the decimated rate to exactly 48000 Hz.
//   6. Gain scaling + clipping to 16-bit signed PCM.
//
// All buffers are processed in blocks to stay cache-friendly and to let the
// compiler auto-vectorize the inner loops (built with -O3 -march=native
// this vectorizes reasonably well without hand-written SIMD intrinsics;
// see README for notes on going further with SIMD if you need it).
//
// NOTE ON GAIN SCALING: the exact scale factor that maps discriminator
// output (radians/sample) to a 16-bit PCM range DSD "likes" is empirical —
// it depends on your SDR's actual frequency deviation and DSD-FME's AGC
// behavior. The default here is a reasonable starting point; expect to
// tune `disc_gain` against a known-good DMR signal.

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <complex>

namespace dsdsrv {

using cf32 = std::complex<float>;

struct FmDemodConfig {
    double input_sample_rate_hz = 2'000'000.0; // rate of incoming IQ
    double output_sample_rate_hz = 48'000.0;   // rate DSD-FME expects
    double channel_bandwidth_hz = 12'500.0;    // DMR channel width
    // Where the channel of interest sits in the incoming IQ, in Hz:
    // positive means the channel is ABOVE 0 Hz and the NCO mixes it down
    // to baseband. (Sign convention pinned by test_afc.cpp.)
    double freq_offset_hz = 0.0;
    float disc_gain = 26000.0f;                // discriminator -> PCM scale, tune empirically
    int fir_taps = 63;                         // channel filter length (odd)

    // --- AFC (automatic frequency control) ---
    // When enabled, the demod measures the DC component of its own
    // discriminator output -- which IS the residual carrier offset, in
    // disguise -- and steers the NCO to zero it. This corrects SDR
    // reference (ppm) error and slow drift without the client having to
    // know its dongle's calibration; the client-supplied freq_offset_hz
    // remains the starting point the correction is applied on top of.
    // The update is gated on the discriminator variance so that
    // no-signal noise (whose phase is random and would random-walk the
    // NCO) never moves the correction. See afc_update() in fm_demod.cpp
    // for the loop math and test_afc.cpp for measured behavior.
    bool afc_enabled = false;
    double afc_time_constant_s = 0.25;   // loop time constant; lock in ~4x this
    double afc_max_correction_hz = 5000.0; // hard clamp on |correction|

    // --- Matched filter (opt-in, experimental) ---
    // A root-raised-cosine FIR applied to the discriminator output, matched
    // to the digital-voice symbol pulse shape, narrowing the post-detection
    // noise bandwidth to roughly the symbol band. FM discriminator noise
    // has a parabolic (rising-with-frequency) spectrum, so most of it sits
    // ABOVE the symbol bandwidth; cutting it can recover a dB or two of SNR
    // into DSD's slicer near threshold. Symbol timing is deliberately left
    // to the downstream decoder (dsd-fme/DSDcc do their own), so this only
    // conditions the stream -- the output rate is unchanged.
    //
    // Defaults OFF: it must be A/B tested on real captures before being
    // trusted (it can double-filter against a decoder's own internal symbol
    // filter and then HURT), and the defaults target DMR 4FSK (4800 Bd).
    bool matched_filter_enabled = false;
    double mf_symbol_rate_hz = 4800.0;   // 4FSK symbol rate (DMR/NXDN48)
    double mf_rolloff = 0.2;             // RRC excess bandwidth (0..1)
    int mf_span_symbols = 8;             // FIR length in symbol periods
};

// Design a root-raised-cosine FIR of `num_taps` (forced odd) taps for a
// symbol_rate_hz signal sampled at fs_hz, normalized to unity DC gain.
// Exposed (not file-local) so the matched filter can be unit-tested
// directly -- see tests/test_matched_filter.cpp.
std::vector<float> design_rrc_fir(double fs_hz, double symbol_rate_hz,
                                  double rolloff, int num_taps);

// Streaming FM demodulator. Feed it IQ blocks via process(); it appends
// ready 16-bit PCM samples (mono, output_sample_rate_hz) to `out`.
// Not thread-safe on its own; each client session should own one instance
// and call it from a single worker thread (see session.hpp).
class FmDemodulator {
public:
    explicit FmDemodulator(const FmDemodConfig& cfg);

    // Accepts interleaved complex samples. `in` length is number of
    // complex samples (not floats). Appends demodulated int16 PCM to `out`.
    void process(const cf32* in, std::size_t n, std::vector<int16_t>& out);

    // Reconfigure gain / offset at runtime (safe to call between process()
    // calls from the same thread that owns this object; session.cpp
    // serializes these against process() with its demod mutex).
    void set_gain(float g) { cfg_.disc_gain = g; }
    // Sets a new base offset AND resets any accumulated AFC correction:
    // an explicit retune is a statement of new truth, so stale
    // correction from the old tuning must not carry over.
    void set_freq_offset(double hz);

    const FmDemodConfig& config() const { return cfg_; }
    int decimation_factor() const { return decim_; }
    double decimated_rate_hz() const { return cfg_.input_sample_rate_hz / decim_; }

    // Current AFC correction in Hz (0 when AFC is disabled or not yet
    // locked). The effective NCO frequency is freq_offset_hz + this.
    double afc_correction_hz() const { return afc_correction_hz_; }

private:
    void design_lowpass();
    void design_matched_filter();
    void apply_nco_frequency();
    void afc_update();
    void mix_and_filter_decimate(const cf32* in, std::size_t n);
    void demod_block();
    void apply_matched_filter();
    void resample_to_output();

    FmDemodConfig cfg_;
    int decim_ = 1;
    double afc_correction_hz_ = 0.0;

    // NCO state for optional frequency shifting
    double nco_phase_ = 0.0;
    double nco_incr_ = 0.0;

    // FIR filter taps + history for streaming (overlap) filtering
    std::vector<float> taps_;
    std::vector<cf32> fir_history_; // size = taps_.size() - 1

    // Scratch buffers reused across calls to avoid per-call allocation
    std::vector<cf32> mixed_;      // after NCO mix, pre-filter
    std::vector<cf32> decimated_;  // after filter + decimation

    // Discriminator state
    cf32 last_sample_{1.0f, 0.0f};
    std::vector<float> disc_out_;  // demodulated audio at decimated rate

    // Matched-filter state (real FIR on disc_out_, streamed with history).
    std::vector<float> mf_taps_;
    std::vector<float> mf_history_; // size = mf_taps_.size() - 1

    // Fractional resampler state (decimated rate -> exactly output rate)
    double resample_pos_ = 0.0;
    float resample_prev_ = 0.0f;
    bool have_prev_ = false;
};

} // namespace dsdsrv
