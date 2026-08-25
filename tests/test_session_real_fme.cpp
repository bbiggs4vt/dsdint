// test_session_real_fme.cpp
//
// The production configuration, end to end with nothing faked: the
// stock dsd-server build (Session + FmDemodulator + DsdProcess) with a
// REAL dsd-fme binary on PATH, decoding a REAL DMR transmission
// delivered over a real WebSocket.
//
//   client FM-modulates DSDcc's bundled discriminator capture into IQ
//   -> WebSocket binary frames -> Session -> FmDemodulator ->
//   DsdProcess -> actual dsd-fme (fork/exec, stdin pipe) -> its stdout
//   events and "-o udp" voice PCM -> back over the WebSocket.
//
// This is test_session_dsdcc.cpp's twin for the subprocess backend (see
// there for the modulation-inverse math; same 48 kHz in/out trick), and
// the difference from test_session.cpp is the point: that test uses a
// fake dsd-fme that emits synthetic events no matter what it hears,
// while a pass here requires the real decoder to actually decode the
// signal that traveled the whole pipeline.
//
// Usage: test_session_real_fme <dir-containing-real-dsd-fme> <dmr_it_8.dis>
// The first argument is a DIRECTORY prepended to PATH (the binary in it
// must be literally named "dsd-fme" -- that's how Session's default
// DsdProcessConfig locates it, and exercising that lookup is part of
// the test). Prints SKIPPED and exits 0 when either is missing.

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

// Own port; see the other test binaries' notes on ctest parallelism.
constexpr unsigned short kTestPort = 18768;

constexpr float kDiscGain = 26000.0f;

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("SKIPPED: usage: test_session_real_fme <dir with dsd-fme> <dmr_it_8.dis>\n");
        return 0;
    }
    std::string fme_check = std::string(argv[1]) + "/dsd-fme";
    FILE* probe = std::fopen(fme_check.c_str(), "rb");
    if (!probe) { std::printf("SKIPPED: no dsd-fme at %s\n", fme_check.c_str()); return 0; }
    std::fclose(probe);
    FILE* f = std::fopen(argv[2], "rb");
    if (!f) { std::printf("SKIPPED: cannot open sample file %s\n", argv[2]); return 0; }

    // Same PATH mechanism the fake-based test uses -- but pointing at
    // the real thing.
    std::string new_path = std::string(argv[1]) + ":" +
                           (std::getenv("PATH") ? std::getenv("PATH") : "");
    setenv("PATH", new_path.c_str(), 1);

    net::io_context server_ioc{2};
    Server server(server_ioc, tcp::endpoint{net::ip::make_address("0.0.0.0"), kTestPort});
    std::vector<std::thread> pool;
    for (int i = 0; i < 2; ++i) pool.emplace_back([&server_ioc] { server_ioc.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::printf("test_session_real_fme: real DMR through the stock server + real dsd-fme\n");

    TestClient client;
    check(client.connect(kTestPort), "connects to server");

    check(client.send_text(json::Writer()
        .field("type", std::string("start"))
        .field("sample_rate", 48000.0)
        .field("channel_bandwidth", 12500.0)
        .field("freq_offset", 0.0)
        .field("gain", static_cast<double>(kDiscGain))
        .str()), "sends start message");

    std::string resp; bool is_text = false;
    bool got_started = false;
    int udp_port = 0;
    for (int i = 0; i < 5 && !got_started; ++i) {
        if (!client.read(resp, is_text, std::chrono::seconds(5))) break;
        if (!is_text) continue;
        auto obj = json::parse_flat_object(resp);
        if (json::get_string(obj, "type") == "started") {
            got_started = true;
            udp_port = static_cast<int>(json::get_number(obj, "udp_audio_port", 0.0));
        }
    }
    check(got_started, "pipeline started (real dsd-fme spawned via PATH)");
    check(udp_port >= 40000 && udp_port <= 59000,
          "started reports a udp_audio_port in the allocator's range");

    // Modulate and stream, paced -- same rationale as
    // test_session_dsdcc.cpp (IQ queue caps at 64 blocks and drops).
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

    // Collect. Real dsd-fme's voice lags the input slightly (it decodes
    // as bursts complete), so keep reading until a 3 s quiet gap.
    std::size_t audio_samples = 0;
    std::set<std::string> talkgroups, sources, slots, kinds, color_codes;
    bool saw_ansi = false;
    for (int i = 0; i < 6000; ++i) {
        std::string r; bool text = false;
        if (!client.read(r, text, std::chrono::seconds(3))) break;
        if (text) {
            try {
                auto obj = json::parse_flat_object(r);
                if (json::get_string(obj, "type") == "event") {
                    kinds.insert(json::get_string(obj, "kind"));
                    auto tg = json::get_string(obj, "talkgroup");
                    if (!tg.empty()) talkgroups.insert(tg);
                    auto src = json::get_string(obj, "source_id");
                    if (!src.empty()) sources.insert(src);
                    auto slot = json::get_string(obj, "slot");
                    if (!slot.empty()) slots.insert(slot);
                    auto cc = json::get_string(obj, "color_code");
                    if (!cc.empty()) color_codes.insert(cc);
                    if (json::get_string(obj, "raw").find('\x1b') != std::string::npos) {
                        saw_ansi = true;
                    }
                }
            } catch (const std::exception&) {
                // A frame that fails the flat parser would itself be a
                // bug (invalid JSON reaching the client); count it.
                check(false, std::string("event frame parses as JSON: ") + r.substr(0, 60));
            }
        } else if (!r.empty() && static_cast<uint8_t>(r[0]) == 0x01) {
            audio_samples += (r.size() - 1) / sizeof(int16_t);
        }
    }
    std::printf("  received %zu audio int16s (%.1f s if 8 kHz stereo)\n",
                audio_samples, audio_samples / 16000.0);

    // Ground truth from test_dsd_process on the same capture: ~315k
    // int16s of 8 kHz stereo and events carrying TGT 19535 / SRC
    // 2222223 on slot 2. The demod round trip is sample-exact (verified
    // for the DSDcc variant), so the same numbers apply here.
    check(audio_samples > 200000,
          "received a substantial amount of decoded voice (>200k int16s)");
    check(talkgroups.count("19535") == 1,
          "an event carries the talkgroup real dsd-fme reports (19535)");
    check(sources.count("2222223") == 1,
          "an event carries the capture's source (2222223)");
    check(slots.count("2") == 1, "call activity attributed to TDMA slot 2");
    check(color_codes.count("4") == 1,
          "an event carries the capture's DMR color code (4, from Color Code=04)");
    check(kinds.count("sync") == 1, "sync events were relayed");
    check(!saw_ansi, "no ANSI escapes reach the client in raw event text");

    // See test_session_dsdcc.cpp on why there's no stop/close here: the
    // collection loop's final timeout closes the connection, and the
    // server surviving that teardown is part of what's being verified.

    server_ioc.stop();
    for (auto& t : pool) t.join();

    const int failures = g_failures.load();
    if (failures == 0) {
        std::printf("\nALL REAL-DSD-FME SESSION TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", failures);
    return 1;
}
