// VoiceQuality (audio_quality.hpp): the streaming spectral-flatness analyzer.
// Synthetic signals exercise the flatness -> verdict mapping, the silence
// floor, the minimum-voiced-frames gate, and the FFT (implicitly, via tone vs.
// noise flatness). No field data needed: structured signals must read low
// flatness ("good"), white noise must read high flatness ("unusable").
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "audio_quality.hpp"

using namespace dsdsrv;

static int g_failures = 0;
static void check(bool c, const std::string& what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what.c_str());
    if (!c) ++g_failures;
}

static constexpr int kRate = VoiceQuality::kRate;

// ~1 second of signal => ~60 analysis frames, well over kMinVoicedFrames.
static std::vector<std::int16_t> tone(double hz, double amp, int samples) {
    std::vector<std::int16_t> v(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i)
        v[static_cast<std::size_t>(i)] =
            static_cast<std::int16_t>(amp * std::sin(2.0 * M_PI * hz * i / kRate));
    return v;
}

// A voiced-speech-like signal: a fundamental plus a few harmonics. Energy sits
// in a handful of bins, so spectral flatness is low (structured).
static std::vector<std::int16_t> voiced(double f0, double amp, int samples) {
    std::vector<std::int16_t> v(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        double s = 0.0;
        for (int h = 1; h <= 5; ++h) s += std::sin(2.0 * M_PI * f0 * h * i / kRate) / h;
        v[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(amp * s / 2.0);
    }
    return v;
}

static std::vector<std::int16_t> white_noise(double stddev, int samples, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, stddev);
    std::vector<std::int16_t> v(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        double x = nd(rng);
        x = std::clamp(x, -32767.0, 32767.0);
        v[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(x);
    }
    return v;
}

int main() {
    std::printf("test_audio_quality\n");

    const int one_sec = kRate;

    // ---- white noise: structureless -> high flatness -> unusable ----
    {
        VoiceQuality q;
        auto n = white_noise(5000.0, one_sec, 12345);
        q.feed(n.data(), n.size());
        auto s = q.summary();
        check(s.voiced_frames >= VoiceQuality::kMinVoicedFrames, "noise: enough voiced frames");
        check(s.mean_flatness >= VoiceQuality::kFlatnessUnusable,
              "noise: flatness high (" + std::to_string(s.mean_flatness) + ")");
        check(s.verdict == VoiceQuality::Verdict::Unusable, "noise: verdict unusable");
    }

    // ---- pure tone: maximally structured -> low flatness -> good ----
    {
        VoiceQuality q;
        auto t = tone(440.0, 8000.0, one_sec);
        q.feed(t.data(), t.size());
        auto s = q.summary();
        check(s.mean_flatness <= VoiceQuality::kFlatnessGood,
              "tone: flatness low (" + std::to_string(s.mean_flatness) + ")");
        check(s.verdict == VoiceQuality::Verdict::Good, "tone: verdict good");
    }

    // ---- voiced-speech-like harmonics -> low flatness -> good ----
    {
        VoiceQuality q;
        auto v = voiced(160.0, 9000.0, one_sec);
        q.feed(v.data(), v.size());
        auto s = q.summary();
        check(s.mean_flatness <= VoiceQuality::kFlatnessGood,
              "voiced: flatness low (" + std::to_string(s.mean_flatness) + ")");
        check(s.verdict == VoiceQuality::Verdict::Good, "voiced: verdict good");
    }

    // ---- silence (zeros): excluded -> no voiced frames -> unknown ----
    {
        VoiceQuality q;
        std::vector<std::int16_t> z(static_cast<std::size_t>(one_sec), 0);
        q.feed(z.data(), z.size());
        auto s = q.summary();
        check(s.total_frames > 0, "silence: frames were seen");
        check(s.voiced_frames == 0, "silence: no voiced frames");
        check(s.verdict == VoiceQuality::Verdict::Unknown, "silence: verdict unknown");
    }

    // ---- quiet signal below the floor is treated as silence ----
    {
        VoiceQuality q;
        auto t = tone(440.0, 50.0, one_sec);   // peak 50 << kSilenceRms
        q.feed(t.data(), t.size());
        auto s = q.summary();
        check(s.voiced_frames == 0, "sub-floor tone: no voiced frames");
        check(s.verdict == VoiceQuality::Verdict::Unknown, "sub-floor tone: verdict unknown");
    }

    // ---- too few voiced frames -> unknown even if loud/structured ----
    {
        VoiceQuality q;
        auto t = tone(440.0, 8000.0, 1024);    // ~7 frames < kMinVoicedFrames
        q.feed(t.data(), t.size());
        auto s = q.summary();
        check(s.voiced_frames > 0 && s.voiced_frames < VoiceQuality::kMinVoicedFrames,
              "short tone: some but too few voiced frames");
        check(s.verdict == VoiceQuality::Verdict::Unknown, "short tone: verdict unknown (not enough)");
    }

    // ---- feeding in small chunks matches one big feed (framing is continuous) ----
    {
        auto v = voiced(160.0, 9000.0, one_sec);
        VoiceQuality a;
        a.feed(v.data(), v.size());
        VoiceQuality b;
        std::size_t off = 0;
        while (off < v.size()) {
            std::size_t chunk = std::min<std::size_t>(37, v.size() - off);   // odd chunk size
            b.feed(v.data() + off, chunk);
            off += chunk;
        }
        auto sa = a.summary(), sb = b.summary();
        check(sa.voiced_frames == sb.voiced_frames, "chunked feed: same voiced-frame count");
        check(std::fabs(sa.mean_flatness - sb.mean_flatness) < 1e-6, "chunked feed: same flatness");
    }

    // ---- reset() clears state ----
    {
        VoiceQuality q;
        auto n = white_noise(5000.0, one_sec, 7);
        q.feed(n.data(), n.size());
        q.reset();
        auto s = q.summary();
        check(s.total_frames == 0 && s.voiced_frames == 0, "reset: counters cleared");
        check(s.verdict == VoiceQuality::Verdict::Unknown, "reset: verdict unknown");
    }

    // ---- classify() boundaries ----
    {
        using V = VoiceQuality::Verdict;
        check(VoiceQuality::classify(5, 0.0) == V::Unknown, "classify: too few frames -> unknown");
        check(VoiceQuality::classify(100, VoiceQuality::kFlatnessGood - 0.01) == V::Good, "classify: low flatness -> good");
        check(VoiceQuality::classify(100, VoiceQuality::kFlatnessUnusable + 0.01) == V::Unusable, "classify: high flatness -> unusable");
        check(VoiceQuality::classify(100, (VoiceQuality::kFlatnessGood + VoiceQuality::kFlatnessUnusable) / 2) == V::Marginal,
              "classify: middle flatness -> marginal");
    }

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
