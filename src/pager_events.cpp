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
