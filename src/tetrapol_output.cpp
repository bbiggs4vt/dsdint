// tetrapol_output.cpp -- see tetrapol_output.hpp for the model and rationale.
//
// Output format pinned to tetrapol-kit (aeburriel/tetrapol-kit, lib/tsdu.c
// and lib/addr.c) verbatim printf strings:
//
//   header (every TSDU):  "\tCODOP=0x%02x (%s)\n\tPRIO=%d\n\tID_TSAP=%d\n"
//   D_SYSTEM_INFO body:   "\t\tCOUNTRY_CODE=%d\n"
//                         "\t\tSYSTEM_ID\n\t\t\tVERSION=%d\n\t\t\tNETWORK=%d\n"
//                         "\t\tLOC_AREA_ID\n\t\t\tLOC_ID=%d\n"
//                         "\t\tCELL_ID: BS_ID=%d RWS_ID=%d\n"
//                         "\t\tCELL_BN=%d\n"
//   group CODOPs:         "\t\tGROUP_ID=%d\n"
//   addresses:            "ADDR=%d.%d.0x%03x"  (addr_print)
//
// Because a message spans many lines, the parser accumulates field lines
// under a CODOP header and finalizes (emits) when the next CODOP header or
// end-of-stream arrives. Field matching is on the trimmed line (leading tabs
// stripped) so a build that changes indentation still parses.
//
// STILL source-pinned, NOT validated on a live TETRAPOL signal: the CODOP set
// and which address field carries the talkgroup are the parts to confirm
// against a real capture / tetrapol_dump run.

#include "tetrapol_output.hpp"

#include <regex>

namespace dsdsrv {

namespace {

// The broadcast / network-identity CODOPs -> kind:"sync" (like TETRA's
// NETINFO/FREQINFO). Everything carrying call/channel/group activity ->
// kind:"call". Names are matched as substrings of the CODOP name so a
// build's spelling variants (D_GROUP_ACTIVATION vs D_GROUP_ACTIVATION_...) all
// map. Pinned to the tsdu.h CODOP enum (aeburriel/tetrapol-kit).
bool codop_is_sync(const std::string& name) {
    static const char* kSync[] = {
        "SYSTEM_INFO", "GROUP_LIST", "GROUP_COMPOSITION", "NEIGHBOURING_CELL",
        "ECCH_DESCRIPTION", "ADDITIONAL_PARTICIPANTS", "CELL",
    };
    for (const char* s : kSync)
        if (name.find(s) != std::string::npos) return true;
    return false;
}

bool codop_is_call(const std::string& name) {
    static const char* kCall[] = {
        "ACTIVATION", "PAGING", "SETUP", "RELEASE", "CLOSE", "CONNECT",
        "REJECT", "GROUP_END", "GROUP_IDLE", "EMERGENCY", "OVERLOAD",
        "GRP_WAITING", "OCH", "ECH", "OC_",
    };
    for (const char* s : kCall)
        if (name.find(s) != std::string::npos) return true;
    return false;
}

// Downlink CODOP byte -> message name, from the tetrapol-kit tsdu.h enum
// (aeburriel/tetrapol-kit). tetrapol_dump only prints a full field tree for the
// dozen CODOPs it implements; every other decoded PDU comes out as a one-line
// "unsupported codop 0xNN". Real traffic is dominated by those (a control
// channel pours out D_GROUP_IDLE = 0x58, for instance), so we name them from
// this table and still surface them as events rather than dropping them.
const char* codop_name(unsigned c) {
    switch (c) {
        case 0x02: return "D_LIST";
        case 0x08: return "D_REJECT";
        case 0x09: return "D_REFUSAL";
        case 0x0b: return "D_BACK_CCH";
        case 0x0c: return "D_RELEASE";
        case 0x0f: return "D_HOOK_ON_INVITATION";
        case 0x10: return "D_RETURN";
        case 0x12: return "D_CALL_WAITING";
        case 0x13: return "D_AUTHENTICATION";
        case 0x16: return "D_AUTHORISATION";
        case 0x18: return "D_CHANNEL_INIT";
        case 0x21: return "D_REGISTRATION_NAK";
        case 0x22: return "D_REGISTRATION_ACK";
        case 0x23: return "D_FORCED_REGISTRATION";
        case 0x25: return "D_LOCATION_ACTIVITY_ACK";
        case 0x31: return "D_CALL_ALERT";
        case 0x32: return "D_CALL_SETUP";
        case 0x34: return "D_CALL_CONNECT";
        case 0x35: return "D_CALL_SWITCH";
        case 0x39: return "D_TRANSFER_NAK";
        case 0x3e: return "D_CALL_START";
        case 0x42: return "D_FUNCTIONAL_SHORT_DATA";
        case 0x45: return "D_DATA_MSG_DOWN";
        case 0x46: return "D_EXPLICIT_SHORT_DATA";
        case 0x48: return "D_DATA_END";
        case 0x49: return "D_DATAGRAM_NOTIFY";
        case 0x4a: return "D_DATAGRAM";
        case 0x4b: return "D_BROADCAST";
        case 0x4c: return "D_DATA_SERV";
        case 0x4e: return "D_DATA_DOWN_STATUS";
        case 0x53: return "D_EMERGENCY_NOTIFICATION";
        case 0x55: return "D_GROUP_ACTIVATION";
        case 0x56: return "D_ECH_ACTIVATION";
        case 0x57: return "D_GROUP_END";
        case 0x58: return "D_GROUP_IDLE";
        case 0x59: return "D_GROUP_REJECT";
        case 0x5a: return "D_ECH_REJECT";
        case 0x5b: return "D_GROUP_PAGING";
        case 0x5c: return "D_BROADCAST_NOTIFICATION";
        case 0x5d: return "D_CRISIS_NOTIFICATION";
        case 0x5f: return "D_EMERGENCY_ACK";
        case 0x60: return "D_CONNECT_DCH";
        case 0x62: return "D_CONNECT_CCH";
        case 0x63: return "D_DATA_AUTHENTICATION";
        case 0x64: return "D_DATA_REQUEST";
        case 0x65: return "D_DCH_OPEN";
        case 0x67: return "D_EXTENDED_STATUS";
        case 0x68: return "D_CCH_OPEN";
        case 0x69: return "D_BROADCAST_WAITING";
        case 0x70: return "D_ACCESS_DISABLED";
        case 0x71: return "D_TRAFFIC_ENABLED";
        case 0x72: return "D_TRAFFIC_DISABLED";
        case 0x76: return "D_DEVIATION_ON";
        case 0x77: return "D_ABILITY_MNGT";
        case 0x78: return "D_SERVICE_DISABLED";
        case 0x80: return "D_EMERGENCY_NAK";
        case 0x82: return "D_GROUP_OVERLOAD_ID";
        case 0x83: return "D_ECH_OVERLOAD_ID";
        case 0x84: return "D_PRIORITY_GRP_WAITING";
        case 0x85: return "D_PRIORITY_GRP_ACTIVATION";
        case 0x86: return "D_OC_ACTIVATION";
        case 0x87: return "D_OC_REJECT";
        case 0x88: return "D_OC_PAGING";
        case 0x90: return "D_SYSTEM_INFO";
        case 0x92: return "D_GROUP_LIST";
        case 0x93: return "D_GROUP_COMPOSITION";
        case 0x94: return "D_NEIGHBOURING_CELL";
        case 0x95: return "D_ECCH_DESCRIPTION";
        case 0x96: return "D_ADDITIONAL_PARTICIPANTS";
        case 0xc5: return "D_INFORMATION_DELIVERY";
        case 0xe0: return "D_CALL_ACTIVATION";
        case 0xe1: return "D_CALL_COMPOSITION";
        case 0xe2: return "D_CALL_END";
        case 0xe3: return "D_CALL_OVERLOAD_ID";
        default:   return "";
    }
}

// Trim leading tabs/spaces (the tree indentation) and trailing whitespace.
std::string trim(const std::string& s) {
    std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

bool tetrapol_forward_event(const DsdEvent& ev, bool forward_unknown) {
    return forward_unknown || ev.kind != "unknown";
}

void TetrapolParser::feed_line(const std::string& line, const EventCallback& emit) {
    // The CODOP header opens a new message; verbatim format
    // "\tCODOP=0x%02x (%s)\n" -> capture the hex codop and the name.
    static const std::regex codop_re(
        R"(CODOP=0x([0-9a-fA-F]+)\s*\(([A-Za-z0-9_]+)\))");
    // Body field lines (leading tabs already trimmed by the time we match).
    static const std::regex country_re(R"(^COUNTRY_CODE=(\d+))");
    static const std::regex network_re(R"(^NETWORK=(\d+))");
    static const std::regex locid_re(R"(^LOC_ID=(\d+))");
    static const std::regex cellid_re(R"(^CELL_ID:\s*BS_ID=(\d+)\s+RWS_ID=(\d+))");
    static const std::regex cellbn_re(R"(^CELL_BN=(\d+))");
    static const std::regex group_re(R"(^GROUP_ID=(\d+))");
    static const std::regex addr_re(R"(ADDR=(\d+)\.(\d+)\.0x([0-9a-fA-F]+))");
    // tetrapol_dump prints a decoded-but-unimplemented PDU as a single line,
    // "tsdu:<line> unsupported codop 0x<hh>". The FEC/deinterleave/CRC already
    // passed -- only the field pretty-printer is missing -- so this is a real
    // decoded message; name it from the table and emit it as its own event.
    static const std::regex unsup_re(R"(unsupported codop 0x([0-9a-fA-F]+))");

    const std::string t = trim(line);
    if (t.empty()) return;

    std::smatch m;
    if (std::regex_search(t, m, unsup_re)) {
        finalize(emit);                       // close any tree message in progress
        DsdEvent ev;
        ev.raw_line = t;
        unsigned c = static_cast<unsigned>(std::stoul(m[1].str(), nullptr, 16));
        const char* nm = codop_name(c);
        std::string name = nm[0] ? nm : "";
        ev.extra = "codop=0x" + m[1].str();
        if (!name.empty()) ev.extra += "; msg=" + name;
        ev.extra += "; note=unsupported";     // tetrapol-kit decoded it but has no field printer
        if (codop_is_call(name)) ev.kind = "call";
        else if (codop_is_sync(name)) ev.kind = "sync";
        else ev.kind = "unknown";
        if (emit) emit(ev);
        return;
    }
    if (std::regex_search(t, m, codop_re)) {
        // New message: finalize the one in progress, then start fresh.
        finalize(emit);
        cur_ = DsdEvent{};
        cur_.raw_line = t;
        std::string codop = m[1].str();
        cur_name_ = m[2].str();
        // Keep the codop+name as the first extra tokens so the client sees the
        // message type; kind is decided in finalize() from cur_name_.
        cur_.extra = "codop=0x" + codop + "; msg=" + cur_name_;
        have_ = true;
        return;
    }

    if (!have_) return; // field line with no open header: ignore

    auto add_extra = [&](const std::string& kv) {
        if (!cur_.extra.empty()) cur_.extra += "; ";
        cur_.extra += kv;
    };

    if (std::regex_search(t, m, group_re)) {
        cur_.talkgroup = m[1].str();      // group id is the addressed talkgroup
    } else if (std::regex_search(t, m, country_re)) {
        add_extra("country_code=" + m[1].str());
    } else if (std::regex_search(t, m, network_re)) {
        add_extra("network=" + m[1].str());
    } else if (std::regex_search(t, m, locid_re)) {
        add_extra("loc_id=" + m[1].str());
    } else if (std::regex_search(t, m, cellid_re)) {
        add_extra("bs_id=" + m[1].str());
        add_extra("rws_id=" + m[2].str());
    } else if (std::regex_search(t, m, cellbn_re)) {
        add_extra("cell_bn=" + m[1].str());
    } else if (std::regex_search(t, m, addr_re)) {
        // Address triplet z.y.0xNNN. Record it verbatim; on a call with no
        // GROUP_ID it is the party address, so also fill talkgroup if empty.
        std::string a = m[1].str() + "." + m[2].str() + ".0x" + m[3].str();
        add_extra("addr=" + a);
        if (cur_.talkgroup.empty()) cur_.talkgroup = a;
    }
}

void TetrapolParser::finalize(const EventCallback& emit) {
    if (!have_) return;
    have_ = false;

    if (codop_is_call(cur_name_)) cur_.kind = "call";
    else if (codop_is_sync(cur_name_)) cur_.kind = "sync";
    else cur_.kind = "unknown";

    if (emit) emit(cur_);
    cur_ = DsdEvent{};
    cur_name_.clear();
}

void TetrapolParser::flush(const EventCallback& emit) { finalize(emit); }

void TetrapolParser::reset() {
    have_ = false;
    cur_ = DsdEvent{};
    cur_name_.clear();
}

} // namespace dsdsrv
