// test_pending_decode.cpp
//
// Integration test for PendingDecoder (src/pending_decode.hpp): replay a stored
// capture through a (fake) dsd-fme and confirm the decoded calls are ingested
// back into the association model under a fresh stream anchored at the
// capture's original time. Uses the same fake dsd-fme binary as test_session
// (built as test-fake-dsd-fme, copied to "dsd-fme"), located via the
// TEST_FAKE_DSD_FME_DIR env var the CMake test sets. Does not exercise real
// decryption -- that lives in dsd-fme -- only the capture-replay plumbing: the
// file is fed to the decoder's dsd-fme, whose emitted events become calls.

#include "../src/pending_decode.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace dsdsrv;
namespace fs = std::filesystem;

static int g_failures = 0;
static void check(bool c, const char* what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what);
    if (!c) ++g_failures;
}

int main() {
    std::printf("test_pending_decode\n");

    const char* dir = std::getenv("TEST_FAKE_DSD_FME_DIR");
    if (!dir || !dir[0]) {
        std::printf("  SKIP: TEST_FAKE_DSD_FME_DIR not set (run via ctest)\n");
        return 0;
    }
    const std::string dsd_fme = std::string(dir) + "/dsd-fme";
    if (!fs::exists(dsd_fme)) {
        std::printf("  SKIP: fake dsd-fme not found at %s\n", dsd_fme.c_str());
        return 0;
    }

    const std::string work = "test_pending_decode.tmp";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);

    // A one-second capture: content is irrelevant to the fake (it counts stdin
    // bytes and emits a voice event per chunk), so any S16 pattern will do.
    const std::string s16 = work + "/pend_cap.s16";
    {
        std::ofstream f(s16, std::ios::binary);
        std::vector<int16_t> block(PendingCapture::kRate, 0);  // 1 s @ 48 kHz
        for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<int16_t>((i % 2000) - 1000);
        f.write(reinterpret_cast<const char*>(block.data()),
                static_cast<std::streamsize>(block.size() * sizeof(int16_t)));
    }

    PendingMeta meta;
    meta.id = "pend_cap";
    meta.session = 1;
    meta.started_ms = 1'700'000'000'000LL;   // a fixed point in time to anchor to
    meta.mode = "s";                          // DMR (the fake ignores it)
    meta.protocol = "dmr";
    meta.center_freq_hz = 440'425'000;
    meta.algkids = "AA:05";

    AssocModel model;
    // Audio recording on, so a recovered call that gets voice keeps a file; the
    // fake only sends one preroll packet so this mainly exercises the path.
    model.start_audio(work + "/audio", 64ull * 1024 * 1024, 0);

    const std::size_t before = model.call_count();
    PendingDecoder::Result r = PendingDecoder::decode(model, meta, s16, /*key_args=*/{}, dsd_fme, /*udp_port=*/0);

    check(r.ok, "decode() reports ok");
    check(r.bytes == static_cast<std::uint64_t>(PendingCapture::kRate) * 2, "decode fed the whole capture");
    check(model.call_count() > before, "a call was recovered into the model");
    check(r.recovered > 0, "result counts the recovered call(s)");
    std::printf("  (recovered %zu call(s), %llu bytes: \"%s\")\n", r.recovered,
                static_cast<unsigned long long>(r.bytes), r.message.c_str());

    // A missing capture fails cleanly rather than crashing.
    PendingDecoder::Result bad = PendingDecoder::decode(model, meta, work + "/nope.s16", {}, dsd_fme, 0);
    check(!bad.ok, "decode of a missing capture fails cleanly");

    fs::remove_all(work, ec);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
