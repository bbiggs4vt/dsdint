// test_ws_client.hpp
//
// Shared test harness for the WebSocket integration tests
// (test_session.cpp and test_session_concurrency.cpp): a small
// synchronous client, the check()/g_failures assertion counter, and the
// synthetic IQ generator. Extracted from test_session.cpp so both test
// binaries drive the server through exactly one client implementation
// -- the timeout handling in TestClient::read below is subtle enough
// that having two copies drift apart would be a real hazard.
//
// Header-only and only ever linked into a single translation unit per
// test binary; g_failures is inline so it stays one counter per binary.

#pragma once

#include "session.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace dsdtest {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using dsdsrv::cf32;

inline std::atomic<int> g_failures{0};

// Thread-safe: the concurrency tests call this from many client threads
// at once, so the counter is atomic and the print is serialized -- two
// threads interleaving inside printf produced genuinely unreadable
// output otherwise. The lock also keeps a result line intact rather
// than letting a FAIL get spliced into the middle of an OK.
inline void check(bool condition, const std::string& what) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (condition) {
        std::printf("  OK: %s\n", what.c_str());
    } else {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

// Same serialization for free-form progress lines from worker threads.
inline void note(const std::string& what) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    std::printf("  %s\n", what.c_str());
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
    // timeout/error; on success, fills `out` and sets `is_text`.
    //
    // This has to go through async_read + io_context::run_for rather
    // than the more obvious expires_after() + synchronous ws_.read().
    // beast::tcp_stream's timeout only applies to *asynchronous*
    // operations ("A logical operation is any series of one or more
    // direct or indirect calls to the timeout stream's asynchronous
    // read, asynchronous write, or asynchronous connect functions" --
    // boost/beast/core/basic_stream.hpp); its synchronous read_some
    // goes straight to the underlying socket and never consults the
    // timer. So the synchronous version blocked forever whenever a
    // response legitimately never arrived, which is exactly the case
    // test_binary_before_start_is_silently_ignored is built to check.
    bool read(std::string& out, bool& is_text,
              std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        beast::flat_buffer buffer;
        beast::error_code ec;
        bool done = false;
        ws_.async_read(buffer, [&](beast::error_code e, std::size_t) {
            ec = e;
            done = true;
        });
        if (!run_until(done, timeout)) return false; // timed out
        if (ec) return false;
        is_text = ws_.got_text();
        out = beast::buffers_to_string(buffer.data());
        return true;
    }

    // Errors are ignored: by the time a test closes it has already made
    // its checks, and a client whose read timed out has had its socket
    // closed underneath it (see run_until) so the handshake is expected
    // to fail there.
    // Hard close: drops the TCP connection with no WebSocket closing
    // handshake, the way a crashed or killed client would. The server
    // sees this as a read error rather than websocket::error::closed,
    // which is a distinct teardown path in Session::on_read -- the
    // concurrency tests use this to exercise it while dsd-fme callbacks
    // may still be in flight for that session.
    void abort_connection() {
        beast::error_code ec;
        beast::get_lowest_layer(ws_).socket().close(ec);
    }

    void close() {
        bool done = false;
        ws_.async_close(websocket::close_code::normal, [&](beast::error_code) { done = true; });
        // Bounded, for the same reason read() is: the closing handshake
        // waits for the peer's close frame, and a test should fail
        // rather than hang if that never comes.
        run_until(done, std::chrono::seconds(2));
    }

private:
    // Runs this client's io_context until `done` is set or `timeout`
    // elapses. Returns true if the operation completed in time.
    //
    // On timeout it closes the underlying socket -- which aborts the
    // pending operation, the same way beast::tcp_stream's own timeout
    // is documented to behave -- and then drains the context. That
    // drain is not optional: the completion handlers above capture
    // stack locals by reference, so they must be guaranteed to have run
    // before this function returns. Once a call here times out the
    // stream is finished; every test that hits a timeout only closes
    // afterwards, and close()'s errors are ignored.
    bool run_until(bool& done, std::chrono::milliseconds timeout) {
        ioc_.restart();
        ioc_.run_for(timeout);
        if (done) return true;
        beast::error_code ec;
        beast::get_lowest_layer(ws_).socket().close(ec);
        ioc_.restart();
        ioc_.run();
        return false;
    }

    net::io_context ioc_;
    websocket::stream<beast::tcp_stream> ws_;
};

// Synthetic FM-modulated IQ block, same construction as
// test_fm_demod.cpp/test_fm_demod_liquid.cpp -- content doesn't matter
// much here (the fake dsd-fme produces events regardless of what PCM it
// receives), it just needs to be enough real-looking data to make it
// through FmDemodulator and produce *some* PCM output for DsdProcess to
// forward.
inline std::vector<uint8_t> make_iq_frame(std::size_t n_samples, double fs, double tone_hz, double deviation_hz) {
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

} // namespace dsdtest
