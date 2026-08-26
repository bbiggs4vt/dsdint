// test_dpmr_dsdcc.cpp
//
// Verifies the DSDcc backend's dPMR metadata path against DSDcc's own
// bundled real dPMR capture (samples/dpmr.dis). In "dpmr" mode the
// wrapper reads getOwnId()/getCalledId() and emits call events carrying
// source_id / talkgroup. (getColorCode() is unreliable for dPMR in
// DSDcc, so this backend leaves color_code empty; the dsd-fme backend
// reports the channel code.)

#include "../src/dsdcc_decoder.hpp"
#include <cstdio>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace dsdsrv;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("SKIPPED: usage: test_dpmr_dsdcc <dpmr.dis>\n");
        return 0;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::printf("SKIPPED: cannot open sample %s\n", argv[1]); return 0; }
    std::vector<int16_t> pcm; int16_t s;
    while (std::fread(&s, sizeof(s), 1, f) == 1) pcm.push_back(s);
    std::fclose(f);

    std::set<std::string> sources, talkgroups, kinds;
    DsdccDecoder dec;
    DsdccConfig cfg;
    cfg.mode = "dpmr";
    cfg.input_sample_rate_hz = 48000;
    bool ok = dec.start(cfg,
        [&](const DsdEvent& e) {
            kinds.insert(e.kind);
            if (!e.source_id.empty()) sources.insert(e.source_id);
            if (!e.talkgroup.empty()) talkgroups.insert(e.talkgroup);
        }, nullptr);

    int failures = 0;
    auto check = [&](bool c, const std::string& what) {
        std::printf("  %s: %s\n", c ? "OK" : "FAIL", what.c_str());
        if (!c) ++failures;
    };

    check(ok, "decoder starts in dpmr mode");
    for (std::size_t i = 0; i < pcm.size(); i += 1024) {
        std::size_t n = std::min<std::size_t>(1024, pcm.size() - i);
        dec.write_audio(pcm.data() + i, n);
    }
    dec.stop();

    // Ground truth from DSDcc's dPMR decoder on this file (own/called ids).
    check(kinds.count("call") == 1, "dPMR metadata surfaces as call events");
    check(sources.count("302") == 1, "dPMR event carries a decoded source id (own 302)");
    check(talkgroups.count("14653") == 1, "dPMR event carries a decoded talkgroup (called 14653)");

    if (failures == 0) { std::printf("\nALL DPMR DSDCC TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", failures);
    return 1;
}
