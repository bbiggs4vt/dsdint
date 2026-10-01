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

#include "assoc_model.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
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
    std::string chain;                                   // "fm" | "tetra" | "pager" | ""
    bool active = false;                                 // a decode pipeline is running now
    std::chrono::system_clock::time_point connected;     // wall clock, for display
    std::chrono::steady_clock::time_point connected_mono;// for duration math
    // Every protocol this session has started, in first-use order (a client
    // may switch protocols with a new "start" on the same connection).
    std::vector<std::string> protocols_used;
    // Every protocol this session asked for, in first-request order --
    // including ones whose pipeline failed to start (not in protocols_used).
    std::vector<std::string> protocols_requested;
    std::chrono::steady_clock::time_point active_since;  // valid while active
};

// Cumulative, run-wide usage of one protocol (the resolved hint label, e.g.
// "dmr", "tetra", "pager-auto"). Kept for the whole server run -- the set of
// labels is fixed, so this can't grow without bound.
struct ProtocolUsage {
    std::string protocol;
    std::string chain;                                   // chain of the latest request
    std::uint64_t requests = 0;                          // "start" messages asking for it
    std::uint64_t starts = 0;                            // of those, pipelines that came up
    std::uint64_t sessions = 0;                          // distinct sessions that ran it
    std::size_t active_now = 0;                          // pipelines running it right now
    double decode_s = 0.0;                               // total pipeline run time, all sessions
    std::chrono::system_clock::time_point first_requested;
    std::chrono::system_clock::time_point last_requested;
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
    std::vector<std::string> protocols_used;             // every protocol it started, in order
    std::vector<std::string> protocols_requested;        // every protocol it asked for, in order
};

// One buffered outbound JSON frame, for the status page's log tab.
struct LogEntry {
    std::uint64_t session_id = 0;
    std::chrono::system_clock::time_point ts;
    std::string text;                                    // the JSON frame sent to the client
};

class ServerStats {
public:
    // history_limit: how many of the most-recent finished sessions to keep
    // for the history tab. log_limit: how many of the most-recent outbound
    // JSON frames to keep for the log tab. Both are in-memory only and reset
    // on restart.
    explicit ServerStats(std::size_t history_limit = 50, std::size_t log_limit = 300)
        : history_limit_(history_limit),
          log_limit_(log_limit),
          started_wall_(std::chrono::system_clock::now()),
          started_mono_(std::chrono::steady_clock::now()) {}

    // Record one outbound JSON frame (already serialized) for the log tab.
    // Oversized frames are truncated so one pathological line can't bloat the
    // ring buffer.
    void add_log(std::uint64_t session_id, const std::string& text) {
        if (log_limit_ == 0) return;
        std::lock_guard<std::mutex> lk(mu_);
        LogEntry e;
        e.session_id = session_id;
        e.ts = std::chrono::system_clock::now();
        constexpr std::size_t kMaxLine = 2048;
        e.text = text.size() > kMaxLine ? text.substr(0, kMaxLine) + "\xE2\x80\xA6" : text;
        log_.push_back(std::move(e));
        while (log_.size() > log_limit_) log_.pop_front();
    }

    // The buffered log, newest first.
    std::vector<LogEntry> log_snapshot() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<LogEntry> out;
        out.reserve(log_.size());
        for (auto it = log_.rbegin(); it != log_.rend(); ++it) out.push_back(*it);
        return out;
    }

    // Empty the log ring buffer (the status page's Clear button).
    void clear_log() {
        std::lock_guard<std::mutex> lk(mu_);
        log_.clear();
    }

    // ---- Global IQ-capture toggle (the status page's "Log IQ" checkbox) ----
    //
    // A single server-wide switch: when on, every active session captures its
    // raw IQ to a BLUE file, and any session that starts while it is on begins
    // capturing too. Each Session registers a control callback (keyed by its
    // id) that opens/closes its own capture on its strand; set_iq_logging flips
    // the flag and fans the new state out to all of them. The per-`start`
    // iq_log flag is independent -- this switch is the operator's live override.

    bool iq_logging() const {
        std::lock_guard<std::mutex> lk(mu_);
        return iq_logging_;
    }

    // Session registers (on connect) a callback that applies the on/off state
    // to its own capture. The callback is invoked off-lock; it must be cheap
    // and non-blocking (it posts to the session's strand).
    void register_iq_control(std::uint64_t id, std::function<void(bool)> fn) {
        std::lock_guard<std::mutex> lk(mu_);
        iq_controls_[id] = std::move(fn);
    }
    void unregister_iq_control(std::uint64_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        iq_controls_.erase(id);
    }

    // Flip the global switch and push the new state to every live session.
    // Returns the state now in effect.
    bool set_iq_logging(bool on) {
        std::vector<std::function<void(bool)>> controls;
        {
            std::lock_guard<std::mutex> lk(mu_);
            iq_logging_ = on;
            controls.reserve(iq_controls_.size());
            for (const auto& kv : iq_controls_) controls.push_back(kv.second);
        }
        for (auto& fn : controls) fn(on); // off-lock: each just posts to a strand
        return on;
    }

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

    // A client asked for a protocol (a "start" message, resolved to its
    // canonical label). Counted whether or not the pipeline then comes up, so
    // the protocols view shows everything the server was asked to do.
    void note_request(std::uint64_t id, const std::string& protocol, const std::string& chain) {
        if (protocol.empty()) return;
        std::lock_guard<std::mutex> lk(mu_);
        auto sit = sessions_.find(id);
        if (sit != sessions_.end()) {
            auto& req = sit->second.protocols_requested;
            if (std::find(req.begin(), req.end(), protocol) == req.end()) req.push_back(protocol);
        }
        auto& u = usage_[protocol];
        const auto now = std::chrono::system_clock::now();
        if (u.requests == 0) { u.protocol = protocol; u.first_requested = now; }
        ++u.requests;
        u.chain = chain;
        u.last_requested = now;
    }

    // A session's decode pipeline started (protocol/chain now known) or
    // stopped (active=false). Safe to call for an unknown id (e.g. after the
    // session already left) -- it's simply ignored.
    void set_protocol(std::uint64_t id, const std::string& protocol,
                      const std::string& chain, bool active) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return;
        SessionRow& row = it->second;
        if (row.active) close_active_locked(row); // restart without a stop: bank the old run
        if (!protocol.empty()) row.protocol = protocol;
        row.chain = chain;
        row.active = active;
        if (active && !row.protocol.empty() && row.protocol != "-") {
            auto& u = usage_[row.protocol];
            if (u.protocol.empty()) u.protocol = row.protocol;
            if (u.chain.empty()) u.chain = chain;
            ++u.starts;
            if (std::find(row.protocols_used.begin(), row.protocols_used.end(), row.protocol) ==
                row.protocols_used.end()) {
                row.protocols_used.push_back(row.protocol);
                ++u.sessions;
            }
            row.active_since = std::chrono::steady_clock::now();
        }
    }

    void set_pipeline_active(std::uint64_t id, bool active) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return;
        if (it->second.active && !active) close_active_locked(it->second);
        it->second.active = active;
    }

    // The network explorer's association model (/net). Has its own lock; fed
    // decoded events by every session, independent of the tables above.
    AssocModel& assoc() { return assoc_; }
    const AssocModel& assoc() const { return assoc_; }

    // The WebSocket client disconnected. Archive it into the bounded
    // history (most-recent kept) before dropping it from the live table.
    void remove_session(std::uint64_t id) {
        assoc_.remove_session(id); // its own lock; taken before (never inside) mu_
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return;
        if (it->second.active) close_active_locked(it->second);
        iq_controls_.erase(id); // session gone: drop its capture control
        FinishedRow f;
        f.id = it->second.id;
        f.remote = it->second.remote;
        f.protocol = it->second.protocol;
        f.chain = it->second.chain;
        f.connected = it->second.connected;
        f.ended = std::chrono::system_clock::now();
        f.duration_s = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - it->second.connected_mono).count();
        f.protocols_used = it->second.protocols_used;
        f.protocols_requested = it->second.protocols_requested;
        history_.push_back(std::move(f));
        while (history_.size() > history_limit_) history_.pop_front();
        sessions_.erase(it);
    }

    struct Snapshot {
        std::uint64_t total_sessions = 0;   // cumulative connections since start
        std::size_t current_sessions = 0;   // connected right now
        std::size_t active_pipelines = 0;   // currently decoding
        double uptime_s = 0.0;
        bool iq_logging = false;            // global IQ-capture switch state
        std::chrono::system_clock::time_point started;
        std::vector<SessionRow> rows;                 // sorted by id
        std::map<std::string, std::size_t> by_protocol; // active-pipeline count per protocol
        std::vector<FinishedRow> history;             // finished sessions, most recent first
        std::size_t log_lines = 0;                    // buffered outbound frames (for the tab count)
        // Every protocol requested since the server started, most recently
        // requested first. decode_s includes still-running pipelines.
        std::vector<ProtocolUsage> protocols;
    };

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lk(mu_);
        Snapshot s;
        s.total_sessions = total_sessions_;
        s.current_sessions = sessions_.size();
        s.iq_logging = iq_logging_;
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
        s.log_lines = log_.size();

        std::map<std::string, ProtocolUsage> usage = usage_;
        const auto now = std::chrono::steady_clock::now();
        for (const auto& kv : sessions_) {
            const SessionRow& r = kv.second;
            if (!r.active) continue;
            auto& u = usage[r.protocol];
            if (u.protocol.empty()) u.protocol = r.protocol;
            ++u.active_now;
            u.decode_s += std::chrono::duration<double>(now - r.active_since).count();
        }
        s.protocols.reserve(usage.size());
        for (auto& kv : usage) s.protocols.push_back(std::move(kv.second));
        std::stable_sort(s.protocols.begin(), s.protocols.end(),
                         [](const ProtocolUsage& a, const ProtocolUsage& b) {
                             return a.last_requested > b.last_requested;
                         });
        return s; // sessions_ is a std::map, so rows come out ordered by id
    }

private:
    // Bank a running pipeline's elapsed time into its protocol's total.
    // Caller holds mu_ and has checked row.active.
    void close_active_locked(const SessionRow& row) {
        auto& u = usage_[row.protocol];
        if (u.protocol.empty()) u.protocol = row.protocol;
        u.decode_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - row.active_since).count();
    }

    mutable std::mutex mu_;
    std::map<std::string, ProtocolUsage> usage_;
    std::map<std::uint64_t, SessionRow> sessions_;
    std::deque<FinishedRow> history_;
    std::deque<LogEntry> log_;
    bool iq_logging_ = false;                                   // global IQ-capture switch
    std::map<std::uint64_t, std::function<void(bool)>> iq_controls_; // per-session appliers
    AssocModel assoc_;                                          // network explorer model
    std::size_t history_limit_;
    std::size_t log_limit_;
    std::uint64_t last_id_ = 0;
    std::uint64_t total_sessions_ = 0;
    std::chrono::system_clock::time_point started_wall_;
    std::chrono::steady_clock::time_point started_mono_;
};

} // namespace dsdsrv
