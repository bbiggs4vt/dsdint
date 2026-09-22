// server_stats.hpp
//
// Process-wide, thread-safe registry of connected client sessions, plus
// the renderers for the server's status page (HTML for a browser, JSON for
// tooling). One ServerStats instance is owned by the Server and shared with
// every Session; the status HTTP endpoint reads a consistent snapshot of it.
//
// "Session" here means an accepted WebSocket client connection -- the thing
// a browser hitting the status URL wants a count of. A plain HTTP GET for
// the status page is NOT a session and is never registered.
//
// Concurrency: every mutating call takes one short mutex; sessions register
// from their own strand threads and the decode/reader threads never touch
// this. snapshot() copies out under the same lock so a render can't observe
// a half-updated table.

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dsdsrv {

// One row in the live-session table.
struct SessionRow {
    std::uint64_t id = 0;
    std::string remote;                                  // peer "ip:port" (best effort)
    std::string protocol = "-";                          // decode protocol once started, else "-"
    std::string chain;                                   // "fm" | "tetra" | ""
    bool active = false;                                 // a decode pipeline is running now
    std::chrono::system_clock::time_point connected;     // wall clock, for display
    std::chrono::steady_clock::time_point connected_mono;// for duration math
};

class ServerStats {
public:
    ServerStats()
        : started_wall_(std::chrono::system_clock::now()),
          started_mono_(std::chrono::steady_clock::now()) {}

    // A WebSocket client connected. Returns its stable id; records it in the
    // live table and bumps the cumulative total.
    std::uint64_t add_session(const std::string& remote) {
        std::lock_guard<std::mutex> lk(mu_);
        std::uint64_t id = ++last_id_;
        SessionRow row;
        row.id = id;
        row.remote = remote;
        row.connected = std::chrono::system_clock::now();
        row.connected_mono = std::chrono::steady_clock::now();
        sessions_[id] = std::move(row);
        ++total_sessions_;
        return id;
    }

    // A session's decode pipeline started (protocol/chain now known) or
    // stopped (active=false). Safe to call for an unknown id (e.g. after the
    // session already left) -- it's simply ignored.
    void set_protocol(std::uint64_t id, const std::string& protocol,
                      const std::string& chain, bool active) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return;
        if (!protocol.empty()) it->second.protocol = protocol;
        it->second.chain = chain;
        it->second.active = active;
    }

    void set_pipeline_active(std::uint64_t id, bool active) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return;
        it->second.active = active;
    }

    // The WebSocket client disconnected.
    void remove_session(std::uint64_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        sessions_.erase(id);
    }

    struct Snapshot {
        std::uint64_t total_sessions = 0;   // cumulative connections since start
        std::size_t current_sessions = 0;   // connected right now
        std::size_t active_pipelines = 0;   // currently decoding
        double uptime_s = 0.0;
        std::chrono::system_clock::time_point started;
        std::vector<SessionRow> rows;                 // sorted by id
        std::map<std::string, std::size_t> by_protocol; // active-pipeline count per protocol
    };

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lk(mu_);
        Snapshot s;
        s.total_sessions = total_sessions_;
        s.current_sessions = sessions_.size();
        s.started = started_wall_;
        s.uptime_s = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - started_mono_).count();
        s.rows.reserve(sessions_.size());
        for (const auto& kv : sessions_) {
            s.rows.push_back(kv.second);
            if (kv.second.active) {
                ++s.active_pipelines;
                ++s.by_protocol[kv.second.protocol.empty() ? "-" : kv.second.protocol];
            }
        }
        return s; // sessions_ is a std::map, so rows come out ordered by id
    }

private:
    mutable std::mutex mu_;
    std::map<std::uint64_t, SessionRow> sessions_;
    std::uint64_t last_id_ = 0;
    std::uint64_t total_sessions_ = 0;
    std::chrono::system_clock::time_point started_wall_;
    std::chrono::steady_clock::time_point started_mono_;
};

} // namespace dsdsrv
