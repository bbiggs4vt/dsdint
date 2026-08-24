// test_session_concurrency.cpp
//
// Concurrency stress tests for session.{hpp,cpp} -- the "real stress
// test (many concurrent clients hammering the server)" that
// test_session.cpp's top-of-file comment names as the next thing to
// build. That file stays deliberately simple (one client, one session,
// sequential cases); this one is where the threading gets exercised.
//
// Both drive the server through the same client (test_ws_client.hpp)
// and the same fake dsd-fme (test_fake_dsd_fme.cpp), so what differs
// here is purely the concurrency, not the protocol.
//
// What this targets, and why each case exists:
//
//   1. Many concurrent sessions. Each connected client gets its own
//      Session, its own demod worker thread, and its own dsd-fme child
//      process. This is the basic "does the server hold up with N
//      clients at once" case, and it's what makes every other case
//      below run against a genuinely busy server.
//
//   2. Distinct UDP audio ports. session.cpp allocates each session a
//      random port for dsd-fme's decoded audio "to avoid collisions
//      between concurrent client sessions". A collision would mean one
//      session receiving another's audio, so it's worth asserting --
//      and asserting it paid off immediately; see the comment on that
//      test for the allocator bug it caught.
//
//   3. Start/stop churn. Each start_pipeline/stop_pipeline cycle spawns
//      and reaps a child process and starts and joins a worker thread.
//      Doing that repeatedly on a live connection is where teardown
//      races show up.
//
//   4. Control messages interleaved with streaming IQ. Pointed at a
//      specific hazard: Session::handle_text_message calls
//      demod_->set_gain()/set_freq_offset() on the network thread while
//      Session::demod_worker_loop calls demod_->process() on the worker
//      thread -- and FmDemodulator documents its setters as only safe
//      from the thread that owns the object. When first run under TSan
//      this was a real data race (now serialized by Session's
//      demod_mutex_; this case is what keeps it that way). The client
//      here is deliberately single-threaded; the concurrency under
//      test is server-side, so a serial client is enough.
//
//   5. Abrupt disconnects under load. Clients that vanish mid-stream
//      without a closing handshake, exercising Session teardown from
//      the read-error path while dsd-fme callbacks may still be posting
//      to the strand -- the weak_ptr paths in start_pipeline's
//      callbacks. The server must survive and keep serving.
//
// IMPORTANT -- what a pass here does and does not mean. Several of the
// bugs this file exists to catch are data races, and a data race is
// undefined behavior that usually does nothing visible on x86 with
// these types -- the plain build will almost certainly pass whether or
// not a race is present. (The first TSan run of this file proved the
// point: the plain build was passing while TSan reported 25 races
// across three distinct defects.) To actually detect races, build the
// ThreadSanitizer variant, which CMake offers as a separate target:
//
//     cmake -DDSD_BUILD_TSAN_TESTS=ON -S . -B build
//     cmake --build build --target test_session_concurrency_tsan
//     TEST_FAKE_DSD_FME_DIR=<build dir> ./build/test_session_concurrency_tsan
//
// Treat the plain build as "the server doesn't deadlock, crash, or lose
// frames under load" and the TSan build as "the server has no data
// races on these paths". They are different claims and you want both.

#include "session.hpp"
#include "json_util.hpp"
#include "test_ws_client.hpp"

#include <boost/asio/ip/tcp.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace dsdsrv;
using namespace dsdtest;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

// Deliberately not test_session.cpp's port: the two binaries are
// separate ctest cases and ctest may run them in parallel, so sharing a
// port would make them collide. See test_session.cpp's note on
// Server::Server not reporting bind failures -- that failure mode is
// silent, so keeping the ports distinct matters more than it looks.
constexpr unsigned short kTestPort = 18766;

// How many clients hammer the server at once. Chosen to oversubscribe a
// typical CI box (each client means a Session, a demod worker thread,
// and a dsd-fme child process) without the process count getting silly.
constexpr int kConcurrentClients = 8;

const char* start_msg(double gain = 26000.0) {
    static thread_local std::string s;
    s = json::Writer()
            .field("type", std::string("start"))
            .field("sample_rate", 2'000'000.0)
            .field("channel_bandwidth", 12500.0)
            .field("freq_offset", 0.0)
            .field("gain", gain)
            .str();
    return s.c_str();
}

// Reads frames until one is a text frame with the given "type", or the
// budget is exhausted. Necessary because the server legitimately
// interleaves other traffic with the frame a test is waiting for: the
// fake dsd-fme emits its "ARGS:..." banner the moment it spawns and an
// "EOF received..." line when its stdin closes, and session.cpp turns
// every dsd-fme stdout line into an event frame. So "the next frame" is
// not reliably "the frame I asked for".
bool read_until_type(TestClient& c, const std::string& want, std::string& out,
                     std::chrono::milliseconds timeout = std::chrono::seconds(5),
                     int max_frames = 60) {
    for (int i = 0; i < max_frames; ++i) {
        std::string resp;
        bool is_text = false;
        if (!c.read(resp, is_text, timeout)) return false;
        if (!is_text) continue;
        try {
            auto obj = json::parse_flat_object(resp);
            if (json::get_string(obj, "type") == want) {
                out = resp;
                return true;
            }
        } catch (const std::exception&) {
            // Not parseable as a flat object; not the frame we want.
        }
    }
    return false;
}

// --- Test cases ----------------------------------------------------

// 1. N clients running the full pipeline simultaneously. Each verifies
// its own session end to end, so a failure names the client that broke.
void test_many_concurrent_sessions() {
    std::printf("test_many_concurrent_sessions (%d clients)\n", kConcurrentClients);

    std::atomic<int> started{0}, got_event{0}, got_audio{0}, connected{0};
    std::vector<std::thread> clients;

    for (int id = 0; id < kConcurrentClients; ++id) {
        clients.emplace_back([&, id] {
            TestClient c;
            if (!c.connect(kTestPort)) return;
            ++connected;

            if (!c.send_text(start_msg())) return;
            std::string resp;
            if (!read_until_type(c, "started", resp)) return;
            ++started;

            for (int i = 0; i < 8; ++i) {
                c.send_binary(make_iq_frame(4096, 2'000'000.0, 1000.0, 2000.0));
            }

            bool saw_call_event = false, saw_audio = false;
            for (int i = 0; i < 40 && !(saw_call_event && saw_audio); ++i) {
                std::string r;
                bool is_text = false;
                if (!c.read(r, is_text, std::chrono::seconds(5))) break;
                if (is_text) {
                    try {
                        auto obj = json::parse_flat_object(r);
                        if (json::get_string(obj, "type") == "event" &&
                            json::get_string(obj, "talkgroup") == "12345") {
                            saw_call_event = true;
                        }
                    } catch (const std::exception&) {}
                } else if (!r.empty() && static_cast<uint8_t>(r[0]) == 0x01) {
                    saw_audio = true;
                }
            }
            if (saw_call_event) ++got_event;
            if (saw_audio) ++got_audio;

            c.send_text(json::Writer().field("type", std::string("stop")).str());
            c.close();
        });
    }
    for (auto& t : clients) t.join();

    check(connected == kConcurrentClients, "all clients connected concurrently");
    check(started == kConcurrentClients, "every concurrent session reported \"started\"");
    check(got_event == kConcurrentClients, "every concurrent session received its decode event");
    check(got_audio == kConcurrentClients, "every concurrent session received audio");
}

// 2. Concurrent sessions must not be handed the same UDP audio port --
// dsd-fme sends decoded audio there, so a collision means one client
// hearing another's traffic.
//
// This assertion is a hard guarantee, not probabilistic: session.cpp's
// acquire_udp_port() tracks live allocations and refuses to hand out a
// port already in use. It wasn't always -- the original allocator drew
// from an unlocked shared mt19937 with no bookkeeping, and this very
// test caught it handing identical ports to concurrently-starting
// sessions in roughly 1 run in 5 (racing threads corrupt the RNG state,
// which collides far more often than honest random draws would). If
// this test ever fails again, suspect the allocator's locking first.
void test_concurrent_sessions_get_distinct_udp_ports() {
    std::printf("test_concurrent_sessions_get_distinct_udp_ports\n");

    std::mutex m;
    std::vector<int> ports;
    std::vector<std::thread> clients;
    // Hold every session open until all of them have started, so the
    // ports really are allocated simultaneously rather than being
    // recycled one after another.
    std::atomic<int> ready{0};

    for (int id = 0; id < kConcurrentClients; ++id) {
        clients.emplace_back([&] {
            TestClient c;
            if (!c.connect(kTestPort)) { ++ready; return; }
            if (!c.send_text(start_msg())) { ++ready; return; }

            std::string resp;
            if (read_until_type(c, "started", resp)) {
                try {
                    auto obj = json::parse_flat_object(resp);
                    int port = static_cast<int>(json::get_number(obj, "udp_audio_port", 0.0));
                    std::lock_guard<std::mutex> lock(m);
                    ports.push_back(port);
                } catch (const std::exception&) {}
            }
            ++ready;
            while (ready.load() < kConcurrentClients) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            c.close();
        });
    }
    for (auto& t : clients) t.join();

    check(static_cast<int>(ports.size()) == kConcurrentClients,
          "collected a udp_audio_port from every concurrent session");

    const bool all_in_range = std::all_of(ports.begin(), ports.end(),
                                          [](int p) { return p >= 40000 && p <= 59000; });
    check(all_in_range, "every allocated udp_audio_port is in the documented range");

    std::set<int> distinct(ports.begin(), ports.end());
    check(distinct.size() == ports.size(),
          "concurrent sessions were allocated distinct udp_audio_ports");
}

// 3. Repeated start/stop on one live connection. Each cycle spawns and
// reaps a dsd-fme child and starts and joins a demod worker thread, so
// this is the teardown path under repetition rather than under width.
void test_rapid_start_stop_churn() {
    constexpr int kCycles = 15;
    std::printf("test_rapid_start_stop_churn (%d cycles)\n", kCycles);

    TestClient c;
    check(c.connect(kTestPort), "connects to server");

    int started_count = 0;
    for (int i = 0; i < kCycles; ++i) {
        if (!c.send_text(start_msg())) break;
        std::string resp;
        if (!read_until_type(c, "started", resp)) break;
        ++started_count;

        // A little traffic so the pipeline is actually doing work when
        // the stop arrives, rather than being torn down while idle.
        c.send_binary(make_iq_frame(2048, 2'000'000.0, 1000.0, 2000.0));
        if (!c.send_text(json::Writer().field("type", std::string("stop")).str())) break;
    }
    check(started_count == kCycles, "every start/stop cycle produced a \"started\"");

    // The connection must still be usable after all that churn.
    c.send_text(json::Writer().field("type", std::string("bogus_type")).str());
    std::string resp;
    check(read_until_type(c, "error", resp), "connection still responsive after churn");
    c.close();
}

// 4. Control messages interleaved with streaming IQ -- the
// set_gain/process race described at the top of this file.
//
// The client stays single-threaded on purpose: Beast allows only one
// write in flight per stream, so a multi-threaded client would be
// racing itself rather than testing the server. Serial interleaving is
// enough, because the two sides of the race are both server-side (the
// network thread handling the control message and the worker thread
// running the demod).
void test_control_messages_during_streaming() {
    constexpr int kRounds = 60;
    std::printf("test_control_messages_during_streaming (%d rounds)\n", kRounds);

    TestClient c;
    check(c.connect(kTestPort), "connects to server");
    check(c.send_text(start_msg()), "starts the pipeline");

    std::string resp;
    check(read_until_type(c, "started", resp), "pipeline started");

    bool all_sent = true;
    for (int i = 0; i < kRounds; ++i) {
        // Queue IQ so the worker thread has work in flight...
        if (!c.send_binary(make_iq_frame(4096, 2'000'000.0, 1000.0, 2000.0))) { all_sent = false; break; }
        // ...then mutate the demodulator from under it.
        if (!c.send_text(json::Writer()
                             .field("type", std::string("set_gain"))
                             .field("gain", 20000.0 + (i % 10) * 1000.0).str())) { all_sent = false; break; }
        if (!c.send_binary(make_iq_frame(4096, 2'000'000.0, 1000.0, 2000.0))) { all_sent = false; break; }
        if (!c.send_text(json::Writer()
                             .field("type", std::string("set_freq_offset"))
                             .field("hz", (i % 7) * 500.0).str())) { all_sent = false; break; }
    }
    check(all_sent, "streamed IQ interleaved with live control messages");

    // Liveness probe: an unknown type must still come back as an error,
    // which proves the read loop survived the interleaving.
    c.send_text(json::Writer().field("type", std::string("bogus_type")).str());
    check(read_until_type(c, "error", resp, std::chrono::seconds(10), 400),
          "connection still responsive after interleaved control traffic");

    c.send_text(json::Writer().field("type", std::string("stop")).str());
    c.close();
}

// 5. Clients that disappear mid-stream without a closing handshake,
// while other sessions are live. Session teardown then runs from the
// read-error path with dsd-fme callbacks potentially still posting to
// the strand.
void test_abrupt_disconnects_under_load() {
    constexpr int kAborters = 6;
    std::printf("test_abrupt_disconnects_under_load (%d aborting clients)\n", kAborters);

    std::atomic<int> streamed{0};
    std::vector<std::thread> clients;

    for (int id = 0; id < kAborters; ++id) {
        clients.emplace_back([&, id] {
            TestClient c;
            if (!c.connect(kTestPort)) return;
            if (!c.send_text(start_msg())) return;
            std::string resp;
            if (!read_until_type(c, "started", resp)) return;

            for (int i = 0; i < 6; ++i) {
                c.send_binary(make_iq_frame(4096, 2'000'000.0, 1000.0, 2000.0));
            }
            ++streamed;
            // Stagger the drops so they land at different points in the
            // other sessions' processing rather than all at once.
            std::this_thread::sleep_for(std::chrono::milliseconds(10 * id));
            c.abort_connection(); // no close handshake -- client "crashes"
        });
    }
    for (auto& t : clients) t.join();

    check(streamed == kAborters, "all aborting clients streamed before dropping");

    // Give the server a moment to finish reaping those sessions, then
    // prove it still works: a fresh client must get a complete pipeline.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    TestClient c;
    check(c.connect(kTestPort), "server still accepts connections after abrupt drops");
    check(c.send_text(start_msg()), "sends start on the fresh connection");
    std::string resp;
    check(read_until_type(c, "started", resp), "fresh session starts normally after abrupt drops");

    for (int i = 0; i < 8; ++i) {
        c.send_binary(make_iq_frame(4096, 2'000'000.0, 1000.0, 2000.0));
    }
    bool saw_audio = false;
    for (int i = 0; i < 40 && !saw_audio; ++i) {
        std::string r;
        bool is_text = false;
        if (!c.read(r, is_text, std::chrono::seconds(5))) break;
        if (!is_text && !r.empty() && static_cast<uint8_t>(r[0]) == 0x01) saw_audio = true;
    }
    check(saw_audio, "fresh session still relays audio after abrupt drops");

    c.send_text(json::Writer().field("type", std::string("stop")).str());
    c.close();
}

} // namespace

int main() {
    // Same PATH trick as test_session.cpp -- see its main() for why the
    // fake backend has to be found under the literal name "dsd-fme".
    const char* self_dir_env = std::getenv("TEST_FAKE_DSD_FME_DIR");
    if (!self_dir_env) {
        std::printf("FATAL: TEST_FAKE_DSD_FME_DIR not set -- see CMakeLists.txt's "
                    "test_session_concurrency target.\n");
        return 1;
    }
    std::string new_path = std::string(self_dir_env) + ":" +
                           (std::getenv("PATH") ? std::getenv("PATH") : "");
    setenv("PATH", new_path.c_str(), 1);

    // More io_context threads than test_session.cpp's 2. Sessions are
    // bound to per-connection strands, so concurrency across sessions
    // only actually happens if there are threads to run them on -- with
    // a single-threaded pool this file would be testing sequential
    // execution with extra steps.
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned n_threads = std::max(4u, hw ? hw : 4u);
    std::printf("server io_context threads: %u (hardware_concurrency=%u)\n\n", n_threads, hw);

    net::io_context server_ioc{static_cast<int>(n_threads)};
    Server server(server_ioc, tcp::endpoint{net::ip::make_address("0.0.0.0"), kTestPort});

    std::vector<std::thread> pool;
    for (unsigned i = 0; i < n_threads; ++i) pool.emplace_back([&server_ioc] { server_ioc.run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    test_many_concurrent_sessions();
    test_concurrent_sessions_get_distinct_udp_ports();
    test_rapid_start_stop_churn();
    test_control_messages_during_streaming();
    test_abrupt_disconnects_under_load();

    server_ioc.stop();
    for (auto& t : pool) t.join();

    const int failures = g_failures.load();
    if (failures == 0) {
        std::printf("\nALL CONCURRENCY TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", failures);
    return 1;
}
