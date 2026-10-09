// VoiceQuality (audio_quality.hpp): the AMBE frame-type intelligibility
// analyzer. Validates b0 classification on REAL codewords pulled from captured
// DMR traffic, and the verdict thresholds on the real per-call frame-type
// distributions measured for clear vs. encrypted calls (see
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

using F = VoiceQuality::Frame;
using V = VoiceQuality::Verdict;

// Feed `count` frames of a given b0 class into q (err 0).
static void feed_class(VoiceQuality& q, F cls, int n) {
    // A representative b0 in each class's range.
    int b0 = cls == F::Erasure ? 121 : cls == F::Silence ? 124 : cls == F::Tone ? 127 : 86;
    for (int i = 0; i < n; ++i) q.feed_frame(b0, 0);
}

int main() {
    std::printf("test_audio_quality\n");

    // ---- b0 classification on REAL codewords from captured DMR ----
    check(VoiceQuality::b0_of("F801A99F8CE080") == 124, "b0: real silence codeword -> 124");
    check(VoiceQuality::b0_of("A937BA129E9200") == 86,  "b0: real speech codeword -> 86");
    check(VoiceQuality::b0_of("F93685FFFFFF80") == 127, "b0: real tone codeword -> 127");
    check(VoiceQuality::b0_of("F3F54A6689C980") == 121, "b0: real erasure codeword -> 121");
    check(VoiceQuality::b0_of("") == -1 && VoiceQuality::b0_of("xyz") == -1, "b0: empty / non-hex -> -1");
    check(VoiceQuality::classify_b0(124) == F::Silence && VoiceQuality::classify_b0(125) == F::Silence,
          "classify: 124/125 -> silence");
    check(VoiceQuality::classify_b0(120) == F::Erasure && VoiceQuality::classify_b0(123) == F::Erasure,
          "classify: 120-123 -> erasure");
    check(VoiceQuality::classify_b0(126) == F::Tone && VoiceQuality::classify_b0(127) == F::Tone,
          "classify: 126/127 -> tone");
    check(VoiceQuality::classify_b0(0) == F::Speech && VoiceQuality::classify_b0(119) == F::Speech,
          "classify: < 120 -> speech");

    // ---- real per-call distributions -> expected verdict ----
    // Clear call (capture s=860): 252 frames, ~90% speech, ~10% silence, 0 junk.
    {
        VoiceQuality q;
        feed_class(q, F::Speech, 226); feed_class(q, F::Silence, 26);
        auto s = q.summary();
        check(s.verdict == V::Good, "clear call (10% silence, 0 junk) -> good");
        check(s.sil > 0.09 && s.junk == 0.0, "clear call: silence/junk fractions");
    }
    // Encrypted counting call (capture s=772): 234 frames, 85% speech, 0.4%
    // silence, 9.4% erasure, 5.1% tone -> junk ~14.5%.
    {
        VoiceQuality q;
        feed_class(q, F::Speech, 199); feed_class(q, F::Silence, 1);
        feed_class(q, F::Erasure, 22); feed_class(q, F::Tone, 12);
        auto s = q.summary();
        check(s.junk > 0.13, "encrypted counting: junk ~14.5%");
        check(s.verdict == V::Unusable, "encrypted counting call -> unusable");
    }
    // Encrypted continuous call (capture s=769): 258 frames, 99.6% speech,
    // 0% silence, 0.4% junk -> no natural pauses.
    {
        VoiceQuality q;
        feed_class(q, F::Speech, 257); feed_class(q, F::Erasure, 1);
        auto s = q.summary();
        check(s.junk < VoiceQuality::kJunkMarginal && s.sil <= VoiceQuality::kSilenceLow,
              "encrypted continuous: low junk but ~0% silence");
        check(s.verdict == V::Marginal, "encrypted continuous call -> marginal (no pauses)");
    }

    // ---- threshold boundaries ----
    check(VoiceQuality::classify(49, 0.0, 0.1) == V::Unknown, "verdict: < kMinFrames -> unknown");
    check(VoiceQuality::classify(100, 0.05, 0.1) == V::Unusable, "verdict: junk >= 4% -> unusable");
    check(VoiceQuality::classify(100, 0.03, 0.1) == V::Marginal, "verdict: junk 2-4% -> marginal");
    check(VoiceQuality::classify(100, 0.0, 0.0) == V::Marginal, "verdict: 0% silence -> marginal");
    check(VoiceQuality::classify(100, 0.0, 0.1) == V::Good, "verdict: low junk + healthy silence -> good");

    // ---- err accumulation (RF signal, diagnostic only) ----
    {
        VoiceQuality q;
        for (int i = 0; i < 100; ++i) q.feed_frame(86, 3);   // 3 errors/frame
        auto s = q.summary();
        check(s.err_per_frame > 2.9 && s.err_per_frame < 3.1, "err: per-frame mean accumulated");
    }

    // ---- reset ----
    {
        VoiceQuality q;
        feed_class(q, F::Speech, 100);
        q.reset();
        auto s = q.summary();
        check(s.frames == 0 && s.verdict == V::Unknown, "reset: cleared");
    }

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
