// assoc_replay.hpp
//
// Replays a network-explorer recording (assoc_log.hpp) into an AssocModel:
// every begin / tune / event / end / remove / clear is re-applied with its recorded
// timestamp, so the model ends up exactly where the live one was. Shared by
// tools/net_replay.cpp and tests/test_assoc_log.cpp.

#pragma once

#include "assoc_model.hpp"

#include <zlib.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <string>

namespace dsdsrv {

struct ReplayResult {
    std::uint64_t lines = 0, events = 0, begins = 0, ends = 0, removes = 0, clears = 0, bad = 0;
    std::int64_t t_first = 0, t_last = 0;
    bool header = false, fresh = false, truncated = false;
    std::string start_model, stop_model;   // the recorded snapshots (model JSON), if present
    std::int64_t start_t = 0, stop_t = 0;
};

// Pull the "model" object out of a snapshot line, and its "t".
inline void replay_snapshot(const std::string& line, std::string& model, std::int64_t& t) {
    const std::size_t tp = line.find("\"t\":");
    if (tp != std::string::npos) t = std::stoll(line.substr(tp + 4));
    const std::size_t mp = line.find("\"model\":");
    if (mp != std::string::npos && line.size() > mp + 9) model = line.substr(mp + 8, line.size() - (mp + 8) - 1);
}

// The "families" part of a /net.json model (what the explorer shows), for
// comparing a replay with a recorded snapshot (version / rec status differ).
inline std::string families_of(const std::string& model) {
    const std::size_t p = model.find("\"families\":");
    return p == std::string::npos ? std::string() : model.substr(p);
}

// The recording's start time (its header line's "t"), or 0 if unreadable.
inline std::int64_t recording_start_time(const std::string& path) {
    gzFile f = gzopen(path.c_str(), "rb");
    if (!f) return 0;
    char line[512];
    std::int64_t t = 0;
    if (gzgets(f, line, sizeof line)) {
        std::map<std::string, std::string> k;
        if (assoclog::parse_line(line, k) && k["op"] == "header" && k.count("t")) t = std::stoll(k["t"]);
    }
    gzclose(f);
    return t;
}

// Called on each recorded event before it is ingested, with its stream's
// protocol label, to rewrite it (net-replay --reparse: re-classify the raw line).
using ReplayRewrite = std::function<void(DsdEvent&, const std::string& label)>;

// Replay `path` (gzip or plain JSON Lines) into `m`, stopping after time
// `until`. Returns false (with *err) if the file can't be read.
inline bool replay_log(const std::string& path, AssocModel& m, ReplayResult& r,
                       std::int64_t until = std::numeric_limits<std::int64_t>::max(),
                       std::string* err = nullptr, const ReplayRewrite& rewrite = nullptr) {
    gzFile f = gzopen(path.c_str(), "rb");          // also reads uncompressed files
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    std::string buf, line;
    char chunk[1 << 16];
    bool stop = false;
    std::map<std::uint64_t, std::string> labels;   // session -> protocol label (for `rewrite`)
    auto handle = [&](const std::string& l) {
        if (l.empty()) return;
        ++r.lines;
        if (l.compare(0, 17, "{\"op\":\"snapshot\",") == 0) {
            const bool is_stop = l.find("\"why\":\"stop\"") != std::string::npos;
            if (is_stop) replay_snapshot(l, r.stop_model, r.stop_t);
            else replay_snapshot(l, r.start_model, r.start_t);
            return;
        }
        std::map<std::string, std::string> k;
        try {
            if (!assoclog::parse_line(l, k)) { ++r.bad; return; }
            const std::string& op = k["op"];
            if (op == "truncated") { r.truncated = true; return; }
            const std::int64_t t = k.count("t") ? std::stoll(k["t"]) : 0;
            if (op == "header") {
                r.header = true;
                r.fresh = k["fresh"] == "true";
                // The replayed model is that server run's data: same identity
                // and span, so its export is recognised as such when merged.
                if (k.count("instance")) m.set_identity(k["instance"], k["name"]);
                m.set_since(k.count("since") ? std::stoll(k["since"]) : t);
                // The list size the live model used (recordings before this
                // field: the old fixed 400).
                m.set_max_calls(k.count("max_calls") ? std::stoull(k["max_calls"]) : 400);
                return;
            }
            if (t > until) { stop = true; return; }
            if (!r.t_first) r.t_first = t;
            r.t_last = t;
            const std::uint64_t sid = k.count("s") ? std::stoull(k["s"]) : 0;
            if (op == "ev") {
                DsdEvent e = assoclog::line_event(k);
                if (rewrite) rewrite(e, labels[sid]);
                m.ingest(sid, e, t);
                ++r.events;
            }
            else if (op == "begin") {
                labels[sid] = k["label"];
                m.begin_stream(sid, k["label"], t, k["family"], k.count("freq") ? std::stoll(k["freq"]) : 0);
                ++r.begins;
            }
            else if (op == "tune") m.retune_stream(sid, std::stoll(k["freq"]), t);
            else if (op == "keep") m.mark_keep(k["f"], std::stoull(k["c"]), k["on"] == "1");
            else if (op == "end") { m.end_stream(sid, t); ++r.ends; }
            else if (op == "remove") { m.remove_session(sid, t); ++r.removes; }
            else if (op == "clear") { m.clear(t); ++r.clears; }
            else ++r.bad;
        } catch (...) {
            ++r.bad;
        }
    };
    int n;
    while (!stop && (n = gzread(f, chunk, sizeof chunk)) > 0) {
        buf.append(chunk, static_cast<std::size_t>(n));
        std::size_t pos, start = 0;
        while (!stop && (pos = buf.find('\n', start)) != std::string::npos) {
            handle(buf.substr(start, pos - start));
            start = pos + 1;
        }
        buf.erase(0, start);
    }
    if (!stop && !buf.empty()) handle(buf);         // last line without a newline (cut-off file)
    gzclose(f);
    return true;
}

} // namespace dsdsrv
