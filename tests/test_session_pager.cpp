// Full-stack paging test: the real dsd-server Session/Server, a real
// multimon-ng subprocess, and a real WebSocket client.
//
// For each audio fixture (raw s16 22050 Hz discriminator audio built by
// tools/make_pager_fixtures.sh -- gen-ng synthetic pages plus multimon-ng's
// bundled off-air POCSAG/FLEX recordings) it
//   1. decodes the audio DIRECTLY with multimon-ng -> reference page set;
//   2. FM-modulates the audio into float32 IQ (250 kHz, channel +25 kHz,
//      light noise), streams it with a paging `protocol` hint, and collects
//      the kind:"page" events that come back;
// and requires every reference page (capcode->talkgroup + text) to come back
// through the IQ path, plus hard-coded expectations for known content.
// Pages are read BEFORE any stop is sent, so this also proves they stream
// back live rather than only when the decoder is flushed.
//
// Usage: test_session_pager <fixtures dir>
// Env:   MULTIMON_NG (decoder binary; default "multimon-ng" on PATH)
//        DSD_TEST_PACE_MS (ms slept per 100 ms IQ block; default 4)
// Exit 77 (ctest SKIP) when multimon-ng with --json/FLEX_NEXT or the
// fixtures are missing.

#include "json_util.hpp"
#include "pager_events.hpp"
#include "pager_process.hpp"
#include "test_ws_client.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <thread>

using namespace dsdtest;
using dsdsrv::DsdEvent;
using dsdsrv::MultimonConfig;
using dsdsrv::MultimonProcess;
namespace json = dsdsrv::json;

namespace {

constexpr unsigned short kTestPort = 18769;
constexpr double kIqRate = 250'000.0;
constexpr double kOffset = 25'000.0;

std::vector<int16_t> read_raw(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<int16_t> out(bytes.size() / 2);
    std::memcpy(out.data(), bytes.data(), out.size() * 2);
    return out;
}

// Discriminator audio -> FM IQ: DC removed, 99.5th-percentile magnitude
// scaled to 4.5 kHz deviation, 0.3 s of carrier padding at each end.
std::vector<cf32> fm_modulate(const std::vector<int16_t>& audio) {
    const double fa = 22050.0;
    double mean = 0.0;
    for (int16_t v : audio) mean += v;
    mean /= std::max<std::size_t>(1, audio.size());
    std::vector<double> mag;
    for (int16_t v : audio) mag.push_back(std::fabs(v - mean));
    const std::size_t k = static_cast<std::size_t>(mag.size() * 0.995);
    std::nth_element(mag.begin(), mag.begin() + static_cast<long>(k), mag.end());
    const double peak = std::max(1.0, mag[k]);

    const std::size_t pad = static_cast<std::size_t>(0.3 * fa);
    std::vector<double> a(pad, 0.0);
    for (int16_t v : audio) a.push_back((v - mean) / peak);
    a.insert(a.end(), pad, 0.0);

    const std::size_t n = static_cast<std::size_t>(a.size() * kIqRate / fa);
    std::vector<cf32> iq(n);
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.05f);
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double pos = i * fa / kIqRate;
        const std::size_t j = static_cast<std::size_t>(pos);
        const double x = a[j] + (j + 1 < a.size() ? (a[j + 1] - a[j]) * (pos - j) : 0.0);
        phase += 2.0 * M_PI * (kOffset + 4500.0 * x) / kIqRate;
        iq[i] = cf32(static_cast<float>(std::cos(phase)) + noise(rng),
                     static_cast<float>(std::sin(phase)) + noise(rng));
    }
    return iq;
}

using PageKey = std::pair<std::string, std::string>; // talkgroup (capcode), message

std::set<PageKey> direct_decode(const std::vector<int16_t>& audio, const std::vector<std::string>& demods) {
    std::set<PageKey> out;
    std::mutex m;
    MultimonProcess p;
    MultimonConfig cfg;
    cfg.demods = demods;
    if (!p.start(cfg, [&](const std::string& line) {
            DsdEvent ev;
            if (dsdsrv::multimon_line_to_event(line, ev) && ev.kind == "page" && !ev.talkgroup.empty()) {
                std::lock_guard<std::mutex> lk(m);
                out.insert({ev.talkgroup, ev.message});
            }
        }))
        return out;
    std::vector<int16_t> pad(22050 / 2, 0);
    p.write_audio(pad.data(), pad.size());
    p.write_audio(audio.data(), audio.size());
    p.write_audio(pad.data(), pad.size());
    p.stop();
    return out;
}

int pace_ms() {
    const char* e = std::getenv("DSD_TEST_PACE_MS");
    return e ? std::atoi(e) : 4;
}

struct Case {
    std::string file;
    std::string protocol;            // hint sent in "start"
    std::vector<std::string> demods; // the same selection, for the reference
    PageKey expect;                  // must be received ("" talkgroup = none)
};

void run_case(const std::string& dir, const Case& c) {
    std::printf("--- %s (protocol \"%s\")\n", c.file.c_str(), c.protocol.c_str());
    const auto audio = read_raw(dir + "/" + c.file);
    check(!audio.empty(), c.file + ": fixture present");
    if (audio.empty()) return;
    const auto reference = direct_decode(audio, c.demods);
    check(!reference.empty(), c.file + ": direct multimon-ng decode finds pages");
    const auto iq = fm_modulate(audio);

    TestClient cl;
    if (!cl.connect(kTestPort)) { check(false, c.file + ": connect"); return; }
    cl.send_text(json::Writer().field("type", std::string("start"))
                     .field("sample_rate", kIqRate).field("freq_offset", kOffset)
                     .field("protocol", c.protocol).str());
    std::string msg;
    bool is_text = false;
    check(cl.read(msg, is_text) && msg.find("\"type\":\"started\"") != std::string::npos &&
              msg.find("\"udp_audio_port\":0") != std::string::npos,
          c.file + ": started (udp_audio_port 0)");

    const std::size_t blk = static_cast<std::size_t>(kIqRate / 10);
    for (std::size_t pos = 0; pos < iq.size(); pos += blk) {
        const std::size_t n = std::min(blk, iq.size() - pos);
        std::vector<uint8_t> bytes(n * sizeof(cf32));
        std::memcpy(bytes.data(), iq.data() + pos, bytes.size());
        cl.send_binary(bytes);
        std::this_thread::sleep_for(std::chrono::milliseconds(pace_ms()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // let the worker drain

    // Everything decoded so far has been sent; read until quiet. No stop has
    // been sent, so these pages arrived live (not via the stop-time flush).
    std::set<PageKey> got;
    int frames = 0, errors = 0;
    while (cl.read(msg, is_text, std::chrono::milliseconds(frames ? 1500 : 3000))) {
        if (!is_text) continue;
        ++frames;
        auto obj = json::parse_flat_object(msg);
        const std::string type = json::get_string(obj, "type");
        if (type == "error") { ++errors; note("error frame: " + msg); }
        if (type != "event" || json::get_string(obj, "kind") != "page") continue;
        check(json::get_string(obj, "extra").find("protocol=") == 0, c.file + ": page extra starts protocol=");
        const std::string tg = json::get_string(obj, "talkgroup");
        if (!tg.empty()) got.insert({tg, json::get_string(obj, "message")});
    }
    cl.close();

    check(errors == 0, c.file + ": no error frames");
    std::size_t missing = 0;
    for (const auto& r : reference)
        if (!got.count(r)) { ++missing; note("missing via IQ: [" + r.first + "] " + r.second); }
    check(missing == 0, c.file + ": all " + std::to_string(reference.size()) +
                            " directly-decoded pages came back via IQ (got " + std::to_string(got.size()) + ")");
    if (!c.expect.first.empty())
        check(got.count(c.expect) == 1, c.file + ": expected page [" + c.expect.first + "] '" + c.expect.second + "'");
}

void run_protocol_checks() {
    std::printf("--- protocol checks\n");
    TestClient cl;
    if (!cl.connect(kTestPort)) { check(false, "connect"); return; }
    const std::string& caps = cl.capabilities();
    check(caps.find("pager-auto; pocsag; pocsag512; pocsag1200; pocsag2400; flex") != std::string::npos,
          "capabilities advertise the paging hints");
    check(caps.find("burst; page; unknown") != std::string::npos, "capabilities advertise kind page");
    check(caps.find("\"extra_keys_pager\":\"protocol; baud; message_type") != std::string::npos,
          "capabilities advertise extra_keys_pager");

    std::string msg;
    bool is_text = false;
    cl.send_text(R"({"type":"start","protocol":"pager-auto","pocsag_mode":"klingon"})");
    check(cl.read(msg, is_text) && msg.find("unknown pocsag_mode: klingon") != std::string::npos,
          "bad pocsag_mode -> error");

    // Hint normalization: underscores/case, like every other hint.
    cl.send_text(R"({"type":"start","protocol":"PAGER_AUTO","sample_rate":48000,"pocsag_mode":"alpha"})");
    check(cl.read(msg, is_text) && msg.find("\"type\":\"started\"") != std::string::npos, "PAGER_AUTO starts");
    // The multimon-ng child holds none of the server's sockets (stdio aside:
    // it inherits stderr on purpose). See child_fds.hpp.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::size_t leaked = 0;
    const auto kids = child_pids();
    for (int pid : kids) leaked += inherited_socket_count(pid, 3);
    check(!kids.empty() && leaked == 0, "multimon-ng child holds none of the server's sockets");
    cl.send_text(R"({"type":"set_gain","gain":30000})");       // accepted, no reply
    cl.send_text(R"({"type":"set_freq_offset","hz":1000})");   // applies to the pager demod
    cl.send_text(R"({"type":"stop"})");
    check(!cl.read(msg, is_text, std::chrono::milliseconds(500)), "set_gain / set_freq_offset / stop: no reply");
    cl.close();

    const char* old = std::getenv("MULTIMON_NG");
    const std::string saved = old ? old : "";
    setenv("MULTIMON_NG", "/nonexistent/multimon-ng", 1);
    TestClient c2;
    if (c2.connect(kTestPort)) {
        c2.send_text(R"({"type":"start","protocol":"flex"})");
        check(c2.read(msg, is_text) && msg.find("failed to start pager backend") != std::string::npos,
              "missing decoder -> 'failed to start pager backend'");
        c2.close();
    }
    if (old) setenv("MULTIMON_NG", saved.c_str(), 1);
    else unsetenv("MULTIMON_NG");
}

bool multimon_ok() {
    const std::string cmd = "'" + dsdsrv::default_multimon_path() + "' -h 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    return out.find("--json") != std::string::npos && out.find("FLEX_NEXT") != std::string::npos;
}

} // namespace

int main(int argc, char** argv) {
    if (!multimon_ok()) {
        std::printf("SKIP: multimon-ng with --json/FLEX_NEXT not found (set MULTIMON_NG)\n");
        return 77;
    }
    const std::string dir = argc > 1 ? argv[1] : "pager_fixtures";
    if (!std::ifstream(dir + "/gen_pocsag1200_alpha.raw")) {
        std::printf("SKIP: fixtures not found in %s (run tools/make_pager_fixtures.sh)\n", dir.c_str());
        return 77;
    }

    net::io_context server_ioc{2};
    dsdsrv::Server server(server_ioc, tcp::endpoint{net::ip::make_address("127.0.0.1"), kTestPort});
    std::vector<std::thread> pool;
    for (int i = 0; i < 2; ++i) pool.emplace_back([&] { server_ioc.run(); });

    run_protocol_checks();

    const std::vector<std::string> all = {"POCSAG512", "POCSAG1200", "POCSAG2400", "FLEX_NEXT"};
    const std::vector<std::string> pocsag = {"POCSAG512", "POCSAG1200", "POCSAG2400"};
    const std::vector<Case> cases = {
        {"gen_pocsag1200_alpha.raw", "pager-auto", all, {"424242", "HELLO PAGER 123"}},
        {"gen_pocsag512_numeric.raw", "pocsag", pocsag, {"1000", "5551234"}},
        {"gen_pocsag2400_alpha.raw", "POCSAG 2400", {"POCSAG2400"}, {"2097151", "FAST 2400 BAUD PAGE"}},
        {"gen_flex_alpha.raw", "flex", {"FLEX_NEXT"}, {"1122334", "FLEX TEST MSG"}},
        {"real_pocsag512.raw", "pocsag512", {"POCSAG512"}, {"", ""}},
        {"real_pocsag1200.raw", "pager-auto", all, {"273040", "+++TIME=0008300324+++TIME=0008300324"}},
        {"real_pocsag2400.raw", "pocsag2400", {"POCSAG2400"}, {"", ""}},
        {"real_flex1600.raw", "flex", {"FLEX_NEXT"}, {"1523020", "Passage Ambulance Wilhelminabrug Leiden"}},
    };
    for (const auto& c : cases) run_case(dir, c);

    server_ioc.stop();
    for (auto& t : pool) t.join();
    if (g_failures) {
        std::printf("test_session_pager: %d failure(s)\n", g_failures.load());
        return 1;
    }
    std::printf("test_session_pager: all checks passed\n");
    return 0;
}
