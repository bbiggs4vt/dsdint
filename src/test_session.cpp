// test_session.cpp
//
// Integration test for session.{hpp,cpp}: starts a real Server on a
// local port, connects a real Boost.Beast WebSocket client to it (in the
// same process, different thread), and drives the actual wire protocol
// documented at the top of session.hpp -- not a mock, not a unit test of
// an isolated piece.
//
// This is, by a wide margin, the most complex file in this project that
// I have not been able to compile myself (no Boost in the sandbox this
// was written in). Treat this build as the real first compile of it --
// same as session.cpp originally was, and same as fm_demod_liquid.cpp
// was before you ran it and it turned out fine. I reviewed the Beast
// client API calls carefully (they mirror patterns already used
// server-side in session.cpp, which is some reassurance, but not the
// same as a compiler having checked them) -- if something doesn't
// compile, the errors are the most useful thing you can hand back to me.
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

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  OK: %s\n", what.c_str());
    } else {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

// Thin synchronous WebSocket client wrapper for this test file only.
// Deliberately simple (blocking calls, explicit per-read timeouts) since
// a test harness benefits from being obviously correct more than from
// being efficient.
class TestClient {
public:
    TestClient() : ws_(ioc_) {}

    bool connect(unsigned short port) {
        tcp::resolver resolver(ioc_);
        beast::error_code ec;
        auto results = resolver.resolve("127.0.0.1", std::to_string(port), ec);
        if (ec) { std::printf("  resolve failed: %s\n", ec.message().c_str()); return false; }

        beast::get_lowest_layer(ws_).connect(results, ec);
        if (ec) { std::printf("  connect failed: %s\n", ec.message().c_str()); return false; }

        ws_.handshake("127.0.0.1", "/", ec);
        if (ec) { std::printf("  handshake failed: %s\n", ec.message().c_str()); return false; }
        return true;
    }

    bool send_text(const std::string& msg) {
        ws_.text(true);
        beast::error_code ec;
        ws_.write(net::buffer(msg), ec);
        return !ec;
    }

    bool send_binary(const std::vector<uint8_t>& data) {
        ws_.binary(true);
        beast::error_code ec;
        ws_.write(net::buffer(data), ec);
        return !ec;
    }

    // Blocking read with an explicit timeout. Returns false on
    // timeout/error (check `ec_out` if you need to distinguish them);
    // on success, fills `out` and sets `is_text`.
    bool read(std::string& out, bool& is_text,
              std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        beast::flat_buffer buffer;
        beast::get_lowest_layer(ws_).expires_after(timeout);
        beast::error_code ec;
        ws_.read(buffer, ec);
        beast::get_lowest_layer(ws_).expires_never(); // don't leave a timeout armed between calls
        if (ec) return false;
        is_text = ws_.got_text();
        out = beast::buffers_to_string(buffer.data());
        return true;
    }

    void close() {
        beast::error_code ec;
        ws_.close(websocket::close_code::normal, ec);
    }

private:
    net::io_context ioc_;
    websocket::stream<beast::tcp_stream> ws_;
};

// Synthetic FM-modulated IQ block, same construction as
// test_fm_demod.cpp/test_fm_demod_liquid.cpp -- content doesn't matter
// much here (the fake dsd-fme produces events regardless of what PCM it
// receives), it just needs to be enough real-looking data to make it
// through FmDemodulator and produce *some* PCM output for DsdProcess to
// forward.
std::vector<uint8_t> make_iq_frame(std::size_t n_samples, double fs, double tone_hz, double deviation_hz) {
    std::vector<cf32> iq(n_samples);
    double phase = 0.0;
    for (std::size_t i = 0; i < n_samples; ++i) {
        const double t = i / fs;
        const double inst_freq = deviation_hz * std::sin(2.0 * M_PI * tone_hz * t);
        phase += 2.0 * M_PI * inst_freq / fs;
        iq[i] = cf32{static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
    }
    std::vector<uint8_t> bytes(n_samples * 2 * sizeof(float));
    std::memcpy(bytes.data(), iq.data(), bytes.size());
    return bytes;
}

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

    std::string resp;
    check(client.read(resp, is_text), "receives a response");
    if (is_text) {
        auto obj = json::parse_flat_object(resp);
        check(json::get_string(obj, "type") == "error", "response type is \"error\"");
    }

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
    bool saw_audio = false;
    // Read up to a handful of frames, or until we've seen one of each --
    // events and audio can interleave in either order depending on
    // thread scheduling, so this doesn't assume a fixed sequence.
    for (int i = 0; i < 20 && !(saw_event && saw_audio); ++i) {
        std::string resp;
        if (!client.read(resp, is_text, std::chrono::seconds(3))) break;
        if (is_text) {
            auto obj = json::parse_flat_object(resp);
            if (json::get_string(obj, "type") == "event") {
                saw_event = true;
                check(json::get_string(obj, "talkgroup") == "12345",
                      "event carries the fake dsd-fme's talkgroup (12345)");
            }
        } else {
            if (!resp.empty() && static_cast<uint8_t>(resp[0]) == 0x01) {
                saw_audio = true;
                check(resp.size() > 1, "audio frame has payload beyond the 1-byte tag");
            }
        }
    }
    check(saw_event, "received at least one JSON event frame");
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

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    } else {
        std::printf("\n%d CHECK(S) FAILED\n", g_failures);
        return 1;
    }
}
