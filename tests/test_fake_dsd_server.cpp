// test_fake_dsd_server.cpp
//
// Validates tools/fake_dsd_server.hpp — the drop-in fake dsd-server for
// client test suites — by driving it with an INDEPENDENT WebSocket client
// (Boost.Beast, unrelated to the hand-rolled framing under test). If the
// fake server's handshake/framing/JSON are correct, a real WebSocket
// library talks to it cleanly. This doubles as the worked example of how
// a client test uses the fixture: script the server, assert on what the
// client received, and assert on what the fake server recorded the client
// sending.

#include "../tools/fake_dsd_server.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace fakedsd;
using namespace std::chrono_literals;

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {
int g_failures = 0;
void check(bool c, const std::string& what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what.c_str());
    if (!c) ++g_failures;
}

// Minimal synchronous Boost.Beast client (independent of the server code).
class Client {
public:
    bool connect(uint16_t port) {
        tcp::resolver r(ioc_);
        beast::error_code ec;
        auto res = r.resolve("127.0.0.1", std::to_string(port), ec);
        if (ec) return false;
        beast::get_lowest_layer(ws_).connect(res, ec);
        if (ec) return false;
        ws_.handshake("127.0.0.1", "/", ec);
        return !ec;
    }
    bool send_text(const std::string& s) {
        ws_.text(true); beast::error_code ec; ws_.write(net::buffer(s), ec); return !ec;
    }
    bool send_binary(const std::vector<uint8_t>& b) {
        ws_.binary(true); beast::error_code ec; ws_.write(net::buffer(b), ec); return !ec;
    }
    bool read(std::string& out, bool& is_text, std::chrono::milliseconds timeout = 2s) {
        beast::flat_buffer buf; beast::error_code ec; bool done = false;
        ws_.async_read(buf, [&](beast::error_code e, std::size_t) { ec = e; done = true; });
        ioc_.restart(); ioc_.run_for(timeout);
        if (!done) {
            beast::error_code x; beast::get_lowest_layer(ws_).socket().close(x);
            ioc_.restart(); ioc_.run();
            return false;
        }
        if (ec) return false;
        is_text = ws_.got_text();
        out = beast::buffers_to_string(buf.data());
        return true;
    }
    void close() { beast::error_code ec; ws_.close(websocket::close_code::normal, ec); }
private:
    net::io_context ioc_;
    websocket::stream<beast::tcp_stream> ws_{ioc_};
};

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
} // namespace

int main() {
    std::printf("test_fake_dsd_server\n");

    // --- Core round trip: handshake, start/started, event, audio, IQ ---
    {
        FakeDsdServer::Options opts;
        opts.started_udp_port = 0; // DSDcc-style (mono, no UDP port)
        FakeDsdServer srv(opts);
        srv.start();

        Client client;
        check(client.connect(srv.port()), "Beast client completes the WebSocket handshake");
        check(srv.wait_for_client(1s), "server observes the client connection");

        // Client -> server control frame; the fake records it for assertions.
        check(client.send_text(R"({"type":"start","sample_rate":2000000,"protocol":"dmr","key_type":"bp","key":"1"})"),
              "client sends a start control frame");

        std::string resp; bool is_text = false;
        check(client.read(resp, is_text), "client receives a reply to start");
        check(is_text, "started reply is a text frame");
        check(has(resp, "\"type\":\"started\""), "reply is a started frame");
        check(has(resp, "\"udp_audio_port\":0"), "started reports udp_audio_port 0 (DSDcc-style)");

        check(srv.wait_for_control(1s, 1), "server recorded the control message");
        ControlMessage start = srv.last_start();
        check(start.type == "start", "recorded control type is start");
        check(start.fields["protocol"] == "dmr", "recorded start carried protocol=dmr");
        check(start.fields["key_type"] == "bp", "recorded start carried key_type=bp");
        check(start.fields["key"] == "1", "recorded start carried key=1");
        check(start.fields["sample_rate"] == "2000000", "recorded start carried the sample_rate");

        // Server -> client: a scripted event.
        srv.send_event(Event{}.k("call").tg("150607").src("2222223").sl("2")
                            .rw("(fake) group call"));
        check(client.read(resp, is_text), "client receives the scripted event");
        check(is_text && has(resp, "\"type\":\"event\""), "event frame arrives as text");
        check(has(resp, "\"kind\":\"call\""), "event carries kind=call");
        check(has(resp, "\"talkgroup\":\"150607\""), "event carries the scripted talkgroup");
        check(has(resp, "\"source_id\":\"2222223\""), "event carries the scripted source");
        check(has(resp, "\"nac\":\"\""), "unset event fields are present and empty");

        // Server -> client: a tagged PCM audio frame.
        srv.send_audio(std::vector<int16_t>(160, 0));
        bool audio_is_text = true;
        check(client.read(resp, audio_is_text), "client receives the audio frame");
        check(!audio_is_text, "audio frame is binary");
        check(resp.size() == 1 + 160 * 2, "audio frame is tag byte + 160 int16 samples");
        check(!resp.empty() && static_cast<uint8_t>(resp[0]) == 0x01, "audio frame carries the 0x01 PCM tag");

        // Client -> server: streamed IQ is counted (not decoded).
        check(client.send_binary(std::vector<uint8_t>(800, 0)), "client streams IQ bytes");
        // Give the server thread a moment to drain the frame.
        for (int i = 0; i < 100 && srv.iq_bytes_received() < 800; ++i)
            std::this_thread::sleep_for(10ms);
        check(srv.iq_bytes_received() == 800, "server counted the streamed IQ bytes");

        // Server -> client: injected error path.
        srv.send_error("simulated backend failure");
        check(client.read(resp, is_text), "client receives an injected error");
        check(has(resp, "\"type\":\"error\""), "error frame has type=error");
        check(has(resp, "simulated backend failure"), "error frame carries the message");

        client.close();
        srv.stop();
    }

    // --- dsd-fme-style started (non-zero udp_audio_port) + no auto-started ---
    {
        FakeDsdServer::Options opts;
        opts.started_udp_port = 46000;
        opts.auto_started = false; // drive the reply by hand from on_control
        FakeDsdServer srv(opts);
        bool saw_start = false;
        srv.on_control = [&](const ControlMessage& m, FakeDsdServer& s) {
            if (m.type == "start") { saw_start = true; s.send_started(); }
        };
        srv.start();

        Client client;
        check(client.connect(srv.port()), "second client connects");
        check(client.send_text(R"({"type":"start"})"), "client sends start");

        std::string resp; bool is_text = false;
        check(client.read(resp, is_text), "client receives the hand-driven started reply");
        check(has(resp, "\"udp_audio_port\":46000"), "started reports udp_audio_port 46000 (dsd-fme-style)");
        check(saw_start, "on_control hook observed the start");

        client.close();
        srv.stop();
    }

    // --- Incremental, IQ-driven scenario (the on_iq hook) ---
    // Mirrors the standalone runner: "start" emits nothing but "started";
    // the first IQ push acquires the signal (sync + call), and every push
    // after that streams a voice frame.
    {
        FakeDsdServer srv;
        srv.on_iq = [](std::size_t frame_index, std::size_t, FakeDsdServer& s) {
            if (frame_index == 0) {
                s.send_event(Event{}.k("sync").sl("2").cc("4"));
                s.send_event(Event{}.k("call").tg("150607").src("2222223").sl("2"));
            } else {
                s.send_audio(std::vector<int16_t>(160, 0));
            }
        };
        srv.start();

        Client client;
        check(client.connect(srv.port()), "iq-driven: client connects");
        check(client.send_text(R"({"type":"start"})"), "iq-driven: client sends start");

        std::string resp; bool is_text = false;
        check(client.read(resp, is_text), "iq-driven: start still gets a started reply");
        check(has(resp, "\"type\":\"started\""), "iq-driven: reply is started");

        // No events precede IQ: the sync event below arrives only as the
        // response to the first push (nothing but "started" came before it),
        // which is what proves the output is IQ-driven, not start-driven.

        // First IQ push -> sync + call.
        check(client.send_binary(std::vector<uint8_t>(800, 0)), "iq-driven: first IQ push");
        check(client.read(resp, is_text) && is_text && has(resp, "\"kind\":\"sync\""),
              "iq-driven: first push yields the sync event");
        check(client.read(resp, is_text) && is_text && has(resp, "\"kind\":\"call\"")
              && has(resp, "\"talkgroup\":\"150607\""),
              "iq-driven: first push yields the call event");

        // Subsequent IQ push -> audio.
        check(client.send_binary(std::vector<uint8_t>(800, 0)), "iq-driven: second IQ push");
        bool audio_is_text = true;
        check(client.read(resp, audio_is_text) && !audio_is_text
              && !resp.empty() && static_cast<uint8_t>(resp[0]) == 0x01,
              "iq-driven: later push yields a tagged audio frame");
        check(srv.iq_frames_received() == 2, "iq-driven: server counted both IQ frames");

        client.close();
        srv.stop();
    }

    if (g_failures == 0) { std::printf("\nALL FAKE DSD SERVER TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
