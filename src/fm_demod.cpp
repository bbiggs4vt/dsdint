#include "fm_demod.hpp"

#include <cmath>
#include <algorithm>

namespace dsdsrv {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Windowed-sinc low-pass FIR design (Hamming window). Good enough for
// channel selection ahead of an FM discriminator; not a linear-phase
// concern here since only magnitude response matters for this use.
std::vector<float> design_lowpass_fir(double sample_rate, double cutoff_hz, int taps) {
    if (taps % 2 == 0) taps += 1; // force odd length (Type I FIR)
    std::vector<float> h(taps);
    const int M = taps - 1;
    const double fc = cutoff_hz / sample_rate; // normalized cutoff (0..0.5)

    double sum = 0.0;
    for (int n = 0; n < taps; ++n) {
        const double m = n - M / 2.0;
        double sinc;
        if (m == 0.0) {
            sinc = 2.0 * fc;
        } else {
            sinc = std::sin(2.0 * kPi * fc * m) / (kPi * m);
        }
        const double w = 0.54 - 0.46 * std::cos(2.0 * kPi * n / M); // Hamming
        double v = sinc * w;
        h[n] = static_cast<float>(v);
        sum += v;
    }
    // Normalize DC gain to 1.0
    if (sum != 0.0) {
        for (auto& v : h) v = static_cast<float>(v / sum);
    }
    return h;
}
} // namespace

FmDemodulator::FmDemodulator(const FmDemodConfig& cfg) : cfg_(cfg) {
    // Pick an integer decimation factor that brings us close to, but not
    // below, the target output rate multiplied by a small oversample
    // factor (2x) so the later linear resampler has margin to work with.
    const double target = cfg_.output_sample_rate_hz * 2.0;
    decim_ = std::max(1, static_cast<int>(cfg_.input_sample_rate_hz / target));

    set_freq_offset(cfg_.freq_offset_hz);
    design_lowpass();

    fir_history_.assign(taps_.size() - 1, cf32{0.0f, 0.0f});
}

void FmDemodulator::set_freq_offset(double hz) {
    cfg_.freq_offset_hz = hz;
    afc_correction_hz_ = 0.0; // explicit retune invalidates old correction
    apply_nco_frequency();
}

// Effective NCO frequencies below this are treated as exactly zero, so
// the per-sample mix (cos/sin at the FULL input rate -- measured at
// ~2.4x the whole demod's cost) stays off when there is nothing worth
// correcting. The threshold is chosen to be harmless by a wide margin:
// a 10 Hz residual is ~34 counts of DC at the default gain, noise-level
// against the ~2200-count inner 4FSK symbol spacing, and two orders of
// magnitude inside the measured +/-1 kHz decode tolerance. This matters
// specifically for AFC: its correction settles at some tiny nonzero
// value on a well-centered signal, and without the deadband that alone
// kept the NCO permanently on (52x -> 21x realtime for zero benefit).
// Same constant in fm_demod_liquid.cpp.
static constexpr double kNcoDeadbandHz = 10.0;

void FmDemodulator::apply_nco_frequency() {
    // NEGATIVE increment: a channel at +f is brought to baseband by
    // multiplying with e^{-j2*pi*f*t}. The original code used a positive
    // increment, which silently meant the OPPOSITE sign convention from
    // FmDemodulatorLiquid (whose nco mix-down matched the documented
    // "channel sits at +freq_offset" meaning) -- the same start message
    // with a nonzero offset behaved oppositely between dsd-server and
    // dsd-server-liquid. Pinned by test_afc.cpp for both classes.
    const double effective_hz = cfg_.freq_offset_hz + afc_correction_hz_;
    if (std::fabs(effective_hz) < kNcoDeadbandHz) {
        nco_incr_ = 0.0; // mix_and_filter_decimate skips the NCO entirely
    } else {
        nco_incr_ = -2.0 * kPi * effective_hz / cfg_.input_sample_rate_hz;
    }
}

void FmDemodulator::design_lowpass() {
    // Cutoff a bit past the channel half-bandwidth to leave transition
    // band room for the Hamming window's roll-off.
    const double cutoff = std::min(cfg_.channel_bandwidth_hz * 0.6,
                                    cfg_.input_sample_rate_hz * 0.45);
    taps_ = design_lowpass_fir(cfg_.input_sample_rate_hz, cutoff, cfg_.fir_taps);
}

void FmDemodulator::process(const cf32* in, std::size_t n, std::vector<int16_t>& out) {
    mix_and_filter_decimate(in, n);
    demod_block();
    if (cfg_.afc_enabled) afc_update(); // reads disc_out_ at the decimated rate
    resample_to_output();

    out.reserve(out.size() + disc_out_.size());
    for (float v : disc_out_) {
        float scaled = v * cfg_.disc_gain;
        if (scaled > 32767.0f) scaled = 32767.0f;
        if (scaled < -32768.0f) scaled = -32768.0f;
        out.push_back(static_cast<int16_t>(scaled));
    }
}

void FmDemodulator::mix_and_filter_decimate(const cf32* in, std::size_t n) {
    // 1) Optional NCO mix (frequency shift) into scratch buffer.
    mixed_.resize(n);
    if (nco_incr_ != 0.0) {
        for (std::size_t i = 0; i < n; ++i) {
            const float c = static_cast<float>(std::cos(nco_phase_));
            const float s = static_cast<float>(std::sin(nco_phase_));
            const cf32 shift{c, s};
            mixed_[i] = in[i] * shift;
            nco_phase_ += nco_incr_;
            if (nco_phase_ > kPi) nco_phase_ -= 2.0 * kPi;
            if (nco_phase_ < -kPi) nco_phase_ += 2.0 * kPi;
        }
    } else {
        std::copy(in, in + n, mixed_.begin());
    }

    // 2) FIR filter with carried-over history, 3) decimate.
    const std::size_t taps = taps_.size();
    const std::size_t hist = fir_history_.size(); // taps - 1

    // Build a contiguous view: history followed by new samples so the
    // convolution can run without branchy edge handling per-sample.
    std::vector<cf32> buf;
    buf.reserve(hist + n);
    buf.insert(buf.end(), fir_history_.begin(), fir_history_.end());
    buf.insert(buf.end(), mixed_.begin(), mixed_.end());

    decimated_.clear();
    decimated_.reserve(n / decim_ + 1);

    // Output sample for center index i (0-based into `buf`, i >= taps-1)
    // corresponds to convolving buf[i-taps+1 .. i] with taps_.
    for (std::size_t i = hist; i < buf.size(); i += static_cast<std::size_t>(decim_)) {
        cf32 acc{0.0f, 0.0f};
        // taps_[0] pairs with the oldest sample in the window.
        for (std::size_t t = 0; t < taps; ++t) {
            acc += buf[i - taps + 1 + t] * taps_[t];
        }
        decimated_.push_back(acc);
    }

    // Carry the last (taps-1) samples forward as history for next call.
    if (buf.size() >= hist) {
        std::copy(buf.end() - static_cast<long>(hist), buf.end(), fir_history_.begin());
    }
}

void FmDemodulator::demod_block() {
    disc_out_.clear();
    disc_out_.reserve(decimated_.size());
    for (const cf32& s : decimated_) {
        const cf32 prod = s * std::conj(last_sample_);
        const float angle = std::atan2(prod.imag(), prod.real());
        disc_out_.push_back(angle);
        last_sample_ = s;
    }
}

void FmDemodulator::afc_update() {
    // The discriminator output is instantaneous frequency: each sample
    // is 2*pi*f_inst/decimated_rate radians. Its mean over a block is
    // therefore the residual carrier offset -- the quantity the NCO
    // failed to remove -- and its variance separates signal from
    // no-signal noise:
    //
    //   DMR 4FSK symbols sit at +/-648 and +/-1944 Hz, so a real signal
    //   block has a frequency std deviation of ~1.5 kHz. Noise-only
    //   input has near-uniform random phase steps: std ~ fs/(2*pi*sqrt(3))
    //   which is ~13.8 kHz even at a 48 kHz discriminator rate. The
    //   variance gate below sits between the two (std 5 kHz), so noise
    //   blocks never move the correction -- without the gate, AFC would
    //   random-walk the NCO whenever the channel goes quiet.
    //
    // The update is a plain first-order integrator: correction +=
    // g * residual, with g = block_duration / time_constant (capped for
    // stability with very large blocks). Since the residual is measured
    // AFTER the correction is applied, this is negative feedback and
    // converges with time constant afc_time_constant_s.
    const std::size_t n = disc_out_.size();
    if (n < 32) return; // too short to estimate anything

    const double fs = decimated_rate_hz();
    const double rad_to_hz = fs / (2.0 * kPi);
    double sum = 0.0, sum2 = 0.0;
    for (float v : disc_out_) { sum += v; sum2 += static_cast<double>(v) * v; }
    const double mean_rad = sum / n;
    const double var_rad = sum2 / n - mean_rad * mean_rad;

    const double mean_hz = mean_rad * rad_to_hz;
    const double var_hz2 = var_rad * rad_to_hz * rad_to_hz;

    constexpr double kSignalVarGateHz2 = 25.0e6; // (5 kHz)^2 -- see above
    if (var_hz2 > kSignalVarGateHz2) return;     // no signal: hold

    double g = (n / fs) / cfg_.afc_time_constant_s;
    if (g > 0.5) g = 0.5; // stability cap for blocks comparable to the time constant

    afc_correction_hz_ += g * mean_hz;
    if (afc_correction_hz_ > cfg_.afc_max_correction_hz)
        afc_correction_hz_ = cfg_.afc_max_correction_hz;
    if (afc_correction_hz_ < -cfg_.afc_max_correction_hz)
        afc_correction_hz_ = -cfg_.afc_max_correction_hz;

    apply_nco_frequency();
}

void FmDemodulator::resample_to_output() {
    // Simple linear-interpolation resampler from decimated_rate -> exactly
    // output_sample_rate_hz. Adequate for voice-grade discriminator audio;
    // swap in a polyphase resampler here if you need higher fidelity for
    // marginal-SNR signals.
    //
    // resample_pos_ is tracked in units of "input samples since the start
    // of the current block", and can be fractional. Position -1 refers to
    // resample_prev_ (the last sample carried over from the previous
    // block), which lets us interpolate smoothly across block boundaries.
    const std::size_t N = disc_out_.size();
    const double in_rate = decimated_rate_hz();
    const double step = in_rate / cfg_.output_sample_rate_hz; // input samples per output sample

    auto sample_at = [&](long i) -> float {
        if (i < 0) return resample_prev_;
        return disc_out_[static_cast<std::size_t>(i)];
    };

    std::vector<float> resampled;
    resampled.reserve(static_cast<std::size_t>(N / std::max(0.001, step)) + 2);

    while (true) {
        const double pos = resample_pos_;
        const long i0 = static_cast<long>(std::floor(pos));
        const long i1 = i0 + 1;
        // Need both i0 and i1 available: i0 >= -1 (always true once we
        // have a prev sample) and i1 <= N-1.
        if (i1 >= static_cast<long>(N)) break;
        if (i0 < -1) break; // shouldn't happen, but guard anyway

        const float s0 = sample_at(i0);
        const float s1 = sample_at(i1);
        const double frac = pos - static_cast<double>(i0);
        resampled.push_back(static_cast<float>(s0 + (s1 - s0) * frac));
        resample_pos_ += step;
    }

    if (N > 0) {
        resample_prev_ = disc_out_[N - 1];
        have_prev_ = true;
    }
    // Re-base position relative to the next block, which restarts indexing
    // at 0. Whatever fractional position remains beyond this block carries
    // forward (will typically be < step).
    resample_pos_ -= static_cast<double>(N);

    disc_out_.swap(resampled);
}

} // namespace dsdsrv
