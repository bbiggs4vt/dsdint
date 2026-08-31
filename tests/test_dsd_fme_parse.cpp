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
        // The "Emergency: <timer>" colon form (a timer table) must NOT set
        // the flag -- only the bare word (DMR/P25) or "Emergency = 1"
        // (dPMR, tested below) does.
        DsdEvent e1 = classify_dsd_fme_line(
            " Timers - Emergency: 3; Packet: 5; MS-MS: 2; Line: 1; ");
        check(e1.emergency.empty(), "'Emergency: <timer>' does NOT set the flag");
    }
    {
        // Alias regex must not fire on the header/error lines (no colon
        // right after "Alias").
        DsdEvent e = classify_dsd_fme_line(" Slot 1 - Talker Alias LC Header; Format 1;");
        check(e.alias.empty(), "'Talker Alias LC Header' is not an alias value");
    }

    // ---- P25 (NAC, encryption ALG/KEY, zero-padded IDs) ----
    // Input strings are dsd-fme's P25 printf formats (dsd_frame.c,
    // p25p1_hdu.c, p25p1_ldu2.c).
    {
        DsdEvent e = classify_dsd_fme_line(
            "2023/10/02 10:23:18 P25 TGT: 00000100; SRC: 00002048; NAC: 293; ");
        check(e.nac == "293", "P25: NAC 293");
        check(e.talkgroup == "100", "P25: talkgroup 100 (zero-padding stripped)");
        check(e.source_id == "2048", "P25: source 2048 (zero-padding stripped)");
    }
    {
        DsdEvent e = classify_dsd_fme_line(
            " HDU  ALG ID: 0x84 KEY ID: 0x0001 MI: 0x0123456789ABCDEF");
        check(e.extra.find("alg_id=84") != std::string::npos, "P25 HDU: alg_id=84 (AES256)");
        check(e.extra.find("key_id=0001") != std::string::npos, "P25 HDU: key_id=0001");
    }
    {
        DsdEvent e = classify_dsd_fme_line(
            " LDU2 ALG ID: 0x80 KEY ID: 0x0000 MI: 0x00000000000000000000");
        check(e.extra.find("alg_id=80") != std::string::npos, "P25 LDU2: alg_id=80 (clear)");
        check(e.extra.find("key_id=0000") != std::string::npos, "P25 LDU2: key_id=0000 (unencrypted)");
    }
    {
        // FEC-ERR encryption line: alg/key still lifted, and crc_error set.
        DsdEvent e = classify_dsd_fme_line(
            " LDU2/ESS_B FEC ERR - ALG: 0xAA KEY ID: 0x0042 LFSR MI: 0x0000000000000000");
        check(e.extra.find("alg_id=AA") != std::string::npos, "P25 ESS FEC ERR: alg_id=AA (ADP)");
        check(e.crc_error == "1", "P25 ESS FEC ERR: crc_error flagged");
    }
    {
        DsdEvent e = classify_dsd_fme_line(" P25 LCW  Group Call; Emergency");
        check(e.emergency == "1", "P25 LCW: emergency flag");
    }
    // P25 trunking system identity -- these are REAL lines from a P25
    // Phase 1 control-channel capture (both dsd-fme forms: colon and
    // bracketed), so this pins the parser against a live decode.
    {
        DsdEvent e = classify_dsd_fme_line("17:30:46 Sync: +P25p1 NAC/CC: 717; RFSS: 001; Site: 097;  TSBK");
        check(e.nac == "717", "P25 CC: NAC 717");
        check(e.extra.find("rfss=1") != std::string::npos, "P25 CC: rfss=1 (colon form)");
        check(e.extra.find("site_id=97") != std::string::npos, "P25 CC: site_id=97 (colon form)");
    }
    {
        DsdEvent e = classify_dsd_fme_line(" LRA [00] CFVA [3] RFSS[001] SITE [091] SYSID [715]");
        check(e.extra.find("rfss=1") != std::string::npos, "P25 netsts: rfss=1 (bracket form)");
        check(e.extra.find("site_id=91") != std::string::npos, "P25 netsts: site_id=91 (bracket form)");
        check(e.extra.find("system_id=715") != std::string::npos, "P25 netsts: system_id=715");
    }
    {
        DsdEvent e = classify_dsd_fme_line(" CHAN-T [52E6] CHAN-R [50D7] SSC [70] WACN [BEE0A]");
        check(e.extra.find("wacn=BEE0A") != std::string::npos, "P25: wacn=BEE0A");
    }
    {
        // NXDN "Site Code" must still NOT be captured as site_id.
        DsdEvent e = classify_dsd_fme_line(
            "Adjacent Information - Cat: Global - Sys Code: 8 - Site Code 2 ");
        check(e.extra.find("site_id=") == std::string::npos,
              "NXDN 'Site Code' is not mis-parsed as P25 site_id");
        check(e.extra.find("site_code=2") != std::string::npos, "NXDN site_code still works");
    }

    // ---- dPMR (real lines from decoding samples/dpmr.dis) ----
    {
        DsdEvent e = classify_dsd_fme_line(" TG=0010011 Src=0000243 Channel Code=31");
        check(e.talkgroup == "10011", "dPMR: talkgroup 10011 (TG=, zero-padding stripped)");
        check(e.source_id == "243", "dPMR: source 243 (Src=, stripped)");
        check(e.color_code == "31", "dPMR: color_code 31 (from Channel Code=)");
    }
    {
        DsdEvent e = classify_dsd_fme_line(" TG=(CRC ERR) Src=(CRC ERR) Channel Code =(CRC ERR)");
        check(e.crc_error == "1", "dPMR CRC ERR line flagged");
        check(e.talkgroup.empty(), "dPMR CRC ERR: no bogus talkgroup");
        check(e.color_code.empty(), "dPMR CRC ERR: no bogus color_code");
    }
    {
        DsdEvent e = classify_dsd_fme_line("Comm Mode = 0 - Comms Format = 1 - Emergency = 1 - ");
        check(e.emergency == "1", "dPMR: Emergency = 1 sets the flag");
    }
    {
        DsdEvent e = classify_dsd_fme_line("Comm Mode = 0 - Emergency = 0 - ");
        check(e.emergency.empty(), "dPMR: Emergency = 0 does NOT set the flag");
    }

    // ---- D-STAR (real lines from decoding samples/dstar_f1zil_1.dis) ----
    // D-STAR / YSF are callsign-based amateur protocols: source_id and
    // talkgroup carry callsigns (not numeric ids), and the numeric access
    // codes (color_code/ran/nac) stay blank. Callsign extraction only runs
    // when the line carries the protocol's sync marker.
    {
        DsdEvent e = classify_dsd_fme_line(
            "18:27:55 Sync: -DSTAR VOICE   RPT 2: F1ZIL  G RPT 1: F1ZIL  B "
            "DST: CQCQCQ   SRC: F1NSR   ID51 REPEATER");
        check(e.source_id == "F1NSR ID51", "D-STAR: source callsign (SRC -> source_id)");
        check(e.talkgroup == "CQCQCQ", "D-STAR: destination callsign (DST -> talkgroup)");
        check(e.extra.find("rpt1=F1ZIL B") != std::string::npos, "D-STAR: rpt1 in extra");
        check(e.extra.find("rpt2=F1ZIL G") != std::string::npos, "D-STAR: rpt2 in extra");
        check(e.kind == "call", "D-STAR: kind call");
        check(e.color_code.empty() && e.ran.empty() && e.nac.empty(),
              "D-STAR: no numeric access code");
    }
    {
        // Radio text on its own slow-data frame (same reprinted sync line).
        DsdEvent e = classify_dsd_fme_line(
            "18:27:56 Sync: -DSTAR VOICE   TEXT: YANNICK ST RAPHAEL");
        check(e.extra.find("radio_text=YANNICK ST RAPHAEL") != std::string::npos,
              "D-STAR: slow-data radio text in extra");
    }
    {
        // A DMR line that happens to contain "SRC:" must NOT be parsed as a
        // callsign (no DSTAR/YSF marker) -- source stays a numeric id.
        DsdEvent e = classify_dsd_fme_line(" SLOT 1 TGT=100 SRC=2222223 Group Call ");
        check(e.source_id == "2222223", "non-amateur line: SRC stays numeric (no callsign path)");
    }

    // ---- YSF / System Fusion (real lines from samples/ysf_f5zoo.dis) ----
    {
        DsdEvent e = classify_dsd_fme_line(
            "18:28:18 Sync: +YSF  V/D2 Group/CQ -Simplex CC FN: 2/7 SRC: F1SER     ");
        check(e.source_id == "F1SER", "YSF: source callsign (SRC -> source_id)");
        check(e.extra.find("call_mode=group_cq") != std::string::npos, "YSF: call_mode group_cq");
        check(e.extra.find("data_type=vd2") != std::string::npos, "YSF: data_type vd2");
        check(e.kind == "call", "YSF: kind call");
    }
    {
        // Masked group-CQ destination ("**********") must not become a
        // bogus talkgroup.
        DsdEvent e = classify_dsd_fme_line(
            "18:28:18 Sync: +YSF  V/D2 Group/CQ -Simplex CC FN: 1/7 DST: ********** ");
        check(e.talkgroup.empty(), "YSF: masked DST (**********) yields no talkgroup");
    }
    {
        DsdEvent e = classify_dsd_fme_line(
            "18:28:18 Sync: +YSF  V/D2 Group/CQ -Simplex CC FN: 3/7 U/L: F5ZOO-R1  ");
        check(e.extra.find("uplink=F5ZOO-R1") != std::string::npos, "YSF: uplink callsign in extra");
        check(e.kind == "call", "YSF: uplink line kind call");
    }
    {
        DsdEvent e = classify_dsd_fme_line(
            "18:28:18 Sync: +YSF  V/D2 Group/CQ -Simplex CC FN: 4/7 D/L: F5ZOO-R1  ");
        check(e.extra.find("downlink=F5ZOO-R1") != std::string::npos, "YSF: downlink callsign in extra");
    }

    // ---- Startup banner / config noise is suppressed by default ----
    // dsd-fme prints an ASCII-art banner and a version/device/config block
    // when it spins up; all of it classifies as kind:"unknown" and should
    // be dropped unless forward_unknown is set. Lines below are verbatim
    // from real dsd-fme startup output.
    {
        const char* noise[] = {
            " ██████╗  ██████╗", // banner art
            "Build Version: AW 2026-37-g198f0ea ",
            "MBElib Version: 1.3.0",
            "Decoding DMR BS/MS Simplex",
            "UDP Blaster Output: 127.0.0.1:47024 ",
            "Audio In Device: -",
        };
        bool all_unknown = true, all_suppressed = true, all_forwardable = true;
        for (const char* s : noise) {
            DsdEvent e = classify_dsd_fme_line(s);
            if (e.kind != "unknown") all_unknown = false;
            if (dsd_fme_forward_event(e, /*forward_unknown=*/false)) all_suppressed = false;
            if (!dsd_fme_forward_event(e, /*forward_unknown=*/true)) all_forwardable = false;
        }
        check(all_unknown, "startup banner/config lines classify as kind:unknown");
        check(all_suppressed, "startup noise is suppressed by default (forward_unknown=false)");
        check(all_forwardable, "startup noise is forwarded when forward_unknown=true (debug)");
    }
    {
        // A real decode event is forwarded regardless of the flag.
        DsdEvent e = classify_dsd_fme_line(" SLOT 2 TGT=19535 SRC=2222223 Group Call  ");
        check(e.kind == "call", "a real call line is not kind:unknown");
        check(dsd_fme_forward_event(e, false), "recognized events forward even with unknowns suppressed");
    }
    // ---- DMR short data / SMS (dsd-fme's own text-label formats) ----
    {
        // UDT message: SRC/TGT and the ISO7 body share one line, so the
        // event carries who sent it, to whom, and the text.
        DsdEvent e = classify_dsd_fme_line("Slot 1 - SRC: 6789; TGT: 12345; UDT ISO7 Text: HELLO WORLD");
        check(e.message == "HELLO WORLD", "UDT ISO7: message body extracted");
        check(e.source_id == "6789", "UDT ISO7: source_id from SRC");
        check(e.talkgroup == "12345", "UDT ISO7: talkgroup from TGT");
        check(e.kind == "message", "UDT ISO7: kind is message");
        check(dsd_fme_forward_event(e, false), "SMS event forwarded even with unknowns suppressed");
    }
    {
        // Short-data UTF8 body on its own line (no SRC/TGT on this line).
        DsdEvent e = classify_dsd_fme_line(" UTF8 Text: on my way");
        check(e.message == "on my way", "UTF8 short data: message body extracted");
        check(e.kind == "message", "UTF8 short data: kind is message");
    }
    {
        // Trailing null/padding runs ('_' for nulls, '-'/space for other
        // non-printables) are trimmed; interior spaces are kept.
        DsdEvent e = classify_dsd_fme_line("UTF16 Text: see you at 5 ____----");
        check(e.message == "see you at 5", "UTF16: trailing padding trimmed, interior kept");
    }
    {
        // An all-padding body yields no message (and stays unknown, so the
        // empty artifact is suppressed rather than sent as a blank message).
        DsdEvent e = classify_dsd_fme_line("ISO7 Text: ________");
        check(e.message.empty(), "all-padding body -> no message");
        check(e.kind == "unknown", "all-padding SMS line stays unknown (suppressed)");
    }

    if (g_failures == 0) {
        std::printf("\nALL DSD-FME PARSE TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
