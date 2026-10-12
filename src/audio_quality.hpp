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
// What the verdict keys on:
//
//  1. FEC/RF ERRORS (the real signal-quality measure). dsd-fme reports the AMBE
//     FEC error counts per frame ("err = [a] [b]"); their mean per frame rises
//     as the signal weakens and the decode degrades. A high mean -> the audio
//     is genuinely garbled -> UNUSABLE. This is independent of content: it is
//     the honest "how well did this decode" signal.
//
//  2. COMFORT NOISE (a positive "clear" confirmation). Clear digital voice
//     repeats the exact standard AMBE comfort-noise ("silence") codeword during
//     natural pauses; a cipher scrambles it away so it never appears. Its
//     presence proves real pauses -> a clear, cleanly-decoded call -> GOOD.
//     Measured: clear calls 6-20% silence codeword; encrypted calls 0%.
//
// What it deliberately does NOT do: infer "unusable" from the ABSENCE of comfort
// noise. Short, rapid, continuous CLEAR speech (a quick callsign/location read)
// has no pause long enough to emit the comfort-noise codeword and changes
// phoneme every 20 ms, so it has no silence frames and few repeats -- identical
// in frame statistics to scrambled speech. Flagging that as unusable gave false
// positives on perfectly intelligible calls, so a call that decodes cleanly
// (low FEC errors) but shows no comfort noise is left UNKNOWN (no badge), not
// unusable. Encrypted calls are surfaced by their own ENCRYPTED flag, not here.
// The consecutive-repeat fraction (Summary::rep) is kept as a diagnostic only.
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

    // The verdict keys on the EXACT comfort-noise codeword, which a cipher can
    // never produce (scrambling the standard value away): its presence means
    // the call has real natural pauses and is clear. Calibrated on real
    // captures -- clear calls always carry several of these frames (a brief
    // pause is multiple 20 ms frames); encrypted calls carry exactly zero.
    static constexpr std::uint32_t kClearSilenceFrames = 2;  // >= -> clear (has real pauses)
    // Mean AMBE FEC errors per frame at or above which the decode is judged
    // unusable (garbled). Calibrated on real captures: cleanly-decoded clear
    // calls sit well under this (around 0.5, up to ~1.3); genuinely weak decodes
    // run 2+. Tunable with DSD_NET_QUALITY_MAX_ERR.
    static double unusable_err_per_frame() {
        static const double v = [] {
            const char* e = std::getenv("DSD_NET_QUALITY_MAX_ERR");
            if (e && e[0]) { double d = std::atof(e); if (d > 0) return d; }
            return 2.0;
        }();
        return v;
    }

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
        s.verdict = classify(frames_, silence_, s.err_per_frame);
        return s;
    }

    // Heavy FEC errors -> the decode is garbled -> unusable (whatever the
    // content). Otherwise, the exact comfort-noise codeword proves real pauses
    // and a clean clear call -> good. A clean decode with no comfort noise is
    // left unknown: continuous clear speech looks the same as scrambled speech
    // in the frame stats, so we don't guess "unusable" from missing pauses.
    static Verdict classify(std::uint32_t frames, std::uint32_t silence, double err_per_frame) {
        if (frames < kMinFrames) return Verdict::Unknown;
        if (err_per_frame >= unusable_err_per_frame()) return Verdict::Unusable;
        if (silence >= kClearSilenceFrames) return Verdict::Good;
        return Verdict::Unknown;
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
