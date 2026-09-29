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

    // ---- payload=encrypted_or_binary ----
    // Real encrypted US POCSAG (samples/s381_..., s487_...): the page itself
    // is still emitted (capcode is in the clear), flagged in extra.
    CHECK(multimon_line_to_event(
        R"({"demod_name":"POCSAG1200","address":1900321,"function":3,"alpha":"p+^L+(0<ETB><SYN>\"<DEL>>~<VT><LF>vC<DLE>3f"})",
        ev));
    CHECK(ev.kind == "page" && ev.talkgroup == "1900321");
    CHECK(ev.extra == "protocol=pocsag; baud=1200; message_type=alpha; function=3; payload=encrypted_or_binary");
    CHECK(multimon_line_to_event(
        R"({"demod_name":"POCSAG1200","address":1900067,"function":1,"alpha":"[f<US>dhfd<SI>Xfps<SUB><EM>`a{G <ENQ>Vy<SYN>nv9T<BEL>D<ACK>#;#baA<K-6[!$w}4GF`t<DC4>9<ETX><DC1>=fL"})",
        ev));
    CHECK(ev.extra.find("payload=encrypted_or_binary") != std::string::npos);
    // The same ciphertext rendered as numeric (multimon-ng's auto mode can
    // emit both renderings of an ambiguous page).
    CHECK(multimon_line_to_event(
        R"({"demod_name":"POCSAG1200","address":1900067,"function":1,"numeric":"U53[78 86339[18533 7].9 083 U[328.06- U5 -6[ 15[0443 8673217838 U56U 6U-019]]-7.-1-806.35273884["})",
        ev));
    CHECK(ev.extra.find("payload=encrypted_or_binary") != std::string::npos);
    // Plaintext is never flagged: real pages, symbol-heavy but readable text,
    // pipe-delimited dispatch formats, and numeric pages with brackets/dots.
    CHECK(multimon_line_to_event(pocsag, ev) && ev.extra.find("payload") == std::string::npos); // +++TIME=...
    CHECK(multimon_line_to_event(flex, ev) && ev.extra.find("payload") == std::string::npos);
    CHECK(!looks_encrypted("ALERT|FIRE|STN 7|UNIT E12 ~ASAP {code 3}", false));
    CHECK(!looks_encrypted("Line one<LF>line two<CR><LF>line three<HT>end", false));
    CHECK(!looks_encrypted("[2] 555-1234", true));
    CHECK(!looks_encrypted("555.123.4567 U", true));
    CHECK(!looks_encrypted("<SYN><ETB>x", false)); // too short to judge
    CHECK(looks_encrypted("ab<SOH>cd<STX>ef<ETX>gh", false));
    // FLEX secure / binary message types are flagged by type.
    CHECK(multimon_line_to_event(
        R"({"baud":1600,"level":2,"phase":"A","cycle":1,"frame":2,"capcode":44,"addr_type":"S","is_group":false,"msg_type":"secure","message":"x"})",
        ev));
    CHECK(ev.extra.find("payload=encrypted_or_binary") != std::string::npos);
    // Tone-only pages carry no payload to judge.
    CHECK(multimon_line_to_event(R"({"demod_name":"POCSAG1200","address":671968,"function":1})", ev));
    CHECK(ev.extra.find("payload") == std::string::npos);

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
