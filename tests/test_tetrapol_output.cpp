// test_tetrapol_output.cpp
//
// Unit test for the TETRAPOL output parser (src/tetrapol_output.*): feeds
// tetrapol_dump's multi-line indented TSDU trees line by line and checks the
// accumulated DsdEvents. Covers:
//   1. a D_SYSTEM_INFO broadcast -> kind "sync" with the network-identity
//      fields (country/network/loc/cell) in `extra`;
//   2. a D_GROUP_ACTIVATION -> kind "call" with GROUP_ID -> talkgroup;
//   3. finalization boundaries -- a message is emitted when the NEXT CODOP
//      header arrives, and the last one on flush();
//   4. an unmapped CODOP -> kind "unknown" (suppressed by the forward filter);
//   5. the tetrapol_forward_event policy;
//   6. reset() discards an in-progress message.
//
// Format strings are pinned to tetrapol-kit (lib/tsdu.c, lib/addr.c).

#include "tetrapol_output.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace dsdsrv;

namespace {
int g_failures = 0;
void check(bool c, const std::string& what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what.c_str());
    if (!c) ++g_failures;
}

// Feed each line of a block to the parser, collecting emitted events.
void feed_block(TetrapolParser& p, const std::vector<std::string>& lines,
                std::vector<DsdEvent>& out) {
    auto emit = [&](const DsdEvent& e) { out.push_back(e); };
    for (const auto& l : lines) p.feed_line(l, emit);
}
} // namespace

int main() {
    std::printf("test_tetrapol_output\n");

    // ---- forward policy ----
    {
        DsdEvent call; call.kind = "call";
        DsdEvent sync; sync.kind = "sync";
        DsdEvent unk;  unk.kind = "unknown";
        check(tetrapol_forward_event(call, false), "forward: call forwarded");
        check(tetrapol_forward_event(sync, false), "forward: sync forwarded");
        check(!tetrapol_forward_event(unk, false), "forward: unknown suppressed by default");
        check(tetrapol_forward_event(unk, true), "forward: unknown forwarded when enabled");
    }

    // ---- 1+2+3: a two-message stream, finalize on next header + flush ----
    {
        TetrapolParser p;
        std::vector<DsdEvent> ev;
        feed_block(p, {
            "\tCODOP=0x90 (D_SYSTEM_INFO)",
            "\tPRIO=0",
            "\tID_TSAP=0",
            "\t\tCOUNTRY_CODE=1",
            "\t\tSYSTEM_ID",
            "\t\t\tVERSION=1",
            "\t\t\tNETWORK=42",
            "\t\tLOC_AREA_ID",
            "\t\t\tLOC_ID=7",
            "\t\tCELL_ID: BS_ID=12 RWS_ID=3",
            "\t\tCELL_BN=5",
            "\tCODOP=0x21 (D_GROUP_ACTIVATION)", // <- finalizes message 1
            "\tPRIO=2",
            "\tID_TSAP=1",
            "\t\tGROUP_ID=1234",
        }, ev);

        check(ev.size() == 1, "first message emitted when the second header arrives");
        if (!ev.empty()) {
            const DsdEvent& s = ev[0];
            check(s.kind == "sync", "D_SYSTEM_INFO -> kind sync");
            check(s.extra.find("msg=D_SYSTEM_INFO") != std::string::npos, "extra names the CODOP");
            check(s.extra.find("codop=0x90") != std::string::npos, "extra carries the codop hex");
            check(s.extra.find("country_code=1") != std::string::npos, "country_code in extra");
            check(s.extra.find("network=42") != std::string::npos, "network in extra");
            check(s.extra.find("loc_id=7") != std::string::npos, "loc_id in extra");
            check(s.extra.find("bs_id=12") != std::string::npos, "bs_id in extra");
            check(s.extra.find("rws_id=3") != std::string::npos, "rws_id in extra");
            check(s.extra.find("cell_bn=5") != std::string::npos, "cell_bn in extra");
            check(s.talkgroup.empty(), "sync has no talkgroup");
        }

        // Message 2 only comes out on flush().
        p.flush([&](const DsdEvent& e) { ev.push_back(e); });
        check(ev.size() == 2, "second message emitted on flush()");
        if (ev.size() == 2) {
            const DsdEvent& c = ev[1];
            check(c.kind == "call", "D_GROUP_ACTIVATION -> kind call");
            check(c.talkgroup == "1234", "GROUP_ID -> talkgroup");
            check(c.extra.find("msg=D_GROUP_ACTIVATION") != std::string::npos, "extra names the call CODOP");
        }
    }

    // ---- 4: unmapped CODOP -> unknown ----
    {
        TetrapolParser p;
        std::vector<DsdEvent> ev;
        feed_block(p, {
            "\tCODOP=0x55 (D_SOMETHING_ELSE)",
            "\t\tFOO=1",
        }, ev);
        p.flush([&](const DsdEvent& e) { ev.push_back(e); });
        check(ev.size() == 1 && ev[0].kind == "unknown", "unmapped CODOP -> unknown");
        check(!tetrapol_forward_event(ev[0], false), "unknown would be suppressed");
    }

    // ---- 5: an address-bearing call CODOP fills talkgroup from ADDR ----
    {
        TetrapolParser p;
        std::vector<DsdEvent> ev;
        feed_block(p, {
            "\tCODOP=0x30 (D_OC_ACTIVATION)",
            "\t\tADDR=1.2.0x0ff",
        }, ev);
        p.flush([&](const DsdEvent& e) { ev.push_back(e); });
        check(ev.size() == 1, "OC activation emitted");
        if (!ev.empty()) {
            check(ev[0].kind == "call", "D_OC_ACTIVATION -> kind call");
            check(ev[0].talkgroup == "1.2.0x0ff", "ADDR fills talkgroup when no GROUP_ID");
            check(ev[0].extra.find("addr=1.2.0x0ff") != std::string::npos, "addr also in extra");
        }
    }

    // ---- 6: reset() discards the in-progress message ----
    {
        TetrapolParser p;
        std::vector<DsdEvent> ev;
        auto emit = [&](const DsdEvent& e) { ev.push_back(e); };
        p.feed_line("\tCODOP=0x90 (D_SYSTEM_INFO)", emit);
        p.feed_line("\t\tCOUNTRY_CODE=1", emit);
        p.reset();
        p.flush(emit);
        check(ev.empty(), "reset() drops the accumulated message");
    }

    if (g_failures == 0) { std::printf("\nALL TETRAPOL OUTPUT TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
