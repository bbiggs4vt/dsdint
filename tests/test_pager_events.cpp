// multimon_line_to_event against real multimon-ng --json output (captured
// from multimon-ng @ 0722194b decoding its bundled off-air samples and gen-ng
// pages). Pins the paging -> DsdEvent mapping documented in pager_events.hpp.

#include "pager_events.hpp"
#include "tetra_kit_json.hpp"

#include <cstdio>
#include <string>

using namespace dsdsrv;

static int g_failures = 0;
#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

int main() {
    DsdEvent ev;

    // POCSAG alpha with trailing <NUL> padding (real 1200 bps sample).
    const std::string pocsag =
        R"({"demod_name":"POCSAG1200","address":273040,"function":3,"alpha":"+++TIME=0008300324+++TIME=0008300324<NUL>"})";
    CHECK(multimon_line_to_event(pocsag, ev));
    CHECK(ev.kind == "page");
    CHECK(ev.talkgroup == "273040");
    CHECK(ev.source_id.empty());
    CHECK(ev.message == "+++TIME=0008300324+++TIME=0008300324");
    CHECK(ev.extra == "protocol=pocsag; baud=1200; message_type=alpha; function=3");
    CHECK(ev.raw_line == pocsag);
    CHECK(ev.crc_error.empty() && ev.emergency.empty());

    // Tone-only page (address, no text).
    CHECK(multimon_line_to_event(R"({"demod_name":"POCSAG1200","address":671968,"function":1})", ev));
    CHECK(ev.kind == "page" && ev.talkgroup == "671968" && ev.message.empty());
    CHECK(ev.extra == "protocol=pocsag; baud=1200; message_type=tone; function=1");

    // Numeric at 2400.
    CHECK(multimon_line_to_event(R"({"demod_name":"POCSAG2400","address":7,"function":0,"numeric":"12345"})", ev));
    CHECK(ev.talkgroup == "7" && ev.message == "12345");
    CHECK(ev.extra == "protocol=pocsag; baud=2400; message_type=numeric; function=0");

    // Lost address codeword: address/function null -> "" (no token).
    CHECK(multimon_line_to_event(R"({"demod_name":"POCSAG512","address":null,"function":null,"alpha":"tail"})", ev));
    CHECK(ev.talkgroup.empty() && ev.message == "tail");
    CHECK(ev.extra == "protocol=pocsag; baud=512; message_type=alpha");

    // FLEX_NEXT alphanumeric (real Dutch P2000 capture).
    const std::string flex =
        R"({"timestamp":"2026-09-28 20:35:25","baud":1600,"level":2,"phase":"A","cycle":14,"frame":118,"capcode":1523020,"addr_type":"S","is_group":false,"msg_type":"alphanumeric","type_tag":"ALN","fragment":"complete","msg_number":0,"retrieval":0,"maildrop":0,"k_ok":true,"sig_ok":true,"message":"Passage Ambulance Wilhelminabrug Leiden"})";
    CHECK(multimon_line_to_event(flex, ev));
    CHECK(ev.kind == "page");
    CHECK(ev.talkgroup == "1523020");
    CHECK(ev.message == "Passage Ambulance Wilhelminabrug Leiden");
    CHECK(ev.extra == "protocol=flex; baud=1600; message_type=alpha; flex_type=alphanumeric; levels=2; "
                      "phase=A; cycle=14; frame=118; addr_type=S; group=0; fragment=complete");
    CHECK(ev.crc_error.empty() && ev.emergency.empty());

    // FLEX priority + failed checksum + nested group list (skipped, still parses).
    CHECK(multimon_line_to_event(
        R"({"baud":3200,"level":4,"phase":"C","cycle":1,"frame":2,"capcode":2029570,"addr_type":"S","is_group":true,"is_priority":true,"msg_type":"special_numeric","group_capcodes":[100,200],"k_ok":false,"message":"911"})",
        ev));
    CHECK(ev.kind == "page" && ev.talkgroup == "2029570" && ev.message == "911");
    CHECK(ev.emergency == "1" && ev.crc_error == "1");
    CHECK(ev.extra.find("message_type=numeric; flex_type=special_numeric; levels=4; phase=C") != std::string::npos);
    CHECK(ev.extra.find("group=1") != std::string::npos);

    // FLEX tone-only.
    CHECK(multimon_line_to_event(
        R"({"baud":1600,"level":2,"phase":"A","cycle":1,"frame":2,"capcode":43,"addr_type":"S","is_group":false,"msg_type":"tone_only"})",
        ev));
    CHECK(ev.kind == "page" && ev.message.empty() && ev.extra.find("message_type=tone") != std::string::npos);

    // BIW date -> sync (real capture); suppressible.
    const std::string biw =
        R"({"timestamp":"2026-09-28 20:35:25","baud":1600,"level":2,"phase":"A","cycle":14,"frame":115,"msg_type":"biw_date","biw_position":1,"type_tag":"BIW_DATE","year":2024,"month":9,"day":4})";
    CHECK(multimon_line_to_event(biw, ev));
    CHECK(ev.kind == "sync" && ev.talkgroup.empty());
    CHECK(ev.extra == "protocol=flex; baud=1600; info_type=biw_date; day=4; month=9; year=2024");
    CHECK(!multimon_line_to_event(biw, ev, /*forward_system=*/false));

    // BCH stats are dropped.
    CHECK(!multimon_line_to_event(
        R"({"baud":1600,"level":2,"phase":"A","cycle":14,"frame":116,"msg_type":"bch_stats","polarity":"POS","bch_0err":87,"bch_1err":1,"bch_2err":0,"bch_uncorr":0,"errbits":1})",
        ev));

    // Not decoder JSON.
    CHECK(!multimon_line_to_event("POCSAG1200: Address:  273040  Function: 3  Alpha:   hi", ev));
    CHECK(!multimon_line_to_event(R"({"demod_name":"DTMF","digit":"5"})", ev));
    CHECK(!multimon_line_to_event("", ev));

    // extra values can't break the token format.
    CHECK(multimon_line_to_event(
        R"({"baud":1600,"level":2,"phase":"A","cycle":1,"frame":1,"msg_type":"biw_sysid","note":"a;b=c"})", ev));
    CHECK(ev.extra.find("note=a,b,c") != std::string::npos);

    // Padding trim.
    CHECK(trim_pager_padding("abc<NUL><NUL>") == "abc");
    CHECK(trim_pager_padding("abc <EOT><ETX> ") == "abc");
    CHECK(trim_pager_padding("a<LF>b") == "a<LF>b");

    // \uXXXX in the shared scanner decodes to UTF-8 (cJSON escapes control
    // chars this way; surrogate pairs too).
    auto rep = parse_tetrakit_json(R"({"m":"a\u0001bé😀"})");
    CHECK(rep.valid && rep.fields["m"] == "a\x01" "b\xc3\xa9\xf0\x9f\x98\x80");

    if (g_failures) {
        std::fprintf(stderr, "test_pager_events: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_pager_events: all checks passed\n");
    return 0;
}
