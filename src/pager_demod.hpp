// pager_demod.hpp
//
// Streaming FM discriminator for narrowband paging channels (POCSAG / FLEX),
// producing the 16-bit mono PCM "discriminator audio" multimon-ng expects on
// its raw input (22050 Hz by default).
//
// Pipeline per IQ block:
//   1. (optional) NCO frequency shift, if the channel of interest is not
//      centered at 0 Hz in the supplied IQ (plus any AFC correction).
//   2. Channel low-pass FIR (windowed-sinc, Hamming) + integer decimation
//      to a rate at or just above 2x the output rate.
//   3. Quadrature FM demodulation: angle(x[n] * conj(x[n-1])).
//   4. (optional) AFC: the block mean of the discriminator output is the
//      residual carrier offset; steer the NCO to remove it.
//   5. Post-detection audio low-pass (removes discriminator noise above the
//      data band so the downsample to 22050 Hz doesn't alias it back in).
//   6. Linear resample to exactly output_sample_rate_hz.
//   7. Scale to int16: a deviation of `full_scale_deviation_hz` maps to
//      +/-16384 counts (times `gain`), independent of the IQ sample rate.
//
// A sibling of fm_demod.* (same streaming/history-carry structure) rather
// than a reuse of it, because paging wants different things: the output rate
// targets multimon-ng (22050 Hz, not DSD's 48 kHz), the DMR matched filter is
// replaced by a plain audio low-pass, and the PCM scaling is normalized to
// deviation in Hz, so the default level is right at any IQ rate and the DSD
// path's `gain` (a radians->counts factor tuned for dsd-fme) doesn't apply.

#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsdsrv {

using cf32 = std::complex<float>;

struct PagerDemodConfig {
    double input_sample_rate_hz = 2'000'000.0; // rate of incoming IQ
    double output_sample_rate_hz = 22'050.0;   // multimon-ng's native raw rate
    double channel_bandwidth_hz = 12'500.0;    // paging channels are 12.5/25 kHz
    // Where the channel sits in the incoming IQ, in Hz: positive means the
    // channel is ABOVE 0 Hz and the NCO mixes it down to baseband.
    double freq_offset_hz = 0.0;
    // PCM scale: `full_scale_deviation_hz` of instantaneous frequency maps to
    // 16384 * gain counts. POCSAG runs +/-4.5 kHz and FLEX +/-4.8 kHz, so the
    // default lands peaks at roughly half of int16 full scale.
    float gain = 1.0f;
    double full_scale_deviation_hz = 5000.0;
    // Negate the discriminator output. Use when the SDR delivers a spectrally
    // inverted IQ stream (I/Q swapped), which turns every 1 into a 0.
    bool invert = false;
    int fir_taps = 63;                   // channel filter length (odd)
    double audio_cutoff_hz = 7000.0;     // post-detection low-pass cutoff
    int audio_taps = 31;                 // post-detection filter length (odd)

    // --- AFC ---
    // Measures the DC of the discriminator output (the residual carrier
    // offset) and steers the NCO to zero it. Gated on the discriminator
    // variance so no-signal noise doesn't random-walk the correction:
    // paging FSK at +/-4.5..4.8 kHz has a frequency std-dev of ~3-5 kHz,
    // noise-only input is ~uniform phase steps (std ~12 kHz at a 44 kHz
    // discriminator rate), so a 7 kHz gate separates the two.
    bool afc_enabled = false;
    double afc_time_constant_s = 0.25;
    double afc_max_correction_hz = 5000.0;
};

// Streaming FM demodulator. Feed IQ blocks via process(); ready int16 PCM
// (mono, output_sample_rate_hz) is appended to `out`. Not thread-safe; the
// owning session serializes access (see session.hpp).
class PagerFmDemodulator {
public:
    explicit PagerFmDemodulator(const PagerDemodConfig& cfg);

    void process(const cf32* in, std::size_t n, std::vector<int16_t>& out);

    void set_gain(float g) { cfg_.gain = g; }
    // New base offset; also resets the accumulated AFC correction (an
    // explicit retune is a statement of new truth).
    void set_freq_offset(double hz);

    const PagerDemodConfig& config() const { return cfg_; }
    int decimation_factor() const { return decim_; }
    double decimated_rate_hz() const { return cfg_.input_sample_rate_hz / decim_; }
    double afc_correction_hz() const { return afc_correction_hz_; }

private:
    void apply_nco_frequency();
    void mix_and_filter_decimate(const cf32* in, std::size_t n);
    void demod_block();
    void afc_update();
    void audio_filter();
    void resample_to_output();

    PagerDemodConfig cfg_;
    int decim_ = 1;
    double afc_correction_hz_ = 0.0;

    double nco_phase_ = 0.0;
    double nco_incr_ = 0.0;

    std::vector<float> taps_;
    std::vector<cf32> fir_history_;
    std::vector<cf32> mixed_;
    std::vector<cf32> buf_;
    std::vector<cf32> decimated_;
    std::size_t dec_phase_ = 0; // decimation phase carried across blocks

    cf32 last_sample_{1.0f, 0.0f};
    std::vector<float> disc_out_;

    std::vector<float> audio_taps_;
    std::vector<float> audio_history_;
    std::vector<float> audio_buf_;
    std::vector<float> scratch_;

    double resample_pos_ = 0.0;
    float resample_prev_ = 0.0f;
};

// Windowed-sinc (Hamming) low-pass, unity DC gain, odd length. Exposed for
// tests.
std::vector<float> pager_design_lowpass_fir(double sample_rate, double cutoff_hz, int taps);

} // namespace dsdsrv
