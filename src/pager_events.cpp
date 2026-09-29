#include "pager_events.hpp"
#include "tetra_kit_json.hpp" // parse_tetrakit_json: tolerant flat-JSON scalar scanner

#include <map>

namespace dsdsrv {

namespace {

using Fields = std::map<std::string, std::string>;

std::string get(const Fields& f, const char* key) {
    auto it = f.find(key);
    if (it == f.end() || it->second == "null") return {};
    return it->second;
}

// `extra` is "; "-joined key=value tokens; keep values from breaking that.
void add_token(std::string& extra, const std::string& key, std::string value) {
    if (value.empty()) return;
    for (char& c : value)
        if (c == ';' || c == '=') c = ',';
    if (!extra.empty()) extra += "; ";
    extra += key + "=" + value;
}

// Control-character names multimon-ng renders as "<NAME>" in POCSAG text.
bool is_control_name(const std::string& n) {
    static const char* kNames[] = {"NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL", "BS",
                                   "HT",  "LF",  "VT",  "FF",  "CR",  "SO",  "SI",  "DLE", "DC1",
                                   "DC2", "DC3", "DC4", "NAK", "SYN", "ETB", "CAN", "EM",  "SUB",
                                   "ESC", "FS",  "GS",  "RS",  "US",  "DEL"};
    for (const char* k : kNames)
        if (n == k) return true;
    return false;
}

// FLEX_NEXT msg_type (demod_flex_next.c, flex_next_json_emit) -> the coarse
// message_type shared with POCSAG. The exact FLEX type rides in flex_type.
const char* flex_message_type(const std::string& mt) {
    if (mt == "alphanumeric") return "alpha";
    if (mt == "numeric" || mt == "special_numeric" || mt == "numbered_numeric") return "numeric";
    if (mt == "tone_only") return "tone";
    if (mt == "binary") return "binary";
    if (mt == "secure") return "secure";
    if (mt == "instruction") return "instruction";
    if (mt == "short_msg") return "short_message";
    return nullptr;
}

// POCSAG: {"demod_name":"POCSAG1200","address":273040,"function":3,
//          "alpha"|"numeric"|"skyper":"..."}; no text key = tone-only page;
// address/function null when the address codeword was lost.
void pocsag_event(const Fields& f, const std::string& demod, DsdEvent& ev) {
    ev.kind = "page";
    ev.talkgroup = get(f, "address");
    std::string type = "tone", text;
    for (const char* k : {"alpha", "numeric", "skyper"}) {
        if (f.count(k)) { type = k; text = f.at(k); break; }
    }
    ev.message = trim_pager_padding(text);
    add_token(ev.extra, "protocol", "pocsag");
    add_token(ev.extra, "baud", demod.substr(6)); // "POCSAG1200" -> "1200"
    add_token(ev.extra, "message_type", type);
    add_token(ev.extra, "function", get(f, "function"));
    if (type != "tone" && looks_encrypted(ev.message, type == "numeric"))
        add_token(ev.extra, "payload", "encrypted_or_binary");
}

bool flex_event(const Fields& f, DsdEvent& ev, bool forward_system) {
    const std::string mt = get(f, "msg_type");
    if (mt == "bch_stats") return false; // per-frame decoder statistics, not an event

    const char* page_type = flex_message_type(mt);
    if (page_type && f.count("capcode")) {
        ev.kind = "page";
        ev.talkgroup = get(f, "capcode");
        ev.message = get(f, "message");
        if (get(f, "is_priority") == "true") ev.emergency = "1";
        if (get(f, "k_ok") == "false") ev.crc_error = "1";
        add_token(ev.extra, "protocol", "flex");
        add_token(ev.extra, "baud", get(f, "baud"));
        add_token(ev.extra, "message_type", page_type);
        add_token(ev.extra, "flex_type", mt);
        add_token(ev.extra, "levels", get(f, "level"));
        add_token(ev.extra, "phase", get(f, "phase"));
        add_token(ev.extra, "cycle", get(f, "cycle"));
        add_token(ev.extra, "frame", get(f, "frame"));
        add_token(ev.extra, "addr_type", get(f, "addr_type"));
        const std::string grp = get(f, "is_group");
        if (!grp.empty()) add_token(ev.extra, "group", grp == "true" ? "1" : "0");
        add_token(ev.extra, "fragment", get(f, "fragment"));
        const std::string pt(page_type);
        if (pt == "secure" || pt == "binary" ||
            ((pt == "alpha" || pt == "numeric") && looks_encrypted(ev.message, pt == "numeric")))
            add_token(ev.extra, "payload", "encrypted_or_binary");
        return true;
    }

    // BIW system info (biw_date / biw_time / biw_sysid / biw_sysinfo /
    // biw_countrycode) and anything newer: a network broadcast -> "sync".
    if (!forward_system) return false;
    ev.kind = "sync";
    add_token(ev.extra, "protocol", "flex");
    add_token(ev.extra, "baud", get(f, "baud"));
    add_token(ev.extra, "info_type", mt.empty() ? std::string("unknown") : mt);
    static const char* kSkip[] = {"timestamp", "msg_type", "type_tag", "baud", "level",
                                  "phase", "cycle", "frame", "biw_position"};
    for (const auto& [k, v] : f) {
        bool skip = false;
        for (const char* s : kSkip) skip = skip || k == s;
        if (!skip && v != "null") add_token(ev.extra, k, v);
    }
    return true;
}

} // namespace

bool looks_encrypted(const std::string& text, bool numeric) {
    std::size_t n = 0, suspicious = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (!numeric && text[i] == '<') {
            const std::size_t close = text.find('>', i + 1);
            if (close != std::string::npos && close - i <= 4 && is_control_name(text.substr(i + 1, close - i - 1))) {
                const std::string name = text.substr(i + 1, close - i - 1);
                ++n;
                if (name != "LF" && name != "CR" && name != "HT") ++suspicious;
                i = close + 1;
                continue;
            }
        }
        const char c = text[i++];
        ++n;
        if (numeric && (c == 'U' || c == '[' || c == ']')) ++suspicious;
    }
    // Thresholds and minimum lengths: see the header. Deliberately
    // conservative -- a false alarm would hide a readable page from a client,
    // which is worse than letting some short ciphertext through unflagged.
    if (n < (numeric ? 16u : 8u)) return false;
    const double frac = static_cast<double>(suspicious) / static_cast<double>(n);
    return numeric ? frac >= 0.12 : frac >= 0.10;
}

std::string trim_pager_padding(std::string s) {
    static const char* kPad[] = {"<NUL>", "<EOT>", "<ETX>", "<ETB>"};
    bool changed = true;
    while (changed) {
        changed = false;
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) { s.pop_back(); changed = true; }
        for (const char* p : kPad) {
            const std::string pad(p);
            if (s.size() >= pad.size() && s.compare(s.size() - pad.size(), pad.size(), pad) == 0) {
                s.erase(s.size() - pad.size());
                changed = true;
            }
        }
    }
    return s;
}

bool multimon_line_to_event(const std::string& line, DsdEvent& ev, bool forward_system) {
    const TetraKitReport rep = parse_tetrakit_json(line);
    if (!rep.valid) return false;
    ev = DsdEvent{};
    ev.raw_line = line;

    const std::string demod = get(rep.fields, "demod_name");
    if (demod.rfind("POCSAG", 0) == 0) {
        pocsag_event(rep.fields, demod, ev);
        return true;
    }
    // FLEX_NEXT objects carry no demod_name but always phase/cycle/frame.
    if (demod.empty() && (rep.fields.count("phase") || rep.fields.count("cycle")))
        return flex_event(rep.fields, ev, forward_system);
    return false;
}

} // namespace dsdsrv
