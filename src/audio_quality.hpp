// audio_quality.hpp
//
// Streaming voice-quality / intelligibility analyzer for decoded 8 kHz mono
// PCM (one decoder slot's voice). It accumulates per-frame spectral and
// energy statistics over a call and produces a provisional quality verdict:
// does the decoded audio resemble intelligible speech, or not?
//
// This is Layer 2 of the design in docs/AUDIO_QUALITY_CHECK.md. It measures
// SIGNAL QUALITY ONLY and deliberately does NOT attribute a cause: noise,
// a bad decode, and encrypted-sounds-like-noise are indistinguishable to it
// by design. Its verdict is "usable / not usable", never "encrypted". The
// backend may cross-reference it with the decoder's FEC error rate (Layer 1)
// to tell a noisy link from a clean-link content problem, but that lives
// elsewhere; this unit only looks at the audio.
//
// The primary feature is spectral flatness (Wiener entropy): the ratio of the
// geometric to the arithmetic mean of the power spectrum. Voiced speech is
// tonal/structured -> low flatness; white noise and structureless audio ->
// flatness near 1. Silence is excluded (an energy floor) so it does not skew
// the average. Zero-crossing rate is kept as a secondary diagnostic.
//
// Self-contained (a tiny radix-2 FFT lives here) so it has no dependencies and
// is unit-testable with synthetic signals. Not thread-safe; feed one call's
// audio from one thread. Buffers are sized once so feed() does not allocate.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsdsrv {

class VoiceQuality {
public:
    static constexpr int kRate = 8000;
    static constexpr int kFftSize = 256;   // 32 ms analysis frame at 8 kHz
    static constexpr int kHop = 128;       // 50% overlap -> ~62 frames/sec

    // Energy floor below which a frame is treated as silence and excluded
    // from the spectral statistics. int16 RMS; ~ -46 dBFS. Keeps room tone
    // and inter-word gaps from dragging the flatness average around.
    static constexpr double kSilenceRms = 180.0;

    // Minimum voiced frames before a verdict is anything but Unknown. ~0.5 s
    // of actual voice; shorter bursts do not carry enough to judge.
    static constexpr std::uint32_t kMinVoicedFrames = 30;

    // PROVISIONAL / UNCALIBRATED verdict thresholds on mean spectral flatness.
    // Speech voiced segments sit low (structured); white noise ~1.0. These are
    // first-pass guesses -- see docs/AUDIO_QUALITY_CHECK.md "Calibration". They
    // MUST be re-tuned against an AWGN gradient and a real encrypted capture
    // before the verdict is trusted; until then surface it as uncalibrated.
    static constexpr double kFlatnessGood = 0.22;      // <= this -> speech-like
    static constexpr double kFlatnessUnusable = 0.50;  // >= this -> structureless

    enum class Verdict { Unknown, Good, Marginal, Unusable };

    struct Summary {
        std::uint32_t voiced_frames = 0;   // frames above the silence floor
        std::uint32_t total_frames = 0;    // all analysis frames (incl. silence)
        double mean_flatness = 0.0;        // over voiced frames, [0..1]
        double mean_zcr = 0.0;             // over voiced frames, crossings/sample
        Verdict verdict = Verdict::Unknown;
    };

    VoiceQuality() { reset(); }

    void reset() {
        pending_.clear();
        re_.assign(static_cast<std::size_t>(kFftSize), 0.0f);
        im_.assign(static_cast<std::size_t>(kFftSize), 0.0f);
        if (window_.empty()) build_window();
        voiced_ = 0;
        total_ = 0;
        flat_sum_ = 0.0;
        zcr_sum_ = 0.0;
    }

    // Feed one block of this call's decoded PCM (8 kHz mono int16). Any number
    // of samples; frames are formed internally across calls.
    void feed(const std::int16_t* pcm, std::size_t n) {
        if (!pcm || !n) return;
        pending_.insert(pending_.end(), pcm, pcm + n);
        std::size_t off = 0;
        const std::size_t fft = static_cast<std::size_t>(kFftSize);
        while (pending_.size() - off >= fft) {
            analyze_frame(&pending_[off]);
            off += static_cast<std::size_t>(kHop);
        }
        if (off) pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(off));
        // Cap the carryover so a pathological feed can't grow it without bound.
        if (pending_.size() > fft) pending_.erase(pending_.begin(), pending_.end() - static_cast<std::ptrdiff_t>(fft));
    }

    Summary summary() const {
        Summary s;
        s.total_frames = total_;
        s.voiced_frames = voiced_;
        if (voiced_) {
            s.mean_flatness = flat_sum_ / voiced_;
            s.mean_zcr = zcr_sum_ / voiced_;
        }
        s.verdict = classify(voiced_, s.mean_flatness);
        return s;
    }

    static Verdict classify(std::uint32_t voiced, double mean_flatness) {
        if (voiced < kMinVoicedFrames) return Verdict::Unknown;
        if (mean_flatness <= kFlatnessGood) return Verdict::Good;
        if (mean_flatness >= kFlatnessUnusable) return Verdict::Unusable;
        return Verdict::Marginal;
    }

    static const char* verdict_str(Verdict v) {
        switch (v) {
            case Verdict::Good:     return "good";
            case Verdict::Marginal: return "marginal";
            case Verdict::Unusable: return "unusable";
            case Verdict::Unknown:  break;
        }
        return "unknown";
    }

private:
    void build_window() {
        window_.resize(static_cast<std::size_t>(kFftSize));
        // Hann window to limit spectral leakage so flatness reflects the signal
        // rather than the rectangular-window sinc skirts.
        for (int i = 0; i < kFftSize; ++i)
            window_[static_cast<std::size_t>(i)] =
                0.5f - 0.5f * std::cos(2.0 * kPi * i / (kFftSize - 1));
    }

    void analyze_frame(const std::int16_t* frame) {
        ++total_;
        // RMS + zero-crossing rate on the raw (unwindowed) frame.
        double sumsq = 0.0;
        std::uint32_t crossings = 0;
        for (int i = 0; i < kFftSize; ++i) {
            const double x = frame[i];
            sumsq += x * x;
            if (i && ((frame[i] >= 0) != (frame[i - 1] >= 0))) ++crossings;
        }
        const double rms = std::sqrt(sumsq / kFftSize);
        if (rms < kSilenceRms) return;   // silence: excluded from spectral stats

        // Windowed real FFT -> power spectrum -> spectral flatness.
        for (int i = 0; i < kFftSize; ++i) {
            re_[static_cast<std::size_t>(i)] = static_cast<float>(frame[i]) * window_[static_cast<std::size_t>(i)];
            im_[static_cast<std::size_t>(i)] = 0.0f;
        }
        fft(re_, im_);
        // Bins 1 .. N/2 (skip DC; up to Nyquist). Geometric mean via mean of
        // logs; arithmetic mean of the same bins. flatness = geo / arith.
        double log_sum = 0.0, lin_sum = 0.0;
        int bins = 0;
        const double eps = 1e-9;
        for (int i = 1; i <= kFftSize / 2; ++i) {
            const double p = static_cast<double>(re_[static_cast<std::size_t>(i)]) * re_[static_cast<std::size_t>(i)] +
                             static_cast<double>(im_[static_cast<std::size_t>(i)]) * im_[static_cast<std::size_t>(i)] + eps;
            log_sum += std::log(p);
            lin_sum += p;
            ++bins;
        }
        const double geo = std::exp(log_sum / bins);
        const double arith = lin_sum / bins;
        const double flatness = arith > 0.0 ? std::clamp(geo / arith, 0.0, 1.0) : 0.0;

        ++voiced_;
        flat_sum_ += flatness;
        zcr_sum_ += static_cast<double>(crossings) / kFftSize;
    }

    // In-place iterative radix-2 Cooley-Tukey FFT. kFftSize is a power of two.
    static void fft(std::vector<float>& re, std::vector<float>& im) {
        const int n = static_cast<int>(re.size());
        // Bit-reversal permutation.
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        for (int len = 2; len <= n; len <<= 1) {
            const double ang = -2.0 * kPi / len;
            const float wr = static_cast<float>(std::cos(ang));
            const float wi = static_cast<float>(std::sin(ang));
            for (int i = 0; i < n; i += len) {
                float cur_wr = 1.0f, cur_wi = 0.0f;
                for (int k = 0; k < len / 2; ++k) {
                    const int a = i + k, b = i + k + len / 2;
                    const float tr = re[b] * cur_wr - im[b] * cur_wi;
                    const float ti = re[b] * cur_wi + im[b] * cur_wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr;        im[a] += ti;
                    const float nwr = cur_wr * wr - cur_wi * wi;
                    cur_wi = cur_wr * wi + cur_wi * wr;
                    cur_wr = nwr;
                }
            }
        }
    }

    static constexpr double kPi = 3.14159265358979323846;

    std::vector<std::int16_t> pending_;   // samples not yet consumed into a frame
    std::vector<float> re_, im_;          // FFT scratch
    std::vector<float> window_;           // Hann window

    std::uint32_t voiced_ = 0, total_ = 0;
    double flat_sum_ = 0.0, zcr_sum_ = 0.0;
};

}  // namespace dsdsrv
