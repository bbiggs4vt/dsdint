// audio_quality.hpp
//
// Per-call voice-quality / intelligibility analyzer for AMBE-based digital
// voice (DMR / NXDN / P25 Phase 2, the AMBE+2 3600x2450 vocoder).
//
// It works from the vocoder's per-frame AMBE codewords -- which dsd-fme emits
// with "-Z" as " AMBE <hex> err = [a] [b]" lines, already parsed into voice
// events -- NOT from the decoded PCM. The PCM is useless here: the vocoder
// synthesizes speech-SHAPED output even from scrambled parameters, so encrypted
// and clear audio read identically after synthesis (measured: spectral flatness
// ~0.15 for both).
//
// An earlier version classified each frame by its b0 pitch index (speech /
// silence / erasure / tone). That ALSO failed: the b0 of an encrypted frame is
// pseudo-random (content XOR keystream), so encrypted calls landed anywhere --
// some looked like 18% "silence" and read "good". Verified on real captures.
//
// The robust signal is FRAME REPETITION. Clear digital voice repeats the exact
// standard AMBE comfort-noise ("silence") codeword during natural pauses, and
// repeats sustained phonemes frame-to-frame. A cipher XORs each frame, so the
// exact silence codeword NEVER appears in encrypted audio, and scrambled speech
// never repeats frame-to-frame. Measured over clear vs. encrypted captures:
//   clear          : silence-codeword 6-20%, consecutive repeats 8-20%
//   encrypted speech: silence-codeword 0%,    consecutive repeats 0-2%
//   encrypted silence: silence-codeword 0%,   consecutive repeats ~8%
// So: real-silence-codeword present -> clear; absent + no repeats -> scrambled
// speech (unusable); absent but repetitive -> ambiguous (encrypted silence, or
// an unusually continuous talker) -> marginal. The per-frame err counts are
// accumulated as the FEC/RF signal (diagnostic only; not yet in the verdict).
//
// SIGNAL QUALITY ONLY -- never a cause. The verdict is "usable / not usable",
// never "encrypted". Not thread-safe; feed one call's frames from one thread.

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace dsdsrv {

class VoiceQuality {
public:
    enum class Verdict { Unknown, Good, Marginal, Unusable };

    // The standard AMBE+2 comfort-noise / silence codeword, as dsd-fme's "-Z"
    // hex (56 bits). Clear digital voice emits this verbatim during pauses;
    // encryption scrambles it so it never appears.
    static constexpr std::uint64_t kSilenceFrame = 0xF801A99F8CE080ULL;

    // Minimum AMBE frames before a verdict is anything but Unknown (~1-2 s of
    // voice). Drops short noise fragments (call edges, bad syncs).
    static constexpr std::uint32_t kMinFrames = 50;

    // Thresholds, calibrated on real clear vs. encrypted captures.
    static constexpr double kSilenceClear = 0.02;   // >= -> has real comfort-noise -> clear
    static constexpr double kRepeatScrambled = 0.03; // < (when no silence) -> no repeats -> scrambled speech

    struct Summary {
        std::uint32_t frames = 0;
        std::uint32_t silence = 0;        // frames equal to the silence codeword
        std::uint32_t repeats = 0;        // frames equal to the previous frame
        double sil = 0.0;                 // silence / frames
        double rep = 0.0;                 // repeats / frames
        double err_per_frame = 0.0;       // mean FEC errors per frame (RF quality)
        Verdict verdict = Verdict::Unknown;
    };

    // Parse a "-Z" AMBE hex codeword (e.g. "F801A99F8CE080") to its 56-bit
    // value. Returns 0 on empty/invalid (callers gate on the err field instead).
    static std::uint64_t frame_of(const std::string& hex) {
        if (hex.empty() || hex.size() > 16) return 0;
        for (char ch : hex)
            if (!std::isxdigit(static_cast<unsigned char>(ch))) return 0;
        return std::strtoull(hex.c_str(), nullptr, 16);
    }

    VoiceQuality() { reset(); }

    void reset() {
        frames_ = silence_ = repeats_ = 0;
        err_sum_ = 0; err_frames_ = 0;
        prev_ = 0; have_prev_ = false;
    }

    // One AMBE voice frame: its codeword (from frame_of) and FEC error count
    // (sum of the "err = [a] [b]" pair; -1 if not known).
    void feed_frame(std::uint64_t frame, int err) {
        ++frames_;
        if (frame == kSilenceFrame) ++silence_;
        if (have_prev_ && frame == prev_) ++repeats_;
        prev_ = frame; have_prev_ = true;
        if (err >= 0) { err_sum_ += static_cast<std::uint64_t>(err); ++err_frames_; }
    }

    Summary summary() const {
        Summary s;
        s.frames = frames_; s.silence = silence_; s.repeats = repeats_;
        if (frames_) {
            s.sil = static_cast<double>(silence_) / frames_;
            s.rep = static_cast<double>(repeats_) / frames_;
        }
        if (err_frames_) s.err_per_frame = static_cast<double>(err_sum_) / err_frames_;
        s.verdict = classify(frames_, s.sil, s.rep);
        return s;
    }

    static Verdict classify(std::uint32_t frames, double sil, double rep) {
        if (frames < kMinFrames) return Verdict::Unknown;
        if (sil >= kSilenceClear) return Verdict::Good;       // real comfort-noise -> clear speech
        if (rep < kRepeatScrambled) return Verdict::Unusable; // no silence, no repeats -> scrambled
        return Verdict::Marginal;                             // no real silence but repetitive
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
    std::uint32_t frames_, silence_, repeats_;
    std::uint64_t err_sum_;
    std::uint32_t err_frames_;
    std::uint64_t prev_;
    bool have_prev_;
};

}  // namespace dsdsrv
