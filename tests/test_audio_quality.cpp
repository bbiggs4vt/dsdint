// VoiceQuality (audio_quality.hpp): the AMBE voice intelligibility analyzer.
// The verdict is driven by the AMBE FEC error rate (heavy errors -> unusable),
// with the exact comfort-noise codeword as a positive "clear" confirmation
// (present -> good). A clean decode without comfort noise is left unknown --
// continuous clear speech has no pause to emit the codeword, so its absence is
// not treated as unusable. See docs/AUDIO_QUALITY_CHECK.md "Calibration".
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

// Feed n distinct (never-repeating) frames starting at `base`, each with FEC
// error count `err`.
static void feed_unique(VoiceQuality& q, std::uint64_t base, int n, int err = 0) {
    for (int i = 0; i < n; ++i) q.feed_frame(base + static_cast<std::uint64_t>(i) * 0x1111, err);
}

int main() {
    std::printf("test_audio_quality\n");

    // ---- codeword parsing on the real silence codeword ----
    check(VoiceQuality::frame_of("F801A99F8CE080") == SIL, "frame_of: real silence codeword hex -> constant");
    check(VoiceQuality::frame_of("A937BA129E9200") == 0xA937BA129E9200ULL, "frame_of: real speech codeword hex");
    check(VoiceQuality::frame_of("") == 0 && VoiceQuality::frame_of("xyz") == 0, "frame_of: empty / non-hex -> 0");

    // ---- clear speech: comfort noise present, clean decode -> good ----
    // Matches captures: silence-codeword ~6-20%, mean FEC errors well under 2.
    {
        VoiceQuality q;
        feed_unique(q, 0xA00000000000, 200);       // speech, no errors
        for (int i = 0; i < 20; ++i) q.feed_frame(SIL, 0);   // ~9% real silence frames
        auto s = q.summary();
        check(s.sil > 0.08, "clear: silence-codeword fraction ~9%");
        check(s.verdict == V::Good, "clear call (comfort noise, clean decode) -> good");
    }

    // ---- continuous clear speech: no pause, clean decode -> unknown (NOT a
    // false "unusable"). This is the regression the FEC gate fixes: a quick
    // callsign/location read has no comfort-noise frame and few repeats, like
    // scrambled speech, but decodes cleanly, so we do not flag it. ----
    {
        VoiceQuality q;
        feed_unique(q, 0x123456789A00, 250, 1);     // 250 unique frames, light FEC (<2/frame)
        auto s = q.summary();
        check(s.sil == 0.0, "continuous clear: 0% comfort-noise codeword");
        check(s.verdict == V::Unknown, "continuous clear speech (clean decode, no pause) -> unknown, not unusable");
    }

    // ---- garbled decode: heavy FEC errors -> unusable (whatever the content) ----
    {
        VoiceQuality q;
        feed_unique(q, 0x55000000000, 250, 3);      // ~3 FEC errors/frame
        auto s = q.summary();
        check(s.err_per_frame >= 2.0, "garbled: mean FEC errors per frame >= 2");
        check(s.verdict == V::Unusable, "heavy FEC errors -> unusable");
    }

    // ---- threshold boundaries (classify(frames, silence count, err/frame)) ----
    check(VoiceQuality::classify(49, 0, 3.0) == V::Unknown, "verdict: < kMinFrames -> unknown");
    check(VoiceQuality::classify(500, 2, 0.5) == V::Good, "verdict: comfort noise + clean decode -> good");
    check(VoiceQuality::classify(500, 1, 0.5) == V::Unknown, "verdict: no comfort noise, clean decode -> unknown (no false unusable)");
    check(VoiceQuality::classify(100, 0, 0.5) == V::Unknown, "verdict: continuous clean call -> unknown");
    check(VoiceQuality::classify(100, 0, 2.5) == V::Unusable, "verdict: heavy FEC errors -> unusable");
    check(VoiceQuality::classify(500, 20, 2.5) == V::Unusable, "verdict: heavy FEC errors override comfort noise -> unusable");

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
