// test_tetra_kit_process.cpp
//
// Integration test for TetraKitProcess (src/tetra_kit_process.*): spawns a
// fake tetra-kit decoder (tests/tetra_kit_fake.cpp, path as argv[1]) and
// verifies the whole path without the real binary -- fork/exec, the -r/-t
// port arguments, the JSON receive/parse/forward path, and that write_bits'
// datagrams reach the child's -r socket.

#include "../src/tetra_kit_process.hpp"

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
    if (argc < 2) { std::fprintf(stderr, "usage: %s <fake-decoder>\n", argv[0]); return 2; }
    const std::string fake = argv[1];
    std::printf("test_tetra_kit_process: TetraKitProcess against a fake decoder\n");

    // ---- forward policy ----
    {
        DsdEvent call; call.kind = "call";
        DsdEvent unk;  unk.kind = "unknown";
        check(tetrakit_forward_event(call, false), "forward: call forwarded");
        check(!tetrakit_forward_event(unk, false), "forward: unknown suppressed by default");
        check(tetrakit_forward_event(unk, true), "forward: unknown forwarded when enabled");
    }

    // ---- default: only the call report is forwarded ----
    {
        std::mutex m;
        std::vector<DsdEvent> events;
        TetraKitProcess proc;
        TetraKitProcessConfig cfg;
        cfg.decoder_path = fake;

        bool ok = proc.start(cfg,
            [&](const DsdEvent& e) { std::lock_guard<std::mutex> lk(m); events.push_back(e); },
            nullptr);
        check(ok, "start() spawned the fake decoder");
        check(proc.running(), "running() true after start");
        check(proc.json_port() != 0 && proc.bitstream_port() != 0, "both UDP ports assigned");

        std::vector<unsigned char> bits(510, 1);
        check(proc.write_bits(bits.data(), bits.size()), "write_bits (UDP send) succeeded");

        bool got = wait_for([&] { std::lock_guard<std::mutex> lk(m); return !events.empty(); });
        check(got, "received the forwarded call event");
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        {
            std::lock_guard<std::mutex> lk(m);
            check(events.size() == 1, "non-call (MM) report suppressed (exactly one event)");
            if (!events.empty()) {
                const DsdEvent& e = events.front();
                check(e.kind == "call", "event kind is call");
                check(e.source_id == "1234567", "event source_id from ssi");
                check(e.extra.find("usage_marker=3") != std::string::npos, "usage_marker in extra");
            }
        }
        proc.stop();
        check(!proc.running(), "running() false after stop");
    }

    // ---- forward_unknown: both reports delivered ----
    {
        std::atomic<int> count{0};
        TetraKitProcess proc;
        TetraKitProcessConfig cfg;
        cfg.decoder_path = fake;
        cfg.forward_unknown = true;
        bool ok = proc.start(cfg, [&](const DsdEvent&) { count.fetch_add(1); }, nullptr);
        check(ok, "start() (forward_unknown) spawned the decoder");
        bool got = wait_for([&] { return count.load() >= 2; });
        check(got, "both reports delivered when forward_unknown=true");
        proc.stop();
    }

    // ---- bits are not lost to UDP datagram truncation ----
    // The fake reads with a 1024-byte buffer like the real decoder and reports
    // the total bytes it received. If write_bits sent an oversized datagram,
    // the kernel would truncate it and the tally would fall short.
    {
        std::mutex m;
        std::vector<DsdEvent> evs;
        TetraKitProcess proc;
        TetraKitProcessConfig cfg;
        cfg.decoder_path = fake;
        cfg.forward_unknown = true; // the BITCOUNT report is kind "unknown"
        bool ok = proc.start(cfg,
            [&](const DsdEvent& e) { std::lock_guard<std::mutex> lk(m); evs.push_back(e); }, nullptr);
        check(ok, "start() for the truncation check");
        // Wait until the fake is up (it binds its -r socket before sending its
        // first JSON, so seeing any event means -r is ready to receive bits).
        // Without this the datagrams would race the child's bind() -- a
        // test-only concern; the real server streams bits continuously.
        wait_for([&] { std::lock_guard<std::mutex> lk(m); return !evs.empty(); });
        std::vector<unsigned char> bits(5000, 1); // > one 1024-byte datagram
        check(proc.write_bits(bits.data(), bits.size()), "write_bits (5000 bits) succeeded");
        DsdEvent bc;
        bool got = wait_for([&] {
            std::lock_guard<std::mutex> lk(m);
            for (const auto& e : evs)
                if (e.raw_line.find("BITCOUNT") != std::string::npos) { bc = e; return true; }
            return false;
        });
        check(got, "fake reported a bit-count");
        check(bc.source_id == "5000",
              "all 5000 bits arrived (datagrams stay within the decoder's read buffer)");
        proc.stop();
    }

    // ---- exec failure reported ----
    {
        TetraKitProcess proc;
        TetraKitProcessConfig cfg;
        cfg.decoder_path = "/nonexistent/tetra-kit-decoder-xyzzy";
        bool ok = proc.start(cfg, [](const DsdEvent&) {}, nullptr);
        check(!ok, "start() returns false when the binary can't be exec'd");
        check(!proc.running(), "running() false after failed start");
    }

    if (g_failures == 0) std::printf("ALL TETRA-KIT PROCESS TESTS PASSED\n");
    else std::printf("%d TETRA-KIT PROCESS TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
