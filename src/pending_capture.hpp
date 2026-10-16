// pending_capture.hpp
//
// Deferred decryption support. An encrypted voice call heard without a
// matching key normally has its (undecodable) audio thrown away. When the
// operator enables pending capture, the server instead keeps the raw
// discriminator audio that was feeding dsd-fme for that session, so the call
// can be decrypted AFTER the fact -- once a key is loaded, the stored audio is
// replayed through a fresh dsd-fme with the key present.
//
// Why demod audio and not "ciphertext": the server never decrypts anything
// itself -- dsd-fme does, from the 48 kHz S16LE mono discriminator audio the
// server pipes to its stdin, using keys handed to it at launch. The key only
// affects dsd-fme's decode of that same audio, so capturing the audio is a
// complete, cheap (96 KB/s) record from which any later key recovers the voice.
// No re-demodulation is needed, which is why the (much larger) raw IQ is not
// what gets stored.
//
// Two pieces:
//   - PendingCapture: the live writer for ONE session's capture. A rolling
//     preroll ring means a call's start survives the latency between the audio
//     and the event that reveals it is encrypted.
//   - PendingStore: directory operations over the capture folder (list, remove,
//     clear, cap enforcement) used by the HTTP endpoints.

#pragma once

#include "json_util.hpp"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace dsdsrv {

// Per-call capture of the discriminator audio (48 kHz S16LE mono -- the exact
// bytes written to dsd-fme's stdin). One file per encrypted call, not per
// session: on a long-lived channel that would otherwise store a fixed 96 KB/s
// of mostly-idle audio, so only each call's span is kept (plus a ~3 s preroll
// so its start isn't clipped, and a short tail).
//
// A call is a run of encrypted events; begin() opens its file (the next feed()
// flushes the preroll), extend() pushes the write deadline out as events keep
// arriving, and when the deadline passes feed() stops appending -- finish()
// then closes it. A rolling preroll ring is kept CONTINUOUSLY (even between and
// during calls) so the next call always has its lead-in.
//
// Thread-safe: feed() runs on the demod worker thread; begin()/extend()/finish()
// on the dsd-fme reader thread (the encrypted-event callback), on the worker
// (idle finalize) and on the strand (stop). The mutex guards all shared state.
class PendingCapture {
public:
    static constexpr int kRate = 48000;                        // S16LE mono
    static constexpr std::size_t kPrerollSamples = kRate * 3;  // ~3 s of lead-in

    // Worker thread. `now_ms` is the current time (same clock as begin/extend).
    // Opens a pending segment (flushing the preroll), appends while the call is
    // active (now <= keep_until), and always keeps the preroll ring current so
    // the next call has its lead-in.
    void feed(const int16_t* pcm, std::size_t n, std::int64_t now_ms) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!fp_ && pending_open_) {
            fp_ = std::fopen(open_path_.c_str(), "wb");
            pending_open_ = false;
            if (fp_) {
                bytes_ = 0;
                flush_ring_locked();                       // preroll: audio BEFORE this block
                bytes_ += std::fwrite(pcm, sizeof(int16_t), n, fp_) * sizeof(int16_t);
                ring_push_locked(pcm, n);                  // keep the ring current for the next call too
                return;
            }
            open_path_.clear();                            // open failed: fall through to ringing
        }
        if (fp_ && now_ms <= keep_until_ms_) {             // active call: append
            bytes_ += std::fwrite(pcm, sizeof(int16_t), n, fp_) * sizeof(int16_t);
        }
        // Idle (fp_ open past the deadline) or no call: just keep the ring rolling.
        ring_push_locked(pcm, n);
    }

    // Reader thread. Start a new segment written to `path`, active until
    // `keep_until_ms`. The caller guarantees no segment is currently open
    // (finish() the previous one first).
    void begin(const std::string& path, std::int64_t keep_until_ms) {
        std::lock_guard<std::mutex> lk(mu_);
        if (fp_ || pending_open_) return;                  // already have one
        open_path_ = path;
        keep_until_ms_ = keep_until_ms;
        pending_open_ = true;
    }
    // Reader thread. The call is still going: push the write deadline out.
    void extend(std::int64_t keep_until_ms) {
        std::lock_guard<std::mutex> lk(mu_);
        if (keep_until_ms > keep_until_ms_) keep_until_ms_ = keep_until_ms;
    }

    bool is_open() const { std::lock_guard<std::mutex> lk(mu_); return fp_ != nullptr || pending_open_; }
    std::uint64_t bytes() const { std::lock_guard<std::mutex> lk(mu_); return bytes_; }

    // Close the current segment; returns its bytes (0 if it never opened). A
    // segment that was armed but never fed is flushed (its preroll) so a call
    // armed right at a stop isn't lost. Safe to call repeatedly.
    std::uint64_t finish() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!fp_ && pending_open_ && !open_path_.empty()) {
            fp_ = std::fopen(open_path_.c_str(), "wb");
            if (fp_) { bytes_ = 0; flush_ring_locked(); }
        }
        pending_open_ = false;
        open_path_.clear();
        const std::uint64_t b = bytes_;
        if (fp_) { std::fclose(fp_); fp_ = nullptr; }
        return b;
    }

private:
    void ring_push_locked(const int16_t* pcm, std::size_t n) {
        if (ring_.capacity() < kPrerollSamples) ring_.reserve(kPrerollSamples);
        for (std::size_t i = 0; i < n; ++i) {
            if (ring_.size() < kPrerollSamples) ring_.push_back(pcm[i]);
            else { ring_[ring_head_] = pcm[i]; ring_head_ = (ring_head_ + 1) % kPrerollSamples; }
        }
    }
    void flush_ring_locked() {
        if (ring_.empty()) return;
        if (ring_.size() < kPrerollSamples) {
            bytes_ += std::fwrite(ring_.data(), sizeof(int16_t), ring_.size(), fp_) * sizeof(int16_t);
        } else {
            // Oldest sample sits at ring_head_; write [head..end) then [0..head).
            bytes_ += std::fwrite(ring_.data() + ring_head_, sizeof(int16_t),
                                  kPrerollSamples - ring_head_, fp_) * sizeof(int16_t);
            bytes_ += std::fwrite(ring_.data(), sizeof(int16_t), ring_head_, fp_) * sizeof(int16_t);
        }
    }

    mutable std::mutex mu_;
    std::FILE* fp_ = nullptr;          // guarded by mu_
    std::string open_path_;            // guarded by mu_
    std::uint64_t bytes_ = 0;          // guarded by mu_ (current segment)
    bool pending_open_ = false;        // guarded by mu_: begin() waiting for a feed to open
    std::int64_t keep_until_ms_ = 0;   // guarded by mu_: append while now <= this

    std::vector<int16_t> ring_;        // guarded by mu_ (touched only in feed)
    std::size_t ring_head_ = 0;        // guarded by mu_
};

// One stored capture's metadata, kept in a flat sidecar JSON next to the .s16
// (flat so json::parse_flat_object can read it back). alg:kid pairs are stored
// comma-joined in `algkids` for the same reason.
struct PendingMeta {
    std::string id;                 // base name, also the .s16 / .json stem
    std::uint64_t session = 0;
    std::int64_t started_ms = 0;
    std::int64_t ended_ms = 0;
    int sample_rate = PendingCapture::kRate;
    std::string mode;               // dsd-fme -f letter the stream used
    std::string protocol;           // human label (e.g. "p25p1")
    std::int64_t center_freq_hz = 0;
    std::string algkids;            // comma-joined "alg:kid" seen (hex, uppercased)
    std::uint64_t bytes = 0;
    bool complete = false;          // false until the session closed cleanly

    double seconds() const {
        return sample_rate > 0 ? static_cast<double>(bytes) / 2.0 / sample_rate : 0.0;
    }
    std::string sidecar_json() const {
        // Large integers go out as integers, not doubles: the JSON writer's
        // number formatter prints a double with the stream default (6 sig
        // figs), which would mangle epoch-ms timestamps and Hz frequencies.
        return json::Writer()
            .field("id", id)
            .field("session", static_cast<long long>(session))
            .field("started_ms", static_cast<long long>(started_ms))
            .field("ended_ms", static_cast<long long>(ended_ms))
            .field("sample_rate", static_cast<long long>(sample_rate))
            .field("mode", mode)
            .field("protocol", protocol)
            .field("center_freq_hz", static_cast<long long>(center_freq_hz))
            .field("algkids", algkids)
            .field("bytes", static_cast<long long>(bytes))
            .field("seconds", seconds())
            .field("complete", complete)
            .str();
    }
    static PendingMeta from_json(const std::string& s) {
        PendingMeta m;
        try {
            auto o = json::parse_flat_object(s);
            m.id = json::get_string(o, "id");
            m.session = static_cast<std::uint64_t>(json::get_number(o, "session"));
            m.started_ms = static_cast<std::int64_t>(json::get_number(o, "started_ms"));
            m.ended_ms = static_cast<std::int64_t>(json::get_number(o, "ended_ms"));
            m.sample_rate = static_cast<int>(json::get_number(o, "sample_rate", PendingCapture::kRate));
            m.mode = json::get_string(o, "mode");
            m.protocol = json::get_string(o, "protocol");
            m.center_freq_hz = static_cast<std::int64_t>(json::get_number(o, "center_freq_hz"));
            m.algkids = json::get_string(o, "algkids");
            m.bytes = static_cast<std::uint64_t>(json::get_number(o, "bytes"));
            m.complete = json::get_bool(o, "complete");
        } catch (...) { m.id.clear(); }
        return m;
    }
};

// Directory operations over the pending-capture folder. Each capture is a pair
// of files sharing a stem: "<id>.s16" (raw audio) and "<id>.json" (metadata).
struct PendingStore {
    // A capture id is a plain stem: letters, digits, '_' and '-' only (so it can
    // never escape the folder or name a hidden/relative path).
    static bool valid_id(const std::string& id) {
        if (id.empty() || id.size() > 128) return false;
        for (char c : id)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
        return true;
    }
    static std::string s16_path(const std::string& dir, const std::string& id) {
        return (std::filesystem::path(dir) / (id + ".s16")).string();
    }
    static std::string json_path(const std::string& dir, const std::string& id) {
        return (std::filesystem::path(dir) / (id + ".json")).string();
    }

    static bool write_sidecar(const std::string& dir, const PendingMeta& m) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream f(json_path(dir, m.id), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const std::string s = m.sidecar_json();
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
        return static_cast<bool>(f);
    }

    static std::vector<PendingMeta> list(const std::string& dir) {
        namespace fs = std::filesystem;
        std::vector<PendingMeta> out;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
            std::ifstream f(it->path(), std::ios::binary);
            if (!f) continue;
            std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            PendingMeta m = PendingMeta::from_json(s);
            if (m.id.empty() || !valid_id(m.id)) continue;
            // Trust the .s16's real size over the sidecar's (a crash mid-session
            // can leave the sidecar's byte count stale).
            const auto bytes = fs::file_size(fs::path(dir) / (m.id + ".s16"), ec);
            if (!ec) m.bytes = bytes;
            out.push_back(std::move(m));
        }
        std::sort(out.begin(), out.end(),
                  [](const PendingMeta& a, const PendingMeta& b) { return a.started_ms < b.started_ms; });
        return out;
    }

    // JSON array of captures for the explorer, newest first. algkids is split
    // back into a real array; a convenience "algkid_list" mirrors it as text.
    static std::string list_json(const std::string& dir) {
        auto v = list(dir);
        std::string out = "[";
        for (auto it = v.rbegin(); it != v.rend(); ++it) {
            if (it != v.rbegin()) out += ',';
            out += json::Writer()
                       .field("id", it->id)
                       .field("session", static_cast<long long>(it->session))
                       .field("started_ms", static_cast<long long>(it->started_ms))
                       .field("ended_ms", static_cast<long long>(it->ended_ms))
                       .field("mode", it->mode)
                       .field("protocol", it->protocol)
                       .field("center_freq_hz", static_cast<long long>(it->center_freq_hz))
                       .field("bytes", static_cast<long long>(it->bytes))
                       .field("seconds", it->seconds())
                       .field("complete", it->complete)
                       .raw_field("algkids", algkids_array(it->algkids))
                       .str();
        }
        out += "]";
        return out;
    }

    static std::pair<std::uint64_t, std::uint64_t> clear(const std::string& dir) {
        namespace fs = std::filesystem;
        std::uint64_t files = 0, bytes = 0;
        std::error_code ec;
        for (const auto& m : list(dir)) {
            bytes += remove_one(dir, m.id, files);
        }
        (void)ec;
        return {files, bytes};
    }

    static bool remove(const std::string& dir, const std::string& id) {
        if (!valid_id(id)) return false;
        std::uint64_t files = 0;
        remove_one(dir, id, files);
        return files > 0;
    }

    // Evict oldest captures until the folder is at or under max_bytes (0 = no
    // cap). Never removes a capture still being written (complete == false and
    // most recent); callers enforce after finishing one.
    static void enforce(const std::string& dir, std::uint64_t max_bytes) {
        if (max_bytes == 0) return;
        auto v = list(dir);  // oldest first
        std::uint64_t total = 0;
        for (const auto& m : v) total += m.bytes;
        std::uint64_t files = 0;
        for (const auto& m : v) {
            if (total <= max_bytes) break;
            std::uint64_t b = remove_one(dir, m.id, files);
            total = b <= total ? total - b : 0;
        }
    }

private:
    static std::uint64_t remove_one(const std::string& dir, const std::string& id, std::uint64_t& files) {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::uint64_t bytes = 0;
        const auto s16 = fs::path(s16_path(dir, id));
        const auto meta = fs::path(json_path(dir, id));
        const auto sz = fs::file_size(s16, ec);
        if (!ec) bytes = sz;
        if (fs::remove(s16, ec)) ++files;
        fs::remove(meta, ec);
        return bytes;
    }

    static std::string algkids_array(const std::string& csv) {
        std::string out = "[";
        std::size_t p = 0;
        bool first = true;
        while (p <= csv.size()) {
            std::size_t e = csv.find(',', p);
            if (e == std::string::npos) e = csv.size();
            const std::string tok = csv.substr(p, e - p);
            if (!tok.empty()) {
                if (!first) out += ',';
                first = false;
                out += '"';
                for (char c : tok) if (c != '"' && c != '\\') out += c;
                out += '"';
            }
            p = e + 1;
        }
        out += "]";
        return out;
    }
};

} // namespace dsdsrv
