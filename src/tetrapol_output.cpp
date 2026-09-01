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

    const std::string t = trim(line);
    if (t.empty()) return;

    std::smatch m;
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
