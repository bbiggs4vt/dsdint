// net_replay.cpp -- replay a network-explorer recording offline.
//
//   net-replay <net_YYYYMMDD_HHMMSS.jsonl.gz> [--at <ms | +seconds>] [--json <file|->]
//                                             [--export <file.json>] [--graphml <file.graphml>]
//                                             [--reparse]
//
// Re-applies every recorded input to a fresh AssocModel with its recorded
// timestamp, then prints a summary of what the explorer would show. With no
// --at, a recording that started "fresh" is checked against the "stop"
// snapshot it ends with: they must match exactly (that proves the replay is
// faithful, so a fix to the model can be re-run on the same data). --at stops
// at a moment in the recording (epoch ms, or +seconds after the first input),
// e.g. when the problem was seen; +N means N seconds after the recording started. --json writes the replayed model (the same
// JSON as GET /net.json) to a file, or "-" for stdout. --export writes an
// explorer export (re-openable in the explorer's "Open..."), --graphml the
// association graph for Gephi / Cytoscape / yEd -- both as of the replayed time.
// --reparse re-classifies each dsd-fme event from its recorded raw line with
// this build's parser instead of using the fields it was recorded with, to see
// what a parser fix makes of old data (the replay then won't match the stop
// snapshot, which was made with the old parser).

#include "assoc_replay.hpp"
#include "dsd_process.hpp"

#include <cstdio>
#include <iostream>
#include <map>
#include <string>

using namespace dsdsrv;

namespace {

// --reparse: classify the event's raw line again. Only dsd-fme streams (TETRA
// and pager lines come from other parsers), and not reassembled data messages
// (built from several lines). Fields the classifier can't know from one line
// alone -- the slot carried from the last burst, a CRC flag -- are kept.
void reparse_event(DsdEvent& e, const std::string& label) {
    const std::string fam = assoc_family(label);
    if (e.raw_line.empty() || fam.empty() || fam == "tetra" || e.kind == "message") return;
    DsdEvent n = classify_dsd_fme_line(e.raw_line);
    if (n.slot.empty()) n.slot = e.slot;
    if (n.crc_error.empty()) n.crc_error = e.crc_error;
    e = std::move(n);
}

std::string utc(std::int64_t ms) {
    if (!ms) return "-";
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char b[40];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(b) + "." + std::to_string(1000 + ms % 1000).substr(1) + "Z";
}

// Tiny scan of the model JSON for a summary line per family (no full parser
// needed: counts of the four arrays and each network's label/calls).
std::size_t count_objects(const std::string& s, std::size_t from, std::size_t to) {
    std::size_t n = 0;
    int depth = 0;
    bool in_str = false;
    for (std::size_t i = from; i < to; ++i) {
        char c = s[i];
        if (in_str) { if (c == '\\') ++i; else if (c == '"') in_str = false; continue; }
        if (c == '"') in_str = true;
        else if (c == '{') { if (depth == 0) ++n; ++depth; }
        else if (c == '}') --depth;
    }
    return n;
}
std::size_t array_end(const std::string& s, std::size_t open) {
    int depth = 0;
    bool in_str = false;
    for (std::size_t i = open; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) { if (c == '\\') ++i; else if (c == '"') in_str = false; continue; }
        if (c == '"') in_str = true;
        else if (c == '[' || c == '{') ++depth;
        else if (c == ']' || c == '}') { if (--depth == 0) return i; }
    }
    return s.size();
}

void summarize(const std::string& model) {
    static const char* fams[] = {"dmr", "p25", "nxdn", "tetra", "dpmr", "dstar", "ysf", "edacs", "x2tdma"};
    bool any = false;
    for (const char* f : fams) {
        const std::string key = std::string("\"") + f + "\":{\"networks\":[";
        std::size_t p = model.find(key);
        if (p == std::string::npos) continue;
        any = true;
        std::size_t nets_open = p + key.size() - 1, nets_close = array_end(model, nets_open);
        auto section = [&](const char* name, std::size_t from) -> std::pair<std::size_t, std::size_t> {
            std::size_t q = model.find(std::string("\"") + name + "\":[", from);
            if (q == std::string::npos) return {0, 0};
            std::size_t o = model.find('[', q);
            return {o, array_end(model, o)};
        };
        auto tg = section("talkgroups", nets_close), ra = section("radios", nets_close), ca = section("calls", nets_close);
        std::printf("  %-6s networks %zu, talkgroups %zu, radios %zu, calls %zu\n", f,
                    count_objects(model, nets_open + 1, nets_close), count_objects(model, tg.first + 1, tg.second),
                    count_objects(model, ra.first + 1, ra.second), count_objects(model, ca.first + 1, ca.second));
        // each network's label + calls
        std::size_t i = nets_open;
        while ((i = model.find("\"label\":\"", i)) != std::string::npos && i < nets_close) {
            std::size_t ls = i + 9, le = model.find('"', ls);
            std::size_t cs = model.find("\"confidence\":\"", le) + 14, ce = model.find('"', cs);
            std::size_t ks = model.find("\"calls\":", ce) + 8;
            std::printf("           - %s [%s] calls %s\n", model.substr(ls, le - ls).c_str(),
                        model.substr(cs, ce - cs).c_str(), model.substr(ks, model.find_first_of(",}", ks) - ks).c_str());
            i = le;
        }
    }
    if (!any) std::printf("  (nothing -- no stream decoded any traffic)\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <recording.jsonl.gz> [--at <ms|+seconds>] [--json <file|->]\n"
                             "       [--export <file.json>] [--graphml <file.graphml>] [--reparse]\n", argv[0]);
        return 2;
    }
    std::string path = argv[1], json_out, at, export_out, graphml_out;
    bool reparse = false;
    for (int i = 2; i < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--reparse") { reparse = true; --i; continue; }
        if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); return 2; }
        if (a == "--at") at = argv[i + 1];
        else if (a == "--json") json_out = argv[i + 1];
        else if (a == "--export") export_out = argv[i + 1];
        else if (a == "--graphml") graphml_out = argv[i + 1];
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    // --at +N is N seconds after the recording started (its header's time).
    std::int64_t until = std::numeric_limits<std::int64_t>::max();
    if (!at.empty()) {
        if (at[0] == '+') {
            const std::int64_t t0 = recording_start_time(path);
            if (!t0) { std::fprintf(stderr, "cannot read the recording's start time from %s\n", path.c_str()); return 1; }
            until = t0 + static_cast<std::int64_t>(std::stod(at.substr(1)) * 1000.0);
        } else {
            until = std::stoll(at);
        }
    }

    AssocModel m;
    ReplayResult r;
    std::string err;
    if (!replay_log(path, m, r, until, &err, reparse ? ReplayRewrite(reparse_event) : ReplayRewrite())) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    const std::int64_t when = at.empty() ? (r.stop_t ? r.stop_t : r.t_last) : until;
    const std::string model = m.to_json(when);

    std::printf("recording   %s\n", path.c_str());
    std::printf("span        %s .. %s (%.1f s)%s\n", utc(r.t_first).c_str(), utc(r.t_last).c_str(),
                (r.t_last - r.t_first) / 1000.0, r.truncated ? "  [TRUNCATED at size cap]" : "");
    std::printf("inputs      %llu lines: %llu events, %llu stream starts, %llu ends, %llu disconnects, %llu clears%s\n",
                (unsigned long long)r.lines, (unsigned long long)r.events, (unsigned long long)r.begins,
                (unsigned long long)r.ends, (unsigned long long)r.removes, (unsigned long long)r.clears,
                r.bad ? (" (" + std::to_string(r.bad) + " unreadable)").c_str() : "");
    std::printf("state at    %s%s\n", utc(when).c_str(), at.empty() ? " (end of recording)" : " (--at)");
    summarize(model);

    int rc = 0;
    if (at.empty()) {
        if (!r.header) {
            std::printf("check       no header line -- not a recording?\n");
        } else if (!r.fresh) {
            std::printf("check       skipped: recording began with data already in the explorer, so earlier\n"
                        "            history isn't in the file (the 'start' snapshot shows it)\n");
        } else if (r.stop_model.empty()) {
            std::printf("check       skipped: no 'stop' snapshot (recording was not stopped cleanly)\n");
        } else if (r.truncated) {
            std::printf("check       skipped: the recording hit its size cap\n");
        } else if (reparse) {
            std::printf("check       skipped: --reparse (the recorded snapshot used the old parser)\n");
        } else if (families_of(model) == families_of(r.stop_model)) {
            std::printf("check       OK -- replay reproduces the recorded explorer state exactly\n");
        } else {
            const std::string a = families_of(model), b = families_of(r.stop_model);
            std::size_t d = 0;
            while (d < a.size() && d < b.size() && a[d] == b[d]) ++d;
            std::printf("check       DIFFERS from the recorded 'stop' snapshot at offset %zu:\n"
                        "              replay:   ...%s...\n              recorded: ...%s...\n",
                        d, a.substr(d > 60 ? d - 60 : 0, 160).c_str(), b.substr(d > 60 ? d - 60 : 0, 160).c_str());
            rc = 3;
        }
    }
    auto write_file = [](const std::string& f, const std::string& body, const char* what) {
        FILE* o = std::fopen(f.c_str(), "w");
        if (!o) { std::fprintf(stderr, "cannot write %s\n", f.c_str()); return false; }
        std::fputs(body.c_str(), o);
        std::fclose(o);
        std::printf("%-11s %s\n", what, f.c_str());
        return true;
    };
    if (!export_out.empty() && !write_file(export_out, m.to_export_json(when), "export")) return 1;
    if (!graphml_out.empty() && !write_file(graphml_out, m.to_graphml(when), "graphml")) return 1;
    if (!json_out.empty()) {
        if (json_out == "-") std::cout << model << "\n";
        else {
            FILE* o = std::fopen(json_out.c_str(), "w");
            if (!o) { std::fprintf(stderr, "cannot write %s\n", json_out.c_str()); return 1; }
            std::fputs(model.c_str(), o);
            std::fclose(o);
            std::printf("model json  %s\n", json_out.c_str());
        }
    }
    return rc;
}
