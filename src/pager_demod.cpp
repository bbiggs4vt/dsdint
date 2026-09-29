#include "pager_demod.hpp"

#include <algorithm>
#include <cmath>

namespace dsdsrv {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Effective NCO frequencies below this are treated as zero so the per-sample
// mix (the most expensive stage at high IQ rates) stays off when there is
// nothing worth correcting -- AFC settles at small nonzero values on a
// well-centered signal. 10 Hz is noise against the kHz-scale FSK deviation.
constexpr double kNcoDeadbandHz = 10.0;
} // namespace

std::vector<float> pager_design_lowpass_fir(double sample_rate, double cutoff_hz, int taps) {
    if (taps % 2 == 0) taps += 1;
    std::vector<float> h(static_cast<std::size_t>(taps));
    const int M = taps - 1;
    const double fc = cutoff_hz / sample_rate;
    double sum = 0.0;
    for (int n = 0; n < taps; ++n) {
        const double m = n - M / 2.0;
        const double sinc = (m == 0.0) ? 2.0 * fc : std::sin(2.0 * kPi * fc * m) / (kPi * m);
        const double w = 0.54 - 0.46 * std::cos(2.0 * kPi * n / M);
        h[static_cast<std::size_t>(n)] = static_cast<float>(sinc * w);
        sum += sinc * w;
    }
    if (sum != 0.0)
        for (auto& v : h) v = static_cast<float>(v / sum);
    return h;
}

PagerFmDemodulator::PagerFmDemodulator(const PagerDemodConfig& cfg) : cfg_(cfg) {
    // Decimate to at or just above 2x the output rate, leaving the linear
    // resampler (after the audio low-pass) a clean 2:1-ish final step.
    const double target = cfg_.output_sample_rate_hz * 2.0;
    decim_ = std::max(1, static_cast<int>(cfg_.input_sample_rate_hz / target));

    set_freq_offset(cfg_.freq_offset_hz);

    const double cutoff = std::min(cfg_.channel_bandwidth_hz * 0.6, cfg_.input_sample_rate_hz * 0.45);
    taps_ = pager_design_lowpass_fir(cfg_.input_sample_rate_hz, cutoff, cfg_.fir_taps);
    fir_history_.assign(taps_.size() - 1, cf32{0.0f, 0.0f});

    // The audio cutoff must sit below the OUTPUT Nyquist or the resample
    // aliases; 7 kHz passes FLEX 3200-baud 4-level and POCSAG 2400 with room.
    const double fs_dec = decimated_rate_hz();
    const double acut = std::min(cfg_.audio_cutoff_hz,
                                 std::min(cfg_.output_sample_rate_hz, fs_dec) * 0.45);
    audio_taps_ = pager_design_lowpass_fir(fs_dec, acut, cfg_.audio_taps);
    audio_history_.assign(audio_taps_.size() - 1, 0.0f);
}

void PagerFmDemodulator::set_freq_offset(double hz) {
    cfg_.freq_offset_hz = hz;
    afc_correction_hz_ = 0.0;
    apply_nco_frequency();
}

void PagerFmDemodulator::apply_nco_frequency() {
    // Negative increment: a channel at +f is brought to baseband by
    // multiplying with e^{-j2*pi*f*t}.
    const double effective_hz = cfg_.freq_offset_hz + afc_correction_hz_;
    nco_incr_ = (std::fabs(effective_hz) < kNcoDeadbandHz)
                    ? 0.0
                    : -2.0 * kPi * effective_hz / cfg_.input_sample_rate_hz;
}

void PagerFmDemodulator::process(const cf32* in, std::size_t n, std::vector<int16_t>& out) {
    mix_and_filter_decimate(in, n);
    demod_block();
    if (cfg_.afc_enabled) afc_update(); // measures raw discriminator stats
    audio_filter();
    resample_to_output();

    // rad/sample -> Hz -> counts. Normalizing by the decimated rate keeps the
    // PCM level (and so multimon-ng's slicer input) the same for any IQ rate.
    const double hz_per_rad = decimated_rate_hz() / (2.0 * kPi);
    float scale = static_cast<float>(hz_per_rad / cfg_.full_scale_deviation_hz * 16384.0) * cfg_.gain;
    if (cfg_.invert) scale = -scale;

    out.reserve(out.size() + disc_out_.size());
    for (float v : disc_out_) {
        float s = v * scale;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        out.push_back(static_cast<int16_t>(s));
    }
}

void PagerFmDemodulator::mix_and_filter_decimate(const cf32* in, std::size_t n) {
    mixed_.resize(n);
    if (nco_incr_ != 0.0) {
        // Phasor rotator (one complex multiply per sample instead of cos+sin),
        // in double so rounding drift over a block is negligible; re-anchored
        // from the exact phase at every block start.
        const std::complex<double> step(std::cos(nco_incr_), std::sin(nco_incr_));
        std::complex<double> rot(std::cos(nco_phase_), std::sin(nco_phase_));
        for (std::size_t i = 0; i < n; ++i) {
            mixed_[i] = in[i] * cf32(static_cast<float>(rot.real()), static_cast<float>(rot.imag()));
            rot *= step;
        }
        nco_phase_ = std::remainder(nco_phase_ + nco_incr_ * static_cast<double>(n), 2.0 * kPi);
    } else {
        std::copy(in, in + n, mixed_.begin());
    }

    // History followed by new samples so the convolution needs no per-sample
    // edge handling. `phase` carries the decimation phase across blocks so the
    // output grid stays uniform when n isn't a multiple of decim_.
    const std::size_t taps = taps_.size();
    const std::size_t hist = fir_history_.size();
    buf_.clear();
    buf_.insert(buf_.end(), fir_history_.begin(), fir_history_.end());
    buf_.insert(buf_.end(), mixed_.begin(), mixed_.end());

    decimated_.clear();
    std::size_t i = hist + dec_phase_;
    for (; i < buf_.size(); i += static_cast<std::size_t>(decim_)) {
        cf32 acc{0.0f, 0.0f};
        const cf32* x = &buf_[i + 1 - taps];
        for (std::size_t t = 0; t < taps; ++t) acc += x[t] * taps_[t];
        decimated_.push_back(acc);
    }
    dec_phase_ = i - buf_.size();

    std::copy(buf_.end() - static_cast<long>(hist), buf_.end(), fir_history_.begin());
}

void PagerFmDemodulator::demod_block() {
    disc_out_.clear();
    disc_out_.reserve(decimated_.size());
    for (const cf32& s : decimated_) {
        const cf32 prod = s * std::conj(last_sample_);
        disc_out_.push_back(std::atan2(prod.imag(), prod.real()));
        last_sample_ = s;
    }
}

void PagerFmDemodulator::afc_update() {
    const std::size_t n = disc_out_.size();
    if (n < 32) return;

    const double fs = decimated_rate_hz();
    const double rad_to_hz = fs / (2.0 * kPi);
    double sum = 0.0, sum2 = 0.0;
    for (float v : disc_out_) { sum += v; sum2 += static_cast<double>(v) * v; }
    const double mean_rad = sum / static_cast<double>(n);
    const double var_rad = sum2 / static_cast<double>(n) - mean_rad * mean_rad;
    const double mean_hz = mean_rad * rad_to_hz;
    const double var_hz2 = var_rad * rad_to_hz * rad_to_hz;

    constexpr double kSignalVarGateHz2 = 49.0e6; // (7 kHz)^2 -- see header
    if (var_hz2 > kSignalVarGateHz2) return;

    double g = (static_cast<double>(n) / fs) / cfg_.afc_time_constant_s;
    if (g > 0.5) g = 0.5;
    afc_correction_hz_ = std::clamp(afc_correction_hz_ + g * mean_hz,
                                    -cfg_.afc_max_correction_hz, cfg_.afc_max_correction_hz);
    apply_nco_frequency();
}

void PagerFmDemodulator::audio_filter() {
    const std::size_t taps = audio_taps_.size();
    const std::size_t hist = audio_history_.size();
    audio_buf_.clear();
    audio_buf_.insert(audio_buf_.end(), audio_history_.begin(), audio_history_.end());
    audio_buf_.insert(audio_buf_.end(), disc_out_.begin(), disc_out_.end());

    scratch_.clear();
    scratch_.reserve(disc_out_.size());
    for (std::size_t i = hist; i < audio_buf_.size(); ++i) {
        const float* x = &audio_buf_[i + 1 - taps];
        float acc = 0.0f;
        for (std::size_t t = 0; t < taps; ++t) acc += x[t] * audio_taps_[t];
        scratch_.push_back(acc);
    }
    std::copy(audio_buf_.end() - static_cast<long>(hist), audio_buf_.end(), audio_history_.begin());
    disc_out_.swap(scratch_);
}

void PagerFmDemodulator::resample_to_output() {
    // Linear interpolation from the decimated rate to exactly the output rate.
    // resample_pos_ is in input samples relative to the current block; index
    // -1 is resample_prev_, the last sample of the previous block.
    const std::size_t N = disc_out_.size();
    const double step = decimated_rate_hz() / cfg_.output_sample_rate_hz;

    scratch_.clear();
    while (true) {
        const long i0 = static_cast<long>(std::floor(resample_pos_));
        const long i1 = i0 + 1;
        if (i1 >= static_cast<long>(N) || i0 < -1) break;
        const float s0 = i0 < 0 ? resample_prev_ : disc_out_[static_cast<std::size_t>(i0)];
        const float s1 = disc_out_[static_cast<std::size_t>(i1)];
        const double frac = resample_pos_ - static_cast<double>(i0);
        scratch_.push_back(static_cast<float>(s0 + (s1 - s0) * frac));
        resample_pos_ += step;
    }
    if (N > 0) resample_prev_ = disc_out_[N - 1];
    resample_pos_ -= static_cast<double>(N);
    disc_out_.swap(scratch_);
}

} // namespace dsdsrv
