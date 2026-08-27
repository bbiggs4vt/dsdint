// fake_dsd_server.hpp
//
// A dependency-free, in-process FAKE of dsd-server, for the unit tests of
// any C++ client that talks to the real server. It speaks the exact same
// WebSocket wire protocol (see PROTOCOL.md) but contains NO DSP: it never
// demodulates or decodes anything. Instead the test scripts what the
// server emits (started/event/error JSON, 0x01-tagged PCM binary) and
// then asserts on what the client sent back (start params, gain/offset
// changes, how much IQ it streamed). That exercises the client's real
// WebSocket + JSON/binary parsing paths against a faithful protocol peer,
// with none of Boost/DSDcc/dsd-fme/real-IQ in the loop.
//
// Dependencies: C++17 + POSIX sockets + std::thread. Nothing else — drop
// this single header into your test target and go. (Server-side WebSocket
// handshake and framing, including SHA-1 and base64, are implemented
// below; it is the mirror image of the stdlib client in
// tools/midas_ws_client.py.)
//
// Typical use in a client's test:
//
//     #include "fake_dsd_server.hpp"
//     using namespace fakedsd;
//
//     FakeDsdServer srv;                 // ephemeral port, replies "started"
//     srv.start();
//     MyClient client(srv.url());        // <-- the code under test
//     client.connect_and_start(/* sample_rate=... protocol="dmr" */);
//
//     srv.wait_for_control(1s);          // wait for the client's "start"
//     auto start = srv.last_start();
//     assert(start.fields["protocol"] == "dmr");
//
//     srv.send_event(Event{}.k("call").tg("150607").src("2222223").sl("2"));
//     srv.send_audio(std::vector<int16_t>(160, 0));   // a 20 ms voice frame
//     // ... assert the client surfaced the call / played the audio ...
//
//     srv.send_error("simulated backend failure");    // inject the error path
//     srv.close_connection();
//
// It also builds as a standalone process (see tools/fake_dsd_server.cpp)
// so a non-C++ or subprocess-style test can spawn it and point a URL at
// it. Handles ONE client connection at a time (accepts the next after the
// current one closes), which is all a unit test needs.

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fakedsd {

// ------------------------------------------------------------------ detail
namespace detail {

// --- SHA-1 (for the WebSocket accept key). Public-domain-style compact
// implementation; only used on the short handshake key, never hot. ---
struct Sha1 {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

    static std::string hash(const std::string& msg) {
        Sha1 s;
        std::vector<uint8_t> data(msg.begin(), msg.end());
        uint64_t bitlen = static_cast<uint64_t>(data.size()) * 8;
        data.push_back(0x80);
        while (data.size() % 64 != 56) data.push_back(0x00);
        for (int i = 7; i >= 0; --i) data.push_back(static_cast<uint8_t>((bitlen >> (i * 8)) & 0xFF));

        for (std::size_t off = 0; off < data.size(); off += 64) {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i)
                w[i] = (data[off + i * 4] << 24) | (data[off + i * 4 + 1] << 16)
                     | (data[off + i * 4 + 2] << 8) | (data[off + i * 4 + 3]);
            for (int i = 16; i < 80; ++i) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

            uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4];
            for (int i = 0; i < 80; ++i) {
                uint32_t f, k;
                if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999u; }
                else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
                else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
                uint32_t tmp = rol(a, 5) + f + e + k + w[i];
                e = d; d = c; c = rol(b, 30); b = a; a = tmp;
            }
            s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d; s.h[4] += e;
        }
        std::string out(20, '\0');
        for (int i = 0; i < 5; ++i) {
            out[i*4+0] = static_cast<char>((s.h[i] >> 24) & 0xFF);
            out[i*4+1] = static_cast<char>((s.h[i] >> 16) & 0xFF);
            out[i*4+2] = static_cast<char>((s.h[i] >> 8) & 0xFF);
            out[i*4+3] = static_cast<char>(s.h[i] & 0xFF);
        }
        return out;
    }
};

inline std::string base64(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i+1]) << 8) | uint8_t(in[i+2]);
        out.push_back(tbl[(n >> 18) & 63]); out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);  out.push_back(tbl[n & 63]);
    }
    if (i < in.size()) {
        uint32_t n = uint8_t(in[i]) << 16;
        if (i + 1 < in.size()) n |= uint8_t(in[i+1]) << 8;
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back((i + 1 < in.size()) ? tbl[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

inline std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c & 0xFF);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Minimal flat-JSON object parser: enough for dsd-server's control frames
// (a single depth of string/number/bool values). Returns the string form
// of each value (numbers kept as written, strings unescaped). Not a
// general JSON parser — control messages are flat by protocol contract.
inline std::map<std::string, std::string> parse_flat_object(const std::string& s) {
    std::map<std::string, std::string> out;
    std::size_t i = 0;
    auto skip_ws = [&] { while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i; };
    auto parse_string = [&](std::string& dst) -> bool {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char e = s[++i];
                switch (e) {
                    case 'n': dst += '\n'; break; case 't': dst += '\t'; break;
                    case 'r': dst += '\r'; break; case '"': dst += '"'; break;
                    case '\\': dst += '\\'; break; case '/': dst += '/'; break;
                    case 'u': { // \uXXXX -> keep ASCII, else '?'
                        if (i + 4 < s.size()) {
                            int v = std::strtol(s.substr(i+1, 4).c_str(), nullptr, 16);
                            dst += (v < 0x80) ? static_cast<char>(v) : '?';
                            i += 4;
                        }
                        break;
                    }
                    default: dst += e; break;
                }
                ++i;
            } else {
                dst += s[i++];
            }
        }
        if (i < s.size()) ++i; // closing quote
        return true;
    };
    skip_ws();
    if (i >= s.size() || s[i] != '{') return out;
    ++i;
    while (true) {
        skip_ws();
        if (i >= s.size() || s[i] == '}') break;
        std::string key;
        if (!parse_string(key)) break;
        skip_ws();
        if (i < s.size() && s[i] == ':') ++i;
        skip_ws();
        std::string val;
        if (i < s.size() && s[i] == '"') {
            parse_string(val);
        } else {
            while (i < s.size() && s[i] != ',' && s[i] != '}') val += s[i++];
            while (!val.empty() && (val.back()==' '||val.back()=='\t')) val.pop_back();
        }
        out[key] = val;
        skip_ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
    }
    return out;
}

} // namespace detail

// ------------------------------------------------------------------ types

// One server->client `event` frame. Every field defaults to "" (the
// protocol's "unknown" value) and every key is always serialized, matching
// the real server's flat 13-key event schema. Chainable setters keep test
// call-sites terse: Event{}.k("call").tg("150607").src("2222223").sl("2").
struct Event {
    std::string kind, talkgroup, source_id, slot, color_code, ran, nac,
                emergency, alias, crc_error, extra, raw;
    Event& k(std::string v)   { kind = std::move(v);       return *this; }
    Event& tg(std::string v)  { talkgroup = std::move(v);  return *this; }
    Event& src(std::string v) { source_id = std::move(v);  return *this; }
    Event& sl(std::string v)  { slot = std::move(v);       return *this; }
    Event& cc(std::string v)  { color_code = std::move(v); return *this; }
    Event& ran_(std::string v){ ran = std::move(v);        return *this; }
    Event& nac_(std::string v){ nac = std::move(v);        return *this; }
    Event& emerg(std::string v){ emergency = std::move(v); return *this; }
    Event& alias_(std::string v){ alias = std::move(v);    return *this; }
    Event& crc(std::string v) { crc_error = std::move(v);  return *this; }
    Event& ex(std::string v)  { extra = std::move(v);      return *this; }
    Event& rw(std::string v)  { raw = std::move(v);        return *this; }

    std::string to_json() const {
        using detail::json_escape;
        std::string s = "{\"type\":\"event\"";
        auto f = [&](const char* key, const std::string& v) {
            s += ",\""; s += key; s += "\":\""; s += json_escape(v); s += "\"";
        };
        f("kind", kind);           f("talkgroup", talkgroup);
        f("source_id", source_id); f("slot", slot);
        f("color_code", color_code); f("ran", ran); f("nac", nac);
        f("emergency", emergency); f("alias", alias); f("crc_error", crc_error);
        f("extra", extra);         f("raw", raw);
        s += "}";
        return s;
    }
};

// A decoded client->server control frame, kept for test assertions.
struct ControlMessage {
    std::string type;                          // "start" / "set_gain" / ...
    std::map<std::string, std::string> fields; // all decoded fields (string form)
    std::string raw_json;                      // the exact text frame received
};

// ------------------------------------------------------------------ server
class FakeDsdServer {
public:
    struct Options {
        uint16_t port = 0;             // 0 = ephemeral (ask the OS); read back via port()
        // Value reported in the "started" reply's udp_audio_port. 0 mimics
        // the in-process DSDcc backend (mono audio, no UDP port); non-zero
        // mimics the dsd-fme subprocess backend (stereo audio). Lets a
        // client test both branches of its own started-handling.
        uint16_t started_udp_port = 0;
        // If true, automatically reply "started" when the client sends a
        // "start" control frame. Turn off to drive the started/error reply
        // by hand from on_control (e.g. to test the error path).
        bool auto_started = true;
    };

    FakeDsdServer() = default; // default Options
    explicit FakeDsdServer(Options opts) : opts_(opts) {}
    ~FakeDsdServer() { stop(); }

    FakeDsdServer(const FakeDsdServer&) = delete;
    FakeDsdServer& operator=(const FakeDsdServer&) = delete;

    // Optional hook, called (on the server thread) for every control frame
    // the client sends, after it is recorded. `srv` is *this, so the hook
    // can script a bespoke response. Set before start().
    std::function<void(const ControlMessage& msg, FakeDsdServer& srv)> on_control;

    // Binds, listens, and starts the accept/read thread. Returns the actual
    // port (useful when Options::port was 0). Throws std::runtime_error on
    // a socket failure.
    uint16_t start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw_errno("socket");
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
        addr.sin_port = htons(opts_.port);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw_errno("bind");
        if (::listen(listen_fd_, 1) < 0) throw_errno("listen");
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
            port_ = ntohs(addr.sin_port);
        running_ = true;
        thread_ = std::thread([this] { accept_loop(); });
        return port_;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (listen_fd_ >= 0) { ::shutdown(listen_fd_, SHUT_RDWR); ::close(listen_fd_); listen_fd_ = -1; }
        close_client_fd();
        if (thread_.joinable()) thread_.join();
    }

    uint16_t port() const { return port_; }
    std::string url() const { return "ws://127.0.0.1:" + std::to_string(port_); }

    // --- scripting the server->client direction (thread-safe) ---
    void send_event(const Event& e) { send_text(e.to_json()); }

    void send_error(const std::string& message) {
        send_text(std::string("{\"type\":\"error\",\"message\":\"")
                  + detail::json_escape(message) + "\"}");
    }

    void send_started() { send_started(opts_.started_udp_port); }
    void send_started(uint16_t udp_audio_port) {
        send_text(std::string("{\"type\":\"started\",\"udp_audio_port\":")
                  + std::to_string(udp_audio_port) + "}");
    }

    // A decoded-voice PCM frame: 0x01 tag byte + little-endian int16 samples,
    // exactly as the real server tags binary audio.
    void send_audio(const std::vector<int16_t>& pcm) {
        std::vector<uint8_t> frame(1 + pcm.size() * 2);
        frame[0] = 0x01;
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            frame[1 + i*2] = static_cast<uint8_t>(pcm[i] & 0xFF);
            frame[2 + i*2] = static_cast<uint8_t>((pcm[i] >> 8) & 0xFF);
        }
        send_binary(frame);
    }

    // Escape hatches for negative tests: inject an arbitrary text/binary
    // frame (e.g. malformed JSON, an unknown type, a wrong tag byte).
    void send_raw_text(const std::string& s) { send_text(s); }
    void send_raw_binary(const std::vector<uint8_t>& b) { send_binary(b); }

    // Close the current connection with a WebSocket close handshake, or drop
    // it abruptly (no close frame) to test the client's disconnect handling.
    void close_connection() {
        std::lock_guard<std::mutex> lk(write_mutex_);
        if (client_fd_ >= 0) {
            uint8_t close_frame[2] = {0x88, 0x00}; // FIN|close, no payload
            ::send(client_fd_, close_frame, 2, MSG_NOSIGNAL);
        }
    }
    void drop_connection() { close_client_fd(); }

    // --- observing the client->server direction (for assertions) ---
    bool client_connected() const { return client_connected_.load(); }

    bool wait_for_client(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(state_mutex_);
        return state_cv_.wait_for(lk, timeout, [this] { return client_connected_.load(); });
    }

    // Block until at least `count` control messages have been received in
    // total (default: at least one more than were seen before this call).
    bool wait_for_control(std::chrono::milliseconds timeout, std::size_t count = 0) {
        std::unique_lock<std::mutex> lk(state_mutex_);
        std::size_t target = count ? count : control_.size() + 1;
        return state_cv_.wait_for(lk, timeout, [&] { return control_.size() >= target; });
    }

    std::vector<ControlMessage> control_messages() const {
        std::lock_guard<std::mutex> lk(state_mutex_);
        return control_;
    }

    // The most recent "start" control frame, or an empty ControlMessage if
    // none has been received.
    ControlMessage last_start() const {
        std::lock_guard<std::mutex> lk(state_mutex_);
        for (auto it = control_.rbegin(); it != control_.rend(); ++it)
            if (it->type == "start") return *it;
        return {};
    }

    // Total bytes of binary IQ the client has streamed (payload only, tag
    // bytes for control frames excluded) — useful to assert the client
    // actually sent IQ after "start".
    std::size_t iq_bytes_received() const { return iq_bytes_.load(); }

private:
    static void throw_errno(const char* what) {
        throw std::runtime_error(std::string("fake_dsd_server: ") + what + ": " + std::strerror(errno));
    }

    void close_client_fd() {
        int fd;
        {
            std::lock_guard<std::mutex> lk(write_mutex_);
            fd = client_fd_;
            client_fd_ = -1;
        }
        if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); ::close(fd); }
        if (client_connected_.exchange(false)) {
            std::lock_guard<std::mutex> lk(state_mutex_);
            state_cv_.notify_all();
        }
    }

    void accept_loop() {
        while (running_.load()) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) { if (running_.load()) continue; else break; }
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            if (!handshake(fd)) { ::close(fd); continue; }
            {
                std::lock_guard<std::mutex> lk(write_mutex_);
                client_fd_ = fd;
            }
            client_connected_ = true;
            { std::lock_guard<std::mutex> lk(state_mutex_); state_cv_.notify_all(); }
            read_loop(fd);
            close_client_fd();
        }
    }

    // Read the HTTP upgrade request, compute Sec-WebSocket-Accept, reply 101.
    bool handshake(int fd) {
        std::string req;
        char buf[1024];
        while (req.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            req.append(buf, static_cast<std::size_t>(n));
            if (req.size() > 65536) return false; // runaway header
        }
        std::string key = header_value(req, "sec-websocket-key");
        if (key.empty()) return false;
        std::string accept = detail::base64(
            detail::Sha1::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        return send_all(fd, resp.data(), resp.size());
    }

    static std::string header_value(const std::string& req, std::string name_lower) {
        // Case-insensitive header search.
        std::string low = req;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::size_t p = low.find(name_lower + ":");
        if (p == std::string::npos) return {};
        p = req.find(':', p) + 1;
        std::size_t e = req.find("\r\n", p);
        std::string v = req.substr(p, e - p);
        std::size_t s = v.find_first_not_of(" \t");
        std::size_t t = v.find_last_not_of(" \t");
        return (s == std::string::npos) ? std::string() : v.substr(s, t - s + 1);
    }

    void read_loop(int fd) {
        std::vector<uint8_t> rx;
        while (running_.load()) {
            uint8_t opcode = 0;
            std::string payload;
            if (!read_frame(fd, rx, opcode, payload)) return;
            if (opcode == 0x8) return;                    // close
            if (opcode == 0x9) { send_pong(payload); continue; } // ping -> pong
            if (opcode == 0xA) continue;                  // pong (ignore)
            if (opcode == 0x1) handle_text(payload);      // text control frame
            else if (opcode == 0x2) iq_bytes_ += payload.size(); // client IQ: count & drain
        }
    }

    // Read one (possibly multi-fragment) frame; returns its opcode+payload.
    // Client frames are always masked (RFC 6455); we unmask.
    bool read_frame(int fd, std::vector<uint8_t>& rx, uint8_t& opcode_out, std::string& payload_out) {
        opcode_out = 0;
        payload_out.clear();
        bool first = true;
        for (;;) {
            uint8_t hdr[2];
            if (!recv_exact(fd, rx, hdr, 2)) return false;
            bool fin = hdr[0] & 0x80;
            uint8_t opcode = hdr[0] & 0x0F;
            bool masked = hdr[1] & 0x80;
            uint64_t len = hdr[1] & 0x7F;
            if (len == 126) {
                uint8_t ext[2]; if (!recv_exact(fd, rx, ext, 2)) return false;
                len = (ext[0] << 8) | ext[1];
            } else if (len == 127) {
                uint8_t ext[8]; if (!recv_exact(fd, rx, ext, 8)) return false;
                len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
            }
            uint8_t mask[4] = {0,0,0,0};
            if (masked && !recv_exact(fd, rx, mask, 4)) return false;
            std::vector<uint8_t> data(len);
            if (len && !recv_exact(fd, rx, data.data(), len)) return false;
            if (masked) for (uint64_t i = 0; i < len; ++i) data[i] ^= mask[i & 3];
            if (first && opcode != 0x0) opcode_out = opcode; // first fragment carries opcode
            first = false;
            payload_out.append(reinterpret_cast<char*>(data.data()), data.size());
            if (fin) break;
        }
        return true;
    }

    bool recv_exact(int fd, std::vector<uint8_t>& rx, uint8_t* dst, uint64_t n) {
        while (rx.size() < n) {
            uint8_t tmp[4096];
            ssize_t r = ::recv(fd, tmp, sizeof(tmp), 0);
            if (r <= 0) return false;
            rx.insert(rx.end(), tmp, tmp + r);
        }
        std::memcpy(dst, rx.data(), n);
        rx.erase(rx.begin(), rx.begin() + n);
        return true;
    }

    void handle_text(const std::string& text) {
        ControlMessage msg;
        msg.raw_json = text;
        msg.fields = detail::parse_flat_object(text);
        auto it = msg.fields.find("type");
        msg.type = (it != msg.fields.end()) ? it->second : "";
        {
            std::lock_guard<std::mutex> lk(state_mutex_);
            control_.push_back(msg);
        }
        state_cv_.notify_all();

        if (msg.type == "start" && opts_.auto_started) send_started();
        if (on_control) on_control(msg, *this);
    }

    // --- framed writes (server->client frames are NOT masked) ---
    void send_text(const std::string& s)   { send_frame(0x1, reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
    void send_binary(const std::vector<uint8_t>& b) { send_frame(0x2, b.data(), b.size()); }
    void send_pong(const std::string& p)   { send_frame(0xA, reinterpret_cast<const uint8_t*>(p.data()), p.size()); }

    void send_frame(uint8_t opcode, const uint8_t* data, std::size_t len) {
        std::vector<uint8_t> frame;
        frame.push_back(0x80 | opcode); // FIN + opcode
        if (len < 126) {
            frame.push_back(static_cast<uint8_t>(len));
        } else if (len <= 0xFFFF) {
            frame.push_back(126);
            frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(len & 0xFF));
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; --i)
                frame.push_back(static_cast<uint8_t>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
        }
        frame.insert(frame.end(), data, data + len);
        std::lock_guard<std::mutex> lk(write_mutex_);
        if (client_fd_ >= 0) send_all(client_fd_, frame.data(), frame.size());
    }

    static bool send_all(int fd, const void* data, std::size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        std::size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    Options opts_;
    int listen_fd_ = -1;
    int client_fd_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> client_connected_{false};
    std::atomic<std::size_t> iq_bytes_{0};

    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    std::vector<ControlMessage> control_;

    std::mutex write_mutex_; // guards client_fd_ for writes/close
};

} // namespace fakedsd
