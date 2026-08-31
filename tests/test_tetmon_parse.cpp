// test_tetmon_parse.cpp
//
// Unit tests for the TETMON parser (src/tetra_tetmon.*): the text protocol
// the sq5bpf osmo-tetra fork's tetra-rx sends over UDP. Tests the exact
// wrapper/field extraction (which is format-confirmed) hardest; the
// func->kind / id mapping is provisional (see tetra_tetmon.hpp CONFIDENCE),
// so it is asserted only where the fork's source confirms the format.
// No process, no socket -- pure string -> struct.

#include "../src/tetra_tetmon.hpp"
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
    std::printf("test_tetmon_parse: parse_tetmon_line / tetmon_to_event\n");

    // ---- wrapper + generic field extraction (format-confirmed) ----
    {
        // Verbatim from the fork's tetra-rx.c AFC status emit.
        TetmonMessage m = parse_tetmon_line("TETMON_begin FUNC:AFCVAL AFC:-3 RX:1 TETMON_end");
        check(m.valid, "AFCVAL: valid wrapper");
        check(m.func == "AFCVAL", "AFCVAL: func extracted");
        check(m.fields["AFC"] == "-3", "AFCVAL: AFC:-3 (negative value kept)");
        check(m.fields["RX"] == "1", "AFCVAL: RX:1");
        check(m.fields.count("FUNC") == 1, "AFCVAL: FUNC stored in fields too");
    }
    {
        // Real NETINFO1 line, verbatim from the fork's tetra-rx.
        TetmonMessage m = parse_tetmon_line(
            "TETMON_begin FUNC:NETINFO1 CCODE:17 MCC:00ea MNC:004e DLF:0 ULF:0 LA:0 CRYPT:0 RX:1 TETMON_end");
        check(m.valid && m.func == "NETINFO1", "NETINFO1: valid + func");
        check(m.fields["CCODE"] == "17", "NETINFO1: CCODE:17");
        check(m.fields["MCC"] == "00ea" && m.fields["MNC"] == "004e", "NETINFO1: MCC/MNC (hex)");
    }
    {
        // Real call-control line (field set from the fork's source).
        TetmonMessage m = parse_tetmon_line(
            "TETMON_begin FUNC:DSETUPDEC IDX:12 SSI:1234567 SSI2:222 CID:5 NID:9 RX:1 TETMON_end");
        check(m.valid && m.func == "DSETUPDEC", "DSETUPDEC: valid + func");
        check(m.fields["SSI"] == "1234567" && m.fields["SSI2"] == "222", "DSETUPDEC: SSI/SSI2");
    }

    // ---- robustness ----
    {
        // Leading/trailing junk and extra interior whitespace are tolerated.
        TetmonMessage m = parse_tetmon_line(
            "  garbage TETMON_begin   FUNC:AFCVAL    AFC:0   RX:2   TETMON_end trailing\n");
        check(m.valid, "whitespace/junk: still valid");
        check(m.func == "AFCVAL" && m.fields["RX"] == "2", "whitespace/junk: fields intact");
    }
    {
        TetmonMessage m = parse_tetmon_line("not a tetmon line at all");
        check(!m.valid, "non-TETMON: invalid");
    }
    {
        // Wrapper present but truncated (no end marker) is rejected.
        TetmonMessage m = parse_tetmon_line("TETMON_begin FUNC:AFCVAL AFC:1");
        check(!m.valid, "missing TETMON_end: invalid");
    }
    {
        // A token without a colon is skipped, not fatal.
        TetmonMessage m = parse_tetmon_line("TETMON_begin FUNC:AFCVAL stray AFC:1 TETMON_end");
        check(m.valid && m.fields["AFC"] == "1", "colon-less token: skipped, rest parsed");
        check(m.fields.count("stray") == 0, "colon-less token: not stored");
    }

    // ---- mapping to DsdEvent (pinned against real tetra-rx output) ----
    {
        // Call setup -> call; SSI = source, SSI2 = talkgroup.
        DsdEvent e = classify_tetmon_line(
            "TETMON_begin FUNC:DSETUPDEC IDX:12 SSI:1234567 SSI2:222 CID:5 NID:9 RX:1 TETMON_end");
        check(e.kind == "call", "DSETUPDEC -> kind call");
        check(e.source_id == "1234567", "DSETUPDEC -> source_id from SSI");
        check(e.talkgroup == "222", "DSETUPDEC -> talkgroup from SSI2");
        check(e.extra.find("idx=12") != std::string::npos, "DSETUPDEC -> extra idx=12");
        check(e.extra.find("func=DSETUPDEC") != std::string::npos, "DSETUPDEC -> extra func token");
    }
    {
        // Real NETINFO1 -> sync; CCODE -> color_code, MCC/MNC in extra.
        DsdEvent e = classify_tetmon_line(
            "TETMON_begin FUNC:NETINFO1 CCODE:17 MCC:00ea MNC:004e DLF:0 ULF:0 LA:0 CRYPT:0 RX:1 TETMON_end");
        check(e.kind == "sync", "NETINFO1 -> kind sync");
        check(e.color_code == "17", "NETINFO1 -> CCODE into color_code");
        check(e.extra.find("mcc=00ea") != std::string::npos, "NETINFO1 -> mcc in extra");
        check(e.extra.find("mnc=004e") != std::string::npos, "NETINFO1 -> mnc in extra");
    }
    {
        // Real FREQINFO1 -> sync.
        DsdEvent e = classify_tetmon_line(
            "TETMON_begin FUNC:FREQINFO1 DLF:393087500 LA:6189 RX:1 TETMON_end");
        check(e.kind == "sync", "FREQINFO1 -> kind sync");
        check(e.extra.find("dlf=393087500") != std::string::npos, "FREQINFO1 -> dlf in extra");
    }
    {
        // Real BURST marker -> unknown (suppressed).
        DsdEvent e = classify_tetmon_line("TETMON_begin FUNC:BURST RX:1 TETMON_end");
        check(e.kind == "unknown", "BURST -> kind unknown (suppressed)");
    }
    {
        DsdEvent e = classify_tetmon_line("TETMON_begin FUNC:AFCVAL AFC:-3 RX:1 TETMON_end");
        check(e.kind == "unknown", "AFCVAL -> kind unknown (diagnostic, suppressed by default)");
        check(e.source_id.empty() && e.talkgroup.empty(), "AFCVAL -> no ids");
        check(e.extra.find("afc=-3") != std::string::npos, "AFCVAL -> afc value kept in extra");
    }
    {
        // A datagram with a binary tail after TETMON_end: raw is trimmed to
        // the text wrapper, fields still parse.
        DsdEvent e = classify_tetmon_line(
            std::string("TETMON_begin FUNC:BURST RX:1 TETMON_end") + '\x03' + "v\x9c\x00binary");
        check(e.raw_line == "TETMON_begin FUNC:BURST RX:1 TETMON_end", "binary tail trimmed from raw");
        check(e.kind == "unknown", "binary-tail line still classified");
    }
    {
        DsdEvent e = classify_tetmon_line("random non-tetmon text");
        check(e.kind == "unknown", "non-TETMON -> kind unknown");
        check(e.raw_line == "random non-tetmon text", "non-TETMON -> raw kept");
    }

    if (g_failures == 0) std::printf("ALL TETMON PARSE TESTS PASSED\n");
    else std::printf("%d TETMON PARSE TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
