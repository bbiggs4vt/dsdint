// test_session_dsdcc.cpp
//
// Full-stack integration test for the DSDcc backend build of the server
// (dsd-server-dsdcc's configuration: session.cpp compiled with
// -DDSD_USE_DSDCC_BACKEND): starts a real Server, connects a real
// WebSocket client, and streams a REAL DMR transmission through the
// entire pipeline --
//
//   client FM-modulates DSDcc's bundled discriminator capture
//   (samples/dmr_it_8.dis) into IQ -> WebSocket binary frames ->
//   Session -> FmDemodulator (recovers the discriminator audio) ->
//   DsdccDecoder (decodes the DMR) -> decoded voice + talkgroup events
//   back over the WebSocket.
//
// So unlike test_session.cpp (whose fake dsd-fme emits synthetic events
// regardless of input), a pass here means actual RF-derived DMR was
// demodulated and decoded end to end: the client must get back real
// 8 kHz voice PCM and an event carrying the talkgroup that's genuinely
// in the capture (150607).
//
// The modulation inverse: FmDemodulator's discriminator outputs
// atan2(phase increment) * disc_gain as int16. With IQ at 48 kHz in and
// 48 kHz out, decimation is 1 and the resampler is pass-through, so
// synthesizing IQ with phase increment = capture_sample / disc_gain per
// sample reproduces the capture at the demod output (verified offline:
// the round trip is sample-exact -- 151680 decoded samples, the same
// count dsdccx produces from the raw file).
//
// Usage: test_session_dsdcc <path-to-dmr_it_8.dis>
// Prints SKIPPED and exits 0 without the sample file, same as
// test_dsdcc_decoder.

#include "session.hpp"
#include "json_util.hpp"
#include "test_ws_client.hpp"

#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace dsdsrv;
using namespace dsdtest;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

// Own port; see the notes in the other two test binaries about ctest
// parallelism and Server's silent bind failure.
constexpr unsigned short kTestPort = 18767;

constexpr float kDiscGain = 26000.0f;

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("SKIPPED: no sample file argument. Pass the path to DSDcc's "
                    "samples/dmr_it_8.dis to run this test.\n");
        return 0;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::printf("SKIPPED: cannot open sample file %s\n", argv[1]);
        return 0;
    }

    net::io_context server_ioc{2};
    Server server(server_ioc, tcp::endpoint{net::ip::make_address("0.0.0.0"), kTestPort});
    std::vector<std::thread> pool;
    for (int i = 0; i < 2; ++i) pool.emplace_back([&server_ioc] { server_ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::printf("test_session_dsdcc: real DMR through the full server stack\n");

    TestClient client;
    check(client.connect(kTestPort), "connects to server");

    // 48 kHz IQ in / 48 kHz discriminator out -- the rates the
    // modulation inverse above is built on. The DSDcc backend requires
    // the 48 kHz output rate anyway (session.cpp hardcodes it).
    check(client.send_text(json::Writer()
        .field("type", std::string("start"))
        .field("sample_rate", 48000.0)
        .field("channel_bandwidth", 12500.0)
        .field("freq_offset", 0.0)
        .field("gain", static_cast<double>(kDiscGain))
        .str()), "sends start message");

    std::string resp; bool is_text = false;
    bool got_started = false;
    for (int i = 0; i < 5 && !got_started; ++i) {
        if (!client.read(resp, is_text, std::chrono::seconds(5))) break;
        if (!is_text) continue;
        auto obj = json::parse_flat_object(resp);
        if (json::get_string(obj, "type") == "started") got_started = true;
    }
    check(got_started, "pipeline started (DSDcc backend, no subprocess)");

    // FM-modulate the capture into IQ and stream it. Paced: Session's
    // IQ queue caps at 64 blocks and drops the oldest on overflow, so
    // flooding at full socket speed would corrupt the signal by design.
    // ~10 ms per 4096-sample block is ~8.5x realtime -- fast enough to
    // keep the test short, slow enough that the demod worker (which
    // runs far faster than realtime) never falls 64 blocks behind.
    // Pacing between 85 ms IQ blocks. The 10 ms default (~8.5x realtime)
    // assumes native execution speed; on a slow target -- QEMU user-mode
    // emulation runs this pipeline ~40x slower than native -- the demod
    // worker falls behind, the session's 64-block IQ queue drops the
    // oldest blocks by design, and the dropped IQ shows up here as
    // missing decoded audio. DSD_TEST_PACE_MS overrides the pace for
    // such environments (the arm64/QEMU runs use 100).
    int pace_ms = 10;
    if (const char* pace_env = std::getenv("DSD_TEST_PACE_MS")) {
        int v = std::atoi(pace_env);
        if (v > 0) pace_ms = v;
    }

    double phase = 0.0;
    std::vector<int16_t> disc(4096);
    std::size_t total_in = 0, nread;
    bool send_failed = false;
    while ((nread = std::fread(disc.data(), sizeof(int16_t), disc.size(), f)) > 0) {
        std::vector<uint8_t> frame(nread * 2 * sizeof(float));
        auto* out = reinterpret_cast<float*>(frame.data());
        for (std::size_t i = 0; i < nread; ++i) {
            phase += disc[i] / static_cast<double>(kDiscGain);
            if (phase > M_PI) phase -= 2.0 * M_PI;
            if (phase < -M_PI) phase += 2.0 * M_PI;
            out[2 * i] = static_cast<float>(std::cos(phase));
            out[2 * i + 1] = static_cast<float>(std::sin(phase));
        }
        if (!client.send_binary(frame)) { send_failed = true; break; }
        total_in += nread;
        std::this_thread::sleep_for(std::chrono::milliseconds(pace_ms));
    }
    std::fclose(f);
    check(!send_failed, "streamed the full capture as IQ frames");
    std::printf("  streamed %zu IQ samples (%.1f s of signal)\n",
                total_in, total_in / 48000.0);

    // Collect what came back. Audio arrives as many small tagged binary
    // frames (one per ~20 ms voice frame); events as JSON text frames.
    std::size_t audio_samples = 0;
    std::set<std::string> talkgroups, kinds, color_codes, extras;
    // Generous frame budget: ~950 audio frames plus events; the loop
    // also ends on the first quiet 3 s once the pipeline has drained.
    for (int i = 0; i < 4000; ++i) {
        std::string r; bool text = false;
        if (!client.read(r, text, std::chrono::seconds(3))) break;
        if (text) {
            try {
                auto obj = json::parse_flat_object(r);
                if (json::get_string(obj, "type") == "event") {
                    kinds.insert(json::get_string(obj, "kind"));
                    auto tg = json::get_string(obj, "talkgroup");
                    if (!tg.empty()) talkgroups.insert(tg);
                    auto cc = json::get_string(obj, "color_code");
                    if (!cc.empty()) color_codes.insert(cc);
                    auto ex = json::get_string(obj, "extra");
                    if (!ex.empty()) extras.insert(ex);
                }
            } catch (const std::exception&) {}
        } else if (!r.empty() && static_cast<uint8_t>(r[0]) == 0x01) {
            audio_samples += (r.size() - 1) / sizeof(int16_t);
        }
    }
    std::printf("  received %zu decoded audio samples (%.1f s at 8 kHz)\n",
                audio_samples, audio_samples / 8000.0);

    // Ground truth: the capture holds ~19 s of voice (151680 samples at
    // 8 kHz through this exact chain, verified offline). Same generous
    // band as test_dsdcc_decoder, for the same reasons.
    check(audio_samples > 100000,
          "received a substantial amount of decoded voice (>100k samples)");
    check(talkgroups.count("150607") == 1,
          "an event carries the capture's real talkgroup (150607)");
    check(color_codes.count("4") == 1,
          "an event carries the capture's DMR color code (4)");
    check(kinds.count("voice") == 1, "voice-kind events were relayed");
    check(kinds.count("burst") == 1,
          "burst-kind events surface address-free slot activity");
    check(extras.count("sync_type=dmr_bs_data") == 1,
          "sync acquisition reports the sync flavor");

    // Note: client.read's timeout closes the connection (see
    // test_ws_client.hpp), so the collection loop ending means this
    // client is done -- no stop/close needed, and the server must
    // simply survive that teardown, which the checks above already
    // required it to outlive.

    server_ioc.stop();
    for (auto& t : pool) t.join();

    const int failures = g_failures.load();
    if (failures == 0) {
        std::printf("\nALL DSDCC SESSION TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", failures);
    return 1;
}
