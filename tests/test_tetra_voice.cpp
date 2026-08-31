// test_tetra_voice.cpp
//
// Unit tests for the TETRA speech-frame extraction (src/tetra_voice.*): the
// base64 decoder and the zlib-inflate of a real tetra-kit UPLANE "frame"
// payload. The fixture below is a verbatim speech frame from the off-air
// capture that the whole chain (our demod -> tetra-kit decoder) produced.
// No codec needed -- the extraction is the license-clean half.

#include "../src/tetra_voice.hpp"
#include <cstdio>
#include <string>

using namespace dsdsrv;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& what) {
    std::printf("  %s: %s\n", cond ? "OK" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

// A real UPLANE/TCH_S report from the capture (frame payload verbatim).
const char* kRealReport =
    "{\"service\":\"UPLANE\",\"pdu\":\"TCH_S\",\"tn\":1,\"fn\":7,\"mn\":33,"
    "\"ssi\":16777215,\"usage marker\":0,\"downlink usage marker\":62,"
    "\"encryption mode\":0,\"uzsize\":1380,\"zsize\":152,\"frame\":\""
    "eJzlU1sOwCAI8yp7Xo5DGE7OshgjlPrh5zKXTQy0TcFtUouaWi1tf6N+7hGeeB2i/D6+qDNwHsN"
    "q8Y2anQG1W7zLwGBuphy1/cM0fJcYV3aCtX4GnCt6R9dqh8Qsm2PsPTLM+zqbM/Yuu0fWfKdmWK"
    "ardgpObVRn5phFleiT3wG8+/lP4LF3oVYW1yWriC+u+xcuH405tZI=\"}";
} // namespace

int main() {
    std::printf("test_tetra_voice: base64 + zlib speech-frame extraction\n");

    // ---- base64 decoder sanity ----
    {
        auto d = base64_decode("aGVsbG8=");      // "hello"
        std::string s(d.begin(), d.end());
        check(s == "hello", "base64: 'aGVsbG8=' -> 'hello'");
        check(base64_decode("Zm9vYmE=").size() == 5, "base64: length with padding");
        check(base64_decode("!!!!").empty() || true, "base64: junk tolerated (no crash)");
    }

    // ---- real speech frame extracts to 690 int16 (== 1380 bytes uzsize) ----
    {
        std::vector<int16_t> frame;
        bool ok = tetrakit_extract_speech_frame(kRealReport, frame);
        check(ok, "real UPLANE/TCH_S frame extracts");
        check(frame.size() == 690, "inflated speech frame is 690 int16 (1380 bytes)");
        // Not silence: a real frame has structure (mix of values).
        bool varied = false;
        for (std::size_t i = 1; i < frame.size(); ++i)
            if (frame[i] != frame[0]) { varied = true; break; }
        check(varied, "frame carries real (non-constant) data");
    }

    // ---- non-speech lines yield nothing ----
    {
        std::vector<int16_t> f;
        check(!tetrakit_extract_speech_frame(
                  "{\"service\":\"MLE\",\"pdu\":\"D-NWRK-BROADCAST\",\"ssi\":1}", f),
              "MLE broadcast: no speech frame");
        check(!tetrakit_extract_speech_frame("not json", f), "non-JSON: no speech frame");
        check(!tetrakit_extract_speech_frame(
                  "{\"service\":\"UPLANE\",\"pdu\":\"TCH_S\",\"ssi\":0}", f),
              "UPLANE without a frame field: nothing");
    }

    // ---- codec seam: absent by default ----
    {
        auto dec = make_tetra_voice_decoder();
        check(dec == nullptr, "no codec compiled in -> make_tetra_voice_decoder() is null");
    }

    if (g_failures == 0) std::printf("ALL TETRA VOICE TESTS PASSED\n");
    else std::printf("%d TETRA VOICE TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
