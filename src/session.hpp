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
//         "channel_bandwidth":12500,"freq_offset":0,"gain":26000,
//         "afc":false}
//       Starts the demod + dsd-fme pipeline for this connection. "afc"
//       (default false) enables automatic frequency control: the demod
//       measures the residual carrier offset in its own discriminator
//       output and steers the NCO to zero it (see fm_demod.hpp).
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
//         tag 0x01: decoded voice PCM (int16 LE; 8 kHz -- stereo from
//         dsd-fme's DMR mode, mono from the DSDcc backend)
//     - Text frame, JSON: event records from the decode backend, e.g.
//         {"type":"event","kind":"call","talkgroup":"19535",
//          "source_id":"2222223","slot":"2","extra":"","raw":"..."}
//     - Text frame, JSON: {"type":"error","message":"..."} on problems.
//
//   Runtime protocol selection: the client's "protocol" hint picks the whole
//   signal chain for the session. Most hints ("dmr", "nxdn48", "p25", ...)
//   and the default run the FM-discriminator + DSD backend; "tetra" and
//   "tetrakit" instead run the π/4-DQPSK modem feeding a TETRA subprocess
//   backend (osmo tetra-rx or tetra-kit's decoder respectively). One binary
//   carries both front ends and instantiates whichever the hint selects; they
//   never run at once within a single session.
//
//   PROTOCOL.md at the repo root is the full reference -- exact shapes,
//   all error texts, per-backend event semantics, real captured
//   examples. Keep it in step with any change to this file's JSON.

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

// The server carries two signal chains and picks one per session at run time
// (from the client's "protocol" hint -- see start_pipeline):
//   * FM-discriminator + DSD backend (dsd-fme subprocess or in-process DSDcc,
//     chosen at build time via dsd_backend_selector.hpp) for the analog-FM
//     digital modes;
//   * π/4-DQPSK modem + a TETRA subprocess backend (osmo tetra-rx or
//     tetra-kit's decoder, chosen at run time via tetra_backend_iface.hpp)
//     for TETRA, which is not an FM mode.
// Both are always compiled in; a given session instantiates only the one its
// hint selects.
#include "fm_demod_selector.hpp"
#include "dsd_backend_selector.hpp"
#include "tetra_frontend.hpp"
#include "tetra_backend_iface.hpp"

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

    void start_pipeline(double sample_rate, double channel_bw, double freq_offset, float gain, bool afc,
                        const std::string& protocol = "",
                        const std::string& key_type = "", const std::string& key = "");
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
    // Which signal chain this session runs. Set in start_pipeline before the
    // worker thread launches, read by the worker loop and the live setters,
    // reset in stop_pipeline. Only touched on the strand thread except the
    // worker, which reads it after worker_running_ is set (so it is stable for
    // the worker's lifetime); the atomic keeps that publication well-defined.
    enum class Chain { Fm, Tetra };
    std::atomic<Chain> chain_{Chain::Fm};

    // Both front ends are members; start_pipeline instantiates exactly one per
    // session, keyed by chain_. FM path: demod_ + dsd_. TETRA path: tetra_demod_
    // (the π/4-DQPSK modem turning IQ into the bitstream) + tetra_backend_ (the
    // chosen subprocess backend). The idle path's members stay null/stopped.
    std::unique_ptr<ActiveFmDemodulator> demod_;
    std::unique_ptr<TetraDemodFrontend> tetra_demod_;
    // Guards every use of the demod (FM or TETRA) -- both the pointer and
    // calls through it. The demodulators document their setters as only safe
    // "from the same thread that owns this object", but three threads
    // genuinely touch it: the connection's strand thread (set_gain/
    // set_freq_offset from control messages, and reset in
    // stop_pipeline), the demod worker thread (process()/demodulate()), and
    // whichever thread drops the last shared_ptr<Session> (~Session ->
    // stop_pipeline -> reset -- that can be a backend reader thread,
    // via a posted callback holding the pointer). The concurrency
    // test's TSan build confirmed the setter-vs-process() race, and the
    // reset path is worse than a race: set_gain through a demod_ being
    // reset concurrently is a use-after-free. Never held while joining
    // worker_thread_ (see stop_pipeline), so it cannot deadlock with
    // the worker taking it around process().
    std::mutex demod_mutex_;
    ActiveDsdBackend dsd_;
    // The runtime-selected TETRA backend, non-null only while a TETRA session
    // is active (created in start_pipeline via make_tetra_backend).
    std::unique_ptr<ITetraBackend> tetra_backend_;
    uint16_t udp_audio_port_ = 0; // only meaningful for the DsdProcess (subprocess) backend; 0 under TETRA/DSDcc

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
