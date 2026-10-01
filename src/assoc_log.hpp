// assoc_log.hpp
//
// Recording of everything the network explorer's association model is fed
// (AssocModel, assoc_model.hpp), so a live test can be captured and replayed
// offline -- byte for byte, with the exact timestamps the model used -- to
// reproduce and analyse what the explorer showed.
//
// File format: gzip-compressed JSON Lines ("net_<UTC>.jsonl.gz"). Every line
// is one flat object whose first key is "op":
//
//   {"op":"header","v":1,"t":<ms>,"fresh":true|false,"instance":..,"name":..,"since":<ms>}
//   {"op":"begin","t":<ms>,"s":<session>,"label":"p25p1"[,"freq":<Hz>][,"family":"p25","resumed":true]}
//   {"op":"tune","t":<ms>,"s":<session>,"freq":<Hz>}   (the session's channel moved)
//   {"op":"ev","t":<ms>,"s":<session>,"kind":..,"tg":..,"src":..,"slot":..,"cc":..,
//        "ran":..,"nac":..,"em":..,"alias":..,"crc":..,"msg":..,"extra":..,"raw":..}
//   {"op":"end","t":<ms>,"s":<session>}        {"op":"remove","t":<ms>,"s":<session>}
//   {"op":"clear","t":<ms>}                    {"op":"truncated"}  (size cap reached)
//   {"op":"snapshot","t":<ms>,"why":"start"|"stop","model":{ ...the /net.json model... }}
//
// Strings escape every byte outside printable ASCII as \u00XX, so any byte
// the decoder printed (control characters, non-UTF-8) round-trips exactly.
// The file is flushed about once a second, so even an abrupt server stop
// leaves a readable file. "fresh" means the model was empty when recording
// began, so a replay of the file must reproduce the "stop" snapshot exactly.
// "instance" / "name" / "since" identify the server run (see assoc_merge.hpp),
// so an export of a replay is recognised as that run's data when merged.

#pragma once

#include "dsd_backend_types.hpp"

#include <zlib.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>

namespace dsdsrv {
namespace assoclog {

inline std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char ch : s) {
        if (ch == '"') o += "\\\"";
        else if (ch == '\\') o += "\\\\";
        else if (ch < 0x20 || ch >= 0x7f) {
            char u[8];
            std::snprintf(u, sizeof u, "\\u%04x", ch);
            o += u;
        } else {
            o += static_cast<char>(ch);
        }
    }
    return o;
}
inline std::string q(const std::string& s) { return "\"" + esc(s) + "\""; }

// Parse one flat log line into key -> value text (strings unescaped byte-
// exactly; numbers / true / false kept as their literal text). Returns false
// on a malformed line. Not for "snapshot" lines (nested) -- callers skip them.
inline bool parse_line(const std::string& s, std::map<std::string, std::string>& out) {
    out.clear();
    std::size_t i = 0;
    auto ws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) ++i; };
    auto str = [&](std::string& v) -> bool {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { v += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case 'n': v += '\n'; break;
                case 'r': v += '\r'; break;
                case 't': v += '\t'; break;
                case 'b': v += '\b'; break;
                case 'f': v += '\f'; break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned cp = static_cast<unsigned>(std::stoul(s.substr(i, 4), nullptr, 16));
                    i += 4;
                    if (cp <= 0xff) {
                        v += static_cast<char>(cp);              // our own escaping: one raw byte
                    } else if (cp < 0x800) {
                        v += static_cast<char>(0xc0 | (cp >> 6));
                        v += static_cast<char>(0x80 | (cp & 0x3f));
                    } else {
                        v += static_cast<char>(0xe0 | (cp >> 12));
                        v += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
                        v += static_cast<char>(0x80 | (cp & 0x3f));
                    }
                    break;
                }
                default: v += e;                                  // \" \\ \/
            }
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    };
    ws();
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    ws();
    if (i < s.size() && s[i] == '}') return true;
    for (;;) {
        ws();
        std::string k, v;
        if (!str(k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        ws();
        if (i < s.size() && s[i] == '"') {
            if (!str(v)) return false;
        } else {
            std::size_t st = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ' ') ++i;
            if (i == st) return false;
            v = s.substr(st, i - st);
        }
        out[k] = v;
        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') return true;
        return false;
    }
}

inline std::string event_line(std::int64_t t, std::uint64_t sid, const DsdEvent& e) {
    std::string o = "{\"op\":\"ev\",\"t\":" + std::to_string(t) + ",\"s\":" + std::to_string(sid);
    auto f = [&](const char* k, const std::string& v) { if (!v.empty()) { o += ",\""; o += k; o += "\":"; o += q(v); } };
    f("kind", e.kind); f("tg", e.talkgroup); f("src", e.source_id); f("slot", e.slot);
    f("cc", e.color_code); f("ran", e.ran); f("nac", e.nac); f("em", e.emergency);
    f("alias", e.alias); f("crc", e.crc_error); f("msg", e.message); f("extra", e.extra);
    f("raw", e.raw_line);
    return o + "}";
}
inline DsdEvent line_event(const std::map<std::string, std::string>& m) {
    auto g = [&](const char* k) { auto it = m.find(k); return it == m.end() ? std::string() : it->second; };
    DsdEvent e;
    e.kind = g("kind"); e.talkgroup = g("tg"); e.source_id = g("src"); e.slot = g("slot");
    e.color_code = g("cc"); e.ran = g("ran"); e.nac = g("nac"); e.emergency = g("em");
    e.alias = g("alias"); e.crc_error = g("crc"); e.message = g("msg"); e.extra = g("extra");
    e.raw_line = g("raw");
    return e;
}

inline std::string utc_stamp(std::int64_t ms) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d_%H%M%S", &tm);
    return b;
}

} // namespace assoclog

// Writes the recording. Owned by AssocModel and only touched under its lock.
class AssocRecorder {
public:
    ~AssocRecorder() { stop(); }

    bool start(const std::string& dir, std::uint64_t max_bytes, std::int64_t now) {
        stop();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::string name = "net_" + assoclog::utc_stamp(now) + ".jsonl.gz";
        std::string path = (std::filesystem::path(dir) / name).string();
        for (int n = 2; std::filesystem::exists(path, ec); ++n)   // two starts in one second
            path = (std::filesystem::path(dir) / ("net_" + assoclog::utc_stamp(now) + "_" +
                                                  std::to_string(n) + ".jsonl.gz")).string();
        gz_ = gzopen(path.c_str(), "wb6");
        if (!gz_) return false;
        path_ = last_path_ = path;
        bytes_ = 0;
        max_ = max_bytes;
        truncated_ = false;
        last_flush_ = now;
        return true;
    }
    void stop() {
        if (!gz_) return;
        gzclose(gz_);
        gz_ = nullptr;
        path_.clear();
    }
    bool on() const { return gz_ != nullptr; }

    // Append one line. Past the size cap it writes a final "truncated" marker
    // and drops further lines (the file stays valid).
    void write(const std::string& line, std::int64_t now) {
        if (!gz_ || truncated_) return;
        if (max_ && bytes_ + line.size() + 1 > max_) {
            truncated_ = true;
            static const char m[] = "{\"op\":\"truncated\"}\n";
            gzwrite(gz_, m, sizeof m - 1);
            gzflush(gz_, Z_SYNC_FLUSH);
            return;
        }
        gzwrite(gz_, line.data(), static_cast<unsigned>(line.size()));
        gzwrite(gz_, "\n", 1);
        bytes_ += line.size() + 1;
        if (now - last_flush_ >= 1000) {   // keep the file readable if the server dies
            gzflush(gz_, Z_SYNC_FLUSH);
            last_flush_ = now;
        }
    }
    void flush() { if (gz_) gzflush(gz_, Z_SYNC_FLUSH); }

    const std::string& path() const { return path_; }            // "" when not recording
    const std::string& last_path() const { return last_path_; }  // current or most recent file
    std::uint64_t bytes() const { return bytes_; }               // uncompressed bytes written
    bool truncated() const { return truncated_; }
    std::uint64_t file_bytes() const {
        struct stat st{};
        return (!last_path_.empty() && ::stat(last_path_.c_str(), &st) == 0) ? static_cast<std::uint64_t>(st.st_size) : 0;
    }

private:
    gzFile gz_ = nullptr;
    std::string path_, last_path_;
    std::uint64_t bytes_ = 0, max_ = 0;
    bool truncated_ = false;
    std::int64_t last_flush_ = 0;
};

} // namespace dsdsrv
