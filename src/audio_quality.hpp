// audio_quality.hpp
//
// Per-call voice-quality / intelligibility analyzer for AMBE-based digital
// voice (DMR / NXDN / P25 Phase 2, which use the AMBE+2 3600x2450 vocoder).
//
// It works from the vocoder's per-frame AMBE codewords -- which dsd-fme emits
// with "-Z" as " AMBE <hex> err = [a] [b]" lines, already parsed into voice
// events -- NOT from the decoded PCM. That matters: the AMBE vocoder
// synthesizes speech-SHAPED output (formants, pitch) even from scrambled or
// corrupt parameters, so the PCM of encrypted/garbled audio looks just like
// clear speech (measured: spectral flatness ~0.15 for both). The discriminating
// signal lives in the codeword's b0 pitch index, BEFORE synthesis:
//
//   b0 120-123 -> erasure frame (unrecoverable)   ] "junk": the vocoder got
//   b0 126-127 -> tone frame (out-of-band)        ] parameters it can't use
//   b0 124-125 -> silence / comfort-noise frame   (a natural speech pause)
//   b0  < 120  -> speech frame
//
// Measured on real captures (see docs/AUDIO_QUALITY_CHECK.md "Calibration"):
//   clear speech : silence 4-24%, junk ~0%
//   encrypted    : silence ~0% (no natural pauses), junk up to ~15%
// So a high junk fraction means garbled/unintelligible, and ~0% silence means
// no natural speech pauses (continuous noise/encryption). The per-frame err
// counts are accumulated too (the FEC/RF-quality signal) but do not yet gate
// the verdict -- that needs weak-signal calibration data we do not have.
//
// This measures SIGNAL QUALITY ONLY and never attributes a cause: a weak link,
// a bad decode, and encryption can all drive the same numbers. Its verdict is
// "usable / not usable", never "encrypted". Not thread-safe; feed one call's
// frames from one thread (the model holds its lock).

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace dsdsrv {

class VoiceQuality {
public:
    enum class Frame { Speech, Silence, Erasure, Tone };
    enum class Verdict { Unknown, Good, Marginal, Unusable };

    // Minimum AMBE frames before a verdict is anything but Unknown (~1-2 s of
    // voice). Drops the short noise fragments (call edges, bad syncs) that the
    // captures showed as ~18-frame bursts with erratic stats.
    static constexpr std::uint32_t kMinFrames = 50;

    // Verdict thresholds, calibrated on real clear vs. encrypted captures.
    // Clear speech measured junk ~0% and silence >= ~4%; the encrypted counting
    // call measured junk ~15%, and continuous encrypted audio measured silence
    // ~0%. See docs/AUDIO_QUALITY_CHECK.md.
    static constexpr double kJunkUnusable = 0.04;   // >= -> garbled / unintelligible
    static constexpr double kJunkMarginal = 0.02;   // >= -> partly garbled
    static constexpr double kSilenceLow   = 0.02;   // <= -> no natural pauses (suspicious)

    struct Summary {
        std::uint32_t frames = 0, speech = 0, silence = 0, erasure = 0, tone = 0;
        double junk = 0.0;            // (erasure + tone) / frames
        double sil = 0.0;             // silence / frames
        double err_per_frame = 0.0;   // mean FEC error count per frame (RF quality)
        Verdict verdict = Verdict::Unknown;
    };

    // The b0 pitch index of an AMBE codeword from dsd-fme's "-Z" hex dump
    // (e.g. "F801A99F8CE080": 56 bits, the top 49 are the codeword). Returns
    // -1 if the hex is empty or unparseable.
    static int b0_of(const std::string& hex) {
        if (hex.empty() || hex.size() > 16) return -1;
        for (char ch : hex)
            if (!std::isxdigit(static_cast<unsigned char>(ch))) return -1;
        const std::uint64_t word = std::strtoull(hex.c_str(), nullptr, 16) >> 7;  // -> 49-bit codeword
        auto bit = [word](int i) { return static_cast<int>((word >> (48 - i)) & 1ULL); };
        return (bit(0) << 6) | (bit(1) << 5) | (bit(2) << 4) | (bit(3) << 3) |
               (bit(37) << 2) | (bit(38) << 1) | bit(39);
    }

    static Frame classify_b0(int b0) {
        if (b0 >= 120 && b0 <= 123) return Frame::Erasure;
        if (b0 == 124 || b0 == 125) return Frame::Silence;
        if (b0 >= 126)              return Frame::Tone;
        return Frame::Speech;
    }

    VoiceQuality() { reset(); }

    void reset() {
        frames_ = speech_ = silence_ = erasure_ = tone_ = 0;
        err_sum_ = 0; err_frames_ = 0;
    }

    // One AMBE voice frame: its b0 pitch index (from b0_of) and its FEC error
    // count (sum of the "err = [a] [b]" pair; pass -1 if not known).
    void feed_frame(int b0, int err) {
        ++frames_;
        switch (classify_b0(b0)) {
            case Frame::Speech:  ++speech_;  break;
            case Frame::Silence: ++silence_; break;
            case Frame::Erasure: ++erasure_; break;
            case Frame::Tone:    ++tone_;    break;
        }
        if (err >= 0) { err_sum_ += static_cast<std::uint64_t>(err); ++err_frames_; }
    }

    Summary summary() const {
        Summary s;
        s.frames = frames_; s.speech = speech_; s.silence = silence_;
        s.erasure = erasure_; s.tone = tone_;
        if (frames_) {
            s.junk = static_cast<double>(erasure_ + tone_) / frames_;
            s.sil = static_cast<double>(silence_) / frames_;
        }
        if (err_frames_) s.err_per_frame = static_cast<double>(err_sum_) / err_frames_;
        s.verdict = classify(frames_, s.junk, s.sil);
        return s;
    }

    static Verdict classify(std::uint32_t frames, double junk, double sil) {
        if (frames < kMinFrames) return Verdict::Unknown;
        if (junk >= kJunkUnusable) return Verdict::Unusable;
        if (junk >= kJunkMarginal || sil <= kSilenceLow) return Verdict::Marginal;
        return Verdict::Good;
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
    std::uint32_t frames_, speech_, silence_, erasure_, tone_;
    std::uint64_t err_sum_;
    std::uint32_t err_frames_;
};

}  // namespace dsdsrv
