// test_bp_key_dsdcc.cpp
//
// Verifies the DSDcc backend's DMR Basic Privacy key plumbing
// (DsdccConfig::bp_key -> DSDDecoder::setDMRBasicPrivacyKey). We have no
// BP-encrypted capture, so this does not assert "correct decryption of a
// real encrypted signal"; instead it proves, against DSDcc's own bundled
// *unencrypted* DMR capture (samples/dmr_it_8.dis), that the key actually
// reaches the voice-decode path and does nothing it shouldn't elsewhere:
//
//   1. A decode with bp_key set still starts and still recovers the call
//      metadata (talkgroup 150607) -- Basic Privacy XORs only the voice
//      frames, not the link-control that carries the addresses.
//   2. The decoded AUDIO differs from a keyless decode of the same file.
//      DSDcc applies the BP XOR to every DMR voice frame when a key is set
//      (there is no reliable in-band "encrypted" flag), so on this
//      unencrypted capture the key must perturb the audio -- which it can
//      only do if bp_key was genuinely wired through to the decoder.
//
// Usage: test_bp_key_dsdcc <path-to-dmr_it_8.dis>
// With a missing/unopenable file it prints SKIPPED and exits 0.

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

struct Result {
    bool started = false;
    unsigned long long audio_checksum = 0;
    std::size_t audio_samples = 0;
    std::set<std::string> talkgroups;
};

Result decode(const std::vector<int16_t>& pcm, unsigned bp_key) {
    Result r;
    DsdccDecoder dec;
    DsdccConfig cfg;                 // defaults: mode "dmr", 48000 Hz
    cfg.bp_key = bp_key;
    r.started = dec.start(cfg,
        [&](const DsdEvent& e) {
            if (!e.talkgroup.empty()) r.talkgroups.insert(e.talkgroup);
        },
        [&](const int16_t* a, std::size_t n) {
            r.audio_samples += n;
            for (std::size_t i = 0; i < n; ++i)
                r.audio_checksum = r.audio_checksum * 131 + static_cast<uint16_t>(a[i]);
        });
    if (!r.started) return r;
    for (std::size_t i = 0; i < pcm.size(); i += 4096) {
        std::size_t n = std::min<std::size_t>(4096, pcm.size() - i);
        dec.write_audio(pcm.data() + i, n);
    }
    dec.stop();
    return r;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("SKIPPED: usage: test_bp_key_dsdcc <dmr_it_8.dis>\n");
        return 0;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::printf("SKIPPED: cannot open sample %s\n", argv[1]); return 0; }
    std::vector<int16_t> pcm; int16_t s;
    while (std::fread(&s, sizeof(s), 1, f) == 1) pcm.push_back(s);
    std::fclose(f);

    Result plain = decode(pcm, 0);   // no key (baseline)
    Result keyed = decode(pcm, 1);   // BP key number 1

    check(plain.started, "decoder starts without a key");
    check(keyed.started, "decoder starts with a BP key set");
    check(plain.audio_samples > 100000, "keyless decode produces the expected voice audio");
    check(keyed.audio_samples > 100000, "keyed decode still produces voice audio");
    // Metadata is unaffected by BP (it rides in link control, not voice).
    check(plain.talkgroups.count("150607") == 1, "keyless decode recovers talkgroup 150607");
    check(keyed.talkgroups.count("150607") == 1, "keyed decode still recovers talkgroup 150607");
    // The key must actually perturb the voice frames -> different audio.
    check(plain.audio_checksum != keyed.audio_checksum,
          "BP key changes the decoded audio (key reaches the voice path)");

    if (g_failures == 0) { std::printf("\nALL BP KEY DSDCC TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
