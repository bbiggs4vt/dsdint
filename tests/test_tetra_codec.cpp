// test_tetra_codec.cpp
//
// Round-trip test for the TETRA voice codec binding, built and run ONLY in a
// DSD_WITH_TETRA_CODEC build (gated in CMake, like the DSDcc tests gate on the
// lib). Extracts the real captured UPLANE speech frame with the in-repo
// extractor, decodes it through make_tetra_voice_decoder()'s codec, and checks
// the result is real (non-silent) 8 kHz PCM. Without the codec this target
// isn't built.

#include "../src/tetra_voice.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace dsdsrv;

static int g_failures = 0;
static void check(bool c, const char* what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what);
    if (!c) ++g_failures;
}

// A real UPLANE/TCH_S report from the off-air capture (same fixture as
// test_tetra_voice.cpp), payload verbatim.
static const char* kRealReport =
    "{\"service\":\"UPLANE\",\"pdu\":\"TCH_S\",\"tn\":1,\"fn\":7,\"mn\":33,"
    "\"ssi\":16777215,\"usage marker\":0,\"downlink usage marker\":62,"
    "\"encryption mode\":0,\"uzsize\":1380,\"zsize\":152,\"frame\":\""
    "eJzlU1sOwCAI8yp7Xo5DGE7OshgjlPrh5zKXTQy0TcFtUouaWi1tf6N+7hGeeB2i/D6+qDNwHsN"
    "q8Y2anQG1W7zLwGBuphy1/cM0fJcYV3aCtX4GnCt6R9dqh8Qsm2PsPTLM+zqbM/Yuu0fWfKdmWK"
    "ardgpObVRn5phFleiT3wG8+/lP4LF3oVYW1yWriC+u+xcuH405tZI=\"}";

int main() {
    std::printf("test_tetra_codec: extraction -> ACELP codec -> PCM\n");

    auto dec = make_tetra_voice_decoder();
    check(dec != nullptr, "make_tetra_voice_decoder() returns a decoder in a codec build");
    if (!dec) { std::printf("\n%d CHECK(S) FAILED\n", g_failures); return 1; }

    std::vector<int16_t> frame;
    check(tetrakit_extract_speech_frame(kRealReport, frame), "real UPLANE frame extracts");
    check(frame.size() == 690, "extracted frame is 690 int16");

    std::vector<int16_t> pcm;
    check(dec->decode_frame(frame, pcm, /*frame_stealing=*/false), "decode_frame succeeds");
    check(pcm.size() == 480, "decode yields 480 int16 (60 ms @ 8 kHz)");

    // Real speech: non-constant with meaningful energy (silence would be ~0).
    double sumsq = 0; int peak = 0; bool varied = false;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        sumsq += double(pcm[i]) * pcm[i];
        peak = std::max(peak, std::abs((int)pcm[i]));
        if (i && pcm[i] != pcm[0]) varied = true;
    }
    double rms = pcm.empty() ? 0 : std::sqrt(sumsq / pcm.size());
    std::printf("  (rms=%.1f peak=%d)\n", rms, peak);
    check(varied, "decoded PCM is not constant");
    check(peak > 50 && rms > 20, "decoded PCM has real audio energy");

    if (g_failures == 0) {
        std::printf("\nALL TETRA CODEC TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
