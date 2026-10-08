#include "session.hpp"
#include "json_util.hpp"
#include "protocol_capabilities.hpp"
#include "status_page.hpp"
#include "net_manual.hpp"
#include "net_page.hpp"
#include "pager_events.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <zlib.h>
#include <iterator>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <ctime>
#include <functional>
#include <random>
#include <set>
#include <vector>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>

namespace dsdsrv {

namespace {
// Network-explorer recordings (assoc_log.hpp): where they go and how big one
// may get. DSD_NET_LOG_DIR, else the IQ-capture dir (DSD_IQ_LOG_DIR -- in the
// Docker image /captures), else the working directory. DSD_NET_LOG_MAX_MB is
// the uncompressed cap per file (default 1024; the gzip file is ~10x smaller).
std::string net_log_dir() {
    for (const char* v : {"DSD_NET_LOG_DIR", "DSD_IQ_LOG_DIR"}) {
        const char* d = std::getenv(v);
        if (d && d[0]) return d;
    }
    return ".";
}
// Where the explorer's network merges are kept: DSD_NET_MERGES_FILE, or
// net_merges.json in the recordings folder.
std::string net_merges_file() {
    const char* f = std::getenv("DSD_NET_MERGES_FILE");
    return (f && f[0]) ? std::string(f) : (std::filesystem::path(net_log_dir()) / "net_merges.json").string();
}
// Where the explorer's encryption keyring is kept: DSD_NET_KEYS_FILE, or
// net_keys.json in the recordings folder. Written owner-only (it holds keys).
std::string net_keys_file() {
    const char* f = std::getenv("DSD_NET_KEYS_FILE");
    return (f && f[0]) ? std::string(f) : (std::filesystem::path(net_log_dir()) / "net_keys.json").string();
}
std::uint64_t net_log_max_bytes() {
    const char* m = std::getenv("DSD_NET_LOG_MAX_MB");
    unsigned long mb = (m && m[0]) ? std::strtoul(m, nullptr, 10) : 1024;
    return static_cast<std::uint64_t>(mb) * 1024ull * 1024ull;
}
std::string net_rec_json(const AssocModel::RecStatus& r) {
    const std::string file = r.last_path.empty() ? std::string()
                                                 : std::filesystem::path(r.last_path).filename().string();
    return json::Writer().field("on", r.on).field("truncated", r.truncated).field("file", file)
        .field("path", r.last_path).field("bytes", static_cast<double>(r.bytes))
        .field("file_bytes", static_cast<double>(r.file_bytes)).str();
}

// A channel frequency as the explorer keys it: snapped to DSD_NET_FREQ_STEP_HZ
// (default 1250 Hz; see AssocModel::channel_hz).
std::int64_t channel_freq(double hz) {
    static const std::int64_t step = [] {
        const char* v = std::getenv("DSD_NET_FREQ_STEP_HZ");
        const long long n = (v && v[0]) ? std::strtoll(v, nullptr, 10) : 1250;
        return static_cast<std::int64_t>(n > 0 ? n : 1);
    }();
    return AssocModel::channel_hz(hz, step);
}

// Network-explorer per-call audio (AssocModel::audio, assoc_audio.hpp):
// DSD_NET_AUDIO_DIR, else <net log dir>/net_audio; capped at
// DSD_NET_AUDIO_MAX_MB (default 1024) and, if set, DSD_NET_AUDIO_MAX_AGE_H.
std::string net_audio_dir() {
    const char* d = std::getenv("DSD_NET_AUDIO_DIR");
    return (d && d[0]) ? std::string(d) : (std::filesystem::path(net_log_dir()) / "net_audio").string();
}
std::uint64_t net_audio_max_bytes() {
    const char* m = std::getenv("DSD_NET_AUDIO_MAX_MB");
    unsigned long mb = (m && m[0]) ? std::strtoul(m, nullptr, 10) : 1024;
    return static_cast<std::uint64_t>(mb) * 1024ull * 1024ull;
}
// Imported calls' audio (an "export with audio" zip's WAVs, uploaded by
// Import): <audio dir>/imported, emptied at startup (imports aren't kept
// across a restart); capped at DSD_NET_IMPORT_AUDIO_MAX_MB (default 1024).
std::string net_import_audio_dir() { return (std::filesystem::path(net_audio_dir()) / "imported").string(); }
std::uint64_t net_import_audio_max_bytes() {
    const char* m = std::getenv("DSD_NET_IMPORT_AUDIO_MAX_MB");
    unsigned long mb = (m && m[0]) ? std::strtoul(m, nullptr, 10) : 1024;
    return static_cast<std::uint64_t>(mb) * 1024ull * 1024ull;
}
std::int64_t net_audio_max_age_ms() {
    const char* h = std::getenv("DSD_NET_AUDIO_MAX_AGE_H");
    return (h && h[0]) ? static_cast<std::int64_t>(std::strtod(h, nullptr) * 3600.0 * 1000.0) : 0;
}
std::string net_audio_json(const CallAudioStore::Status& a) {
    return json::Writer().field("on", a.on).field("dir", a.dir).field("bytes", static_cast<double>(a.bytes))
        .field("cap_bytes", static_cast<double>(a.cap_bytes)).field("files", static_cast<double>(a.files))
        .field("recording", static_cast<double>(a.open)).str();
}
// Network-explorer speech-to-text (the explorer's "Transcribe on play"): the
// browser runs Whisper itself (Transformers.js); the server only serves the
// library and model files, from DSD_NET_ASR_DIR, else <net log dir>/net_asr
// (tools/get_asr_assets.sh fills it). DSD_NET_ASR_MODEL picks the default
// model (else the most accurate one present), DSD_NET_ASR_LANG the default
// language (else english; "auto" detects it).
std::string net_asr_dir() {
    const char* d = std::getenv("DSD_NET_ASR_DIR");
    return (d && d[0]) ? std::string(d) : (std::filesystem::path(net_log_dir()) / "net_asr").string();
}
// A path under the asset folder that may be served: plain names only (no
// "..", no hidden files, no absolute paths).
bool asr_rel_ok(const std::string& rel) {
    if (rel.empty() || rel.size() > 200) return false;
    std::size_t p = 0;
    while (p <= rel.size()) {
        std::size_t e = rel.find('/', p);
        if (e == std::string::npos) e = rel.size();
        const std::string seg = rel.substr(p, e - p);
        if (seg.empty() || seg[0] == '.') return false;
        for (char c : seg)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-') return false;
        p = e + 1;
    }
    return true;
}
// What the asset folder holds, for the page: whether the library is there,
// which models (org/name with the quantized ONNX files), and the defaults.
std::string net_asr_config_json() {
    namespace fs = std::filesystem;
    const fs::path dir = net_asr_dir();
    std::error_code ec;
    const bool lib = fs::is_regular_file(dir / "transformers.min.js", ec) &&
                     fs::is_regular_file(dir / "ort" / "ort-wasm-simd-threaded.asyncify.wasm", ec);
    std::vector<std::string> models;
    auto subdirs = [](const fs::path& p) {
        std::vector<fs::path> out;
        std::error_code e;
        for (fs::directory_iterator it(p, e), end; !e && it != end; it.increment(e))
            if (it->is_directory(e) && asr_rel_ok(it->path().filename().string())) out.push_back(it->path());
        return out;
    };
    for (const fs::path& org : subdirs(dir / "models"))
        for (const fs::path& m : subdirs(org))
            if (fs::is_regular_file(m / "config.json", ec) && fs::is_regular_file(m / "onnx" / "encoder_model_quantized.onnx", ec) &&
                fs::is_regular_file(m / "onnx" / "decoder_model_merged_quantized.onnx", ec))
                models.push_back(org.filename().string() + "/" + m.filename().string());
    std::sort(models.begin(), models.end());
    const char* em = std::getenv("DSD_NET_ASR_MODEL");
    const char* el = std::getenv("DSD_NET_ASR_LANG");
    std::string model = (em && em[0]) ? em : "";
    if (model.empty()) {
        // The most accurate model present: on vocoder audio small is right
        // where base and tiny are badly wrong (it is also ~3x slower).
        for (const char* m : {"Xenova/whisper-small.en", "Xenova/whisper-small", "Xenova/whisper-base.en",
                              "Xenova/whisper-base", "Xenova/whisper-tiny.en", "Xenova/whisper-tiny"})
            if (std::find(models.begin(), models.end(), m) != models.end()) { model = m; break; }
        if (model.empty()) model = models.empty() ? "Xenova/whisper-small" : models.front();
    }
    std::string list = "[";
    for (std::size_t i = 0; i < models.size(); ++i) list += (i ? "," : "") + assocjson::q(models[i]);
    list += "]";
    return std::string("{\"local\":") + (lib && !models.empty() ? "true" : "false") + ",\"lib\":" + (lib ? "true" : "false") +
           ",\"models\":" + list + ",\"model\":" + assocjson::q(model) +
           ",\"language\":" + assocjson::q((el && el[0]) ? el : "english") + ",\"dir\":" + assocjson::q(dir.string()) + "}";
}
std::string asr_content_type(const std::string& rel) {
    auto ends = [&](const char* x) { const std::size_t n = std::strlen(x); return rel.size() >= n && rel.compare(rel.size() - n, n, x) == 0; };
    if (ends(".js") || ends(".mjs")) return "text/javascript";
    if (ends(".wasm")) return "application/wasm";
    if (ends(".json")) return "application/json";
    if (ends(".txt")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

bool env_on(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return false;
    const std::string s(v);
    return s[0] == '1' || s[0] == 'y' || s[0] == 'Y' || s[0] == 't' || s[0] == 'T' || s == "on";
}

// The explorer's switches remembered across restarts ({"audio":true} -- the
// Audio switch): DSD_NET_SETTINGS_FILE, or net_settings.json in the
// recordings folder. Missing or unreadable = nothing remembered.
std::string net_settings_file() {
    const char* f = std::getenv("DSD_NET_SETTINGS_FILE");
    return (f && f[0]) ? std::string(f) : (std::filesystem::path(net_log_dir()) / "net_settings.json").string();
}
// -1 = not remembered, 0 = off, 1 = on.
int net_setting(const char* key) {
    std::ifstream in(net_settings_file(), std::ios::binary);
    if (!in) return -1;
    std::stringstream ss;
    ss << in.rdbuf();
    mjson::V root;
    if (!mjson::parse(ss.str(), root) || root.t != mjson::V::Obj) return -1;
    const mjson::V* v = root.get(key);
    if (!v || v->t != mjson::V::Bool) return -1;
    return v->b ? 1 : 0;
}
void remember_net_setting(const char* key, bool on) {
    static std::mutex mu;                      // two requests at once: one writer
    std::lock_guard<std::mutex> lk(mu);
    const std::string path = net_settings_file(), tmp = path + ".tmp";
    mjson::V root;
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        if (in) ss << in.rdbuf();
        if (!mjson::parse(ss.str(), root) || root.t != mjson::V::Obj) root = mjson::V();
    }
    std::map<std::string, bool> vals;          // keep the other switches
    if (root.t == mjson::V::Obj)
        for (const auto& kv : root.o) if (kv.second.t == mjson::V::Bool) vals[kv.first] = kv.second.b;
    vals[key] = on;
    std::error_code ec;
    const auto dir = std::filesystem::path(path).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << "{";
        bool first = true;
        for (const auto& kv : vals) { out << (first ? "" : ",") << assocjson::q(kv.first) << ":" << (kv.second ? "true" : "false"); first = false; }
        out << "}\n";
        if (!out) return;
    }
    std::filesystem::rename(tmp, path, ec);
}

// The pages' UI build: a fingerprint of each page's code (the status page's
// rendered with no data), stamped into the page ("%%UI_BUILD%%") and sent
// with every poll (/net.json and /status.json "ui"). A page loaded from older
// code sees the difference and reloads itself -- after a server update the
// open pages pick up the new UI without the user refreshing. Restarting the
// same code changes nothing.
void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
}
struct UiPage { std::string html, id; };
const UiPage& net_ui_page() {
    static const UiPage p = [] {
        UiPage u;
        u.html = render_net_page_html();
        u.id = fnv_hex(u.html).substr(0, 12);
        replace_all(u.html, "%%UI_BUILD%%", u.id);
        return u;
    }();
    return p;
}
const std::string& status_ui_id() {
    static const std::string id = fnv_hex(render_status_html(ServerStats::Snapshot{})).substr(0, 12);
    return id;
}
// {"ui":"<id>", ...rest of the object}
std::string with_ui(const std::string& json, const std::string& id) {
    if (json.size() < 2 || json[0] != '{') return json;
    return "{\"ui\":\"" + id + "\"" + (json[1] == '}' ? "" : ",") + json.substr(1);
}

// gzip `in` into `out` (fast compression level: the explorer's poll).
bool gzip_string(const std::string& in, std::string& out) {
    z_stream z{};
    if (deflateInit2(&z, 1, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
    out.resize(deflateBound(&z, static_cast<uLong>(in.size())) + 32);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = reinterpret_cast<Bytef*>(&out[0]);
    z.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return rc == Z_STREAM_END;
}

// Largest HTTP request body accepted (explorer imports / merges of exports).
constexpr std::uint64_t kMaxHttpBody = 128ull << 20;

// "a%20b+c" -> "a b c" (query-string values).
std::string url_decode(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') { o += ' '; continue; }
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
            continue;
        }
        o += s[i];
    }
    return o;
}
// The value of `key` in a query string, decoded ("" if absent).
std::string query_param(const std::string& query, const std::string& key) {
    std::size_t p = 0;
    while (p <= query.size()) {
        std::size_t e = query.find('&', p);
        if (e == std::string::npos) e = query.size();
        const std::string kv = query.substr(p, e - p);
        const std::size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key) return eq == std::string::npos ? std::string() : url_decode(kv.substr(eq + 1));
        p = e + 1;
    }
    return std::string();
}

// /net/merge: several exports, merged without touching the live model (the
// explorer's Open... with several files). Body: {"files":[{"name":..,"text":..}]}
// with each file's text verbatim. Returns {"report":[..],"export":{..}}.
std::string net_merge_response(const std::string& body) {
    mjson::V root;
    std::string err;
    if (!mjson::parse(body, root, &err) || !root.get("files") || root.get("files")->t != mjson::V::Arr)
        return "{\"error\":" + assocjson::q("expected {\"files\":[{\"name\":..,\"text\":..}]}" +
                                              (err.empty() ? std::string() : " (" + err + ")")) + "}";
    std::vector<std::pair<std::string, std::string>> files;
    for (auto& f : root.get("files")->a) {
        if (f.t != mjson::V::Obj) continue;
        files.emplace_back(f.str("name"), f.str("text"));
    }
    std::vector<MergeReport> report;
    ChannelMerge cm;
    cm.per_receiver = AssocModel::default_per_receiver();
    const Dataset merged = merge_exports(files, report, cm);
    std::string rep = "[";
    for (std::size_t i = 0; i < report.size(); ++i)
        rep += (i ? "," : "") + std::string("{\"name\":") + assocjson::q(report[i].label) + ",\"status\":" +
               assocjson::q(report[i].status) + ",\"message\":" + assocjson::q(report[i].message) + "}";
    rep += "]";
    const std::int64_t now = AssocModel::now_ms();
    return "{\"report\":" + rep + ",\"export\":" + export_json(merged, now, "", "merged") + "}";
}

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

Session::Session(tcp::socket socket, std::shared_ptr<ServerStats> stats)
    : ws_(std::move(socket)), stats_(std::move(stats)) {}

Session::~Session() {
    stop_pipeline();
    // Drop this connection from the live-session table (no-op if it never
    // registered, e.g. an HTTP status request).
    if (stats_ && stats_id_) stats_->remove_session(stats_id_);
}

void Session::run() {
    // Note the peer for the status table before we do anything else (the
    // socket is still connected here).
    beast::error_code rec;
    auto ep = beast::get_lowest_layer(ws_).socket().remote_endpoint(rec);
    if (!rec) remote_ = ep.address().to_string() + ":" + std::to_string(ep.port());

    // Read one HTTP request first. This connection is either a WebSocket
    // upgrade (a client session) or a plain GET for the status page; we
    // can't tell until we've seen the request line and headers. Bound the
    // read with a timeout so a silent client can't tie up the socket.
    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
    auto self = shared_from_this();
    http_parser_.emplace();
    http_parser_->body_limit(kMaxHttpBody);     // /net/import, /net/merge uploads
    http::async_read(ws_.next_layer(), read_buffer_, *http_parser_,
        [self](beast::error_code ec, std::size_t) {
            if (!ec) self->http_req_ = self->http_parser_->release();
            self->http_parser_.reset();
            self->on_http_read(ec);
        });
}

void Session::on_http_read(beast::error_code ec) {
    if (ec) {
        // EOF/connection-closed before a full request is routine (health
        // probes, port scans); don't log it noisily.
        if (ec != http::error::end_of_stream && ec != net::error::operation_aborted)
            std::cerr << "http read error: " << ec.message() << "\n";
        return;
    }

    if (websocket::is_upgrade(http_req_)) {
        // A WebSocket client. Hand the already-parsed request to the
        // WebSocket accept so the handshake completes normally.
        //
        // Timeout policy for the live connection: bound the handshake, but
        // set idle_timeout = none() so an idle client is NEVER dropped for
        // inactivity. That matters here for two reasons: a client may just
        // hold the socket open to verify the server is up (a health check
        // that sends nothing), and a real decode session legitimately goes
        // quiet for minutes on an idle RF channel -- neither should be
        // disconnected. (The 30s deadline armed in run() was only ever meant
        // to bound the *initial HTTP read*; on_accept clears it below so it
        // can't fire mid-session.)
        websocket::stream_base::timeout opt{};
        opt.handshake_timeout = std::chrono::seconds(30);
        opt.idle_timeout = websocket::stream_base::none();
        opt.keep_alive_pings = false;
        ws_.set_option(opt);
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) {
                res.set(http::field::server, "dsd-server/1.0");
            }));
        auto self = shared_from_this();
        ws_.async_accept(http_req_, [self](beast::error_code aec) { self->on_accept(aec); });
        return;
    }

    // Not an upgrade: serve the status page (or a 404) and close.
    serve_http();
}

void Session::serve_http() {
    auto res = std::make_shared<http::response<http::string_body>>();
    res->version(http_req_.version());
    res->keep_alive(false);
    res->set(http::field::server, "dsd-server/1.0");

    std::string target(http_req_.target());
    std::string query;
    if (auto q = target.find('?'); q != std::string::npos) { query = target.substr(q + 1); target = target.substr(0, q); }

    // Download the current (or most recent) network-explorer recording,
    // streamed from disk. Only the file the server itself wrote is served.
    if (target == "/net/log/download" && http_req_.method() == http::verb::get && stats_) {
        stats_->assoc().flush_recording();               // make everything so far readable
        const auto rs = stats_->assoc().recording();
        http::file_body::value_type body;
        beast::error_code fec;
        if (!rs.last_path.empty()) body.open(rs.last_path.c_str(), beast::file_mode::scan, fec);
        if (!rs.last_path.empty() && !fec) {
            auto fres = std::make_shared<http::response<http::file_body>>(
                std::piecewise_construct, std::make_tuple(std::move(body)),
                std::make_tuple(http::status::ok, http_req_.version()));
            fres->set(http::field::server, "dsd-server/1.0");
            fres->set(http::field::content_type, "application/gzip");
            fres->set(http::field::content_disposition,
                      "attachment; filename=\"" + std::filesystem::path(rs.last_path).filename().string() + "\"");
            fres->keep_alive(false);
            fres->prepare_payload();
            auto self = shared_from_this();
            http::async_write(ws_.next_layer(), *fres, [self, fres](beast::error_code, std::size_t) {
                beast::error_code ig;
                beast::get_lowest_layer(self->ws_).socket().shutdown(tcp::socket::shutdown_send, ig);
            });
            return;
        }
        res->result(http::status::not_found);
        res->set(http::field::content_type, "application/json");
        res->body() = "{\"error\":\"no recording yet\"}";
        res->prepare_payload();
        auto self = shared_from_this();
        http::async_write(ws_.next_layer(), *res, [self, res](beast::error_code, std::size_t) {
            beast::error_code ig;
            beast::get_lowest_layer(self->ws_).socket().shutdown(tcp::socket::shutdown_send, ig);
        });
        return;
    }

    // Speech-to-text assets (/net/asr/<path>, see net_asr_dir): the library,
    // its WebAssembly and the model files -- tens of MB, streamed from disk.
    // Revalidated by ETag, so a browser downloads them once. Served with the
    // explorer's cross-origin isolation headers (multi-threaded WebAssembly).
    if (target.rfind("/net/asr/", 0) == 0 && target != "/net/asr/config.json" && http_req_.method() == http::verb::get) {
        namespace fs = std::filesystem;
        const std::string rel = target.substr(9);
        std::error_code ec;
        const fs::path path = fs::path(net_asr_dir()) / rel;
        const bool ok = asr_rel_ok(rel) && fs::is_regular_file(path, ec);
        const std::uint64_t size = ok ? fs::file_size(path, ec) : 0;
        const auto mtime = ok ? fs::last_write_time(path, ec).time_since_epoch().count() : 0;
        char tag[64];
        std::snprintf(tag, sizeof tag, "\"%llx-%llx\"", static_cast<unsigned long long>(size), static_cast<unsigned long long>(mtime));
        auto finish = [this](auto r) {
            r->set(http::field::server, "dsd-server/1.0");
            r->set("Cross-Origin-Opener-Policy", "same-origin");
            r->set("Cross-Origin-Embedder-Policy", "credentialless");
            r->set("Cross-Origin-Resource-Policy", "same-origin");
            r->keep_alive(false);
            r->prepare_payload();
            auto self = shared_from_this();
            http::async_write(ws_.next_layer(), *r, [self, r](beast::error_code, std::size_t) {
                beast::error_code ig;
                beast::get_lowest_layer(self->ws_).socket().shutdown(tcp::socket::shutdown_send, ig);
            });
        };
        if (ok && std::string(http_req_[http::field::if_none_match]) == tag) {
            res->result(http::status::not_modified);
            res->set(http::field::etag, tag);
            res->set(http::field::cache_control, "no-cache");
            finish(res);
            return;
        }
        http::file_body::value_type body;
        beast::error_code fec;
        if (ok) body.open(path.string().c_str(), beast::file_mode::scan, fec);
        if (!ok || fec) {
            res->result(http::status::not_found);
            res->set(http::field::content_type, "text/plain; charset=utf-8");
            res->body() = "no such file -- see tools/get_asr_assets.sh\n";
            finish(res);
            return;
        }
        auto fres = std::make_shared<http::response<http::file_body>>(
            std::piecewise_construct, std::make_tuple(std::move(body)), std::make_tuple(http::status::ok, http_req_.version()));
        fres->set(http::field::content_type, asr_content_type(rel));
        fres->set(http::field::etag, tag);
        fres->set(http::field::cache_control, "no-cache");
        finish(fres);
        return;
    }

    // A call's audio (/net/audio/<file>.wav): only files the audio store
    // made. Byte ranges are supported (Safari requires them for media, and
    // they make seeking work); a recording still in progress is served as far
    // as it has got.
    if (target.rfind("/net/audio/", 0) == 0 && target != "/net/audio/on" && target != "/net/audio/off" &&
        http_req_.method() == http::verb::get && stats_) {
        const std::string path = stats_->assoc().audio_path(target.substr(11));
        std::string data;
        if (!path.empty()) {
            std::ifstream f(path, std::ios::binary);
            data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        if (path.empty() || data.size() < 44) {
            res->result(http::status::not_found);
            res->set(http::field::content_type, "text/plain; charset=utf-8");
            res->body() = "no such audio\n";
        } else {
            std::size_t a = 0, z = data.size() - 1;
            bool partial = false;
            const std::string range(http_req_[http::field::range]);
            if (range.rfind("bytes=", 0) == 0 && range.find(',') == std::string::npos) {
                const std::string spec = range.substr(6);
                const std::size_t dash = spec.find('-');
                if (dash != std::string::npos) {
                    const std::string lo = spec.substr(0, dash), hi = spec.substr(dash + 1);
                    if (lo.empty() && !hi.empty()) {                         // last N bytes
                        const std::size_t nlast = std::min<std::size_t>(std::strtoull(hi.c_str(), nullptr, 10), data.size());
                        a = data.size() - nlast;
                    } else {
                        a = std::strtoull(lo.c_str(), nullptr, 10);
                        if (!hi.empty()) z = std::min<std::size_t>(std::strtoull(hi.c_str(), nullptr, 10), data.size() - 1);
                    }
                    partial = true;
                }
            }
            if (partial && (a > z || a >= data.size())) {
                res->result(http::status::range_not_satisfiable);
                res->set(http::field::content_range, "bytes */" + std::to_string(data.size()));
            } else {
                res->result(partial ? http::status::partial_content : http::status::ok);
                res->set(http::field::content_type, "audio/wav");
                res->set(http::field::accept_ranges, "bytes");
                res->set(http::field::cache_control, "no-store");
                if (partial)
                    res->set(http::field::content_range, "bytes " + std::to_string(a) + "-" + std::to_string(z) + "/" +
                                                         std::to_string(data.size()));
                res->body() = partial ? data.substr(a, z - a + 1) : std::move(data);
            }
        }
        res->prepare_payload();
        auto self = shared_from_this();
        http::async_write(ws_.next_layer(), *res, [self, res](beast::error_code, std::size_t) {
            beast::error_code ig;
            beast::get_lowest_layer(self->ws_).socket().shutdown(tcp::socket::shutdown_send, ig);
        });
        return;
    }

    const bool post = http_req_.method() == http::verb::post;
    if (post && target == "/net/import") {
        // Explorer Import...: add an export (body, JSON or gzip) as a layer on
        // top of the live data. ?name= labels it. 200 with the outcome --
        // imported / replaced / skipped / refused / invalid -- and a reason.
        res->result(http::status::ok);
        res->set(http::field::content_type, "application/json");
        if (stats_) {
            std::string name = query_param(query, "name");
            if (name.empty()) name = "import";
            const auto r = stats_->assoc().import_export(std::move(http_req_.body()), name);
            if (r.status == "invalid") res->result(http::status::bad_request);
            std::cerr << "net import: " << name << ": " << r.status << " (" << r.message << ")\n";
            res->body() = "{\"name\":" + assocjson::q(name) + ",\"status\":" + assocjson::q(r.status) +
                          ",\"message\":" + assocjson::q(r.message) + ",\"id\":" + std::to_string(r.id) + "}";
        } else {
            res->body() = "{}";
        }
    } else if (post && target == "/net/import/audio") {
        // One call's WAV from an imported "export with audio" zip (body),
        // for import ?id= -- kept only if a call in that import names ?name=.
        res->set(http::field::content_type, "application/json");
        if (stats_) {
            const auto r = stats_->assoc().import_audio(std::strtoull(query_param(query, "id").c_str(), nullptr, 10),
                                                        query_param(query, "name"), http_req_.body());
            res->result(r.status == "added" || r.status == "have" ? http::status::ok
                        : r.status == "full" ? http::status::insufficient_storage
                        : r.status == "failed" ? http::status::internal_server_error
                        : r.status == "invalid" ? http::status::bad_request : http::status::not_found);
            res->body() = "{\"status\":" + assocjson::q(r.status) + ",\"message\":" + assocjson::q(r.message) +
                          ",\"calls\":" + std::to_string(r.calls) + "}";
        } else {
            res->result(http::status::not_found);
            res->body() = "{}";
        }
    } else if (post && target == "/net/keys/set") {
        // Add/replace a decryption key for (fam, net, kid). The value is
        // stored but never echoed back; /net.json only shows the id is set.
        res->result(http::status::ok);
        res->set(http::field::content_type, "application/json");
        bool ok = false;
        if (stats_) {
            const auto obj = json::parse_flat_object(http_req_.body());
            ok = stats_->assoc().set_key(json::get_string(obj, "fam"), json::get_string(obj, "net"),
                                         json::get_string(obj, "kid"), json::get_string(obj, "alg"),
                                         json::get_string(obj, "key"));
        }
        if (!ok) res->result(http::status::bad_request);
        res->body() = std::string("{\"ok\":") + (ok ? "true" : "false") + "}";
    } else if (post && target == "/net/merge") {
        const std::string out = net_merge_response(http_req_.body());
        res->result(out.compare(0, 9, "{\"error\":") == 0 ? http::status::bad_request : http::status::ok);
        res->set(http::field::content_type, "application/json");
        res->body() = out;
    } else if (http_req_.method() != http::verb::get) {
        res->result(http::status::method_not_allowed);
        res->set(http::field::content_type, "text/plain; charset=utf-8");
        res->body() = "405 method not allowed\n";
    } else if (target == "/" || target == "/status" || target == "/status.html") {
        res->result(http::status::ok);
        res->set(http::field::content_type, "text/html; charset=utf-8");
        res->set(http::field::cache_control, "no-cache");     // always this server's own page
        res->body() = stats_ ? render_status_html(stats_->snapshot()) : std::string("no stats\n");
        replace_all(res->body(), "%%UI_BUILD%%", status_ui_id());
    } else if (target == "/status.json") {
        res->result(http::status::ok);
        res->set(http::field::content_type, "application/json");
        res->body() = with_ui(stats_ ? render_status_json(stats_->snapshot()) : std::string("{}"), status_ui_id());
    } else if (target == "/log.json") {
        res->result(http::status::ok);
        res->set(http::field::content_type, "application/json");
        res->body() = stats_ ? render_log_json(stats_->log_snapshot()) : std::string("{\"log\":[]}");
    } else if (target == "/log/clear") {
        // Status-page Clear button: empty the log ring, return the now-empty
        // log. (A side-effecting GET, kept simple for this local debug page.)
        if (stats_) stats_->clear_log();
        res->result(http::status::ok);
        res->set(http::field::content_type, "application/json");
        res->body() = std::string("{\"log\":[]}");
    } else if (target == "/net" || target == "/net.html") {
        // Network explorer: calls, talkgroups, radios and their associations.
        res->result(http::status::ok);
        res->set(http::field::content_type, "text/html; charset=utf-8");
        // Cross-origin isolated (where the browser allows it: HTTPS or
        // localhost), so speech-to-text can use several CPU threads.
        res->set("Cross-Origin-Opener-Policy", "same-origin");
        res->set("Cross-Origin-Embedder-Policy", "credentialless");
        res->set(http::field::cache_control, "no-cache");     // always this server's own page
        res->body() = net_ui_page().html;
    } else if (target == "/net/manual.pdf") {
        // The explorer's user manual (Help), built into the server.
        const std::string_view pdf = net_manual_pdf();
        if (pdf.empty()) {
            res->result(http::status::not_found);
            res->set(http::field::content_type, "text/plain; charset=utf-8");
            res->body() = "This server was built without the manual (docs/NET_EXPLORER.pdf).\n";
        } else {
            res->result(http::status::ok);
            res->set(http::field::content_type, "application/pdf");
            res->set(http::field::content_disposition, "inline; filename=\"dsd-server-network-explorer-manual.pdf\"");
            res->set(http::field::cache_control, "no-cache");
            res->body().assign(pdf.data(), pdf.size());
        }
    } else if (target == "/net/asr_worker.js") {
        // The explorer's speech-to-text worker (net_page.hpp).
        res->result(http::status::ok);
        res->set(http::field::content_type, "text/javascript");
        res->set(http::field::cache_control, "no-cache");
        res->set("Cross-Origin-Embedder-Policy", "credentialless");
        res->set("Cross-Origin-Resource-Policy", "same-origin");
        res->body() = render_asr_worker_js();
    } else if (target == "/net/asr/config.json") {
        res->result(http::status::ok);
        res->set(http::field::content_type, "application/json");
        res->set(http::field::cache_control, "no-store");
        res->body() = net_asr_config_json();
    } else if (target == "/net.json") {
        res->result(http::status::ok);
        res->set(http::field::content_type, "application/json");
        res->body() = with_ui(stats_ ? stats_->assoc().to_json() : std::string("{\"families\":{}}"), net_ui_page().id);
        // Clients connected (any session, decoding or not): the explorer's connection summary.
        if (stats_) res->body().insert(1, "\"clients\":" + std::to_string(stats_->connected()) + ",");
        // The explorer polls this every 1.5 s and, with thousands of calls
        // listed, it runs to megabytes: send it compressed (~10x smaller)
        // when the client accepts gzip (every browser does).
        const std::string ae(http_req_[http::field::accept_encoding]);
        if (ae.find("gzip") != std::string::npos && res->body().size() > 1024) {
            std::string gz;
            if (gzip_string(res->body(), gz)) {
                res->body() = std::move(gz);
                res->set(http::field::content_encoding, "gzip");
                res->set(http::field::vary, "Accept-Encoding");
            }
        }
    } else if (target == "/net/export.json" || target == "/net/export.graphml") {
        // Explorer Export: what the explorer shows, as a file to keep -- the
        // native format (re-openable in the explorer) or GraphML for graph tools.
        const std::int64_t now = AssocModel::now_ms();
        const bool graphml = target == "/net/export.graphml";
        const std::string name = "net_export_" + assoclog::utc_stamp(now) + (graphml ? ".graphml" : ".json");
        res->result(http::status::ok);
        res->set(http::field::content_type, graphml ? "application/graphml+xml" : "application/json");
        res->set(http::field::content_disposition, "attachment; filename=\"" + name + "\"");
        res->body() = !stats_ ? std::string() : graphml ? stats_->assoc().to_graphml(now) : stats_->assoc().to_export_json(now);
    } else if (target == "/net/log/on" || target == "/net/log/off") {
        // Explorer Record button: record every input to the association model
        // (assoc_log.hpp) for offline replay. "?clear=1" wipes the model first
        // so the recording starts fresh and replays exactly.
        bool ok = true;
        if (stats_) {
            if (target == "/net/log/on") {
                const bool clear = query.find("clear=1") != std::string::npos;
                ok = stats_->assoc().start_recording(net_log_dir(), net_log_max_bytes(), clear);
                if (ok) std::cerr << "net recording: writing " << stats_->assoc().recording().path << "\n";
                else std::cerr << "net recording: could not create a file in " << net_log_dir() << "\n";
            } else {
                stats_->assoc().stop_recording();
            }
        }
        res->result(ok ? http::status::ok : http::status::internal_server_error);
        res->set(http::field::content_type, "application/json");
        res->body() = stats_ ? net_rec_json(stats_->assoc().recording()) : std::string("{}");
    } else if (target == "/net/audio/on" || target == "/net/audio/off") {
        // Explorer Audio switch: record each call's decoded voice (off by
        // default; see net_audio_dir and the DSD_NET_AUDIO_* settings). The
        // choice is remembered for the next start (net_settings_file).
        bool ok = true;
        if (stats_) {
            if (target == "/net/audio/on") {
                ok = stats_->assoc().start_audio(net_audio_dir(), net_audio_max_bytes(), net_audio_max_age_ms());
                std::cerr << (ok ? "net audio: recording calls into " : "net audio: cannot write to ") << net_audio_dir() << "\n";
                if (ok) remember_net_setting("audio", true);
            } else {
                stats_->assoc().stop_audio();
                remember_net_setting("audio", false);
            }
        }
        res->result(ok ? http::status::ok : http::status::internal_server_error);
        res->set(http::field::content_type, "application/json");
        res->body() = stats_ ? net_audio_json(stats_->assoc().audio_status()) : std::string("{}");
    } else if (target == "/net/networks/merge" || target == "/net/networks/unmerge") {
        // Explorer network merges (NetMerges, assoc_merge.hpp), shared by every
        // viewer: ?fam=dmr&from=KEY&to=KEY shows `from` as part of `to`;
        // unmerge ?fam=dmr&key=KEY undoes it. Returns the rules now in effect.
        bool ok = false;
        std::string merges = "{}";
        if (stats_) {
            AssocModel& m = stats_->assoc();
            const std::string fam = query_param(query, "fam");
            ok = target == "/net/networks/merge"
                     ? m.merge_networks(fam, query_param(query, "from"), query_param(query, "to"))
                     : m.unmerge_network(fam, query_param(query, "key"));
            merges = merges_json(m.merges());
        }
        res->result(http::status::ok);
        res->set(http::field::content_type, "application/json");
        res->body() = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"merges\":" + merges + "}";
    } else if (target == "/net/keys/remove") {
        bool ok = stats_ && stats_->assoc().remove_key(query_param(query, "fam"), query_param(query, "net"),
                                                       query_param(query, "kid"));
        res->result(ok ? http::status::ok : http::status::bad_request);
        res->set(http::field::content_type, "application/json");
        res->body() = std::string("{\"ok\":") + (ok ? "true" : "false") + "}";
    } else if (target == "/net/keys/list") {
        // Download the dsd-fme hex key list (-K) for one network. This is the
        // one place a key VALUE leaves the server (for the operator's decoder).
        const std::string fam = query_param(query, "fam"), net = query_param(query, "net");
        res->result(http::status::ok);
        res->set(http::field::content_type, "text/csv; charset=utf-8");
        std::string fn = "dsd_keys_" + fam + ".csv";
        for (char& c : fn) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_') c = '_';
        res->set(http::field::content_disposition, "attachment; filename=\"" + fn + "\"");
        res->set(http::field::cache_control, "no-store");
        res->body() = stats_ ? stats_->assoc().keys_csv(fam, net) : std::string("KEY ID,KEY\n");
    } else if (target == "/net/imports/remove" || target == "/net/imports/clear") {
        // Remove one import (?id=N) or all of them; the live data stays.
        bool ok = true;
        if (stats_) {
            if (target == "/net/imports/clear") stats_->assoc().clear_imports();
            else ok = stats_->assoc().remove_import(std::strtoull(query_param(query, "id").c_str(), nullptr, 10));
        }
        res->result(ok ? http::status::ok : http::status::not_found);
        res->set(http::field::content_type, "application/json");
        res->body() = std::string("{\"ok\":") + (ok ? "true" : "false") + "}";
    } else if (target == "/net/clear") {
        // Explorer Clear button: forget everything learned so far, and the
        // imports (a side-effecting GET, matching /log/clear's style for
        // these pages).
        // ?audio=1 also deletes the call audio files.
        std::pair<std::uint64_t, std::uint64_t> gone{0, 0};
        if (stats_) {
            if (query.find("audio=1") != std::string::npos) gone = stats_->assoc().clear_with_audio();
            else stats_->assoc().clear();
        }
        res->result(http::status::ok);
        res->set(http::field::content_type, "application/json");
        res->body() = "{\"ok\":true,\"audio_files\":" + std::to_string(gone.first) +
                      ",\"audio_bytes\":" + std::to_string(gone.second) + "}";
    } else if (target == "/iq_log/on" || target == "/iq_log/off") {
        // Status-page "Log IQ" switch: flip the global IQ-capture toggle. Every
        // active session opens (on) or closes (off) its BLUE capture, and new
        // sessions follow the switch at start. Returns the state now in effect.
        // (A side-effecting GET, matching /log/clear's style for this page.)
        bool on = (target == "/iq_log/on");
        bool state = stats_ ? stats_->set_iq_logging(on) : false;
        res->result(http::status::ok);
        res->set(http::field::content_type, "application/json");
        res->body() = std::string("{\"iq_log_enabled\":") + (state ? "true" : "false") + "}";
    } else {
        res->result(http::status::not_found);
        res->set(http::field::content_type, "text/plain; charset=utf-8");
        res->body() = "404 not found\n";
    }
    res->prepare_payload();

    auto self = shared_from_this();
    http::async_write(ws_.next_layer(), *res,
        [self, res](beast::error_code, std::size_t) {
            // One request, one response: close the connection cleanly.
            beast::error_code ig;
            beast::get_lowest_layer(self->ws_).socket().shutdown(tcp::socket::shutdown_send, ig);
        });
}

void Session::on_accept(beast::error_code ec) {
    if (ec) {
        std::cerr << "accept error: " << ec.message() << "\n";
        return;
    }
    // Handshake done: drop the initial-HTTP-read deadline armed in run() so
    // it can't fire mid-session and tear down an otherwise-healthy idle
    // connection. The websocket idle policy (none, set above) now governs
    // inactivity. Turn on TCP keep-alive so the OS still eventually reaps a
    // genuinely dead peer without us ever dropping a live idle client.
    beast::get_lowest_layer(ws_).expires_never();
    beast::error_code ka;
    beast::get_lowest_layer(ws_).socket().set_option(net::socket_base::keep_alive(true), ka);

    // A WebSocket client is now connected: register it so the status page
    // can count it and show its protocol once a pipeline starts.
    if (stats_) {
        stats_id_ = stats_->add_session(remote_);
        // Let the status page's global "Log IQ" switch reach this session: the
        // callback (invoked on whatever thread flips the switch) just posts the
        // open/close onto this connection's strand. The weak_ptr breaks the
        // cycle and guards the window between the session dying and its control
        // being unregistered.
        std::weak_ptr<Session> wp = weak_from_this();
        stats_->register_iq_control(stats_id_, [wp](bool on) {
            if (auto self = wp.lock())
                net::post(self->ws_.get_executor(), [self, on] { self->set_iq_logging(on); });
        });
    }
    // Advertise what this build can emit before the client sends anything,
    // so it can prepare to parse the event kinds and `extra` token keys
    // without hard-coding them from PROTOCOL.md.
    send_text(build_capabilities_json());
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
            // Experimental: apply the RRC symbol matched filter in the FM
            // path (opt-in, default off; see FmDemodConfig). Only the FM/DSD
            // chain uses it -- the TETRA chain ignores it.
            bool matched_filter = json::get_bool(obj, "matched_filter", false);
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
            // Paging-only options (ignored by the FM/DSD and TETRA chains):
            // POCSAG text interpretation and discriminator polarity.
            std::string pocsag_mode = json::get_string(obj, "pocsag_mode");
            bool invert = json::get_bool(obj, "invert", false);
            // Optional raw-IQ capture: log the whole session's incoming IQ to a
            // MIDAS BLUE file (server-side; see DSD_IQ_LOG_DIR). Off by default.
            bool iq_log = json::get_bool(obj, "iq_log", false);
            // Optional absolute tuner centre frequency (Hz). With it the
            // channel's frequency is known (centre + freq_offset), which the
            // network explorer uses to label and key what it hears.
            center_freq_ = json::get_number(obj, "center_freq", 0.0);
            start_pipeline(sample_rate, bw, offset, gain, afc, protocol, key_type, key, matched_filter,
                           pocsag_mode, invert, iq_log);
        } else if (type == "set_gain") {
            // Only the FM discriminator has a gain knob; the TETRA (π/4) modem
            // doesn't, and the paging demod auto-scales PCM by deviation --
            // accept the message (no error) and ignore it there.
            if (chain_.load() == Chain::Fm) {
                std::lock_guard<std::mutex> lock(demod_mutex_);
                if (demod_) demod_->set_gain(static_cast<float>(json::get_number(obj, "gain", 26000.0)));
            }
        } else if (type == "set_freq_offset") {
            if (stats_ && center_freq_ > 0)
                stats_->set_freq(stats_id_, center_freq_ + json::get_number(obj, "hz", 0.0));
            // The TETRA π/4 demod estimates and removes residual CFO itself
            // (±Rs/8), so it has no live NCO to retune -- accept and ignore.
            if (chain_.load() == Chain::Fm) {
                const double hz = json::get_number(obj, "hz", 0.0);
                {
                    std::lock_guard<std::mutex> lock(demod_mutex_);
                    if (demod_) demod_->set_freq_offset(hz);
                }
                // Another channel now: the explorer starts this stream's identity over.
                if (stats_ && stats_id_ && center_freq_ > 0)
                    stats_->assoc().retune_stream(stats_id_, channel_freq(center_freq_ + hz));
            } else if (chain_.load() == Chain::Pager) {
                std::lock_guard<std::mutex> lock(demod_mutex_);
                if (pager_demod_) pager_demod_->set_freq_offset(json::get_number(obj, "hz", 0.0));
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
    // Capture the raw IQ verbatim (it's already CF data) before backpressure
    // may drop it downstream, so the BLUE file is a faithful record of what the
    // client sent. Runs on this connection's strand, same as start/stop.
    if (iq_log_) iq_log_->write(data, len);

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
                          Tetra, Tetrakit,
                          // dsd-fme-only modes (no DSDcc decoder): EDACS trunking
                          // + its ProVoice digital voice, and legacy Motorola
                          // X2-TDMA. Edacs = Standard/NET, EdacsEa = Extended
                          // Addressing; the *Esk forms add EDACS's 0xA0 ESK mask.
                          ProVoice, Edacs, EdacsEsk, EdacsEa, EdacsEaEsk, X2tdma,
                          // Paging (FM discriminator -> multimon-ng): all paging
                          // decoders at once, or one family / rate.
                          PagerAuto, Pocsag, Pocsag512, Pocsag1200, Pocsag2400, Flex };

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
    // EDACS (+ its ProVoice digital voice) and legacy Motorola X2-TDMA -- all
    // decoded by the dsd-fme backend only. "esk" selects the 0xA0 ESK-masked
    // control-channel variant; "ea" selects Extended Addressing.
    if (k == "provoice" || k == "pv") return ProtocolHint::ProVoice;
    if (k == "edacsesk" || k == "edacsstdesk" || k == "edacsnetesk") return ProtocolHint::EdacsEsk;
    if (k == "edacseaesk") return ProtocolHint::EdacsEaEsk;
    if (k == "edacsea") return ProtocolHint::EdacsEa;
    if (k == "edacs" || k == "edacsstd" || k == "edacsnet") return ProtocolHint::Edacs;
    if (k == "x2tdma" || k == "x2" || k == "x2t") return ProtocolHint::X2tdma;
    // Paging runs its own chain (FM -> multimon-ng). Note plain "auto" stays
    // the DSD auto-detect; paging is only selected by these explicit hints.
    if (k == "pagerauto" || k == "pager" || k == "paging") return ProtocolHint::PagerAuto;
    if (k == "pocsag") return ProtocolHint::Pocsag;
    if (k == "pocsag512") return ProtocolHint::Pocsag512;
    if (k == "pocsag1200") return ProtocolHint::Pocsag1200;
    if (k == "pocsag2400") return ProtocolHint::Pocsag2400;
    if (k == "flex" || k == "flexnext") return ProtocolHint::Flex;
    // "auto", "unknown", "notsure", and anything else -> auto-detect
    return ProtocolHint::Auto;
}

bool hint_is_tetra(ProtocolHint h) {
    return h == ProtocolHint::Tetra || h == ProtocolHint::Tetrakit;
}

bool hint_is_pager(ProtocolHint h) {
    return h == ProtocolHint::PagerAuto || h == ProtocolHint::Pocsag || h == ProtocolHint::Pocsag512 ||
           h == ProtocolHint::Pocsag1200 || h == ProtocolHint::Pocsag2400 || h == ProtocolHint::Flex;
}

// multimon-ng demodulators for a paging hint. Running all of them at once is
// cheap (each is a correlator on 22 kHz audio) and is what makes pager-auto
// work: whichever one syncs produces the pages.
std::vector<std::string> pager_demods(ProtocolHint h) {
    switch (h) {
        case ProtocolHint::Pocsag:     return {"POCSAG512", "POCSAG1200", "POCSAG2400"};
        case ProtocolHint::Pocsag512:  return {"POCSAG512"};
        case ProtocolHint::Pocsag1200: return {"POCSAG1200"};
        case ProtocolHint::Pocsag2400: return {"POCSAG2400"};
        case ProtocolHint::Flex:       return {"FLEX_NEXT"};
        default:                       return {"POCSAG512", "POCSAG1200", "POCSAG2400", "FLEX_NEXT"};
    }
}

// Canonical, human-readable label for the status page's "Protocol" column.
// This reflects what the session is actually decoding (the resolved hint),
// not the client's raw free-form string.
const char* protocol_hint_label(ProtocolHint h) {
    switch (h) {
        case ProtocolHint::Dmr:        return "dmr";
        case ProtocolHint::Nxdn48:     return "nxdn48";
        case ProtocolHint::Nxdn96:     return "nxdn96";
        case ProtocolHint::P25p1:      return "p25p1";
        case ProtocolHint::P25p2:      return "p25p2";
        case ProtocolHint::Dpmr:       return "dpmr";
        case ProtocolHint::Dstar:      return "dstar";
        case ProtocolHint::Ysf:        return "ysf";
        case ProtocolHint::Tetra:      return "tetra";
        case ProtocolHint::Tetrakit:   return "tetrakit";
        case ProtocolHint::ProVoice:   return "provoice";
        case ProtocolHint::Edacs:      return "edacs";
        case ProtocolHint::EdacsEsk:   return "edacs_esk";
        case ProtocolHint::EdacsEa:    return "edacs_ea";
        case ProtocolHint::EdacsEaEsk: return "edacs_ea_esk";
        case ProtocolHint::X2tdma:     return "x2tdma";
        case ProtocolHint::PagerAuto:  return "pager-auto";
        case ProtocolHint::Pocsag:     return "pocsag";
        case ProtocolHint::Pocsag512:  return "pocsag512";
        case ProtocolHint::Pocsag1200: return "pocsag1200";
        case ProtocolHint::Pocsag2400: return "pocsag2400";
        case ProtocolHint::Flex:       return "flex";
        case ProtocolHint::Auto:       return "auto";
        case ProtocolHint::Default:    return "dmr"; // historical default
    }
    return "dmr";
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

// Where per-session IQ captures are written (DSD_IQ_LOG_DIR, default ".")
// and the per-file size cap in bytes (DSD_IQ_LOG_MAX_MB, default 1024; 0 =
// unlimited -- use with care, a high IQ rate fills disk fast).
std::string iq_log_dir() {
    const char* d = std::getenv("DSD_IQ_LOG_DIR");
    return (d && d[0]) ? std::string(d) : std::string(".");
}
std::uint64_t iq_log_max_bytes() {
    const char* m = std::getenv("DSD_IQ_LOG_MAX_MB");
    unsigned long mb = (m && m[0]) ? std::strtoul(m, nullptr, 10) : 1024;
    return static_cast<std::uint64_t>(mb) * 1024ull * 1024ull;
}

// A filesystem-safe token from a free-form protocol string.
std::string sanitize_token(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (c == '-' || c == '_') out += c;
    }
    if (out.empty()) out = "auto";
    if (out.size() > 24) out.resize(24);
    return out;
}

} // namespace

void Session::open_iq_log(const std::string& protocol, double sample_rate) {
    if (iq_log_) return; // already capturing
    std::error_code ec;
    std::filesystem::create_directories(iq_log_dir(), ec);
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char ts[24];
    std::strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tm);
    char name[160];
    // "_c<Hz>" = the tuner centre frequency, when the client sent it, so a
    // replay of the capture (tools/midas_ws_client.py) can send it again.
    char center[40] = "";
    if (center_freq_ > 0) std::snprintf(center, sizeof center, "_c%lldHz", static_cast<long long>(center_freq_ + 0.5));
    std::snprintf(name, sizeof name, "iq_%s_s%llu_%s_%lldHz%s.blue", ts,
                  static_cast<unsigned long long>(stats_id_),
                  sanitize_token(protocol).c_str(),
                  static_cast<long long>(sample_rate), center);
    std::string path = (std::filesystem::path(iq_log_dir()) / name).string();
    auto w = std::make_unique<BlueFileWriter>();
    if (w->open(path, sample_rate, iq_log_max_bytes())) {
        iq_log_ = std::move(w);
        std::cerr << "iq capture: writing " << path << "\n";
    } else {
        std::cerr << "iq capture: could not open " << path << " (check DSD_IQ_LOG_DIR)\n";
    }
}

void Session::set_iq_logging(bool on) {
    // Runs on this session's strand (posted by ServerStats::set_iq_logging).
    if (on) {
        // Only an active pipeline has IQ flowing; an idle session will open its
        // capture when its next start_pipeline sees the global switch on.
        if (pipeline_active_.load() && !iq_log_)
            open_iq_log(iq_protocol_, iq_sample_rate_);
    } else if (iq_log_) {
        if (iq_log_->truncated())
            std::cerr << "iq capture: reached size cap at " << iq_log_->bytes()
                      << " bytes -- " << iq_log_->path() << "\n";
        iq_log_.reset(); // close() patches data_size
    }
}

void Session::start_pipeline(double sample_rate, double channel_bw, double freq_offset,
                             float gain, bool afc, const std::string& protocol,
                             const std::string& key_type, const std::string& key,
                             bool matched_filter, const std::string& pocsag_mode, bool invert,
                             bool iq_log) {
    stop_pipeline(); // clean slate if already running

    const ProtocolHint hint = parse_protocol_hint(protocol);
    const bool want_pager = hint_is_pager(hint);
    // Record the request for the status page's run-wide protocols view, before
    // any of the ways a start can fail below (bad key, missing decoder, ...).
    if (stats_) {
        stats_->note_request(stats_id_, protocol_hint_label(hint),
                             hint_is_tetra(hint) ? "tetra" : want_pager ? "pager" : "fm");
        // Network explorer: open a fresh stream context BEFORE any backend can
        // emit, so even its first events land in this stream (paging maps to
        // no family and is ignored by the model).
        if (stats_id_)
            stats_->assoc().begin_stream(stats_id_, protocol_hint_label(hint), AssocModel::now_ms(), std::string(),
                                         center_freq_ > 0 ? channel_freq(center_freq_ + freq_offset) : 0);
        // The status page's frequency column (0 = the client didn't say).
        stats_->set_freq(stats_id_, center_freq_ > 0 ? center_freq_ + freq_offset : 0.0);
    }

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
        // Feed the network explorer's association model (structured, before
        // serialization). It has its own lock; this runs on the reader thread.
        if (self->stats_ && self->stats_id_) self->stats_->assoc().ingest(self->stats_id_, ev);
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
            .field("crc_error", crc_error_wire(ev.crc_error))
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
            // Detection defaults to coherent (Costas): it resolves its π/4
            // parity from burst-grid lock, decodes ~1.5-1.7 dB cleaner above the
            // low-SNR crossover, and falls back to differential on its own when
            // it can't lock (see tetra_frontend.hpp). Set DSD_TETRA_COHERENT=0
            // to force the differential path (e.g. for the very low-SNR fringe
            // below the crossover, where the loop can slip).
            const char* coh = std::getenv("DSD_TETRA_COHERENT");
            tdcfg.coherent = !(coh && coh[0] == '0');
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
        chain_.store(Chain::Tetra);
    } else if (want_pager) {
        // ---- Paging: FM discriminator (22050 Hz) + multimon-ng subprocess ----
        // gain / matched_filter / key_type / key are DSD-path knobs: the paging
        // demod scales PCM by deviation in Hz (right at any IQ rate), and
        // pagers aren't encrypted. Accepted for a uniform "start" shape and
        // ignored. sample_rate, channel_bandwidth, freq_offset and afc apply.
        (void)gain; (void)matched_filter; (void)key_type; (void)key;

        std::string mode = pocsag_mode;
        for (char& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!mode.empty() && mode != "auto" && mode != "alpha" && mode != "numeric" && mode != "skyper") {
            send_text(json::Writer().field("type", std::string("error"))
                          .field("message", std::string("unknown pocsag_mode: ") + pocsag_mode).str());
            return;
        }

        PagerDemodConfig pcfg;
        pcfg.input_sample_rate_hz = sample_rate;
        pcfg.channel_bandwidth_hz = channel_bw;
        pcfg.freq_offset_hz = freq_offset;
        pcfg.afc_enabled = afc;
        pcfg.invert = invert;

        // Decoder lines arrive on the MultimonProcess reader thread. Capture
        // the strand executor and a weak_ptr and only lock it ON the strand:
        // if the reader thread ever held the last shared_ptr, ~Session would
        // run there and join that very thread in stop_pipeline.
        auto ex = ws_.get_executor();
        auto on_line = [weak_self, ex](const std::string& line) {
            DsdEvent ev;
            if (!multimon_line_to_event(line, ev)) {
                if (line.empty() || line[0] != '{') std::cerr << "multimon-ng: " << line << "\n";
                return;
            }
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
                .field("crc_error", crc_error_wire(ev.crc_error))
                .field("message", ev.message)
                .field("extra", ev.extra)
                .field("raw", ev.raw_line)
                .str();
            net::post(ex, [weak_self, s = std::move(s)] {
                if (auto self = weak_self.lock()) self->send_text(s);
            });
        };
        auto on_exit = [weak_self, ex] {
            net::post(ex, [weak_self] {
                auto self = weak_self.lock();
                if (self && self->pipeline_active_.load() && self->chain_.load() == Chain::Pager &&
                    !self->pager_failed_.exchange(true)) {
                    self->send_text(json::Writer().field("type", std::string("error"))
                                        .field("message", std::string("pager decoder exited unexpectedly")).str());
                }
            });
        };

        MultimonConfig mcfg;
        mcfg.demods = pager_demods(hint);
        mcfg.pocsag_mode = mode;
        pager_failed_ = false;
        auto proc = std::make_unique<MultimonProcess>();
        if (!proc->start(mcfg, on_line, on_exit)) {
            send_text(json::Writer().field("type", std::string("error"))
                          .field("message", std::string("failed to start pager backend")).str());
            return;
        }
        {
            std::lock_guard<std::mutex> lock(demod_mutex_);
            pager_demod_ = std::make_unique<PagerFmDemodulator>(pcfg);
        }
        pager_proc_ = std::move(proc);
        chain_.store(Chain::Pager);
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
        cfg.matched_filter_enabled = matched_filter;
        {
            std::lock_guard<std::mutex> lock(demod_mutex_);
            demod_ = std::make_unique<FmDemodulator>(cfg);
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
            // DSDcc has no EDACS/ProVoice/X2-TDMA decoder; fall back to
            // auto-detect (honest "nothing" rather than mis-decoding as DMR).
            // Use the dsd-fme backend for these protocols.
            case ProtocolHint::ProVoice:
            case ProtocolHint::Edacs:
            case ProtocolHint::EdacsEsk:
            case ProtocolHint::EdacsEa:
            case ProtocolHint::EdacsEaEsk:
            case ProtocolHint::X2tdma: dcfg.mode = "auto";   break;
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
        // verified against dsd-fme's source (dsd_main.c, getopt 'f' case, which
        // matches on optarg): s=DMR, i=NXDN48/IDAS, n=NXDN96, a=auto-detect,
        // d=D-STAR, y=YSF, m=dPMR. P25: 1=Phase 1, 2=Phase 2 (6000 sps TDMA).
        // EDACS/ProVoice + X2-TDMA (dsd-fme only; no DSDcc decoder): p=ProVoice,
        // h=EDACS STD/NET, H=EDACS STD/NET+ESK(0xA0), e=EDACS EA, E=EDACS EA+ESK,
        // x=X2-TDMA. (EDACS modes also enable ProVoice; the H/E forms apply the
        // 0xA0 ESK control-channel mask, with dsd-fme's default 4:4:3 AFS.)
        switch (hint) {
            case ProtocolHint::Nxdn48:     dcfg.mode_flag = "i"; break;
            case ProtocolHint::Nxdn96:     dcfg.mode_flag = "n"; break;
            case ProtocolHint::P25p1:      dcfg.mode_flag = "1"; break;
            case ProtocolHint::P25p2:      dcfg.mode_flag = "2"; break;
            case ProtocolHint::Dpmr:       dcfg.mode_flag = "m"; break;
            case ProtocolHint::Dstar:      dcfg.mode_flag = "d"; break;
            case ProtocolHint::Ysf:        dcfg.mode_flag = "y"; break;
            case ProtocolHint::ProVoice:   dcfg.mode_flag = "p"; break;
            case ProtocolHint::Edacs:      dcfg.mode_flag = "h"; break;
            case ProtocolHint::EdacsEsk:   dcfg.mode_flag = "H"; break;
            case ProtocolHint::EdacsEa:    dcfg.mode_flag = "e"; break;
            case ProtocolHint::EdacsEaEsk: dcfg.mode_flag = "E"; break;
            case ProtocolHint::X2tdma:     dcfg.mode_flag = "x"; break;
            case ProtocolHint::Auto:       dcfg.mode_flag = "a"; break;
            case ProtocolHint::Dmr:
            case ProtocolHint::Default:
            default:                       dcfg.mode_flag = "s"; break;
        }
        dcfg.udp_audio_port = udp_audio_port_;
        dcfg.on_suppressed = [weak_self](const DsdEvent& ev) {
            auto self = weak_self.lock();
            if (self && self->stats_ && self->stats_id_) self->stats_->assoc().ingest(self->stats_id_, ev);
        };
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

        // Network explorer per-call audio (off unless enabled): every slot's
        // decoded voice, before the client's mono mix. A session with a key
        // hears encrypted calls in the clear, so those may be recorded.
        if (stats_ && stats_id_) {
            auto st = stats_;
            const std::uint64_t sid = stats_id_;
            const bool keyed = key_type_enum != KeyType::None;
            dcfg.on_slot_audio = [st, sid, keyed](int slot, const int16_t* pcm, std::size_t n) {
                st->assoc().audio(sid, slot, pcm, n, keyed);
            };
        }

        bool ok = dsd_.start(dcfg, on_event, on_audio);
        if (!ok) {
            send_text(json::Writer().field("type", std::string("error"))
                          .field("message", std::string("failed to start DSD backend")).str());
            std::lock_guard<std::mutex> lock(demod_mutex_);
            demod_.reset();
            return;
        }
    }

    // Remember the pipeline's params so a later live toggle (the status page's
    // global "Log IQ" switch, via set_iq_logging) can open a capture with the
    // right filename and xdelta without re-plumbing them.
    iq_protocol_ = protocol;
    iq_sample_rate_ = sample_rate;
    // Optional raw-IQ capture: open a BLUE (CF) file now that the backend
    // started, so a failed start never leaves an empty file. No IQ is logged
    // before pipeline_active_ anyway (handle_binary_message gates on it).
    // Enabled by the client's iq_log flag or the global switch being on.
    if (iq_log || (stats_ && stats_->iq_logging()))
        open_iq_log(protocol, sample_rate);

    pipeline_active_ = true;
    worker_running_ = true;
    worker_thread_ = std::thread(&Session::demod_worker_loop, this);

    // Publish the decode protocol/chain to the status page now that the
    // pipeline is up.
    if (stats_ && stats_id_) {
        stats_->set_protocol(stats_id_, protocol_hint_label(hint),
                             want_tetra ? "tetra" : want_pager ? "pager" : "fm", /*active=*/true);
    }

    {
        json::Writer w;
        w.field("type", std::string("started"))
         .field("udp_audio_port", static_cast<double>(udp_audio_port_));
        // Tell the client where the IQ capture is being written (server-side
        // path), so it knows what to retrieve.
        if (iq_log_ && iq_log_->is_open()) w.field("iq_log_file", iq_log_->path());
        send_text(w.str());
    }
}

void Session::stop_pipeline() {
    if (!pipeline_active_.exchange(false)) return;

    // Mark this session idle on the status page (it keeps its last protocol
    // label; the "State" column flips to idle).
    if (stats_ && stats_id_) stats_->set_pipeline_active(stats_id_, false);

    worker_running_ = false;
    iq_cv_.notify_all();
    if (worker_thread_.joinable()) worker_thread_.join();

    // Stop whichever chain this session ran. chain_ was set in start_pipeline;
    // the idle chains' members are null/stopped, so keying off chain_ just
    // avoids redundant stops. Reset to Fm (the default) as we tear down.
    const Chain chain = chain_.exchange(Chain::Fm);
    switch (chain) {
        case Chain::Tetra: if (tetra_backend_) tetra_backend_->stop(); break;
        case Chain::Pager: if (pager_proc_) pager_proc_->stop(); break; // flushes pending pages
        case Chain::Fm:    dsd_.stop(); break;
    }
    {
        // Safe to take here: the worker was joined above, so nothing is
        // inside process()/demodulate() holding this.
        std::lock_guard<std::mutex> lock(demod_mutex_);
        switch (chain) {
            case Chain::Tetra: tetra_demod_.reset(); tetra_backend_.reset(); break;
            case Chain::Pager: pager_demod_.reset(); pager_proc_.reset(); break;
            case Chain::Fm:    demod_.reset(); break;
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

    // Network explorer: the backend is stopped (its reader threads joined),
    // so every event is in -- close this stream's open calls.
    if (stats_ && stats_id_) stats_->assoc().end_stream(stats_id_);

    // Finalize the IQ capture, if any (patches the BLUE header's data_size).
    if (iq_log_) {
        if (iq_log_->truncated())
            std::cerr << "iq capture: reached size cap at " << iq_log_->bytes()
                      << " bytes -- " << iq_log_->path() << "\n";
        iq_log_.reset();
    }
}

void Session::demod_worker_loop() {
    // chain_ is fixed for this worker's lifetime: start_pipeline sets it (and
    // creates the matching demod/backend) before launching the thread, and
    // stop_pipeline joins the thread before clearing it. So we read it once and
    // take the matching path.
    //
    // The TETRA path demodulates each IQ block to bits and relays them to the
    // subprocess decoder; the FM path demodulates to PCM and feeds the DSD
    // backend. Both demodulators are streaming -- filter history, the timing
    // loop, the differential/CFO state and the AGC carry across
    // demodulate()/process() calls -- so decoding per WebSocket frame is
    // continuous with no per-frame restart or seam glitch (a fresh demod is
    // created per start(), the clean-slate boundary). The few trailing samples
    // held back for filter context at a stop are immaterial.
    const Chain chain = chain_.load();
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

        if (chain == Chain::Pager) {
            pcm_scratch.clear();
            {
                std::lock_guard<std::mutex> lock(demod_mutex_);
                if (pager_demod_) pager_demod_->process(block.data(), block.size(), pcm_scratch);
            }
            if (!pcm_scratch.empty() && pager_proc_) pager_proc_->write_audio(pcm_scratch.data(), pcm_scratch.size());
        } else if (chain == Chain::Tetra) {
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

namespace {
// The status-page log captures the JSON frames the server sends clients, but
// skips two kinds that add noise without much value: the high-rate voice
// events (the JSON side of "voice data") and the large, once-per-connect
// capabilities greeting (identical every time). Cheap substring tests on the
// serialized frame -- the field values here are server-controlled, not user
// text.
bool skip_from_log(const std::string& msg) {
    if (msg.find("\"type\":\"capabilities\"") != std::string::npos) return true;
    return msg.find("\"type\":\"event\"") != std::string::npos &&
           msg.find("\"kind\":\"voice\"") != std::string::npos;
}
} // namespace

void Session::send_text(const std::string& msg) {
    // Mirror the outbound JSON into the shared log buffer for the status
    // page's log tab (voice events + the capabilities greeting excluded;
    // binary voice never comes here).
    if (stats_ && stats_id_ && !skip_from_log(msg))
        stats_->add_log(stats_id_, msg);

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

    // DSD_NET_LOG=1: record the network explorer's inputs from startup (the
    // model is empty, so the recording replays exactly). Stop/download it from
    // the explorer page or /net/log/off and /net/log/download.
    if (const char* nl = std::getenv("DSD_NET_LOG"); nl && (nl[0] == '1' || nl[0] == 'y' || nl[0] == 'Y' ||
                                                           nl[0] == 't' || nl[0] == 'T' || std::string(nl) == "on")) {
        if (stats_->assoc().start_recording(net_log_dir(), net_log_max_bytes(), false))
            std::cerr << "net recording: writing " << stats_->assoc().recording().path << "\n";
        else
            std::cerr << "net recording: could not create a file in " << net_log_dir() << "\n";
    }

    // The explorer's network merges, kept across restarts.
    if (stats_->assoc().use_merges_file(net_merges_file()))
        std::cerr << "net merges: loaded " << merges_count(stats_->assoc().merges()) << " from " << net_merges_file() << "\n";

    // The explorer's encryption keyring (values on disk, owner-only), kept
    // across restarts. Entered in the UI, fed to the operator's own decoder.
    if (stats_->assoc().use_keys_file(net_keys_file()))
        std::cerr << "net keys: loaded " << stats_->assoc().keys_count() << " from " << net_keys_file() << "\n";

    stats_->assoc().use_import_audio_dir(net_import_audio_dir(), net_import_audio_max_bytes());

    // Record each call's decoded voice from startup: DSD_NET_AUDIO=1 (=0 never),
    // or, with it unset, as the explorer's Audio switch was last left.
    const char* na = std::getenv("DSD_NET_AUDIO");
    const bool audio_at_start = (na && na[0]) ? env_on("DSD_NET_AUDIO") : net_setting("audio") == 1;
    if (audio_at_start) {
        if (stats_->assoc().start_audio(net_audio_dir(), net_audio_max_bytes(), net_audio_max_age_ms()))
            std::cerr << "net audio: recording calls into " << net_audio_dir() << "\n";
        else
            std::cerr << "net audio: cannot write to " << net_audio_dir() << "\n";
    }

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
                std::make_shared<Session>(std::move(socket), stats_)->run();
            } else {
                std::cerr << "accept error: " << ec.message() << "\n";
            }
            do_accept();
        });
}

} // namespace dsdsrv
