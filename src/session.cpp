#include "session.hpp"
#include "json_util.hpp"

#include <iostream>
#include <cstring>
#include <random>
#include <set>

namespace dsdsrv {

namespace {
// Allocate a local UDP port per session for dsd-fme's decoded audio
// output, avoiding collisions between concurrent client sessions -- a
// collision would mean one session receiving another's audio.
//
// Two things about the previous version of this (a bare static mt19937,
// no lock, no bookkeeping), both found by test_session_concurrency's
// distinct-ports case:
//   - Sessions start concurrently on different strand threads, so the
//     shared RNG state was mutated from several threads at once. That's
//     a data race, and in practice racing threads handed out IDENTICAL
//     ports far more often than the birthday math says honest random
//     draws would (roughly 1 in 5 test runs, vs ~1 in 700 expected).
//   - Even with the race fixed, random selection without bookkeeping
//     still collides eventually. Tracking live allocations makes
//     distinctness a guarantee instead of a probability.
// stop_pipeline() releases the port when the session's pipeline stops.
//
// Only compiled for the subprocess backend: the DSDcc backend decodes
// in-process and has no UDP audio port to allocate (udp_audio_port_
// stays 0), so this whole allocator would be dead code there.
#if !defined(DSD_USE_DSDCC_BACKEND)
std::mutex g_udp_port_mutex;
std::set<uint16_t> g_udp_ports_in_use;

uint16_t acquire_udp_port() {
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<int> dist(40000, 59000);
    std::lock_guard<std::mutex> lock(g_udp_port_mutex);
    // The range holds ~19k ports and a session uses one, so in any sane
    // deployment this terminates almost immediately; it could only spin
    // if ~19k pipelines were live at once.
    for (;;) {
        auto port = static_cast<uint16_t>(dist(rng));
        if (g_udp_ports_in_use.insert(port).second) return port;
    }
}

void release_udp_port(uint16_t port) {
    if (port == 0) return; // never allocated (e.g. the DSDcc in-process backend)
    std::lock_guard<std::mutex> lock(g_udp_port_mutex);
    g_udp_ports_in_use.erase(port);
}
#endif // !DSD_USE_DSDCC_BACKEND
} // namespace

Session::Session(tcp::socket socket) : ws_(std::move(socket)) {}

Session::~Session() {
    stop_pipeline();
}

void Session::run() {
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(beast::http::field::server, "dsd-server/1.0");
        }));

    auto self = shared_from_this();
    ws_.async_accept([self](beast::error_code ec) { self->on_accept(ec); });
}

void Session::on_accept(beast::error_code ec) {
    if (ec) {
        std::cerr << "accept error: " << ec.message() << "\n";
        return;
    }
    do_read();
}

void Session::do_read() {
    auto self = shared_from_this();
    ws_.async_read(read_buffer_,
        [self](beast::error_code ec, std::size_t n) { self->on_read(ec, n); });
}

void Session::on_read(beast::error_code ec, std::size_t /*bytes_transferred*/) {
    if (ec == websocket::error::closed) {
        stop_pipeline();
        return;
    }
    if (ec) {
        std::cerr << "read error: " << ec.message() << "\n";
        stop_pipeline();
        return;
    }

    if (ws_.got_text()) {
        std::string msg = beast::buffers_to_string(read_buffer_.data());
        handle_text_message(msg);
    } else {
        auto data = static_cast<const uint8_t*>(read_buffer_.data().data());
        handle_binary_message(data, read_buffer_.size());
    }
    read_buffer_.consume(read_buffer_.size());

    do_read();
}

void Session::handle_text_message(const std::string& msg) {
    try {
        auto obj = json::parse_flat_object(msg);
        std::string type = json::get_string(obj, "type");

        if (type == "start") {
            double sample_rate = json::get_number(obj, "sample_rate", 2'000'000.0);
            double bw = json::get_number(obj, "channel_bandwidth", 12'500.0);
            double offset = json::get_number(obj, "freq_offset", 0.0);
            float gain = static_cast<float>(json::get_number(obj, "gain", 26000.0));
            bool afc = json::get_bool(obj, "afc", false);
            start_pipeline(sample_rate, bw, offset, gain, afc);
        } else if (type == "set_gain") {
            std::lock_guard<std::mutex> lock(demod_mutex_);
            if (demod_) demod_->set_gain(static_cast<float>(json::get_number(obj, "gain", 26000.0)));
        } else if (type == "set_freq_offset") {
            std::lock_guard<std::mutex> lock(demod_mutex_);
            if (demod_) demod_->set_freq_offset(json::get_number(obj, "hz", 0.0));
        } else if (type == "stop") {
            stop_pipeline();
        } else {
            send_text(json::Writer().field("type", std::string("error"))
                          .field("message", std::string("unknown message type: ") + type).str());
        }
    } catch (const std::exception& e) {
        send_text(json::Writer().field("type", std::string("error"))
                      .field("message", std::string("bad control message: ") + e.what()).str());
    }
}

void Session::handle_binary_message(const uint8_t* data, std::size_t len) {
    if (!pipeline_active_.load()) {
        return; // ignore IQ sent before a "start" control message
    }
    // Wire format: interleaved little-endian float32 I/Q samples.
    // len must be a multiple of 8 bytes (2 floats per complex sample).
    if (len % (2 * sizeof(float)) != 0) {
        send_text(json::Writer().field("type", std::string("error"))
                      .field("message", std::string("binary frame length not a multiple of 8 bytes")).str());
        return;
    }
    std::size_t n = len / (2 * sizeof(float));
    std::vector<cf32> block(n);
    std::memcpy(block.data(), data, len); // cf32 is {float,float}, same layout as interleaved I,Q

    {
        std::lock_guard<std::mutex> lock(iq_mutex_);
        // Simple backpressure: cap queued blocks so a slow demod thread
        // can't grow memory unboundedly if the client sends faster than
        // we can process. Tune to your expected block size / IQ rate.
        constexpr std::size_t kMaxQueuedBlocks = 64;
        if (iq_queue_.size() >= kMaxQueuedBlocks) {
            iq_queue_.pop_front(); // drop oldest rather than stall the network thread
        }
        iq_queue_.push_back(std::move(block));
    }
    iq_cv_.notify_one();
}

void Session::start_pipeline(double sample_rate, double channel_bw, double freq_offset, float gain, bool afc) {
    stop_pipeline(); // clean slate if already running

    FmDemodConfig cfg;
    cfg.input_sample_rate_hz = sample_rate;
    cfg.output_sample_rate_hz = 48'000.0;
    cfg.channel_bandwidth_hz = channel_bw;
    cfg.freq_offset_hz = freq_offset;
    cfg.disc_gain = gain;
    cfg.afc_enabled = afc;
    {
        std::lock_guard<std::mutex> lock(demod_mutex_);
        demod_ = std::make_unique<ActiveFmDemodulator>(cfg);
    }

    ActiveDsdBackendConfig dcfg;
#if defined(DSD_USE_DSDCC_BACKEND)
    dcfg.mode = "dmr";
    dcfg.input_sample_rate_hz = 48000;
    // DsdccDecoder is in-process, so there's no UDP audio port to
    // allocate -- udp_audio_port_ stays 0 and the "started" message
    // below reports that accurately (0 meaning "not applicable here",
    // not "failed to allocate").
#else
    udp_audio_port_ = acquire_udp_port();
    dcfg.input_sample_rate_hz = 48000;
    dcfg.mode_flag = "s"; // dsd-fme's DMR TDMA mode (-fs); "-fd" would be D-STAR
    dcfg.udp_audio_port = udp_audio_port_;
#endif

    auto self = shared_from_this();
    std::weak_ptr<Session> weak_self = self;

    bool ok = dsd_.start(
        dcfg,
        // Event callback: fires from a dedicated reader thread for the
        // DsdProcess (subprocess) backend, or synchronously on this
        // thread (Session's demod worker) for the DsdccDecoder backend
        // -- see dsdcc_decoder.hpp's top-of-file note. Either way this
        // needs the same weak_ptr treatment: this callback is stored
        // inside dsd_, which is itself a member of Session, so capturing
        // shared_ptr here would create Session -> dsd_ -> callback ->
        // shared_ptr<Session>, a strong reference cycle that leaks the
        // session forever.
        [weak_self](const DsdEvent& ev) {
            auto self = weak_self.lock();
            if (!self) return;
            // .str() chained in the same expression as the Writer
            // temporary's construction, rather than split across two
            // statements (auto w = ...; ...; w.str()) -- Writer is
            // copyable now (see json_util.hpp), so the split form would
            // compile too, but this avoids the extra copy and matches
            // the pattern used everywhere else in this file.
            std::string s = json::Writer()
                .field("type", std::string("event"))
                .field("kind", ev.kind)
                .field("talkgroup", ev.talkgroup)
                .field("source_id", ev.source_id)
                .field("slot", ev.slot)
                .field("color_code", ev.color_code)
                .field("crc_error", ev.crc_error)
                .field("extra", ev.extra)
                .field("raw", ev.raw_line)
                .str();
            // This inner lambda IS safe to capture shared_ptr in: it's
            // posted once to the connection's strand executor and
            // discarded, never stored.
            net::post(self->ws_.get_executor(), [self, s = std::move(s)] { self->send_text(s); });
        },
        // Audio callback: same threading note as the event callback above.
        [weak_self](const int16_t* pcm, std::size_t n) {
            auto self = weak_self.lock();
            if (!self) return;
            std::vector<uint8_t> frame(1 + n * sizeof(int16_t));
            frame[0] = 0x01; // tag: decoded voice PCM
            std::memcpy(frame.data() + 1, pcm, n * sizeof(int16_t));
            net::post(self->ws_.get_executor(), [self, f = std::move(frame)]() mutable {
                self->send_binary(std::move(f));
            });
        });

    if (!ok) {
        send_text(json::Writer().field("type", std::string("error"))
                      .field("message", std::string("failed to start DSD backend")).str());
        demod_.reset();
        return;
    }

    pipeline_active_ = true;
    worker_running_ = true;
    worker_thread_ = std::thread(&Session::demod_worker_loop, this);

    send_text(json::Writer().field("type", std::string("started"))
                  .field("udp_audio_port", static_cast<double>(udp_audio_port_)).str());
}

void Session::stop_pipeline() {
    if (!pipeline_active_.exchange(false)) return;

    worker_running_ = false;
    iq_cv_.notify_all();
    if (worker_thread_.joinable()) worker_thread_.join();

    dsd_.stop();
    {
        // Safe to take here: the worker was joined above, so nothing is
        // inside process() holding this.
        std::lock_guard<std::mutex> lock(demod_mutex_);
        demod_.reset();
    }
#if !defined(DSD_USE_DSDCC_BACKEND)
    release_udp_port(udp_audio_port_);
    udp_audio_port_ = 0;
#endif

    {
        std::lock_guard<std::mutex> lock(iq_mutex_);
        iq_queue_.clear();
    }
}

void Session::demod_worker_loop() {
    std::vector<int16_t> pcm_scratch;
    while (worker_running_.load()) {
        std::vector<cf32> block;
        {
            std::unique_lock<std::mutex> lock(iq_mutex_);
            iq_cv_.wait(lock, [this] { return !iq_queue_.empty() || !worker_running_.load(); });
            if (!worker_running_.load()) break;
            block = std::move(iq_queue_.front());
            iq_queue_.pop_front();
        }

        pcm_scratch.clear();
        {
            std::lock_guard<std::mutex> lock(demod_mutex_);
            demod_->process(block.data(), block.size(), pcm_scratch);
        }
        if (!pcm_scratch.empty()) {
            dsd_.write_audio(pcm_scratch.data(), pcm_scratch.size());
        }
    }
}

void Session::send_text(const std::string& msg) {
    std::vector<uint8_t> data(msg.begin(), msg.end());
    queue_and_send(std::move(data), /*is_text=*/true);
}

void Session::send_binary(std::vector<uint8_t> data) {
    queue_and_send(std::move(data), /*is_text=*/false);
}

void Session::queue_and_send(std::vector<uint8_t> frame, bool is_text) {
    // out_queue_/writing_ are also touched from send_text/send_binary
    // calls made directly on the network thread (inside on_read's call
    // chain) as well as from the strand-posted callbacks above, so this
    // still needs its own lock even though both call sites already run on
    // ws_'s strand executor (belt-and-suspenders against future callers
    // that might not).
    std::lock_guard<std::mutex> lock(out_mutex_);
    out_queue_.emplace_back(std::move(frame), is_text);
    if (!writing_) do_write_next();
}

void Session::do_write_next() {
    // Caller holds out_mutex_.
    if (out_queue_.empty()) { writing_ = false; return; }
    writing_ = true;

    auto& [data, is_text] = out_queue_.front();
    ws_.text(is_text);
    ws_.binary(!is_text);

    auto self = shared_from_this();
    ws_.async_write(net::buffer(data),
        [self](beast::error_code ec, std::size_t bytes_transferred) {
            self->on_write(ec, bytes_transferred);
        });
}

void Session::on_write(beast::error_code ec, std::size_t /*bytes_transferred*/) {
    std::lock_guard<std::mutex> lock(out_mutex_);
    if (!out_queue_.empty()) out_queue_.pop_front();

    if (ec) {
        std::cerr << "write error: " << ec.message() << "\n";
        writing_ = false;
        return;
    }
    do_write_next();
}

// ---------------------------------------------------------------------

Server::Server(net::io_context& ioc, const tcp::endpoint& endpoint)
    : ioc_(ioc), acceptor_(ioc) {
    beast::error_code ec;

    acceptor_.open(endpoint.protocol(), ec);
    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    acceptor_.bind(endpoint, ec);
    if (ec) {
        std::cerr << "bind failed: " << ec.message() << "\n";
        return;
    }
    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        std::cerr << "listen failed: " << ec.message() << "\n";
        return;
    }
    do_accept();
}

void Server::do_accept() {
    // Bind each accepted socket to its own strand. This makes ws_'s
    // executor (in the resulting Session) a strand, which is what lets us
    // safely call net::post(ws_.get_executor(), ...) from the dsd-fme
    // reader threads to schedule writes without racing this connection's
    // own async_read/async_write calls when multiple threads are running
    // ioc.run() (see main.cpp's thread pool).
    acceptor_.async_accept(net::make_strand(ioc_),
        [this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<Session>(std::move(socket))->run();
            } else {
                std::cerr << "accept error: " << ec.message() << "\n";
            }
            do_accept();
        });
}

} // namespace dsdsrv
