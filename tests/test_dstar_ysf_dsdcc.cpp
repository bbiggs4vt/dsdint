// test_dstar_ysf_dsdcc.cpp
//
// Verifies the DSDcc backend's D-STAR and YSF metadata paths against
// DSDcc's own bundled real captures (samples/dstar_f1zil_1.dis and
// samples/ysf_f5zoo.dis). Both are callsign-based amateur protocols, so
// source_id/talkgroup carry callsigns (not numeric ids) and the numeric
// access-code fields stay blank -- see PROTOCOL.md.
//
// Ground truth was established with DSDcc's own dsdccx CLI on the same
// files (dsdccx -fd / -fy, formatted-message log):
//   D-STAR (dstar_f1zil_1.dis): MYCALL F1NSR/ID51 -> CQCQCQ via repeater
//     F1ZIL B, slow-data radio text "YANNICK ST RAPHAEL".
//   YSF (ysf_f5zoo.dis): sources F1SER/F6FCE, group CQ, V/D type 2,
//     repeater uplink/downlink F5ZOO-R1.
//
// Usage: test_dstar_ysf_dsdcc <dstar_f1zil_1.dis> <ysf_f5zoo.dis>
// With missing/unopenable files it prints SKIPPED and exits 0.

#include "../src/dsdcc_decoder.hpp"
#include <cstdio>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace dsdsrv;

namespace {

int g_failures = 0;
void check(bool c, const std::string& what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what.c_str());
    if (!c) ++g_failures;
}

struct Collected {
    std::set<std::string> kinds, sources, talkgroups, extras, sync_types;
    bool started = false;
};

// Does any element of the set contain the substring?
bool any_contains(const std::set<std::string>& s, const std::string& sub) {
    for (const auto& e : s) if (e.find(sub) != std::string::npos) return true;
    return false;
}

Collected run(const char* path, const std::string& mode) {
    Collected c;
    FILE* f = std::fopen(path, "rb");
    if (!f) return c;
    std::vector<int16_t> pcm; int16_t s;
    while (std::fread(&s, sizeof(s), 1, f) == 1) pcm.push_back(s);
    std::fclose(f);

    DsdccDecoder dec;
    DsdccConfig cfg;
    cfg.mode = mode;
    cfg.input_sample_rate_hz = 48000;
    c.started = dec.start(cfg,
        [&](const DsdEvent& e) {
            c.kinds.insert(e.kind);
            if (!e.source_id.empty()) c.sources.insert(e.source_id);
            if (!e.talkgroup.empty()) c.talkgroups.insert(e.talkgroup);
            if (!e.extra.empty()) c.extras.insert(e.extra);
            // sync events carry sync_type=<flavor> in extra
            if (e.kind == "sync" && e.extra.rfind("sync_type=", 0) == 0)
                c.sync_types.insert(e.extra.substr(std::string("sync_type=").size()));
        }, nullptr);
    for (std::size_t i = 0; i < pcm.size(); i += 1024) {
        std::size_t n = std::min<std::size_t>(1024, pcm.size() - i);
        dec.write_audio(pcm.data() + i, n);
    }
    dec.stop();
    return c;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("SKIPPED: usage: test_dstar_ysf_dsdcc <dstar_f1zil_1.dis> <ysf_f5zoo.dis>\n");
        return 0;
    }

    // ---- D-STAR ----
    {
        Collected c = run(argv[1], "dstar");
        if (!c.started) { std::printf("SKIPPED: cannot open D-STAR sample %s\n", argv[1]); return 0; }
        std::printf("D-STAR:\n");
        check(c.kinds.count("call") == 1, "D-STAR call info surfaces as a call event");
        check(any_contains(c.sources, "F1NSR"),
              "call event carries the source callsign (F1NSR)");
        check(c.talkgroups.count("CQCQCQ") == 1,
              "call event carries the destination callsign (CQCQCQ)");
        check(any_contains(c.extras, "rpt1=F1ZIL B"),
              "repeater path surfaces as rpt1=F1ZIL B");
        check(any_contains(c.extras, "radio_text=YANNICK ST RAPHAEL"),
              "slow-data radio text surfaces (YANNICK ST RAPHAEL)");
        check(c.sync_types.count("dstar") == 1 || c.sync_types.count("dstar_header") == 1,
              "sync acquisition reports a D-STAR sync flavor");
    }

    // ---- YSF ----
    {
        Collected c = run(argv[2], "ysf");
        if (!c.started) { std::printf("SKIPPED: cannot open YSF sample %s\n", argv[2]); return 0; }
        std::printf("YSF:\n");
        check(c.kinds.count("call") == 1, "YSF call info surfaces as a call event");
        check(any_contains(c.sources, "F1SER") || any_contains(c.sources, "F6FCE"),
              "call event carries a source callsign (F1SER / F6FCE)");
        check(any_contains(c.extras, "downlink=F5ZOO-R1") || any_contains(c.extras, "uplink=F5ZOO-R1"),
              "repeater uplink/downlink callsign surfaces (F5ZOO-R1)");
        check(any_contains(c.extras, "call_mode=group_cq"),
              "FICH call mode surfaces (group_cq)");
        check(any_contains(c.extras, "data_type=vd2"),
              "FICH data type surfaces (vd2)");
        check(c.sync_types.count("ysf") == 1, "sync acquisition reports the YSF sync flavor");
    }

    if (g_failures == 0) { std::printf("\nALL DSTAR/YSF DSDCC TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
