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

    // func -> kind. Pinned against a real capture (this fork's tetra-rx run on
    // an off-air UK downlink) and cross-checked against the fork's source for
    // the funcs that capture didn't exercise:
    //   call control  DSETUPDEC / DCONNECTDEC / DRELEASEDEC / DTXGRANTDEC -> call
    //   network/sync  NETINFO1 / FREQINFO1 / FREQINFO2                    -> sync
    //   everything else (BURST markers, ENCINFO1, AFCVAL diagnostics, the
    //   SDS/DSTATUS text funcs) -> unknown, i.e. suppressed by default but
    //   still carrying their fields for a forward_unknown consumer.
    if (func == "DSETUPDEC" || func == "DCONNECTDEC" || func == "DRELEASEDEC" ||
        func == "DTXGRANTDEC") {
        ev.kind = "call";
    } else if (func == "NETINFO1" || func == "FREQINFO1" || func == "FREQINFO2") {
        ev.kind = "sync";
    } else {
        ev.kind = "unknown"; // BURST / ENCINFO1 / AFCVAL / SDS / DSTATUS / unrecognized
    }

    // Ids. TETRA identifies parties by SSI; the calling party SSI maps to
    // source_id and the called party (SSI2, a group GSSI or an individual)
    // to talkgroup.
    const std::string ssi = field(msg.fields, "SSI");
    const std::string ssi2 = field(msg.fields, "SSI2");
    if (!ssi.empty()) ev.source_id = ssi;
    if (!ssi2.empty()) ev.talkgroup = ssi2;

    // TETRA colour code (NETINFO1 CCODE) -> the shared color_code field, the
    // same slot DMR/dPMR use for their access code.
    const std::string ccode = field(msg.fields, "CCODE");
    if (!ccode.empty()) ev.color_code = ccode;

    // Structured extras kept as "; "-joined key=value tokens, mirroring the
    // dsd-fme extra convention. IDX associates later voice with this call;
    // cid/nid/la locate the cell; mcc/mnc are the network id (hex, as the
    // fork emits them); dlf/ulf are the down/uplink frequencies (Hz).
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
    add("mcc", "MCC");   // hex, e.g. 00ea = 234
    add("mnc", "MNC");   // hex, e.g. 004e = 78
    add("la", "LA");
    add("dlf", "DLF");
    add("ulf", "ULF");
    add("crypt", "CRYPT");
    add("status", "STATUS");
    add("afc", "AFC"); // AFCVAL diagnostics: kept for debug, suppressed by default
    add("func", "FUNC");

    return ev;
}

DsdEvent classify_tetmon_line(const std::string& line) {
    // Some datagrams carry a binary payload appended after TETMON_end; keep
    // only the text wrapper as `raw` so the event stays clean printable text.
    const std::string kEnd = "TETMON_end";
    const auto e = line.find(kEnd);
    const std::string clean =
        (e == std::string::npos) ? line : line.substr(0, e + kEnd.size());
    return tetmon_to_event(parse_tetmon_line(line), clean);
}

} // namespace dsdsrv
