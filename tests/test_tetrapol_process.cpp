// test_tetrapol_process.cpp
//
// Integration test for TetrapolProcess (src/tetrapol_process.*): spawns a fake
// tetrapol_dump (tests/tetrapol_fake_dump.cpp, path passed as argv[1]) and
// verifies the whole subprocess path end to end WITHOUT the real decoder --
// fork/exec, the stdin bit pipe, and the stdout parse/forward path through the
// stateful TetrapolParser.
//
// The fake prints one D_SYSTEM_INFO tree (kind "sync") and one
// D_GROUP_ACTIVATION tree (kind "call") on startup, then drains stdin. Both
// have recognized kinds, so both are always forwarded; the test checks their
// parsed fields and that exec failure is reported rather than a phantom
// success.

#include "../src/tetrapol_process.hpp"

#include <cstdio>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <thread>

using namespace dsdsrv;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& what) {
    std::printf("  %s: %s\n", cond ? "OK" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

template <class Pred>
bool wait_for(Pred pred) {
    for (int i = 0; i < 200; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-fake-tetrapol_dump>\n", argv[0]);
        return 2;
    }
    const std::string fake = argv[1];
    std::printf("test_tetrapol_process: TetrapolProcess against a fake tetrapol_dump\n");

    // ---- both events forwarded; fields parsed ----
    {
        std::mutex m;
        std::vector<DsdEvent> events;
        TetrapolProcess proc;
        TetrapolProcessConfig cfg;
        cfg.tetrapol_dump_path = fake;

        bool ok = proc.start(cfg,
            [&](const DsdEvent& e) { std::lock_guard<std::mutex> lk(m); events.push_back(e); });
        check(ok, "start() spawned the fake child");
        check(proc.running(), "running() true after start");

        // Feed some bits (the fake discards them; this exercises write_bits).
        std::vector<unsigned char> bits(160, 0);
        check(proc.write_bits(bits.data(), bits.size()), "write_bits succeeded");

        bool got = wait_for([&] { std::lock_guard<std::mutex> lk(m); return events.size() >= 2; });
        check(got, "received both forwarded events");
        {
            std::lock_guard<std::mutex> lk(m);
            if (events.size() >= 2) {
                const DsdEvent& s = events[0];
                check(s.kind == "sync", "first event is sync (D_SYSTEM_INFO)");
                check(s.extra.find("network=42") != std::string::npos, "sync carries network=42");
                check(s.extra.find("bs_id=12") != std::string::npos, "sync carries bs_id=12");
                const DsdEvent& c = events[1];
                check(c.kind == "call", "second event is call (D_GROUP_ACTIVATION)");
                check(c.talkgroup == "1234", "call talkgroup from GROUP_ID");
            }
        }
        proc.stop();
        check(!proc.running(), "running() false after stop");
    }

    // ---- exec failure is reported, not a phantom success ----
    {
        TetrapolProcess proc;
        TetrapolProcessConfig cfg;
        cfg.tetrapol_dump_path = "/nonexistent/tetrapol_dump-xyzzy";
        bool ok = proc.start(cfg, [](const DsdEvent&) {});
        check(!ok, "start() returns false when the binary can't be exec'd");
        check(!proc.running(), "running() false after failed start");
    }

    if (g_failures == 0) std::printf("ALL TETRAPOL PROCESS TESTS PASSED\n");
    else std::printf("%d TETRAPOL PROCESS TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
