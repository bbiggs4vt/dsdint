// VoiceQuality (audio_quality.hpp): the AMBE frame-repetition intelligibility
// analyzer. Clear digital voice repeats the standard comfort-noise codeword
// during pauses; a cipher scrambles every frame so that codeword never appears
// and frames never repeat. Verdict thresholds are validated on the real
// per-call repetition stats measured for clear vs. encrypted calls (see
// docs/AUDIO_QUALITY_CHECK.md "Calibration").
#include <cstdio>
#include <string>

#include "audio_quality.hpp"

using namespace dsdsrv;

static int g_failures = 0;
static void check(bool c, const std::string& what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what.c_str());
    if (!c) ++g_failures;
}

using V = VoiceQuality::Verdict;
static const std::uint64_t SIL = VoiceQuality::kSilenceFrame;

// Feed n distinct (never-repeating) frames starting at `base`.
static void feed_unique(VoiceQuality& q, std::uint64_t base, int n) {
    for (int i = 0; i < n; ++i) q.feed_frame(base + static_cast<std::uint64_t>(i) * 0x1111, 0);
}

int main() {
    std::printf("test_audio_quality\n");

    // ---- codeword parsing on the real silence codeword ----
    check(VoiceQuality::frame_of("F801A99F8CE080") == SIL, "frame_of: real silence codeword hex -> constant");
    check(VoiceQuality::frame_of("A937BA129E9200") == 0xA937BA129E9200ULL, "frame_of: real speech codeword hex");
    check(VoiceQuality::frame_of("") == 0 && VoiceQuality::frame_of("xyz") == 0, "frame_of: empty / non-hex -> 0");

    // ---- clear speech: real comfort-noise frames present -> good ----
    // Matches captures: silence-codeword ~6-20%, lots of repeats.
    {
        VoiceQuality q;
        feed_unique(q, 0xA00000000000, 200);       // speech
        for (int i = 0; i < 20; ++i) q.feed_frame(SIL, 0);   // ~9% real silence frames
        auto s = q.summary();
        check(s.sil > 0.08, "clear: silence-codeword fraction ~9%");
        check(s.verdict == V::Good, "clear call (has comfort-noise) -> good");
    }

    // ---- encrypted speech: no silence codeword, never repeats -> unusable ----
    // Matches captures s=860/862/772: silence 0%, consecutive repeats ~0-2%.
    {
        VoiceQuality q;
        feed_unique(q, 0x123456789A00, 250);        // all unique, none == SIL
        auto s = q.summary();
        check(s.sil == 0.0, "encrypted speech: 0% silence codeword");
        check(s.rep < 0.03, "encrypted speech: ~0% repeats");
        check(s.verdict == V::Unusable, "encrypted speech call -> unusable");
    }

    // ---- encrypted silence: no real comfort-noise codeword -> unusable ----
    // Even though the encrypted-silence value repeats (capture s=769 had ~8%
    // repeats), there is 0% REAL comfort-noise, so it reads unusable (red):
    // encrypted calls all flag.
    {
        VoiceQuality q;
        std::uint64_t encsil = 0xDEADBEEFCAFE01ULL;   // not SIL
        for (int i = 0; i < 250; ++i) {
            if (i % 12 == 0 || (i > 0 && i % 12 == 1)) q.feed_frame(encsil, 0);  // repeats
            else q.feed_frame(0x1000 + static_cast<std::uint64_t>(i) * 7, 0);
        }
        auto s = q.summary();
        check(s.sil == 0.0, "encrypted silence: 0% real comfort-noise codeword");
        check(s.rep >= 0.03, "encrypted silence: some consecutive repeats (diagnostic)");
        check(s.verdict == V::Unusable, "encrypted silence call -> unusable (no comfort-noise)");
    }

    // ---- threshold boundaries ----
    check(VoiceQuality::classify(49, 0.0) == V::Unknown, "verdict: < kMinFrames -> unknown");
    check(VoiceQuality::classify(100, 0.03) == V::Good, "verdict: silence >= 2% -> good");
    check(VoiceQuality::classify(100, 0.0) == V::Unusable, "verdict: no comfort-noise -> unusable");
    check(VoiceQuality::classify(100, 0.01) == V::Marginal, "verdict: trace comfort-noise (0.5-2%) -> marginal");

    // ---- err accumulation (RF signal, diagnostic only) ----
    {
        VoiceQuality q;
        for (int i = 0; i < 100; ++i) q.feed_frame(0x2000 + static_cast<std::uint64_t>(i), 2);
        auto s = q.summary();
        check(s.err_per_frame > 1.9 && s.err_per_frame < 2.1, "err: per-frame mean accumulated");
    }

    // ---- reset ----
    {
        VoiceQuality q;
        feed_unique(q, 1, 100);
        q.reset();
        auto s = q.summary();
        check(s.frames == 0 && s.verdict == V::Unknown, "reset: cleared");
    }

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
