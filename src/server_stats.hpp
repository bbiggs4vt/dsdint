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
#include <deque>
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

// One row in the session-history table: a client that has disconnected.
struct FinishedRow {
    std::uint64_t id = 0;
    std::string remote;
    std::string protocol = "-";
    std::string chain;
    std::chrono::system_clock::time_point connected;     // when it connected
    std::chrono::system_clock::time_point ended;         // when it disconnected
    double duration_s = 0.0;                             // total connected lifetime
};

class ServerStats {
public:
    // history_limit: how many of the most-recent finished sessions to keep
    // for the history tab (in memory only; reset on restart).
    explicit ServerStats(std::size_t history_limit = 50)
        : history_limit_(history_limit),
          started_wall_(std::chrono::system_clock::now()),
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

    // The WebSocket client disconnected. Archive it into the bounded
    // history (most-recent kept) before dropping it from the live table.
    void remove_session(std::uint64_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return;
        FinishedRow f;
        f.id = it->second.id;
        f.remote = it->second.remote;
        f.protocol = it->second.protocol;
        f.chain = it->second.chain;
        f.connected = it->second.connected;
        f.ended = std::chrono::system_clock::now();
        f.duration_s = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - it->second.connected_mono).count();
        history_.push_back(std::move(f));
        while (history_.size() > history_limit_) history_.pop_front();
        sessions_.erase(it);
    }

    struct Snapshot {
        std::uint64_t total_sessions = 0;   // cumulative connections since start
        std::size_t current_sessions = 0;   // connected right now
        std::size_t active_pipelines = 0;   // currently decoding
        double uptime_s = 0.0;
        std::chrono::system_clock::time_point started;
        std::vector<SessionRow> rows;                 // sorted by id
        std::map<std::string, std::size_t> by_protocol; // active-pipeline count per protocol
        std::vector<FinishedRow> history;             // finished sessions, most recent first
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
        // History newest-first (history_ keeps oldest at the front).
        s.history.reserve(history_.size());
        for (auto it = history_.rbegin(); it != history_.rend(); ++it) s.history.push_back(*it);
        return s; // sessions_ is a std::map, so rows come out ordered by id
    }

private:
    mutable std::mutex mu_;
    std::map<std::uint64_t, SessionRow> sessions_;
    std::deque<FinishedRow> history_;
    std::size_t history_limit_;
    std::uint64_t last_id_ = 0;
    std::uint64_t total_sessions_ = 0;
    std::chrono::system_clock::time_point started_wall_;
    std::chrono::steady_clock::time_point started_mono_;
};

} // namespace dsdsrv
