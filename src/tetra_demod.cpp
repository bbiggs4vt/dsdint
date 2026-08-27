#include "tetra_demod.hpp"

#include <algorithm>
#include <cmath>

namespace dsdsrv {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Root-raised-cosine taps for `sps` samples/symbol, roll-off `alpha`, and a
// one-sided span of `span` symbols (total 2*span*sps + 1 taps). Normalised
// to unit energy. Standard closed form with the two removable
// singularities handled explicitly.
std::vector<float> make_rrc(int sps, float alpha, int span) {
    const int half = span * sps;
    std::vector<float> h(2 * half + 1);
    const float a = alpha;
    for (int i = -half; i <= half; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(sps); // time in symbols
        float v;
        if (i == 0) {
            v = 1.0f + a * (4.0f / kPi - 1.0f);
        } else if (a > 0.0f && std::fabs(std::fabs(4.0f * a * x) - 1.0f) < 1e-4f) {
            // x = ±1/(4α)
            v = (a / std::sqrt(2.0f)) *
                ((1.0f + 2.0f / kPi) * std::sin(kPi / (4.0f * a)) +
                 (1.0f - 2.0f / kPi) * std::cos(kPi / (4.0f * a)));
        } else {
            const float num = std::sin(kPi * x * (1.0f - a)) +
                              4.0f * a * x * std::cos(kPi * x * (1.0f + a));
            const float den = kPi * x * (1.0f - (4.0f * a * x) * (4.0f * a * x));
            v = num / den;
        }
        h[i + half] = v;
    }
    float energy = 0.0f;
    for (float t : h) energy += t * t;
    const float scale = 1.0f / std::sqrt(energy);
    for (float& t : h) t *= scale;
    return h;
}

// 4-point cubic (Catmull-Rom) interpolation at fractional offset mu in
// [0,1) between a[base] and a[base+1]; caller guarantees base-1 .. base+2
// are in range.
std::complex<float> cubic_interp(const std::complex<float>* a, int base, float mu) {
    const std::complex<float> x0 = a[base - 1], x1 = a[base], x2 = a[base + 1], x3 = a[base + 2];
    const std::complex<float> c0 = x1;
    const std::complex<float> c1 = 0.5f * (x2 - x0);
    const std::complex<float> c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const std::complex<float> c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * mu + c2) * mu + c1) * mu + c0;
}

} // namespace

TetraDpqskDemod::TetraDpqskDemod(const TetraDemodConfig& cfg) : cfg_(cfg) {
    rrc_taps_ = make_rrc(cfg_.samples_per_symbol, cfg_.rrc_alpha, cfg_.rrc_span_symbols);
}

std::vector<std::complex<float>>
TetraDpqskDemod::matched_filter(const std::complex<float>* iq, std::size_t n) const {
    const int ntaps = static_cast<int>(rrc_taps_.size());
    const int delay = ntaps / 2;
    std::vector<std::complex<float>> out(n);
    // 'valid'-centred convolution: out[i] aligns the filter's centre tap on
    // input i, so the group delay is removed and out[i] tracks input i.
    for (std::size_t i = 0; i < n; ++i) {
        std::complex<float> acc(0.0f, 0.0f);
        for (int k = 0; k < ntaps; ++k) {
            const long idx = static_cast<long>(i) + k - delay;
            if (idx >= 0 && idx < static_cast<long>(n))
                acc += rrc_taps_[k] * iq[idx];
        }
        out[i] = acc;
    }
    return out;
}

void TetraDpqskDemod::recover_symbols(const std::vector<std::complex<float>>& mf) {
    symbols_.clear();
    const int sps = cfg_.samples_per_symbol;
    const int M = static_cast<int>(mf.size());
    if (M < 4 * sps) return;

    // AGC: scale to unit RMS so the Gardner detector gain is predictable
    // (its output scales with signal power, and the loop gains below assume
    // O(1) amplitude).
    double p = 0.0;
    for (const auto& s : mf) p += std::norm(s);
    const float rms = static_cast<float>(std::sqrt(p / std::max(1, M)));
    const float ag = (rms > 1e-12f) ? 1.0f / rms : 1.0f;
    std::vector<std::complex<float>> x(M);
    for (int i = 0; i < M; ++i) x[i] = mf[i] * ag;

    // Second-order (PI) timing loop. Overdamped, gains scaled by the
    // normalised loop bandwidth. detector gain is ~1 after the AGC above.
    const float Bn = cfg_.timing_loop_bandwidth;
    const float zeta = 1.0f;
    const float Kp = 2.0f * zeta * Bn;
    const float Ki = Bn * Bn;

    // Read position (fractional) into x[]. Start with enough history for the
    // interpolator (base-1) and the half-symbol look-back for the TED.
    double pos = 2.0 * sps;
    const double halfsym = sps / 2.0;
    float integ = 0.0f;

    auto interp_at = [&](double q) -> std::complex<float> {
        int base = static_cast<int>(std::floor(q));
        float mu = static_cast<float>(q - base);
        return cubic_interp(x.data(), base, mu);
    };

    std::complex<float> prev_on = interp_at(pos);
    symbols_.push_back(prev_on);

    while (true) {
        const double next = pos + sps;
        // Need base-1 .. base+2 for both the on-time sample at `next` and
        // the mid sample at next-halfsym; stop before running off the end.
        if (next - halfsym - 1.0 < 1.0 || next + 2.0 >= M) break;

        const std::complex<float> on = interp_at(next);
        const std::complex<float> mid = interp_at(next - halfsym);

        // Gardner TED (complex, non-data-aided): zero at the correct
        // sampling instant. Then a PI update nudges the next step.
        const float e = std::real(std::conj(mid) * (on - prev_on));
        integ += Ki * e;
        const float correction = Kp * e + integ;

        // Negative feedback: a positive Gardner error means we sampled late,
        // so retard the next instant. (The opposite sign is positive
        // feedback and the loop diverges as the gain rises.)
        pos = next - std::max(-halfsym, std::min<double>(halfsym, correction));
        symbols_.push_back(on);
        prev_on = on;
    }
}

std::vector<unsigned char>
TetraDpqskDemod::demodulate(const std::complex<float>* iq, std::size_t n) {
    std::vector<unsigned char> bits;
    if (n == 0) return bits;

    const std::vector<std::complex<float>> mf = matched_filter(iq, n);
    recover_symbols(mf);

    // Differential detection: the angle of s[k]·conj(s[k-1]) is the
    // π/4-DQPSK phase transition. Map per the TETRA table (see header):
    //   sign  bit B(2k-1) = 1 when the transition is negative,
    //   mag   bit B(2k)   = 1 when |transition| is 3π/4 (vs π/4).
    bits.reserve(symbols_.size() * 2);
    for (std::size_t k = 1; k < symbols_.size(); ++k) {
        const std::complex<float> d = symbols_[k] * std::conj(symbols_[k - 1]);
        const float ang = std::atan2(std::imag(d), std::real(d));
        const unsigned char b1 = (ang < 0.0f) ? 1u : 0u;
        const unsigned char b2 = (std::fabs(ang) > (kPi / 2.0f)) ? 1u : 0u;
        bits.push_back(b1);
        bits.push_back(b2);
    }
    return bits;
}

} // namespace dsdsrv
