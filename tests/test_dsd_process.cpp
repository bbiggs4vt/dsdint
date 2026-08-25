// test_dsd_process.cpp
//
// Verifies DsdProcess (dsd_process.cpp) against a REAL dsd-fme binary
// decoding a REAL DMR signal -- the same ground-truth approach as
// test_dsdcc_decoder, applied to the subprocess backend. It spawns the
// actual dsd-fme through DsdProcess's own fork/exec path (so the
// verified command line in build_argv is what's being tested), feeds it
// DSDcc's bundled discriminator capture (samples/dmr_it_8.dis, S16LE
// 48 kHz; a DMR group call on TDMA slot 2), and asserts on what comes
// back through both channels:
//
//   - stdout/stderr events, through classify_line: the capture's real
//     talkgroup (dsd-fme reports TGT=19535 for this capture) and source
//     (2222223), the correct slot (2, via the bracketed [SLOT2] marker),
//     sync-kind events, and no ANSI escape bytes leaking into raw_line.
//   - UDP audio: dsd-fme's "-o udp:127.0.0.1:<port>" stream, raw 8 kHz
//     stereo PCM for the DMR mode -- a substantial sample count proves
//     the -o wiring and the udp_reader_loop work against the real thing.
//
// Ground truth was established by running the same dsd-fme build on the
// same file directly: ~660 sync lines, "SLOT 2 TGT=19535 SRC=2222223
// Group Call" call lines, and ~315k int16s of UDP audio.
//
// Usage: test_dsd_process <path-to-dsd-fme-binary> <path-to-dmr_it_8.dis>
// Prints SKIPPED and exits 0 when either is missing, so ctest doesn't
// fail on machines without a dsd-fme build -- see CMakeLists.txt's
// DSD_FME_BIN option.

#include "dsd_process.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
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
    if (argc < 3) {
        std::printf("SKIPPED: usage: test_dsd_process <dsd-fme binary> <dmr_it_8.dis>\n");
        return 0;
    }
    FILE* probe = std::fopen(argv[1], "rb");
    if (!probe) { std::printf("SKIPPED: cannot open dsd-fme binary %s\n", argv[1]); return 0; }
    std::fclose(probe);
    FILE* f = std::fopen(argv[2], "rb");
    if (!f) { std::printf("SKIPPED: cannot open sample file %s\n", argv[2]); return 0; }

    // Callback state. DsdProcess invokes these from its reader threads,
    // so collection is mutex-guarded.
    std::mutex m;
    std::set<std::string> talkgroups, sources, slots, kinds;
    int event_count = 0;
    bool saw_ansi_or_cr = false;
    std::atomic<std::size_t> audio_samples{0};

    DsdProcess proc;
    DsdProcessConfig cfg;
    cfg.dsd_fme_path = argv[1];
    cfg.udp_audio_port = 39271; // fixed test port; nothing else in the suite uses it
    // mode_flag stays at its default -- that the default decodes DMR is
    // exactly one of the things this test exists to pin (the original
    // guessed default selected D-STAR).

    bool started = proc.start(
        cfg,
        [&](const DsdEvent& ev) {
            std::lock_guard<std::mutex> lock(m);
            ++event_count;
            if (!ev.talkgroup.empty()) talkgroups.insert(ev.talkgroup);
            if (!ev.source_id.empty()) sources.insert(ev.source_id);
            if (!ev.slot.empty()) slots.insert(ev.slot);
            kinds.insert(ev.kind);
            if (ev.raw_line.find('\x1b') != std::string::npos ||
                ev.raw_line.find('\r') != std::string::npos) {
                saw_ansi_or_cr = true;
            }
        },
        [&](const int16_t* /*pcm*/, std::size_t n) { audio_samples += n; });
    check(started, "DsdProcess spawns the real dsd-fme");
    check(proc.running(), "reports running");

    // Feed the capture at a modest pace. dsd-fme reads stdin through
    // libsndfile and decodes faster than realtime, but its UDP audio is
    // emitted as decoding progresses -- pacing (roughly 8x realtime)
    // keeps the pipe from buffering the whole file before the decoder
    // has produced anything, which makes the test's timing behavior
    // representative of the server's streaming use.
    std::vector<int16_t> buf(4096);
    std::size_t total_in = 0, nread;
    bool write_failed = false;
    while ((nread = std::fread(buf.data(), sizeof(int16_t), buf.size(), f)) > 0) {
        if (!proc.write_audio(buf.data(), nread)) { write_failed = true; break; }
        total_in += nread;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::fclose(f);
    check(!write_failed, "streamed the full capture into dsd-fme's stdin");
    std::printf("  fed %zu input samples (%.1f s at 48 kHz)\n", total_in, total_in / 48000.0);

    // Let the decoder finish the tail, then stop (which closes stdin,
    // delivering EOF, and reaps the child).
    std::this_thread::sleep_for(std::chrono::seconds(2));
    proc.stop();
    check(!proc.running(), "reports stopped after stop()");

    std::lock_guard<std::mutex> lock(m);
    std::printf("  %d events; %zu UDP audio samples (%.1f s if 8 kHz stereo)\n",
                event_count, audio_samples.load(),
                audio_samples.load() / 16000.0);

    // Events, against what this capture genuinely contains. Note the
    // talkgroup: dsd-fme reports TGT=19535 for this capture's voice LC
    // (DSDcc's embedded-LC path reports 150607 for the same signal, and
    // shows 19535 too -- the capture carries both; each backend's test
    // asserts what its own decoder reports).
    check(event_count > 100, "a substantial number of events was parsed");
    check(talkgroups.count("19535") == 1,
          "events carry the talkgroup dsd-fme reports for this capture (19535)");
    check(sources.count("2222223") == 1,
          "events carry the capture's source (2222223)");
    check(slots.count("2") == 1,
          "call activity attributed to TDMA slot 2 (bracketed-slot parsing)");
    check(kinds.count("sync") == 1, "sync-kind events were emitted");
    check(kinds.count("call") == 1, "call-kind events were emitted");
    check(!saw_ansi_or_cr, "no ANSI escapes or CRs leak into raw_line");

    // A child that dies immediately must NOT take the process down: the
    // next write hits a broken pipe, and before DsdProcess ignored
    // SIGPIPE that raised the default TERMINATE action -- one crashed
    // dsd-fme killed the whole server. Now the write just returns false.
    {
        DsdProcess dead;
        DsdProcessConfig dcfg2;
        dcfg2.dsd_fme_path = "/bin/false"; // exits instantly, reads nothing
        bool started2 = dead.start(dcfg2, nullptr, nullptr);
        check(started2, "spawns a child that exits immediately");
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // let it die
        int16_t junk[256] = {0};
        bool any_ok = true;
        for (int i = 0; i < 50 && any_ok; ++i) {
            any_ok = dead.write_audio(junk, 256); // must fail, not SIGPIPE-kill us
        }
        check(!any_ok, "write to a dead child's pipe fails gracefully (no SIGPIPE death)");
        dead.stop();
    }

    // A binary that can't be exec'd at all is different from one that
    // dies after exec: start() must return false (via the exec-status
    // pipe), so the Session replies "failed to start DSD backend"
    // instead of a phantom "started". Seen in the field: running
    // dsd-server without dsd-fme on PATH produced "started" plus an
    // unknown-kind event carrying the exec error text.
    {
        DsdProcess missing;
        DsdProcessConfig mcfg;
        mcfg.dsd_fme_path = "/nonexistent/no-such-dsd-fme";
        bool started3 = missing.start(mcfg, nullptr, nullptr);
        check(!started3, "start() fails (not phantom-succeeds) when the binary can't be exec'd");
        missing.stop(); // must be a safe no-op after a failed start
    }

    // Audio through the real -o udp path. Ground truth from dsd-fme run
    // directly on this file: ~315k int16s (8 kHz stereo, ~19 s of
    // voice). Same generous banding as the DSDcc tests.
    check(audio_samples.load() > 200000,
          "received a substantial amount of UDP audio (>200k int16s)");
    check(audio_samples.load() < 500000,
          "UDP audio volume is plausible (<500k int16s)");

    if (g_failures == 0) {
        std::printf("\nALL DSD-FME PROCESS TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
