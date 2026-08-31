#include "tetrapol_demod.hpp"

#include <algorithm>
#include <cmath>

namespace dsdsrv {

namespace {
constexpr float kPi = 3.14159265358979323846f;

// 4-point cubic (Catmull-Rom) interpolation of a REAL signal at fractional
// offset mu in [0,1) between a[base] and a[base+1]; caller guarantees
// base-1 .. base+2 are in range.
float cubic_interp(const float* a, int base, float mu) {
    const float x0 = a[base - 1], x1 = a[base], x2 = a[base + 1], x3 = a[base + 2];
    const float c0 = x1;
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * mu + c2) * mu + c1) * mu + c0;
}

// Windowed-sinc (Hamming) low-pass, unit DC gain. `fc` is the single-sided
// cutoff in cycles/sample.
std::vector<float> make_lowpass(int ntaps, float fc) {
    std::vector<float> h(ntaps);
    const int m = ntaps - 1;
    float sum = 0.0f;
    for (int i = 0; i < ntaps; ++i) {
        const float t = static_cast<float>(i) - m / 2.0f;
        const float sinc = (t == 0.0f) ? 2.0f * fc
                                       : std::sin(2.0f * kPi * fc * t) / (kPi * t);
        const float win = 0.54f - 0.46f * std::cos(2.0f * kPi * i / m); // Hamming
        h[i] = sinc * win;
        sum += h[i];
    }
    for (float& v : h) v /= sum; // normalise to unit DC gain
    return h;
}
} // namespace

TetrapolGmskDemod::TetrapolGmskDemod(const TetrapolDemodConfig& cfg) : cfg_(cfg) {
    if (cfg_.channel_lowpass_symbols > 0.0f && cfg_.channel_lowpass_span > 0) {
        const int ntaps = 2 * cfg_.channel_lowpass_span * cfg_.samples_per_symbol + 1;
        const float fc = cfg_.channel_lowpass_symbols / cfg_.samples_per_symbol; // cycles/sample
        lp_taps_ = make_lowpass(ntaps, fc);
    }
}

std::vector<unsigned char>
TetrapolGmskDemod::run(const std::complex<float>* iq, std::size_t n, bool final_flush) {
    std::vector<unsigned char> bits;
    const int sps = cfg_.samples_per_symbol;
    const double halfsym = sps / 2.0;

    // 0. Pre-detection channel low-pass (streaming FIR with carried history):
    //    band-limit the IQ to the GMSK channel so the discriminator isn't fed
    //    out-of-band noise. Constant group delay -> the timing loop is
    //    unaffected. With no taps, pass the IQ through unchanged.
    std::vector<std::complex<float>> filt;
    if (lp_taps_.empty()) {
        filt.assign(iq, iq + n);
    } else {
        const int nt = static_cast<int>(lp_taps_.size());
        if (static_cast<int>(iq_hist_.size()) != nt - 1)
            iq_hist_.assign(nt - 1, std::complex<float>(0.0f, 0.0f)); // zero-pad at stream start
        std::vector<std::complex<float>> ext;
        ext.reserve(iq_hist_.size() + n);
        ext.insert(ext.end(), iq_hist_.begin(), iq_hist_.end());
        ext.insert(ext.end(), iq, iq + n);
        filt.resize(n);
        for (std::size_t j = 0; j < n; ++j) {
            std::complex<float> acc(0.0f, 0.0f);
            for (int k = 0; k < nt; ++k) acc += lp_taps_[k] * ext[j + k];
            filt[j] = acc;
        }
        // Carry the last nt-1 samples for the next block's overlap.
        const std::size_t keep = static_cast<std::size_t>(nt - 1);
        iq_hist_.assign(ext.end() - static_cast<std::ptrdiff_t>(keep), ext.end());
    }

    // 1. Quadrature (FM) discriminator: d[i] = arg(x[i]·conj(x[i-1])) on the
    //    filtered IQ. The carried prev_iq_ makes the transition across a block
    //    boundary correct.
    std::vector<float> nd;
    nd.reserve(filt.size());
    for (const std::complex<float>& cur : filt) {
        if (have_prev_iq_) {
            const std::complex<float> p = cur * std::conj(prev_iq_);
            nd.push_back(std::atan2(p.imag(), p.real()));
        }
        prev_iq_ = cur;
        have_prev_iq_ = true;
    }

    if (!nd.empty()) {
        // 2. DC removal (cancels a residual CFO's constant discriminator bias),
        //    a per-sample IIR so the result is identical regardless of how the
        //    stream is chunked. Amplitude normalisation (AGC) follows; it only
        //    scales, so it can stay a cheap per-block EMA (sign, hence the bit,
        //    is unaffected).
        if (cfg_.dc_removal_symbols > 0.0f) {
            const float alpha = 1.0f / (cfg_.dc_removal_symbols * sps);
            for (float& v : nd) {
                if (!dc_init_) { dc_ = v; dc_init_ = true; }
                const float x = v;
                v = x - dc_;
                dc_ += alpha * (x - dc_);
            }
        }
        double pow = 0.0;
        for (float v : nd) pow += static_cast<double>(v) * v;
        pow /= static_cast<double>(nd.size());
        const float rms = (pow > 1e-20) ? static_cast<float>(std::sqrt(pow)) : 1.0f;
        const float beta_a = std::min(1.0f, static_cast<float>(nd.size()) / (32.0f * sps));
        if (!agc_init_) { agc_ = rms; agc_init_ = true; }
        else agc_ += beta_a * (rms - agc_);
        const float g = (agc_ > 1e-12f) ? 1.0f / agc_ : 1.0f;
        for (float& v : nd) v *= g;
    }

    // 3. Work buffer: carried (already-processed) tail + this block's samples.
    std::vector<float> work;
    work.reserve(disc_tail_.size() + nd.size());
    work.insert(work.end(), disc_tail_.begin(), disc_tail_.end());
    work.insert(work.end(), nd.begin(), nd.end());
    const int W = static_cast<int>(work.size());
    symbols_.clear();
    if (W == 0) return bits;

    auto interp_at = [&](double q) -> float {
        int base = static_cast<int>(std::floor(q));
        float mu = static_cast<float>(q - base);
        return cubic_interp(work.data(), base, mu);
    };

    // Second-order (PI) timing loop gains.
    const float Bn = cfg_.timing_loop_bandwidth;
    const float zeta = 1.0f;
    const float Kp = 2.0f * zeta * Bn;
    const float Ki = Bn * Bn;

    // Cubic needs base-1..base+2 in range; hold back a small end margin (needs
    // future context) unless flushing.
    const int endmargin = final_flush ? 0 : 2;

    if (!timing_started_) {
        const double seedpos = 2.0 * sps;
        if (seedpos - halfsym - 1.0 < 1.0 ||
            seedpos + sps + 2.0 >= static_cast<double>(W - endmargin)) {
            disc_tail_ = std::move(work); // not enough yet; wait for more input
            if (final_flush) disc_tail_.clear();
            return bits;
        }
        pos_ = seedpos;
        integ_ = 0.0f;
        prev_on_ = interp_at(pos_);
        timing_started_ = true;
    }

    while (true) {
        const double next = pos_ + sps;
        if (next - halfsym - 1.0 < 1.0 ||
            next + 2.0 >= static_cast<double>(W - endmargin)) break;

        const float on = interp_at(next);
        const float mid = interp_at(next - halfsym);

        // Gardner TED for a real bipolar signal: e = mid·(on − prev_on).
        // Negative feedback nudges the next sampling instant.
        const float e = mid * (on - prev_on_);
        integ_ += Ki * e;
        const float correction = Kp * e + integ_;
        pos_ = next - std::max(-halfsym, std::min<double>(halfsym, correction));

        symbols_.push_back(on);
        const unsigned char bit = ((on > 0.0f) != cfg_.invert) ? 1u : 0u;
        bits.push_back(bit);
        prev_on_ = on;
    }

    // Retain a tail for next call (or clear on flush): keep a small margin
    // before pos_ (interpolator look-back) through the end, then rebase pos_.
    if (final_flush) {
        disc_tail_.clear();
    } else {
        int drop = static_cast<int>(std::floor(pos_)) - 3;
        if (drop < 0) drop = 0;
        if (drop > W) drop = W;
        disc_tail_.assign(work.begin() + drop, work.end());
        pos_ -= drop;
    }
    return bits;
}

std::vector<unsigned char>
TetrapolGmskDemod::demodulate(const std::complex<float>* iq, std::size_t n) {
    return run(iq, n, /*final_flush=*/false);
}

std::vector<unsigned char> TetrapolGmskDemod::flush() {
    return run(nullptr, 0, /*final_flush=*/true);
}

void TetrapolGmskDemod::reset() {
    symbols_.clear();
    iq_hist_.clear();
    prev_iq_ = {0.0f, 0.0f};
    have_prev_iq_ = false;
    disc_tail_.clear();
    pos_ = 0.0;
    integ_ = 0.0f;
    prev_on_ = 0.0f;
    timing_started_ = false;
    dc_ = 0.0f;
    dc_init_ = false;
    agc_ = 1.0f;
    agc_init_ = false;
}

} // namespace dsdsrv
