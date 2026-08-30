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
        // Call-control style (field set per telive docs).
        TetmonMessage m = parse_tetmon_line(
            "TETMON_begin FUNC:SETUPDEC IDX:12 SSI:1234567 CID:5 NID:9 RX:1 TETMON_end");
        check(m.valid && m.func == "SETUPDEC", "SETUPDEC: valid + func");
        check(m.fields["IDX"] == "12", "SETUPDEC: IDX:12");
        check(m.fields["SSI"] == "1234567", "SETUPDEC: SSI:1234567");
        check(m.fields["CID"] == "5" && m.fields["NID"] == "9", "SETUPDEC: CID/NID");
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

    // ---- mapping to DsdEvent ----
    {
        DsdEvent e = classify_tetmon_line(
            "TETMON_begin FUNC:SETUPDEC IDX:12 SSI:1234567 CID:5 NID:9 RX:1 TETMON_end");
        check(e.kind == "call", "SETUPDEC -> kind call");
        check(e.source_id == "1234567", "SETUPDEC -> source_id from SSI");
        check(e.extra.find("idx=12") != std::string::npos, "SETUPDEC -> extra idx=12");
        check(e.extra.find("cid=5") != std::string::npos, "SETUPDEC -> extra cid=5");
        check(e.extra.find("func=SETUPDEC") != std::string::npos, "SETUPDEC -> extra func token");
        check(e.raw_line.find("TETMON_begin") != std::string::npos, "raw preserved");
    }
    {
        DsdEvent e = classify_tetmon_line("TETMON_begin FUNC:AFCVAL AFC:-3 RX:1 TETMON_end");
        check(e.kind == "unknown", "AFCVAL -> kind unknown (diagnostic, suppressed by default)");
        check(e.source_id.empty() && e.talkgroup.empty(), "AFCVAL -> no ids");
        check(e.extra.find("afc=-3") != std::string::npos, "AFCVAL -> afc value kept in extra");
    }
    {
        // GSSI, when present, is treated as the talkgroup.
        DsdEvent e = classify_tetmon_line(
            "TETMON_begin FUNC:SETUPDEC SSI:111 GSSI:222 TETMON_end");
        check(e.source_id == "111" && e.talkgroup == "222", "GSSI -> talkgroup, SSI -> source");
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
