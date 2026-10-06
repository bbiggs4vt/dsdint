// assoc_audio.hpp
//
// Per-call voice audio for the network explorer: one WAV file (8000 Hz,
// mono, 16-bit PCM) per call, written as the call's decoded audio arrives
// (AssocModel decides which call a block of audio belongs to). Off unless
// enabled (DSD_NET_AUDIO=1 or the explorer's Audio switch).
//
// Disk use is bounded: the store keeps an index of its files (including ones
// left by earlier runs in the same directory) and deletes the oldest finished
// ones when the total passes the cap (DSD_NET_AUDIO_MAX_MB, default 1024 MB --
// about 18 hours of speech) or, with DSD_NET_AUDIO_MAX_AGE_H, when they are
// older than that. Files being written are never deleted.
//
// Only names the store created (or found in its directory with its naming
// pattern) are ever served -- path_for() rejects anything else -- so the HTTP
// endpoint can't be pointed at other files.
//
// Not thread-safe on its own: AssocModel calls it under its mutex.

#pragma once

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace dsdsrv {

class CallAudioStore {
public:
    static constexpr int kRate = 8000;

    struct Status {
        bool on = false;
        std::string dir;
        std::uint64_t bytes = 0, cap_bytes = 0, files = 0, open = 0;
        std::int64_t max_age_ms = 0;
    };

    ~CallAudioStore() { disable(); }

    // Start recording into `dir` (created if missing). Existing files there
    // that look like ours join the index, so the cap covers earlier runs too.
    bool enable(const std::string& dir, std::uint64_t cap_bytes, std::int64_t max_age_ms, std::int64_t now) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (!std::filesystem::is_directory(dir, ec)) return false;
        dir_ = dir;
        cap_ = cap_bytes;
        max_age_ = max_age_ms;
        index_.clear();
        total_ = 0;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::string name = e.path().filename().string();
            if (!valid_name(name) || !e.is_regular_file(ec)) continue;
            struct stat st {};
            if (::stat(e.path().c_str(), &st) != 0) continue;
            index_.push_back({name, static_cast<std::uint64_t>(st.st_size),
                              static_cast<std::int64_t>(st.st_mtime) * 1000});
            total_ += static_cast<std::uint64_t>(st.st_size);
        }
        std::sort(index_.begin(), index_.end(), [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });
        on_ = true;
        enforce(now);
        return true;
    }
    // Stop recording: finish every open file (they stay playable).
    void disable() {
        for (auto& kv : open_) close_file(kv.second);
        open_.clear();
        on_ = false;
    }
    bool on() const { return on_; }

    // Create the file for a call. Returns false (nothing created) on failure.
    bool open(const std::string& name, std::int64_t now) {
        if (!on_ || !valid_name(name) || open_.count(name)) return false;
        Writer w;
        w.f = std::fopen((dir_ + "/" + name).c_str(), "wb");
        if (!w.f) return false;
        write_header(w.f, 0);
        w.patched = now;
        open_[name] = w;
        index_.push_back({name, 44, now});
        total_ += 44;
        return true;
    }
    void append(const std::string& name, const int16_t* pcm, std::size_t n, std::int64_t now) {
        auto it = open_.find(name);
        if (it == open_.end() || !n) return;
        Writer& w = it->second;
        std::fwrite(pcm, sizeof(int16_t), n, w.f);       // little-endian hosts only (as BlueFileWriter)
        w.samples += n;
        total_ += n * sizeof(int16_t);
        if (Entry* e = find(name)) { e->bytes += n * sizeof(int16_t); e->mtime = now; }
        if (now - w.patched >= 1000) patch(w, now);      // keep the file playable while it grows
        if (total_ > cap_) enforce(now);
    }
    // The call ended: make the file final.
    void finish(const std::string& name) {
        auto it = open_.find(name);
        if (it == open_.end()) return;
        close_file(it->second);
        open_.erase(it);
    }
    // Drop a call's audio (e.g. it turned out to be encrypted).
    void discard(const std::string& name) {
        auto it = open_.find(name);
        if (it != open_.end()) { close_file(it->second); open_.erase(it); }
        remove_entry(name);
    }
    // Delete every finished file of ours (the explorer's Clear, when asked):
    // the files this store indexed -- this run's and earlier runs' in its
    // folder. A call still being recorded keeps its file. Returns how many
    // files and bytes went.
    std::pair<std::uint64_t, std::uint64_t> remove_all() {
        std::uint64_t n = 0, bytes = 0;
        for (auto it = index_.begin(); it != index_.end();) {
            if (open_.count(it->name)) { ++it; continue; }
            std::error_code ec;
            std::filesystem::remove(dir_ + "/" + it->name, ec);
            ++n; bytes += it->bytes;
            total_ -= std::min(total_, it->bytes);
            it = index_.erase(it);
        }
        return { n, bytes };
    }
    std::uint64_t samples(const std::string& name) const {
        auto it = open_.find(name);
        return it == open_.end() ? 0 : it->second.samples;
    }
    bool has(const std::string& name) const { return valid_name(name) && const_cast<CallAudioStore*>(this)->find(name); }

    // Path of a known file, flushed and with its header current; "" if the
    // name isn't one of ours.
    std::string path_for(const std::string& name, std::int64_t now) {
        if (!valid_name(name) || !find(name)) return std::string();
        auto it = open_.find(name);
        if (it != open_.end()) patch(it->second, now);
        return dir_ + "/" + name;
    }

    Status status() const {
        Status s;
        s.on = on_;
        s.dir = dir_;
        s.bytes = total_;
        s.cap_bytes = cap_;
        s.files = index_.size();
        s.open = open_.size();
        s.max_age_ms = max_age_;
        return s;
    }

    // "call_<digits>_<word>.wav" with a safe alphabet: what make_name()
    // produces, and all path_for() will accept.
    static bool valid_name(const std::string& n) {
        if (n.size() < 10 || n.size() > 96 || n.compare(0, 5, "call_") != 0 || n.compare(n.size() - 4, 4, ".wav") != 0)
            return false;
        for (char c : n)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
        return n.find("..") == std::string::npos;
    }
    static std::string make_name(std::int64_t start_ms, const std::string& instance, std::uint64_t call_id) {
        std::string inst;
        for (char c : instance) if (std::isalnum(static_cast<unsigned char>(c)) && inst.size() < 8) inst += c;
        char b[96];
        std::snprintf(b, sizeof b, "call_%lld_%s_%llu.wav", static_cast<long long>(start_ms),
                      inst.empty() ? "x" : inst.c_str(), static_cast<unsigned long long>(call_id));
        return b;
    }

private:
    struct Writer {
        std::FILE* f = nullptr;
        std::uint64_t samples = 0;
        std::int64_t patched = 0;
    };
    struct Entry {
        std::string name;
        std::uint64_t bytes = 0;
        std::int64_t mtime = 0;
    };

    static void put32(unsigned char* p, std::uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
    static void put16(unsigned char* p, std::uint16_t v) { p[0] = v; p[1] = v >> 8; }
    static void write_header(std::FILE* f, std::uint64_t samples) {
        const std::uint32_t data = static_cast<std::uint32_t>(std::min<std::uint64_t>(samples * 2, 0xFFFFFFF0u));
        unsigned char h[44];
        std::memcpy(h, "RIFF", 4); put32(h + 4, 36 + data); std::memcpy(h + 8, "WAVEfmt ", 8);
        put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 1); put32(h + 24, kRate); put32(h + 28, kRate * 2);
        put16(h + 32, 2); put16(h + 34, 16); std::memcpy(h + 36, "data", 4); put32(h + 40, data);
        std::fwrite(h, 1, sizeof h, f);
    }
    void patch(Writer& w, std::int64_t now) {
        if (!w.f) return;
        const long pos = std::ftell(w.f);
        std::fseek(w.f, 0, SEEK_SET);
        write_header(w.f, w.samples);
        std::fseek(w.f, pos, SEEK_SET);
        std::fflush(w.f);
        w.patched = now;
    }
    void close_file(Writer& w) {
        if (!w.f) return;
        patch(w, w.patched);
        std::fclose(w.f);
        w.f = nullptr;
    }
    Entry* find(const std::string& name) {
        for (auto& e : index_) if (e.name == name) return &e;
        return nullptr;
    }
    void remove_entry(const std::string& name) {
        for (auto it = index_.begin(); it != index_.end(); ++it)
            if (it->name == name) {
                total_ -= std::min(total_, it->bytes);
                std::error_code ec;
                std::filesystem::remove(dir_ + "/" + name, ec);
                index_.erase(it);
                return;
            }
    }
    // Oldest finished files go first, until under the cap and the age limit.
    void enforce(std::int64_t now) {
        for (auto it = index_.begin(); it != index_.end();) {
            const bool over = total_ > cap_ || (max_age_ > 0 && now - it->mtime > max_age_);
            if (!over) break;
            if (open_.count(it->name)) { ++it; continue; }
            total_ -= std::min(total_, it->bytes);
            std::error_code ec;
            std::filesystem::remove(dir_ + "/" + it->name, ec);
            it = index_.erase(it);
        }
    }

    bool on_ = false;
    std::string dir_;
    std::uint64_t cap_ = 0, total_ = 0;
    std::int64_t max_age_ = 0;
    std::deque<Entry> index_;                       // oldest first
    std::map<std::string, Writer> open_;
};

} // namespace dsdsrv
