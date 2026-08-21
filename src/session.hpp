// session.hpp
//
// One Session per connected WebSocket client. Owns:
//   - an FmDemodulator (configured from the client's "start" control msg)
//   - a DSD backend (DsdProcess, spawning a dedicated dsd-fme/DSD child
//     process per client; or DsdccDecoder, decoding in-process — see
//     dsd_backend_selector.hpp for the compile-time choice)
//   - a worker thread that drains incoming IQ frames, demodulates them,
//     and feeds the result to dsd-fme's stdin
//
// Wire protocol (WebSocket frames):
//
//   Client -> Server:
//     - Text frame, JSON: {"type":"start","sample_rate":2000000,
//         "channel_bandwidth":12500,"freq_offset":0,"gain":26000}
//       Starts the demod + dsd-fme pipeline for this connection.
//     - Text frame, JSON: {"type":"set_gain","gain":30000}
//       Adjusts discriminator gain live.
//     - Text frame, JSON: {"type":"set_freq_offset","hz":1500}
//       Adjusts NCO shift live (if the channel isn't centered at 0 Hz).
//     - Binary frame: raw interleaved IQ samples, little-endian float32
//       (I0,Q0,I1,Q1,...). See handle_binary_message in session.cpp.
//     - Text frame: {"type":"stop"} to end the session's pipeline (the
//       WebSocket connection itself can stay open).
//
//   Server -> Client:
//     - Binary frame, 1-byte tag + payload:
//         tag 0x01: decoded voice PCM (int16 LE) from dsd-fme's UDP output
//     - Text frame, JSON: event records parsed from dsd-fme's stdout, e.g.
//         {"type":"event","kind":"voice","talkgroup":"12345",
//          "source_id":"6789","slot":"1","raw":"..."}
//     - Text frame, JSON: {"type":"error","message":"..."} on problems.

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <memory>
#include <thread>
#include <atomic>
#include <deque>
#include <mutex>
#include <condition_variable>

#include "fm_demod_selector.hpp"
#include "dsd_backend_selector.hpp"

namespace dsdsrv {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket);
    ~Session();

    void run();

private:
    void on_accept(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void on_write(beast::error_code ec, std::size_t bytes_transferred);

    void handle_text_message(const std::string& msg);
    void handle_binary_message(const uint8_t* data, std::size_t len);

    void start_pipeline(double sample_rate, double channel_bw, double freq_offset, float gain);
    void stop_pipeline();

    // Thread-safe send of a text/binary frame; queues if a write is
    // already in flight (Beast only supports one write in flight at a
    // time per stream).
    void send_text(const std::string& msg);
    void send_binary(std::vector<uint8_t> data);
    void queue_and_send(std::vector<uint8_t> frame, bool is_text);
    void do_write_next();

    // Worker thread: drains iq_queue_, demodulates, writes to dsd_.
    void demod_worker_loop();

    websocket::stream<beast::tcp_stream> ws_;
    // NOTE: ws_'s executor is a per-connection strand, bound at accept
    // time in Server::do_accept() via net::make_strand(). That's what
    // makes it safe to call net::post(ws_.get_executor(), ...) from other
    // threads (the dsd-fme reader threads) to schedule writes on this
    // connection without them racing the network thread's own reads/
    // writes on ws_.
    beast::flat_buffer read_buffer_;
    std::unique_ptr<ActiveFmDemodulator> demod_;
    ActiveDsdBackend dsd_;
    uint16_t udp_audio_port_ = 0; // only meaningful for the DsdProcess (subprocess) backend

    // Producer (network thread via on_binary) / consumer (worker thread)
    // queue of raw IQ blocks awaiting demodulation.
    std::deque<std::vector<cf32>> iq_queue_;
    std::mutex iq_mutex_;
    std::condition_variable iq_cv_;
    std::thread worker_thread_;
    std::atomic<bool> worker_running_{false};

    // Outgoing frame queue (network thread only, but demod/event
    // callbacks arrive from other threads so this needs a lock).
    std::deque<std::pair<std::vector<uint8_t>, bool>> out_queue_; // (data, is_text)
    std::mutex out_mutex_;
    bool writing_ = false;

    std::atomic<bool> pipeline_active_{false};
};

class Server {
public:
    Server(net::io_context& ioc, const tcp::endpoint& endpoint);

private:
    void do_accept();

    net::io_context& ioc_;
    tcp::acceptor acceptor_;
};

} // namespace dsdsrv
