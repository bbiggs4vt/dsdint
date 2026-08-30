// test_tetra_process.cpp
//
// Integration test for TetraProcess (src/tetra_process.*): spawns a fake
// tetra-rx (tests/tetra_fake_rx.cpp, path passed as argv[1]) and verifies
// the whole subprocess path end to end WITHOUT the real decoder --
// fork/exec, the env TetraProcess hands the child (TETRA_HACK_PORT etc.),
// the stdin bit pipe, and the UDP TETMON receive/parse/forward path.
//
// The fake sends one call-control TETMON message and one AFC diagnostic on
// startup. With forward_unknown=false only the call is delivered; with
// forward_unknown=true both are.

#include "../src/tetra_process.hpp"

#include <cstdio>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>

using namespace dsdsrv;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& what) {
    std::printf("  %s: %s\n", cond ? "OK" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

// Wait up to ~2s for `pred` to hold, polling briefly. Returns pred's result.
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
        std::fprintf(stderr, "usage: %s <path-to-fake-tetra-rx>\n", argv[0]);
        return 2;
    }
    const std::string fake = argv[1];
    std::printf("test_tetra_process: TetraProcess against a fake tetra-rx\n");

    // ---- pure forward-policy unit checks ----
    {
        DsdEvent call; call.kind = "call";
        DsdEvent unk;  unk.kind = "unknown";
        check(tetra_forward_event(call, false), "forward: call forwarded");
        check(!tetra_forward_event(unk, false), "forward: unknown suppressed by default");
        check(tetra_forward_event(unk, true), "forward: unknown forwarded when enabled");
    }

    // ---- default: only the call event is forwarded ----
    {
        std::mutex m;
        std::vector<DsdEvent> events;
        TetraProcess proc;
        TetraProcessConfig cfg;
        cfg.tetra_rx_path = fake;
        // forward_unknown stays false

        bool ok = proc.start(cfg,
            [&](const DsdEvent& e) { std::lock_guard<std::mutex> lk(m); events.push_back(e); },
            nullptr);
        check(ok, "start() spawned the fake child");
        check(proc.running(), "running() true after start");
        check(proc.tetmon_port() != 0, "a TETMON UDP port was bound");

        // Feed some bits (the fake discards them; this exercises write_bits).
        std::vector<unsigned char> bits(510, 0);
        check(proc.write_bits(bits.data(), bits.size()), "write_bits succeeded");

        bool got = wait_for([&] { std::lock_guard<std::mutex> lk(m); return events.size() >= 1; });
        check(got, "received the forwarded call event");
        // Give any (suppressed) second event a chance to wrongly arrive.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        {
            std::lock_guard<std::mutex> lk(m);
            check(events.size() == 1, "AFC diagnostic suppressed (exactly one event)");
            if (!events.empty()) {
                const DsdEvent& e = events.front();
                check(e.kind == "call", "event kind is call");
                check(e.source_id == "1234567", "event source_id from SSI");
                check(e.extra.find("idx=3") != std::string::npos, "event carries idx=3");
            }
        }
        proc.stop();
        check(!proc.running(), "running() false after stop");
    }

    // ---- forward_unknown: both messages delivered ----
    {
        std::mutex m;
        std::atomic<int> count{0};
        TetraProcess proc;
        TetraProcessConfig cfg;
        cfg.tetra_rx_path = fake;
        cfg.forward_unknown = true;

        bool ok = proc.start(cfg,
            [&](const DsdEvent&) { count.fetch_add(1); },
            nullptr);
        check(ok, "start() (forward_unknown) spawned the child");
        bool got = wait_for([&] { return count.load() >= 2; });
        check(got, "both events delivered when forward_unknown=true");
        proc.stop();
    }

    // ---- exec failure is reported, not a phantom success ----
    {
        TetraProcess proc;
        TetraProcessConfig cfg;
        cfg.tetra_rx_path = "/nonexistent/tetra-rx-xyzzy";
        bool ok = proc.start(cfg, [](const DsdEvent&) {}, nullptr);
        check(!ok, "start() returns false when the binary can't be exec'd");
        check(!proc.running(), "running() false after failed start");
    }

    if (g_failures == 0) std::printf("ALL TETRA PROCESS TESTS PASSED\n");
    else std::printf("%d TETRA PROCESS TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
