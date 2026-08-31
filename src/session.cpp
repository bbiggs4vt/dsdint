#include "session.hpp"
#include "json_util.hpp"

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <functional>
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
// Only compiled for the dsd-fme subprocess backend: the DSDcc backend
// decodes in-process, and the TETRA backends bind their own UDP ports
// internally, so neither has an audio port to allocate here (udp_audio_port_
// stays 0). A TETRA session never calls acquire_udp_port() at run time.
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
            // Advisory protocol hint: the client tells us what it thinks
            // the signal is so the decoder can be told which mode to run
            // instead of guessing. Absent/"" keeps the historical DMR
            // default (no behavior change for existing clients).
            std::string protocol = json::get_string(obj, "protocol");
            // Optional decryption key. key_type names the scheme (e.g.
            // "bp" for DMR Basic Privacy); key is its value. Absent/""
            // means no key (unchanged behavior). See start_pipeline.
            std::string key_type = json::get_string(obj, "key_type");
            std::string key = json::get_string(obj, "key");
            start_pipeline(sample_rate, bw, offset, gain, afc, protocol, key_type, key);
        } else if (type == "set_gain") {
            // TETRA has no FM discriminator, so discriminator gain doesn't
            // apply there -- accept the message (no error) and ignore it.
            if (!tetra_active_.load()) {
                std::lock_guard<std::mutex> lock(demod_mutex_);
                if (demod_) demod_->set_gain(static_cast<float>(json::get_number(obj, "gain", 26000.0)));
            }
        } else if (type == "set_freq_offset") {
            // The π/4 demod estimates and removes residual CFO itself (±Rs/8),
            // so a TETRA session has no live NCO to retune -- accept and ignore.
            if (!tetra_active_.load()) {
                std::lock_guard<std::mutex> lock(demod_mutex_);
                if (demod_) demod_->set_freq_offset(json::get_number(obj, "hz", 0.0));
            }
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

namespace {

// Canonical protocol hint from the client's free-form "protocol" field.
// The hint is advisory ("tip"), so it is forgiving: an empty/absent hint
// keeps the server's historical default, an explicit "auto"/"unknown"/
// "not sure" asks the decoder to auto-detect, and anything unrecognized
// also falls back to auto-detect rather than erroring (a typo shouldn't
// kill a stream).
enum class ProtocolHint { Default, Dmr, Nxdn48, Nxdn96, P25p1, P25p2, Dpmr, Dstar, Ysf, Auto,
                          Tetra, Tetrakit };

ProtocolHint parse_protocol_hint(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // strip spaces/underscores/hyphens so "nxdn 48", "nxdn_48", "not_sure" all match
    std::string k;
    for (char c : s) if (c != ' ' && c != '_' && c != '-') k.push_back(c);
    if (k.empty()) return ProtocolHint::Default;
    if (k == "dmr") return ProtocolHint::Dmr;
    if (k == "nxdn48" || k == "nxdn" || k == "idas") return ProtocolHint::Nxdn48;
    if (k == "nxdn96") return ProtocolHint::Nxdn96;
    if (k == "p25p2" || k == "p25phase2") return ProtocolHint::P25p2;
    if (k == "p25" || k == "p25p1" || k == "p25phase1") return ProtocolHint::P25p1;
    if (k == "dpmr") return ProtocolHint::Dpmr;
    if (k == "dstar") return ProtocolHint::Dstar; // "d-star", "d star" also normalize here
    if (k == "ysf" || k == "fusion" || k == "systemfusion" || k == "c4fm") return ProtocolHint::Ysf;
    // TETRA runs a different signal chain (π/4-DQPSK modem + a TETRA
    // subprocess backend), selected at run time: "tetra" -> osmo tetra-rx,
    // "tetrakit" -> tetra-kit's decoder. Both alias forms normalize here.
    if (k == "tetra" || k == "tetraosmo" || k == "osmotetra") return ProtocolHint::Tetra;
    if (k == "tetrakit") return ProtocolHint::Tetrakit;
    // "auto", "unknown", "notsure", and anything else -> auto-detect
    return ProtocolHint::Auto;
}

bool hint_is_tetra(ProtocolHint h) {
    return h == ProtocolHint::Tetra || h == ProtocolHint::Tetrakit;
}

// Key handling is DSD-only (a TETRA session decrypts nothing here -- TETRA's
// TEA ciphers aren't handled), but these are always compiled now: the DSD
// path is always present in the binary and the TETRA branch simply never
// calls them.

// Optional decryption key scheme from the client's "key_type" field. Only
// DMR Basic Privacy (`Bp`) is decryptable on both backends; the rest are
// dsd-fme-backend only (DSDcc has no RC4/AES/DES/scrambler support). An
// empty/absent/unrecognized value means "no key" (leave decryption off).
enum class KeyType { None, Bp, Rc4, Des, Aes, Hytera, Scrambler };

KeyType parse_key_type(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string k;
    for (char c : s) if (c != ' ' && c != '_' && c != '-') k.push_back(c);
    if (k.empty()) return KeyType::None;
    if (k == "bp" || k == "basicprivacy" || k == "dmrbp") return KeyType::Bp;
    if (k == "rc4") return KeyType::Rc4;
    if (k == "des") return KeyType::Des;
    if (k == "aes" || k == "aes128" || k == "aes256") return KeyType::Aes;
    if (k == "hytera" || k == "hyterabp") return KeyType::Hytera;
    if (k == "scrambler" || k == "nxdnscrambler" || k == "dpmrscrambler" || k == "ehr")
        return KeyType::Scrambler;
    return KeyType::None; // unknown scheme -> don't guess, run without a key
}

// Every key scheme we expose takes a numeric value: decimal for
// Bp/Scrambler, hex for Rc4/Des/Aes/Hytera. AES-128/256 and Hytera keys
// are given to dsd-fme as space-separated 64-bit hex words (its own
// documented format, e.g. "736B9A9C5645288B 243AD5CB8701EF8A"), so an
// internal space is allowed; every other character is rejected. This
// validates client input and guarantees only hex digits and spaces ever
// reach dsd-fme's argv (a space in a single argv token is inert — there
// is no shell — so this is not an injection surface). At least one hex
// digit must be present.
bool key_value_is_valid(const std::string& v) {
    bool saw_digit = false;
    for (char c : v) {
        if (std::isxdigit(static_cast<unsigned char>(c))) saw_digit = true;
        else if (c != ' ') return false;
    }
    return saw_digit;
}

} // namespace

void Session::start_pipeline(double sample_rate, double channel_bw, double freq_offset,
                             float gain, bool afc, const std::string& protocol,
                             const std::string& key_type, const std::string& key) {
    stop_pipeline(); // clean slate if already running

    const ProtocolHint hint = parse_protocol_hint(protocol);

    const bool want_tetra = hint_is_tetra(hint);

    // Event/audio callbacks are backend-agnostic (every backend's start()
    // takes these same std::function signatures and hands us the same
    // DsdEvent / int16 PCM), so they're built once and passed to whichever
    // chain this session selects below.
    //
    // weak_ptr, not shared_ptr: this callback is stored inside the backend,
    // which is itself a member of Session, so capturing shared_ptr here
    // would form Session -> backend -> callback -> shared_ptr<Session>, a
    // strong reference cycle that leaks the session forever. The subprocess
    // backends also fire the callback from a reader thread; the lock() both
    // breaks the cycle and makes the after-free case safe.
    std::weak_ptr<Session> weak_self = shared_from_this();
    std::function<void(const DsdEvent&)> on_event = [weak_self](const DsdEvent& ev) {
        auto self = weak_self.lock();
        if (!self) return;
        std::string s = json::Writer()
            .field("type", std::string("event"))
            .field("kind", ev.kind)
            .field("talkgroup", ev.talkgroup)
            .field("source_id", ev.source_id)
            .field("slot", ev.slot)
            .field("color_code", ev.color_code)
            .field("ran", ev.ran)
            .field("nac", ev.nac)
            .field("emergency", ev.emergency)
            .field("alias", ev.alias)
            .field("crc_error", ev.crc_error)
            .field("message", ev.message)
            .field("extra", ev.extra)
            .field("raw", ev.raw_line)
            .str();
        // This inner lambda IS safe to capture shared_ptr in: it's posted
        // once to the connection's strand executor and discarded.
        net::post(self->ws_.get_executor(), [self, s = std::move(s)] { self->send_text(s); });
    };
    std::function<void(const int16_t*, std::size_t)> on_audio =
        [weak_self](const int16_t* pcm, std::size_t n) {
        auto self = weak_self.lock();
        if (!self) return;
        std::vector<uint8_t> frame(1 + n * sizeof(int16_t));
        frame[0] = 0x01; // tag: decoded voice PCM
        std::memcpy(frame.data() + 1, pcm, n * sizeof(int16_t));
        net::post(self->ws_.get_executor(), [self, f = std::move(frame)]() mutable {
            self->send_binary(std::move(f));
        });
    };

    if (want_tetra) {
        // ---- TETRA: π/4-DQPSK modem front end + a TETRA subprocess backend ----
        // The FM-path knobs (channel bandwidth, freq offset, gain, AFC) and the
        // DSD key options don't apply to the TETRA chain; accept them for a
        // uniform "start" shape and ignore them. Encryption (TEA) is not handled
        // here.
        (void)channel_bw; (void)freq_offset; (void)gain; (void)afc;
        (void)key_type; (void)key;

        // TETRA IQ must arrive at samples_per_symbol * 18000 Hz; derive sps from
        // the client's sample_rate (e.g. 72000 -> 4) and clamp to a sane floor.
        int tetra_sps = static_cast<int>(std::llround(sample_rate / 18000.0));
        if (tetra_sps < 2) tetra_sps = 2;
        {
            TetraFrontendConfig tdcfg;
            tdcfg.demod.samples_per_symbol = tetra_sps;
            tdcfg.demod.correct_cfo = true; // pull in residual SDR ppm error (±Rs/8)
            // Detection defaults to differential (robust, phase-blind). Setting
            // DSD_TETRA_COHERENT=1 switches to Costas coherent detection, which
            // resolves its π/4 parity from burst-grid lock and falls back to
            // differential if it can't lock (see tetra_frontend.hpp). It buys
            // ~1.5-1.7 dB at mid SNR but can slip at the very low-SNR fringe,
            // so it stays opt-in.
            const char* coh = std::getenv("DSD_TETRA_COHERENT");
            tdcfg.coherent = (coh && coh[0] == '1');
            std::lock_guard<std::mutex> lock(demod_mutex_);
            tetra_demod_ = std::make_unique<TetraDemodFrontend>(tdcfg);
        }

        // "tetra" -> osmo tetra-rx, "tetrakit" -> tetra-kit's decoder.
        const TetraBackendKind kind = (hint == ProtocolHint::Tetrakit)
            ? TetraBackendKind::Tetrakit : TetraBackendKind::Osmo;
        tetra_backend_ = make_tetra_backend(kind);

        // TETRA voice is a separate per-call stream needing the ETSI codec;
        // on_audio is wired for parity but the backends do not feed it yet.
        bool ok = tetra_backend_->start(on_event, on_audio);
        if (!ok) {
            send_text(json::Writer().field("type", std::string("error"))
                          .field("message", std::string("failed to start TETRA backend")).str());
            std::lock_guard<std::mutex> lock(demod_mutex_);
            tetra_demod_.reset();
            tetra_backend_.reset();
            return;
        }
        tetra_active_.store(true);
    } else {
        // ---- FM discriminator + DSD backend ----
        // Validate the optional decryption key up front: a named key_type with
        // a missing or non-hex/decimal value is a client error, and rejecting
        // it here also guarantees only a digits-only token can ever reach
        // dsd-fme's argv.
        const KeyType key_type_enum = parse_key_type(key_type);
        if (key_type_enum != KeyType::None && !key_value_is_valid(key)) {
            send_text(json::Writer().field("type", std::string("error"))
                          .field("message", std::string("invalid or missing key for key_type '")
                                 + key_type + "' (expected decimal/hex digits)").str());
            return;
        }

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
        // Map the hint to DsdccDecoder's mode string (see DsdccDecoder::start,
        // which forwards it to DSDDecoder::setDecodeMode). Default -> "dmr",
        // matching the historical behavior. NOTE: DSDcc's NXDN symbol recovery
        // is fragile on real off-air signals (verified against a real NXDN48
        // capture); the dsd-fme backend is the reliable NXDN decoder.
        switch (hint) {
            case ProtocolHint::Nxdn48: dcfg.mode = "nxdn48"; break;
            case ProtocolHint::Nxdn96: dcfg.mode = "nxdn96"; break;
            // DSDcc has a P25 Phase 1 decode mode; there's no separate Phase 2
            // decoder, so both P25 hints select it. The DSDcc wrapper does not
            // yet extract P25 metadata into fields (it emits sync only) -- the
            // dsd-fme backend is the P25 decoder to use.
            case ProtocolHint::P25p1:
            case ProtocolHint::P25p2:  dcfg.mode = "p25";    break;
            case ProtocolHint::Dpmr:   dcfg.mode = "dpmr";   break;
            case ProtocolHint::Dstar:  dcfg.mode = "dstar";  break;
            case ProtocolHint::Ysf:    dcfg.mode = "ysf";    break;
            case ProtocolHint::Auto:   dcfg.mode = "auto";   break;
            case ProtocolHint::Dmr:
            case ProtocolHint::Default:
            default:                   dcfg.mode = "dmr";    break;
        }
        dcfg.input_sample_rate_hz = 48000;
        // DMR Basic Privacy is the only decryption DSDcc can do; the key value
        // is the BP key NUMBER (decimal 1–255). Any other scheme is dsd-fme
        // only, so warn and run without a key rather than pretending.
        if (key_type_enum == KeyType::Bp) {
            unsigned long n = std::strtoul(key.c_str(), nullptr, 10);
            dcfg.bp_key = (n > 255) ? 255u : static_cast<unsigned>(n);
        } else if (key_type_enum != KeyType::None) {
            std::cerr << "dsd-server: DSDcc backend supports only DMR Basic Privacy "
                         "('bp') keys; ignoring key_type='" << key_type << "'\n";
        }
        // DsdccDecoder is in-process, so there's no UDP audio port to
        // allocate -- udp_audio_port_ stays 0 and the "started" message
        // below reports that accurately (0 meaning "not applicable here",
        // not "failed to allocate").
#else
        udp_audio_port_ = acquire_udp_port();
        dcfg.input_sample_rate_hz = 48000;
        // Map the hint to dsd-fme's "-f<letter>" mode. Default -> "s" (DMR),
        // matching the historical behavior; dsd-fme applies the matching
        // input matched-filter for the selected mode automatically. Letters
        // verified against `dsd-fme -h`: s=DMR, i=NXDN48/IDAS, n=NXDN96,
        // a=auto-detect, d=D-STAR, y=YSF, m=dPMR. P25: 1=Phase 1, 2=Phase 2
        // (6000 sps TDMA).
        switch (hint) {
            case ProtocolHint::Nxdn48: dcfg.mode_flag = "i"; break;
            case ProtocolHint::Nxdn96: dcfg.mode_flag = "n"; break;
            case ProtocolHint::P25p1:  dcfg.mode_flag = "1"; break;
            case ProtocolHint::P25p2:  dcfg.mode_flag = "2"; break;
            case ProtocolHint::Dpmr:   dcfg.mode_flag = "m"; break;
            case ProtocolHint::Dstar:  dcfg.mode_flag = "d"; break;
            case ProtocolHint::Ysf:    dcfg.mode_flag = "y"; break;
            case ProtocolHint::Auto:   dcfg.mode_flag = "a"; break;
            case ProtocolHint::Dmr:
            case ProtocolHint::Default:
            default:                   dcfg.mode_flag = "s"; break;
        }
        dcfg.udp_audio_port = udp_audio_port_;
        // Decryption key -> the matching dsd-fme flag, appended as a separate
        // argv token (never a shell string, and key was validated digits-only
        // above). Flag/value formats verified against dsd-fme's source
        // (dsd_main.c): -b <dec> DMR Basic Privacy key number; -1 <hex>
        // RC4/DES; -H <hex> Hytera BP or AES-128/256 (disambiguated by length
        // inside dsd-fme); -R <dec> NXDN/dPMR EHR scrambler.
        {
            std::string kflag;
            switch (key_type_enum) {
                case KeyType::Bp:        kflag = "b"; break;
                case KeyType::Rc4:
                case KeyType::Des:       kflag = "1"; break;
                case KeyType::Aes:
                case KeyType::Hytera:    kflag = "H"; break;
                case KeyType::Scrambler: kflag = "R"; break;
                case KeyType::None:      break;
            }
            if (!kflag.empty()) {
                dcfg.extra_args.push_back("-" + kflag);
                dcfg.extra_args.push_back(key);
            }
        }
#endif // DSD_USE_DSDCC_BACKEND

        bool ok = dsd_.start(dcfg, on_event, on_audio);
        if (!ok) {
            send_text(json::Writer().field("type", std::string("error"))
                          .field("message", std::string("failed to start DSD backend")).str());
            std::lock_guard<std::mutex> lock(demod_mutex_);
            demod_.reset();
            return;
        }
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

    // Stop whichever chain this session ran. tetra_active_ was set in
    // start_pipeline; the idle chain's members are null/stopped, so both
    // stops are safe to attempt but we key off the flag for clarity.
    const bool was_tetra = tetra_active_.exchange(false);
    if (was_tetra) {
        if (tetra_backend_) tetra_backend_->stop();
    } else {
        dsd_.stop();
    }
    {
        // Safe to take here: the worker was joined above, so nothing is
        // inside process()/demodulate() holding this.
        std::lock_guard<std::mutex> lock(demod_mutex_);
        if (was_tetra) {
            tetra_demod_.reset();
            tetra_backend_.reset();
        } else {
            demod_.reset();
        }
    }
#if !defined(DSD_USE_DSDCC_BACKEND)
    // Only the dsd-fme subprocess path allocates an audio port; a TETRA
    // session never did (udp_audio_port_ stayed 0, and release ignores 0).
    release_udp_port(udp_audio_port_);
    udp_audio_port_ = 0;
#endif

    {
        std::lock_guard<std::mutex> lock(iq_mutex_);
        iq_queue_.clear();
    }
}

void Session::demod_worker_loop() {
    // tetra_active_ is fixed for this worker's lifetime: start_pipeline sets
    // it (and creates the matching demod/backend) before launching the
    // thread, and stop_pipeline joins the thread before clearing it. So we
    // read it once and take the matching path.
    //
    // TETRA path: demodulate each IQ block to bits and relay them to the
    // decoder. TetraDpqskDemod is streaming -- filter history, the timing
    // loop, the differential reference, the CFO estimate and the AGC all
    // carry across demodulate() calls -- so decoding per WebSocket frame is
    // continuous with no per-frame restart or seam glitch (a fresh demod is
    // created per start(), the clean-slate boundary). The few trailing
    // samples held back for filter context at a stop are immaterial.
    const bool tetra = tetra_active_.load();
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

        if (tetra) {
            std::vector<unsigned char> bits;
            {
                std::lock_guard<std::mutex> lock(demod_mutex_);
                if (tetra_demod_) bits = tetra_demod_->demodulate(block.data(), block.size());
            }
            if (!bits.empty() && tetra_backend_) {
                tetra_backend_->write_bits(bits.data(), bits.size());
            }
        } else {
            pcm_scratch.clear();
            {
                std::lock_guard<std::mutex> lock(demod_mutex_);
                if (demod_) demod_->process(block.data(), block.size(), pcm_scratch);
            }
            if (!pcm_scratch.empty()) {
                dsd_.write_audio(pcm_scratch.data(), pcm_scratch.size());
            }
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
