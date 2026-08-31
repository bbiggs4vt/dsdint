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

std::vector<unsigned char>
TetraDpqskDemod::run(const std::complex<float>* iq, std::size_t n, bool final_flush) {
    std::vector<unsigned char> bits;
    const int sps = cfg_.samples_per_symbol;
    const int delay = static_cast<int>(rrc_taps_.size() / 2); // = span*sps
    const double halfsym = sps / 2.0;

    // Working buffer: the carried-over tail (filter history + the samples not
    // yet consumed last call) followed by the new input.
    std::vector<std::complex<float>> work;
    work.reserve(in_tail_.size() + n);
    work.insert(work.end(), in_tail_.begin(), in_tail_.end());
    if (n) work.insert(work.end(), iq, iq + n);
    const int W = static_cast<int>(work.size());
    symbols_.clear();
    if (W == 0) return bits;

    // Matched filter over the whole work buffer (valid-centred; the last
    // `delay` outputs are edge-tapered because they lack future input -- we
    // hold those symbols back below unless this is a flush).
    const std::vector<std::complex<float>> mf =
        matched_filter(work.data(), static_cast<std::size_t>(W));

    // Running AGC toward unit RMS. Carrying the level across calls (rather
    // than a per-block RMS) keeps the gain near-constant through the overlap
    // region, so the Gardner detector sees no per-block amplitude step.
    double bp = 0.0;
    for (const auto& s : mf) bp += std::norm(s);
    bp /= std::max(1, W);
    if (!agc_init_) { agc_pow_ = static_cast<float>(bp); agc_init_ = true; }
    else agc_pow_ = 0.9f * agc_pow_ + 0.1f * static_cast<float>(bp);
    const float ag = (agc_pow_ > 1e-12f) ? 1.0f / std::sqrt(agc_pow_) : 1.0f;
    std::vector<std::complex<float>> x(W);
    for (int i = 0; i < W; ++i) x[i] = mf[i] * ag;

    auto interp_at = [&](double q) -> std::complex<float> {
        int base = static_cast<int>(std::floor(q));
        float mu = static_cast<float>(q - base);
        return cubic_interp(x.data(), base, mu);
    };

    // Second-order (PI) timing loop gains; detector gain ~1 after the AGC.
    const float Bn = cfg_.timing_loop_bandwidth;
    const float zeta = 1.0f;
    const float Kp = 2.0f * zeta * Bn;
    const float Ki = Bn * Bn;

    // Hold back the edge-tapered tail (needs future input) unless flushing.
    const int endmargin = final_flush ? 0 : delay;

    // Seed the loop the first time there is enough data to place the
    // interpolator with history on both sides and take one step.
    if (!timing_started_) {
        const double seedpos = 2.0 * sps;
        if (seedpos - halfsym - 1.0 < 1.0 ||
            seedpos + sps + 2.0 >= static_cast<double>(W - endmargin)) {
            // Not enough yet: keep the buffer and wait for more input.
            if (!final_flush) in_tail_ = std::move(work);
            else in_tail_.clear();
            return bits;
        }
        pos_ = seedpos;
        integ_ = 0.0f;
        prev_on_ = interp_at(pos_);
        prev_symbol_ = prev_on_; // differential reference seed
        have_prev_symbol_ = true;
        timing_started_ = true;
    }

    // Recover on-time symbols and their differential products. prev_symbol_
    // carries across calls, so the transition straddling a block boundary is
    // formed normally -- no lost or duplicated symbol at the seam.
    std::vector<std::complex<float>> diff;
    while (true) {
        const double next = pos_ + sps;
        if (next - halfsym - 1.0 < 1.0 ||
            next + 2.0 >= static_cast<double>(W - endmargin)) break;

        const std::complex<float> on = interp_at(next);
        const std::complex<float> mid = interp_at(next - halfsym);

        // Gardner TED (complex, non-data-aided); PI update nudges the next
        // instant. Negative feedback: positive error = sampled late = retard.
        const float e = std::real(std::conj(mid) * (on - prev_on_));
        integ_ += Ki * e;
        const float correction = Kp * e + integ_;
        pos_ = next - std::max(-halfsym, std::min<double>(halfsym, correction));

        symbols_.push_back(on);
        if (have_prev_symbol_) diff.push_back(on * std::conj(prev_symbol_));
        prev_symbol_ = on;
        prev_on_ = on;
    }

    // Running CFO estimate (non-data-aided 4th power: the four transition
    // phases {±π/4, ±3π/4} all map to +π under d^4, so -d^4 = e^{j·8πν}).
    // A decaying accumulator smooths it across calls, so small blocks still
    // give a stable estimate. Unambiguous for |ν| < 1/8 (±Rs/8 = ±2250 Hz).
    if (cfg_.correct_cfo) {
        if (!diff.empty()) {
            std::complex<double> block(0.0, 0.0);
            for (const auto& d : diff) {
                const std::complex<double> dd(d.real(), d.imag());
                const std::complex<double> d4 = dd * dd * dd * dd;
                block += -d4;
            }
            cfo_acc_ = 0.85 * cfo_acc_ + block;
            const double ang8 = std::atan2(cfo_acc_.imag(), cfo_acc_.real()); // = 8πν
            nu_ = static_cast<float>(ang8 / (8.0 * kPi));
        }
        cfo_hz_ = static_cast<double>(nu_) * 18000.0; // ν * Rs
    }
    const float cfo_bias = 2.0f * kPi * nu_; // phase the CFO adds per symbol

    if (cfg_.coherent) {
        // ---- Coherent detection: Costas carrier recovery + differential
        // decode of the hard symbol decisions. ----
        // A decision-directed loop tracks absolute carrier phase; each symbol
        // is de-rotated by the (feed-forward CFO + loop) phase, collapsed onto
        // one QPSK grid, and hard-decided. Differencing consecutive *decisions*
        // (each taken against the clean recovered reference) is what buys the
        // ~2 dB over differencing two noisy samples. The loop's 4-fold lock
        // ambiguity is harmless — a constant rotation cancels in the
        // differential decode — but the even/odd collapse parity does matter
        // (coherent_parity_offset; a real receiver resolves it from the sync).
        const float Bn = 0.02f, zeta = 1.0f;
        const float Kp = 2.0f * zeta * Bn, Ki = Bn * Bn;
        const int paroff = cfg_.coherent_parity_offset & 1;
        auto wrap = [](float a) { while (a > kPi) a -= 2.0f * kPi; while (a < -kPi) a += 2.0f * kPi; return a; };
        bits.reserve(symbols_.size() * 2);
        for (const auto& s : symbols_) {
            coh_cfo_phase_ = std::fmod(coh_cfo_phase_ + cfo_bias, 2.0 * kPi);
            // De-rotate by feed-forward CFO estimate and the Costas phase.
            const float rot = -static_cast<float>(coh_cfo_phase_) - coh_car_phase_;
            const std::complex<float> v = s * std::polar(1.0f, rot);
            // Collapse the two alternating constellations onto one QPSK grid
            // ({0, π/2, π, 3π/2}) by de-rotating the "odd" symbols by −π/4.
            const int parity = (coh_idx_ + paroff) & 1;
            const std::complex<float> c = parity ? v * std::polar(1.0f, -kPi / 4.0f) : v;
            // Decision-directed phase error to the nearest grid point.
            const float ang = std::atan2(c.imag(), c.real());
            const float kq = std::round(ang / (kPi / 2.0f));
            const float perr = wrap(ang - kq * (kPi / 2.0f));
            // PI update of the carrier NCO (phase + per-symbol frequency).
            coh_car_phase_ = wrap(coh_car_phase_ + coh_car_freq_ + Kp * perr);
            coh_car_freq_ += Ki * perr;
            // Hard QPSK index (0..3) -> absolute 8-PSK index (0..7).
            int q = static_cast<int>(kq) & 3;
            const int p = (2 * q + parity) & 7;
            if (coh_have_prev_) {
                const int d = (p - coh_prev_abs_) & 7; // one of {1,3,5,7}
                // Same TETRA transition->bits table as the differential path:
                //   +π/4->(0,0)  +3π/4->(0,1)  −3π/4->(1,1)  −π/4->(1,0)
                unsigned char b1 = 0, b2 = 0;
                switch (d) {
                    case 1: b1 = 0; b2 = 0; break;
                    case 3: b1 = 0; b2 = 1; break;
                    case 5: b1 = 1; b2 = 1; break;
                    case 7: b1 = 1; b2 = 0; break;
                    default: break; // even d only under gross error; emit (0,0)
                }
                bits.push_back(b1);
                bits.push_back(b2);
            }
            coh_prev_abs_ = p;
            coh_have_prev_ = true;
            ++coh_idx_;
        }
    } else {
        // ---- Differential detection (default). Slice each (CFO-corrected)
        // transition per the TETRA table (header):
        //   sign bit B(2k-1) = 1 when the transition is negative,
        //   mag  bit B(2k)   = 1 when |transition| is 3π/4 (vs π/4).
        bits.reserve(diff.size() * 2);
        for (const auto& d : diff) {
            float ang = std::atan2(std::imag(d), std::real(d)) - cfo_bias;
            while (ang > kPi) ang -= 2.0f * kPi;
            while (ang < -kPi) ang += 2.0f * kPi;
            const unsigned char b1 = (ang < 0.0f) ? 1u : 0u;
            const unsigned char b2 = (std::fabs(ang) > (kPi / 2.0f)) ? 1u : 0u;
            bits.push_back(b1);
            bits.push_back(b2);
        }
    }

    // Retain the tail for next call (or clear on flush): keep a small margin
    // before pos_ (interpolator look-back + filter delay) through the end, so
    // the held-back symbols and full filter history are available, then
    // rebase pos_ into the new, shorter buffer.
    if (final_flush) {
        in_tail_.clear();
    } else {
        int drop = static_cast<int>(std::floor(pos_)) - (sps + delay + 2);
        if (drop < 0) drop = 0;
        if (drop > W) drop = W;
        in_tail_.assign(work.begin() + drop, work.end());
        pos_ -= drop;
    }
    return bits;
}

std::vector<unsigned char>
TetraDpqskDemod::demodulate(const std::complex<float>* iq, std::size_t n) {
    return run(iq, n, /*final_flush=*/false);
}

std::vector<unsigned char> TetraDpqskDemod::flush() {
    return run(nullptr, 0, /*final_flush=*/true);
}

void TetraDpqskDemod::reset() {
    in_tail_.clear();
    symbols_.clear();
    pos_ = 0.0;
    integ_ = 0.0f;
    prev_on_ = {0.0f, 0.0f};
    prev_symbol_ = {0.0f, 0.0f};
    timing_started_ = false;
    have_prev_symbol_ = false;
    cfo_acc_ = {0.0, 0.0};
    nu_ = 0.0f;
    cfo_hz_ = 0.0;
    agc_pow_ = 0.0f;
    agc_init_ = false;
    coh_cfo_phase_ = 0.0;
    coh_car_phase_ = 0.0f;
    coh_car_freq_ = 0.0f;
    coh_idx_ = 0;
    coh_prev_abs_ = 0;
    coh_have_prev_ = false;
}

} // namespace dsdsrv
