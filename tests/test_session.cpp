// test_session.cpp
//
// Integration test for session.{hpp,cpp}: starts a real Server on a
// local port, connects a real Boost.Beast WebSocket client to it (in the
// same process, different thread), and drives the actual wire protocol
// documented at the top of session.hpp -- not a mock, not a unit test of
// an isolated piece.
//
// This file was originally written without a Boost install to compile
// against. It has since been built and run (gcc 13, Boost 1.83) and
// passes: the Beast client API calls all compiled as written. Two
// things did have to be fixed once it actually ran, both in the test
// harness rather than in the code under test -- see TestClient::read
// for why the read timeout needed rewriting, and
// test_audio_pipeline_relays_events_and_audio for why its talkgroup
// assertion was too strict.
//
// What this DOES verify, if it passes:
//   - The server accepts a WebSocket connection and the "start" control
//     message produces the documented "started" response.
//   - Sending IQ binary frames after "start" actually flows through
//     FmDemodulator -> DsdProcess -> the fake dsd-fme (see
//     test_fake_dsd_fme.cpp) -> back out as both a JSON "event" text
//     frame and a tagged binary audio frame -- i.e. the whole pipeline
//     wired up in session.cpp actually moves data end to end.
//   - Malformed control messages and malformed binary frame lengths get
//     the documented {"type":"error",...} response instead of crashing
//     or hanging the connection.
//   - Binary IQ frames sent before any "start" message are silently
//     ignored (per handle_binary_message's documented behavior) rather
//     than causing an error or a crash.
//
// What this does NOT verify:
//   - Real DMR decoding (the fake dsd-fme produces synthetic events
//     regardless of what audio it receives -- see test_fake_dsd_fme.cpp).
//   - Behavior of the liquid-dsp or DSDcc backends (this always builds
//     against the default FmDemodulator + DsdProcess backends; add
//     -DDSD_USE_LIQUID_DEMOD / -DDSD_USE_DSDCC_BACKEND to a variant of
//     this target if you want backend-specific coverage later).
//   - Multiple concurrent sessions, reconnection behavior, or anything
//     about the strand/threading fix described in session.cpp's
//     comments -- this test is deliberately simple (one client, one
//     session, sequential test cases) precisely because concurrency
//     bugs are the hardest thing to catch with a hand-rolled test like
//     this. A real stress test (many concurrent clients hammering the
//     server) would be a good next thing to build once this passes.

#include "session.hpp"
#include "json_util.hpp"
#include "test_ws_client.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <thread>
#include <vector>
#include <complex>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>

using namespace dsdsrv;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

// Test port. Server::Server() doesn't currently report bind failure back
// to the caller (see session.cpp) -- it just logs to stderr and leaves
// the acceptor not listening -- so if this port happens to be in use on
// your machine, every test below will hang waiting for a connection that
// never completes rather than failing fast with a clear error. Change
// this if that happens to you, or watch for a "bind failed" line in
// stderr before assuming a hang means a real bug.
constexpr unsigned short kTestPort = 18765;

// The client, check()/g_failures, and make_iq_frame now live in
// test_ws_client.hpp, shared with test_session_concurrency.cpp.
using namespace dsdtest;

// --- Test cases ----------------------------------------------------

void test_start_produces_started_response() {
    std::printf("test_start_produces_started_response\n");
    TestClient client;
    check(client.connect(kTestPort), "connects to server");

    check(client.send_text(json::Writer()
        .field("type", std::string("start"))
        .field("sample_rate", 2'000'000.0)
        .field("channel_bandwidth", 12500.0)
        .field("freq_offset", 0.0)
        .field("gain", 26000.0)
        .str()), "sends start message");

    std::string resp; bool is_text = false;
    check(client.read(resp, is_text), "receives a response after start");
    check(is_text, "response is a text frame");
    if (is_text) {
        auto obj = json::parse_flat_object(resp);
        check(json::get_string(obj, "type") == "started", "response type is \"started\"");
    }

    client.close();
}

void test_malformed_json_gets_error_response() {
    std::printf("test_malformed_json_gets_error_response\n");
    TestClient client;
    check(client.connect(kTestPort), "connects to server");

    check(client.send_text("{not valid json"), "sends malformed JSON");

    std::string resp; bool is_text = false;
    check(client.read(resp, is_text), "receives a response");
    if (is_text) {
        auto obj = json::parse_flat_object(resp);
        check(json::get_string(obj, "type") == "error", "response type is \"error\"");
    }

    client.close();
}

void test_unknown_message_type_gets_error_response() {
    std::printf("test_unknown_message_type_gets_error_response\n");
    TestClient client;
    check(client.connect(kTestPort), "connects to server");

    check(client.send_text(json::Writer().field("type", std::string("frobnicate")).str()),
          "sends an unrecognized message type");

    std::string resp; bool is_text = false;
    check(client.read(resp, is_text), "receives a response");
    if (is_text) {
        auto obj = json::parse_flat_object(resp);
        check(json::get_string(obj, "type") == "error", "response type is \"error\"");
    }

    client.close();
}

void test_malformed_binary_frame_gets_error_after_start() {
    std::printf("test_malformed_binary_frame_gets_error_after_start\n");
    TestClient client;
    check(client.connect(kTestPort), "connects to server");

    client.send_text(json::Writer().field("type", std::string("start"))
        .field("sample_rate", 2'000'000.0).str());
    std::string started; bool is_text = false;
    client.read(started, is_text); // consume the "started" response

    // 7 bytes: not a multiple of 8 (2 floats per complex sample).
    std::vector<uint8_t> bad_frame(7, 0);
    check(client.send_binary(bad_frame), "sends a malformed-length binary frame");

    // Read until the error frame, skipping anything else. The skip is
    // load-bearing, not defensive: this test started a pipeline, so the
    // fake dsd-fme's "ARGS:..." banner becomes an event frame that
    // RACES the error reply -- the error is usually queued first, but
    // not always, and reading exactly one frame here failed
    // intermittently (roughly 1 in 30 suite runs) when the banner won.
    bool got_error = false;
    for (int i = 0; i < 10 && !got_error; ++i) {
        std::string resp;
        if (!client.read(resp, is_text)) break;
        if (!is_text) continue;
        auto obj = json::parse_flat_object(resp);
        if (json::get_string(obj, "type") == "error") got_error = true;
    }
    check(got_error, "receives an \"error\" response");

    client.close();
}

void test_binary_before_start_is_silently_ignored() {
    std::printf("test_binary_before_start_is_silently_ignored\n");
    TestClient client;
    check(client.connect(kTestPort), "connects to server");

    // Well-formed frame, but no "start" message sent yet.
    auto frame = make_iq_frame(4096, 2'000'000.0, 1000.0, 2000.0);
    check(client.send_binary(frame), "sends a well-formed binary frame before start");

    // Expect no response at all -- a short timeout here is deliberate;
    // this test is specifically checking for silence, so it has to wait
    // out a timeout rather than react to a response.
    std::string resp; bool is_text = false;
    bool got_response = client.read(resp, is_text, std::chrono::milliseconds(500));
    check(!got_response, "no response arrives (frame is silently dropped, not errored)");

    client.close();
}

void test_audio_pipeline_relays_events_and_audio() {
    std::printf("test_audio_pipeline_relays_events_and_audio\n");
    TestClient client;
    check(client.connect(kTestPort), "connects to server");

    client.send_text(json::Writer().field("type", std::string("start"))
        .field("sample_rate", 2'000'000.0)
        .field("channel_bandwidth", 12500.0)
        .field("gain", 26000.0).str());
    std::string started; bool is_text = false;
    check(client.read(started, is_text), "receives started response");

    // Send several IQ blocks -- see make_iq_frame's comment: content
    // doesn't need to be a real DMR signal, just needs to produce PCM
    // for the fake dsd-fme (test_fake_dsd_fme.cpp) to receive and react
    // to, which is what actually exercises session.cpp's plumbing.
    for (int i = 0; i < 8; ++i) {
        auto frame = make_iq_frame(4096, 2'000'000.0, 1000.0, 2000.0);
        client.send_binary(frame);
    }

    bool saw_event = false;
    bool saw_call_event = false;
    bool saw_audio = false;
    // Read up to a handful of frames, or until we've seen both a
    // talkgroup-carrying event and an audio frame -- events and audio
    // can interleave in either order depending on thread scheduling, so
    // this doesn't assume a fixed sequence.
    //
    // Note this waits for an event carrying the talkgroup rather than
    // asserting that every event frame carries it. session.cpp emits
    // one event frame per line dsd-fme writes to stdout, and not every
    // such line is a decode: the fake's very first line is the
    // "ARGS:..." banner it echoes its argv with (see
    // test_fake_dsd_fme.cpp), which classify_line correctly reports as
    // kind "unknown" with no talkgroup. That is the intended behavior
    // of both ends -- the client gets the raw line either way -- so
    // what's worth asserting is that the fake's TG=12345 decode line
    // makes it through, not that nothing else does.
    for (int i = 0; i < 20 && !(saw_call_event && saw_audio); ++i) {
        std::string resp;
        if (!client.read(resp, is_text, std::chrono::seconds(3))) break;
        if (is_text) {
            auto obj = json::parse_flat_object(resp);
            if (json::get_string(obj, "type") == "event") {
                saw_event = true;
                if (json::get_string(obj, "talkgroup") == "12345") saw_call_event = true;
            }
        } else {
            if (!resp.empty() && static_cast<uint8_t>(resp[0]) == 0x01) {
                saw_audio = true;
                check(resp.size() > 1, "audio frame has payload beyond the 1-byte tag");
            }
        }
    }
    check(saw_event, "received at least one JSON event frame");
    check(saw_call_event, "an event carries the fake dsd-fme's talkgroup (12345)");
    check(saw_audio, "received at least one tagged binary audio frame");

    client.send_text(json::Writer().field("type", std::string("stop")).str());
    client.close();
}

} // namespace

int main() {
    // Point the server's DsdProcess at the fake dsd-fme built alongside
    // this test (see CMakeLists.txt's test_session target) instead of a
    // real dsd-fme binary. DsdProcessConfig::dsd_fme_path defaults to
    // the literal string "dsd-fme" and is resolved via PATH -- session.cpp
    // doesn't currently expose a way to override this per-session, so the
    // only lever available to a test is PATH itself: prepend a directory
    // containing an executable literally named "dsd-fme" before any
    // Session gets a chance to fork one.
    const char* self_dir_env = std::getenv("TEST_FAKE_DSD_FME_DIR");
    if (!self_dir_env) {
        std::printf("FATAL: TEST_FAKE_DSD_FME_DIR not set -- see CMakeLists.txt's "
                     "test_session target, which is supposed to set this to the "
                     "directory containing a \"dsd-fme\"-named copy of "
                     "test-fake-dsd-fme before running this test.\n");
        return 1;
    }
    std::string new_path = std::string(self_dir_env) + ":" + (std::getenv("PATH") ? std::getenv("PATH") : "");
    setenv("PATH", new_path.c_str(), 1);

    net::io_context server_ioc{2};
    Server server(server_ioc, tcp::endpoint{net::ip::make_address("0.0.0.0"), kTestPort});

    std::vector<std::thread> pool;
    for (int i = 0; i < 2; ++i) pool.emplace_back([&server_ioc] { server_ioc.run(); });

    // Give the acceptor a moment to actually be listening before the
    // first client tries to connect.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    test_start_produces_started_response();
    test_malformed_json_gets_error_response();
    test_unknown_message_type_gets_error_response();
    test_malformed_binary_frame_gets_error_after_start();
    test_binary_before_start_is_silently_ignored();
    test_audio_pipeline_relays_events_and_audio();

    server_ioc.stop();
    for (auto& t : pool) t.join();

    const int failures = g_failures.load();
    if (failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    } else {
        std::printf("\n%d CHECK(S) FAILED\n", failures);
        return 1;
    }
}
