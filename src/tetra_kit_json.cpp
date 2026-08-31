// tetra_kit_json.cpp -- see tetra_kit_json.hpp for the format and rationale.

#include "tetra_kit_json.hpp"

#include <cctype>

namespace dsdsrv {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string field(const std::map<std::string, std::string>& f, const char* key) {
    auto it = f.find(key);
    return it == f.end() ? std::string() : it->second;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// Parse a JSON string literal starting at s[i]=='"'; advance i past the
// closing quote; return the unescaped contents (minimal unescaping).
std::string parse_string(const std::string& s, std::size_t& i) {
    std::string out;
    ++i; // opening quote
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                default: out += c; break; // incl. \uXXXX left as-is-ish (rare here)
            }
            i += 2;
        } else {
            out += s[i];
            ++i;
        }
    }
    if (i < s.size()) ++i; // closing quote
    return out;
}

// Skip a balanced {...} or [...] starting at s[i] (an opening bracket),
// respecting quoted strings; advance i past the matching close.
void skip_container(const std::string& s, std::size_t& i) {
    const char open = s[i];
    const char close = (open == '{') ? '}' : ']';
    int depth = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') { parse_string(s, i); continue; }
        if (c == open) ++depth;
        else if (c == close) { --depth; if (depth == 0) { ++i; return; } }
        ++i;
    }
}

// Read a primitive scalar (number/true/false/null) as text; advance i.
std::string parse_scalar(const std::string& s, std::size_t& i) {
    std::size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           !std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    std::string tok = s.substr(start, i - start);
    // Trim a trailing decimal that RapidJSON won't emit, but be safe.
    return tok;
}

} // namespace

TetraKitReport parse_tetrakit_json(const std::string& line) {
    TetraKitReport rep;
    std::size_t i = 0;
    auto ws = [&] { while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i; };

    ws();
    if (i >= line.size() || line[i] != '{') return rep; // not a JSON object
    ++i;
    rep.valid = true;
    ws();
    if (i < line.size() && line[i] == '}') return rep; // empty object

    while (i < line.size()) {
        ws();
        if (i >= line.size() || line[i] != '"') break; // malformed; keep what we have
        std::string key = parse_string(line, i);
        ws();
        if (i >= line.size() || line[i] != ':') break;
        ++i;
        ws();
        if (i >= line.size()) break;

        if (line[i] == '"') {
            rep.fields[key] = parse_string(line, i);
        } else if (line[i] == '{' || line[i] == '[') {
            skip_container(line, i); // nested value: skip, don't store
        } else {
            rep.fields[key] = parse_scalar(line, i);
        }

        ws();
        if (i < line.size() && line[i] == ',') { ++i; continue; }
        if (i < line.size() && line[i] == '}') { ++i; break; }
        // otherwise malformed: stop, keep parsed fields
        break;
    }
    return rep;
}

DsdEvent tetrakit_to_event(const TetraKitReport& rep, const std::string& raw_line) {
    DsdEvent ev;
    ev.raw_line = raw_line;

    if (!rep.valid) { ev.kind = "unknown"; return ev; }

    const std::string service = upper(field(rep.fields, "service"));
    const std::string pdu = upper(field(rep.fields, "pdu"));

    // service/pdu -> kind. Pinned against real tetra-kit output from an
    // off-air UK TETRA downlink (see tests): the traffic channel is
    // service "UPLANE" (no hyphen) / pdu "TCH_S"; MAC-SYNC/BSCH is sync;
    // CMCE setup/connect PDUs are calls. Everything else (e.g. MLE
    // D-NWRK-BROADCAST system info) passes through as "unknown" carrying its
    // fields, so nothing decoded is dropped but broadcasts are suppressed by
    // default.
    if (service == "UPLANE" || service == "U-PLANE" ||
        pdu.rfind("TCH", 0) == 0 || contains(pdu, "TRAFFIC") ||
        contains(pdu, "ACELP") || contains(pdu, "SPEECH")) {
        ev.kind = "voice";
    } else if (contains(pdu, "SYNC") || service == "BSCH") {
        ev.kind = "sync";
    } else if (contains(pdu, "SETUP") || contains(pdu, "CONNECT") ||
               contains(pdu, "D-CALL") || contains(pdu, "ALERT") ||
               contains(pdu, "TX-") || contains(pdu, "RELEASE")) {
        ev.kind = "call";
    } else {
        ev.kind = "unknown";
    }

    // Identity. ssi is the party/source when assigned; on traffic bursts it
    // is often 0 or the all-ones broadcast id (16777215) -- the real caller
    // SSI arrives on the CMCE call-setup PDU and is associated by usage
    // marker (that association is a downstream concern, not this parser's).
    // Group vs individual is in address_type (pending), so talkgroup is left
    // empty rather than guessed.
    const std::string ssi = field(rep.fields, "ssi");
    if (!ssi.empty()) ev.source_id = ssi;

    // Structured extras, "; "-joined key=value (mirrors the dsd-fme/osmo
    // convention). usage_marker associates later speech with this call.
    auto add = [&](const char* label, const char* key) {
        const std::string v = field(rep.fields, key);
        if (v.empty()) return;
        if (!ev.extra.empty()) ev.extra += "; ";
        ev.extra += label; ev.extra += '='; ev.extra += v;
    };
    add("service", "service");
    add("pdu", "pdu");
    add("usage_marker", "usage marker");
    add("dl_usage_marker", "downlink usage marker"); // the traffic-channel marker
    add("encr", "encryption mode");

    return ev;
}

DsdEvent classify_tetrakit_json(const std::string& line) {
    return tetrakit_to_event(parse_tetrakit_json(line), line);
}

} // namespace dsdsrv
