// tetra_tetmon.cpp -- see tetra_tetmon.hpp for the format and rationale.

#include "tetra_tetmon.hpp"

#include <sstream>
#include <algorithm>
#include <cctype>

namespace dsdsrv {

namespace {

// Uppercase a short ASCII key for case-insensitive matching, leaving the
// stored field keys as they arrived. TETMON keys are emitted uppercase, but
// matching defensively costs nothing.
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Look up a field case-insensitively; return "" if absent.
std::string field(const std::map<std::string, std::string>& f, const char* key) {
    const std::string want = upper(key);
    for (const auto& kv : f)
        if (upper(kv.first) == want) return kv.second;
    return "";
}

} // namespace

TetmonMessage parse_tetmon_line(const std::string& line) {
    TetmonMessage msg;

    const std::string kBegin = "TETMON_begin";
    const std::string kEnd = "TETMON_end";
    const auto b = line.find(kBegin);
    if (b == std::string::npos) return msg; // not TETMON: valid stays false
    const auto e = line.find(kEnd, b + kBegin.size());
    if (e == std::string::npos) return msg;

    // Body is whatever sits between the markers.
    const std::string body = line.substr(b + kBegin.size(), e - (b + kBegin.size()));

    // Split on whitespace; each token is KEY:VALUE (values here never contain
    // spaces -- TETMON is a flat token list). A token without a ':' is
    // ignored rather than rejected, so a future stray word doesn't drop the
    // whole message.
    std::istringstream iss(body);
    std::string tok;
    while (iss >> tok) {
        const auto colon = tok.find(':');
        if (colon == std::string::npos) continue;
        std::string key = tok.substr(0, colon);
        std::string val = tok.substr(colon + 1);
        if (key.empty()) continue;
        msg.fields[key] = val;
    }

    msg.func = field(msg.fields, "FUNC");
    msg.valid = true; // well-formed wrapper; may legitimately carry no fields
    return msg;
}

DsdEvent tetmon_to_event(const TetmonMessage& msg, const std::string& raw_line) {
    DsdEvent ev;
    ev.raw_line = raw_line;

    if (!msg.valid) { ev.kind = "unknown"; return ev; }

    const std::string func = upper(msg.func);

    // PROVISIONAL func -> kind. The call-control funcs are the best current
    // reading from telive's docs; unknown funcs pass through as "unknown"
    // carrying their fields so nothing decoded is silently dropped.
    //
    // AFCVAL is deliberately classed "unknown": it is the fork's periodic
    // AFC/PHY status (confirmed verbatim in the source), carries no call
    // information, and is redundant here because we run our own CFO
    // correction. Mapping it to "unknown" means the standard
    // forward_unknown=false suppression drops this telemetry by default
    // (the same treatment dsd-fme's banner noise gets) without a second
    // flag; its afc/rx values are still preserved in `extra` for anyone who
    // forwards unknowns for debugging.
    if (func == "SETUPDEC" || func == "DSETUP" || func == "SETUP" ||
        func == "DSETUPDEC" || func == "CALLDEC") {
        ev.kind = "call";
    } else {
        ev.kind = "unknown"; // incl. AFCVAL diagnostics and any unrecognized func
    }

    // Ids. TETRA identifies parties by SSI; the transmitting/party SSI maps
    // to source_id. Group vs individual (GSSI/ISSI) disambiguation is
    // pending real output, so a dedicated GSSI field, when present, is what
    // we treat as the talkgroup.
    const std::string ssi = field(msg.fields, "SSI");
    const std::string gssi = field(msg.fields, "GSSI");
    if (!ssi.empty()) ev.source_id = ssi;
    if (!gssi.empty()) ev.talkgroup = gssi;

    // Structured extras kept as "; "-joined key=value tokens, mirroring the
    // dsd-fme extra convention. IDX is the traffic-channel index used to
    // associate later voice frames with this call; cid/nid locate the cell.
    auto add = [&](const char* label, const char* key) {
        const std::string v = field(msg.fields, key);
        if (v.empty()) return;
        if (!ev.extra.empty()) ev.extra += "; ";
        ev.extra += label;
        ev.extra += '=';
        ev.extra += v;
    };
    add("idx", "IDX");
    add("cid", "CID");
    add("nid", "NID");
    add("afc", "AFC"); // AFCVAL diagnostics: kept for debug, suppressed by default
    add("func", "FUNC");

    return ev;
}

DsdEvent classify_tetmon_line(const std::string& line) {
    return tetmon_to_event(parse_tetmon_line(line), line);
}

} // namespace dsdsrv
