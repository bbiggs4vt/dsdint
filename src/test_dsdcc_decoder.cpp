// test_dsdcc_decoder.cpp
//
// Verifies DsdccDecoder (dsdcc_decoder.cpp) against a REAL DMR signal:
// DSDcc's own bundled discriminator capture, samples/dmr_it_8.dis in
// the DSDcc source tree (S16LE 48 kHz mono; an Italian-language DMR
// conversation on TDMA slot #2, talkgroup 150607, group call). Ground
// truth for the assertions below was established by running upstream's
// own dsdccx CLI on the same file (-fr -T 3): ~150k samples of decoded
// 8 kHz audio, sources 2222223 and 2220175 calling TG 150607 on slot 2.
//
// This is the strongest check this backend has: not "the API compiles"
// but "real RF yields the right voice audio and the right metadata
// through our wrapper". What it does NOT cover is the Session plumbing
// around the backend — that's test_session's job, and Session treats
// both backends identically through the ActiveDsdBackend alias.
//
// Usage: test_dsdcc_decoder <path-to-dmr_it_8.dis>
// With no argument (or a missing file) it prints SKIPPED and exits 0,
// so the ctest suite doesn't fail on machines without a DSDcc source
// checkout -- see CMakeLists.txt's DSDCC_SAMPLES_DIR option.

#include "dsdcc_decoder.hpp"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace dsdsrv;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  OK: %s\n", what.c_str());
    } else {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("SKIPPED: no sample file argument. Pass the path to DSDcc's "
                    "samples/dmr_it_8.dis to run this test.\n");
        return 0;
    }

    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::printf("SKIPPED: cannot open sample file %s\n", argv[1]);
        return 0;
    }

    std::size_t total_audio_samples = 0;
    std::set<std::string> talkgroups, sources, slots, kinds;
    int event_count = 0;

    DsdccDecoder dec;
    DsdccConfig cfg; // defaults: mode "dmr", 48000 Hz — matches the capture

    bool started = dec.start(
        cfg,
        [&](const DsdEvent& ev) {
            ++event_count;
            if (!ev.talkgroup.empty()) talkgroups.insert(ev.talkgroup);
            if (!ev.source_id.empty()) sources.insert(ev.source_id);
            if (!ev.slot.empty()) slots.insert(ev.slot);
            kinds.insert(ev.kind);
        },
        [&](const int16_t* /*pcm*/, std::size_t n) { total_audio_samples += n; });
    check(started, "decoder starts");
    check(dec.running(), "decoder reports running");

    // Feed the capture through in Session-sized chunks (write_audio is
    // called with whatever the demod produced per IQ block; the exact
    // chunking must not matter to the decoder).
    std::vector<int16_t> buf(4096);
    std::size_t total_in = 0;
    std::size_t nread;
    while ((nread = std::fread(buf.data(), sizeof(int16_t), buf.size(), f)) > 0) {
        if (!dec.write_audio(buf.data(), nread)) {
            check(false, "write_audio accepts samples");
            break;
        }
        total_in += nread;
    }
    std::fclose(f);
    std::printf("  fed %zu input samples (%.1f s at 48 kHz)\n",
                total_in, total_in / 48000.0);
    std::printf("  decoded %zu audio samples (%.1f s at 8 kHz), %d events\n",
                total_audio_samples, total_audio_samples / 8000.0, event_count);

    // Ground truth from dsdccx on the same capture: 151680 decoded
    // samples with both slots enabled. Assert a generous band around
    // that rather than exact equality — mbelib synthesis output length
    // can shift slightly across versions, and the point is "roughly all
    // of the ~19 s of speech decoded", not bit-exactness.
    check(total_audio_samples > 100000,
          "decoded a substantial amount of voice audio (>100k samples)");
    check(total_audio_samples < 300000,
          "decoded audio volume is plausible (<300k samples)");

    // Metadata, against what's actually transmitted in the capture.
    check(talkgroups.count("150607") == 1,
          "events carry the capture's real talkgroup (150607)");
    check(sources.count("2222223") == 1,
          "events carry the capture's primary source (2222223)");
    check(slots.count("2") == 1, "call activity reported on TDMA slot 2");
    check(kinds.count("voice") == 1, "voice-kind events were emitted");
    check(kinds.count("sync") == 1, "sync acquisition was reported");

    // The reverse checks: things that are NOT in this capture must not
    // be invented. (Slot 1 carries only idle bursts — no addresses.)
    check(slots.count("1") == 0, "no call events fabricated for idle slot 1");

    dec.stop();
    check(!dec.running(), "decoder reports stopped after stop()");

    // A stopped decoder must reject input rather than crash.
    int16_t one = 0;
    check(!dec.write_audio(&one, 1), "write_audio refuses input after stop");

    // Bad config must be rejected up front (DSDcc's rate is fixed).
    DsdccDecoder dec2;
    DsdccConfig bad;
    bad.input_sample_rate_hz = 44100;
    check(!dec2.start(bad, nullptr, nullptr),
          "start rejects a non-48kHz input rate");

    if (g_failures == 0) {
        std::printf("\nALL DSDCC DECODER TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
