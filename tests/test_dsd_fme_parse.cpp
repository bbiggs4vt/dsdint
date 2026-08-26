// test_dsd_fme_parse.cpp
//
// Unit tests for classify_dsd_fme_line -- the regex classifier that turns
// a cleaned dsd-fme log line into a structured DsdEvent. Every NXDN input
// string below is a REAL line captured from dsd-fme decoding the project's
// off-air NXDN48/IDAS reference capture (see the DSD-FME + NXDN
// verification work); the DMR lines are the ones the other tests already
// pin. No process, no signal -- just the pure string->struct mapping.

#include "../src/dsd_process.hpp"
#include <cstdio>
#include <string>

using namespace dsdsrv;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& what) {
    std::printf("  %s: %s\n", cond ? "OK" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}
} // namespace

int main() {
    std::printf("test_dsd_fme_parse: classify_dsd_fme_line on real DMR + NXDN lines\n");

    // ---- DMR (must not regress) ----
    {
        DsdEvent e = classify_dsd_fme_line(" SLOT 2 TGT=19535 SRC=2222223 Group Call  ");
        check(e.talkgroup == "19535", "DMR call: talkgroup 19535");
        check(e.source_id == "2222223", "DMR call: source 2222223");
        check(e.slot == "2", "DMR call: slot 2");
        check(e.kind == "call", "DMR call: kind call");
        check(e.ran.empty(), "DMR call: no RAN");
    }
    {
        DsdEvent e = classify_dsd_fme_line(
            "20:37:20 Sync: +DMR   slot1  [SLOT2] | Color Code=04 | VC6 ");
        check(e.color_code == "4", "DMR sync: color code 4 (from 04)");
        check(e.slot == "2", "DMR sync: bracketed slot 2");
        check(e.kind == "sync", "DMR sync: kind sync");
        check(e.crc_error.empty(), "DMR sync: no crc flag");
    }
    {
        DsdEvent e = classify_dsd_fme_line(
            "13:37:43 Sync: +DMR ... | Color Code=05 | MBCC (FEC ERR)");
        check(e.crc_error == "1", "DMR FEC ERR line flagged");
    }

    // ---- NXDN / IDAS ----
    {
        DsdEvent e = classify_dsd_fme_line("15:08:09 Sync: NXDN48  RTCH Voice  RAN 02 PF X/4");
        check(e.ran == "2", "NXDN voice sync: RAN 2 (from 02)");
        check(e.kind == "voice", "NXDN voice sync: kind voice");
        check(e.color_code.empty(), "NXDN: no DMR color_code");
    }
    {
        DsdEvent e = classify_dsd_fme_line(
            " Session Call -   Transmission Release  - Src=958 - Dst/TG=2043 - Prefix Ch: 3 ");
        check(e.source_id == "958", "NXDN call: source 958");
        check(e.talkgroup == "2043", "NXDN call: talkgroup 2043 (from Dst/TG=)");
        check(e.kind == "call", "NXDN call: kind call");
    }
    {
        DsdEvent e = classify_dsd_fme_line(
            "Site ID Message - Area: 0; Site Type: 8 Narrow; Site Code: 1 Open Access;  FACCH3");
        check(e.extra == "site_code=1", "NXDN site id: extra carries site_code=1");
        check(e.talkgroup.empty(), "NXDN site id: no talkgroup fabricated");
        check(e.source_id.empty(), "NXDN site id: no source fabricated");
        check(e.slot.empty(), "NXDN site id: no slot fabricated (Site Type 8 is not a slot)");
    }
    {
        DsdEvent e = classify_dsd_fme_line(
            "Adjacent Information - Cat: Global - Sys Code: 8 - Site Code 2 ");
        check(e.extra == "site_code=2; system_code=8; category=Global",
              "NXDN adjacent: site_code+system_code+category in extra");
    }
    {
        DsdEvent e = classify_dsd_fme_line(
            "Service Information - Location ID [008002] SVC [01A8] RST [000000] ");
        check(e.extra == "location_id=008002", "NXDN service info: location_id=008002");
    }
    {
        DsdEvent e = classify_dsd_fme_line(
            " Channel Update - CH: 31 - TGT: 2043 Group Call Termination");
        check(e.talkgroup == "2043", "NXDN channel update: talkgroup 2043 (from TGT:)");
    }
    {
        // Idle / release with a zero target -- talkgroup should reflect it,
        // not be dropped.
        DsdEvent e = classify_dsd_fme_line(
            " Idle -   Transmission Release  - Src=2048 - Dst/TG=0 ");
        check(e.source_id == "2048", "NXDN idle: source 2048");
        check(e.talkgroup == "0", "NXDN idle: talkgroup 0 (broadcast/none)");
    }

    // ---- DMR trunking / LC (Con+/Cap+/Tier III, emergency, talker alias) ----
    // These input strings are dsd-fme's own printf formats (dmr_csbk.c,
    // dmr_flco.c, dsd_alias.c); the project has no trunking capture, so
    // these pin the parser against dsd-fme's source formats.
    {
        DsdEvent e = classify_dsd_fme_line(
            " Connect Plus Group Voice Channel Grant; Target: 100; Source: 2048; LCN: 3; TS: 1;");
        check(e.source_id == "2048", "Con+ grant: source 2048");
        check(e.extra.find("network_type=con+") != std::string::npos, "Con+ grant: network_type=con+");
        check(e.extra.find("lcn=3") != std::string::npos, "Con+ grant: lcn=3");
    }
    {
        DsdEvent e = classify_dsd_fme_line(
            " Capacity Plus Channel Status - FL: 1 TS: 1 RS: 0 - Rest LSN: 5");
        check(e.extra.find("network_type=cap+") != std::string::npos, "Cap+ status: network_type=cap+");
        check(e.extra.find("rest_channel=5") != std::string::npos, "Cap+ status: rest_channel=5");
    }
    {
        DsdEvent e = classify_dsd_fme_line(
            " C_ALOHA_SYS_PARMS: Tier III; Net ID: 9; Site ID: 1;");
        check(e.extra.find("network_id=9") != std::string::npos, "Aloha: network_id=9");
        check(e.extra.find("site_id=1") != std::string::npos, "Aloha: site_id=1");
    }
    {
        DsdEvent e = classify_dsd_fme_line(" TG: 100; SRC: 2048; Talker Alias: JOHN SMITH");
        check(e.alias == "JOHN SMITH", "talker alias captured to end of line");
        check(e.source_id == "2048", "alias line: source still parsed");
    }
    {
        DsdEvent e = classify_dsd_fme_line(" SLOT 1 TGT=100 SRC=2048 Group Emergency");
        check(e.emergency == "1", "emergency flag set on 'Group Emergency'");
    }
    {
        // The negative-lookahead cases: these must NOT set emergency (the
        // value forms are a timer table and a dPMR field, not a flag).
        DsdEvent e1 = classify_dsd_fme_line(
            " Timers - Emergency: 3; Packet: 5; MS-MS: 2; Line: 1; ");
        check(e1.emergency.empty(), "'Emergency: <timer>' does NOT set the flag");
        DsdEvent e2 = classify_dsd_fme_line("Emergency = 1 - ");
        check(e2.emergency.empty(), "'Emergency = <n>' does NOT set the flag");
    }
    {
        // Alias regex must not fire on the header/error lines (no colon
        // right after "Alias").
        DsdEvent e = classify_dsd_fme_line(" Slot 1 - Talker Alias LC Header; Format 1;");
        check(e.alias.empty(), "'Talker Alias LC Header' is not an alias value");
    }

    if (g_failures == 0) {
        std::printf("\nALL DSD-FME PARSE TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
