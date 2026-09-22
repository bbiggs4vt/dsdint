// test_dmr_slot_aggregator.cpp
//
// Unit tests for the reference client-side per-slot DMR aggregator
// (tools/dmr_slot_aggregator.hpp). Pure logic on synthetic events with a
// controlled clock -- no networking, no external deps, always runs.

#include "../tools/dmr_slot_aggregator.hpp"

#include <cstdio>
#include <string>

using namespace dsdclient;

static int g_failures = 0;
static void check(bool c, const char* what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what);
    if (!c) ++g_failures;
}

// Compact event builder (only the fields a case needs).
static Event ev(std::string kind, std::string tg, std::string src, std::string slot,
                std::string extra = "", std::string alias = "", std::string msg = "",
                std::string crc = "", std::string cc = "", std::string emer = "",
                std::string raw = "") {
    Event e;
    e.kind = kind; e.talkgroup = tg; e.source_id = src; e.slot = slot;
    e.extra = extra; e.alias = alias; e.message = msg; e.crc_error = crc;
    e.color_code = cc; e.emergency = emer; e.raw = raw;
    return e;
}

int main() {
    std::printf("test_dmr_slot_aggregator\n");

    // ---- gating: only DMR is timeslotted here ----
    check(is_timeslotted_protocol("dmr"), "dmr is timeslotted");
    check(is_timeslotted_protocol("DMR"), "DMR (case) is timeslotted");
    check(is_timeslotted_protocol("d-m_r"), "d-m_r (separators) is timeslotted");
    check(!is_timeslotted_protocol("nxdn48"), "nxdn is not timeslotted");
    check(!is_timeslotted_protocol("p25"), "p25 is not timeslotted");
    check(!is_timeslotted_protocol("tetra"), "tetra is not (this aggregator is DMR-only)");
    check(!is_timeslotted_protocol(""), "empty protocol is not timeslotted");

    // ---- end-to-end scenario: two concurrent slots, rollover, empty-slot
    // metadata attribution, terminator vs timeout ----
    DmrSlotAggregator agg(std::chrono::milliseconds(1000));
    int starts = 0, ends = 0;
    agg.on_call_start = [&](const Call&) { ++starts; };
    agg.on_call_end   = [&](const Call&) { ++ends; };

    const auto t0 = Clock::now();
    auto at = [&](int ms) { return t0 + std::chrono::milliseconds(ms); };

    agg.on_event(ev("sync", "", "", ""), at(0));                            // channel-wide -> ignored
    agg.on_event(ev("call", "1", "123", "1"), at(10));                      // slot1 TG1 SRC123 start
    agg.on_event(ev("voice", "5", "999", "2"), at(20));                     // slot2 TG5 SRC999 start
    agg.on_event(ev("voice", "1", "123", "1", "", "", "", "1"), at(70));    // slot1 voice + a CRC error
    agg.on_event(ev("voice", "5", "999", "2"), at(80));                     // slot2 voice
    agg.on_event(ev("call", "1", "123", "", "", "JOHN SMITH"), at(120));    // slot="" alias, SRC123 -> slot1
    agg.on_event(ev("message", "1", "123", "", "", "", "ON MY WAY"), at(140)); // slot="" SMS -> slot1
    agg.on_event(ev("voice", "1", "123", "1"), at(180));
    agg.on_event(ev("burst", "", "", "1", "burst=TLC"), at(240));           // slot1 terminator
    agg.on_event(ev("voice", "5", "999", "2"), at(300));
    agg.on_event(ev("voice", "5", "777", "2"), at(360));                    // slot2 rollover SRC999 -> SRC777
    agg.tick(at(1500));                                                     // slot2 (777) times out

    check(agg.history().size() == 3, "three completed calls recorded");
    check(starts == 3 && ends == 3, "start/end callbacks balanced (3 each)");

    if (agg.history().size() == 3) {
        const Call& c1 = agg.history()[0];   // slot1, ended on terminator
        check(c1.slot == 1, "call[0] is slot 1");
        check(c1.source == "123" && c1.talkgroup == "1", "call[0] identity TG1/SRC123");
        check(c1.group, "call[0] is a group call");
        check(c1.crc_errors == 1, "call[0] counted the one CRC error");
        check(c1.alias == "JOHN SMITH", "call[0] absorbed the slot-less alias (by source match)");
        check(c1.messages.size() == 1 && c1.messages[0] == "ON MY WAY",
              "call[0] absorbed the slot-less SMS");
        check(c1.voice_events == 2, "call[0] counted 2 voice events");

        const Call& c2 = agg.history()[1];   // slot2, ended on rollover
        check(c2.slot == 2 && c2.source == "999" && c2.talkgroup == "5",
              "call[1] is slot2 TG5/SRC999 (closed by rollover)");
        check(c2.voice_events == 3, "call[1] counted 3 voice events");

        const Call& c3 = agg.history()[2];   // slot2, ended on timeout
        check(c3.slot == 2 && c3.source == "777", "call[2] is slot2 SRC777 (closed by timeout)");
    }

    // ---- a lone sync must not open a call ----
    {
        DmrSlotAggregator a;
        a.on_event(ev("sync", "", "", ""));
        check(!a.active(1) && !a.active(2), "a lone sync opens no call");
    }

    // ---- private (unit-to-unit) call via unit_target token ----
    {
        DmrSlotAggregator a;
        a.on_event(ev("call", "", "2048", "1", "unit_target=100"));
        check(a.active(1).has_value(), "private call opens on slot 1");
        check(a.active(1) && !a.active(1)->group && a.active(1)->unit_target == "100",
              "private call: group=false, unit_target=100");
    }

    if (g_failures == 0) {
        std::printf("\nALL DMR SLOT AGGREGATOR TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
