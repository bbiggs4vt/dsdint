// test_tetra_kit_json.cpp
//
// Unit tests for the tetra-kit JSON report parser (src/tetra_kit_json.*).
// The field extraction is format-confirmed (flat RapidJSON object, keys from
// the decoder's common/report.cc) and tested hardest, including tolerance of
// nested/array values; the pdu->kind / id mapping is provisional (see the
// header CONFIDENCE note) and asserted only where reasonable. Pure string ->
// struct; no process, no socket.

#include "../src/tetra_kit_json.hpp"
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
    std::printf("test_tetra_kit_json: parse_tetrakit_json / tetrakit_to_event\n");

    // ---- flat field extraction (string + number values) ----
    {
        TetraKitReport r = parse_tetrakit_json(
            "{\"service\":\"CMCE\",\"pdu\":\"D-SETUP\",\"ssi\":1234567,"
            "\"usage marker\":3,\"tn\":1,\"fn\":18}");
        check(r.valid, "valid JSON object");
        check(r.fields["service"] == "CMCE", "service extracted");
        check(r.fields["pdu"] == "D-SETUP", "pdu extracted");
        check(r.fields["ssi"] == "1234567", "numeric ssi stringified");
        check(r.fields["usage marker"] == "3", "key with a space parsed");
        check(r.fields["fn"] == "18", "fn extracted");
    }

    // ---- tolerance: nested object and array values are skipped, not fatal ----
    {
        TetraKitReport r = parse_tetrakit_json(
            "{\"service\":\"U-PLANE\",\"frame\":[1,2,3],"
            "\"meta\":{\"a\":1,\"b\":\"x\"},\"ssi\":42,\"zsize\":17}");
        check(r.valid, "valid despite nested array/object");
        check(r.fields.count("frame") == 0, "array value skipped");
        check(r.fields.count("meta") == 0, "nested object skipped");
        check(r.fields["ssi"] == "42", "scalar after nested value still parsed");
        check(r.fields["zsize"] == "17", "trailing scalar parsed");
    }

    // ---- escapes and whitespace ----
    {
        TetraKitReport r = parse_tetrakit_json(
            "  { \"pdu\" : \"D-SDS-DATA\" , \"text\" : \"hi\\\"there\" } ");
        check(r.valid && r.fields["pdu"] == "D-SDS-DATA", "whitespace tolerated");
        check(r.fields["text"] == "hi\"there", "escaped quote unescaped");
    }

    // ---- non-JSON ----
    {
        TetraKitReport r = parse_tetrakit_json("not json");
        check(!r.valid, "non-JSON -> invalid");
    }

    // ---- mapping to DsdEvent ----
    {
        DsdEvent e = classify_tetrakit_json(
            "{\"service\":\"CMCE\",\"pdu\":\"D-SETUP\",\"ssi\":1234567,\"usage marker\":3}");
        check(e.kind == "call", "D-SETUP -> kind call");
        check(e.source_id == "1234567", "ssi -> source_id");
        check(e.extra.find("usage_marker=3") != std::string::npos, "usage_marker in extra");
        check(e.extra.find("pdu=D-SETUP") != std::string::npos, "pdu in extra");
        check(e.raw_line.find("D-SETUP") != std::string::npos, "raw preserved");
    }
    {
        DsdEvent e = classify_tetrakit_json("{\"service\":\"U-PLANE\",\"ssi\":42,\"usage marker\":3}");
        check(e.kind == "voice", "U-PLANE -> kind voice");
        check(e.source_id == "42", "U-PLANE ssi -> source_id");
    }
    {
        DsdEvent e = classify_tetrakit_json("{\"service\":\"MM\",\"pdu\":\"D-LOCATION-UPDATE-ACCEPT\",\"ssi\":7}");
        check(e.kind == "unknown", "non-call/voice pdu -> unknown (suppressed by default)");
        check(e.extra.find("service=MM") != std::string::npos, "service kept in extra");
    }
    {
        DsdEvent e = classify_tetrakit_json("garbage");
        check(e.kind == "unknown" && e.raw_line == "garbage", "non-JSON -> unknown, raw kept");
    }

    // ---- REAL captured lines (off-air UK TETRA, MCC/MNC 234/78, decoded
    //      through our own demod; frame payload trimmed for the literal) ----
    {
        // Traffic channel = voice. Note the real values: service "UPLANE"
        // (no hyphen), pdu "TCH_S", a distinct "downlink usage marker".
        DsdEvent e = classify_tetrakit_json(
            "{\"service\":\"UPLANE\",\"pdu\":\"TCH_S\",\"tn\":2,\"fn\":3,\"mn\":33,"
            "\"ssi\":0,\"usage marker\":0,\"address_type\":0,"
            "\"downlink usage marker\":62,\"encryption mode\":0,"
            "\"uzsize\":1380,\"zsize\":157,\"frame\":\"eJzlUlsOwCAI...\"}");
        check(e.kind == "voice", "real UPLANE/TCH_S -> voice");
        check(e.extra.find("dl_usage_marker=62") != std::string::npos,
              "real traffic: downlink usage marker in extra");
        check(e.extra.find("pdu=TCH_S") != std::string::npos, "real traffic: pdu token");
    }
    {
        // MLE network broadcast: nested "cell 0" array must be skipped, and
        // the flat fields around it still parse. Classified unknown
        // (suppressed by default) -- it's system info, not a call.
        DsdEvent e = classify_tetrakit_json(
            "{\"time\":\"2026-08-31T13:36:05Z\",\"service\":\"MLE\","
            "\"pdu\":\"D-NWRK-BROADCAST\",\"tn\":4,\"fn\":3,\"mn\":33,"
            "\"ssi\":16777215,\"usage marker\":0,\"encryption mode\":0,"
            "\"address_type\":1,\"actual ssi\":16777215,"
            "\"number of neighbour cells\":3,"
            "\"cell 0\":[{\"Cell identifier CA\":11},{\"LA\":6189}]}");
        check(e.kind == "unknown", "real MLE broadcast -> unknown");
        check(e.extra.find("service=MLE") != std::string::npos, "real MLE: service token");
        check(e.extra.find("pdu=D-NWRK-BROADCAST") != std::string::npos, "real MLE: pdu token past the nested array");
    }
    {
        // A synthesized MAC-SYNC (BSCH) line -> sync.
        DsdEvent e = classify_tetrakit_json("{\"service\":\"MAC\",\"pdu\":\"MAC-SYNC\",\"ssi\":0}");
        check(e.kind == "sync", "MAC-SYNC -> sync");
    }

    if (g_failures == 0) std::printf("ALL TETRA-KIT JSON TESTS PASSED\n");
    else std::printf("%d TETRA-KIT JSON TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
