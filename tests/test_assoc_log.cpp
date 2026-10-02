// test_assoc_log.cpp
//
// Recording + replay of the network explorer's association model
// (assoc_log.hpp, assoc_replay.hpp): the line codec round-trips any bytes,
// and a recorded session mix replays into a fresh model with byte-identical
// explorer output -- the property that makes a user's recording useful for
// offline analysis.

#include "../src/assoc_replay.hpp"
#include "../src/dsd_process.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <zlib.h>

using namespace dsdsrv;
namespace fs = std::filesystem;

static int g_failures = 0;
static void check(bool c, const std::string& what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what.c_str());
    if (!c) ++g_failures;
}
static void line(AssocModel& m, std::uint64_t sid, const std::string& l, std::int64_t t) {
    m.ingest(sid, classify_dsd_fme_line(l), t);
}
// Everything one model saw, read back as text (for content checks).
static std::string slurp_gz(const std::string& p) {
    gzFile f = gzopen(p.c_str(), "rb");
    std::string s;
    char b[4096];
    int n;
    while (f && (n = gzread(f, b, sizeof b)) > 0) s.append(b, static_cast<std::size_t>(n));
    if (f) gzclose(f);
    return s;
}

// A realistic mix of streams and lifecycle operations, timestamps from t0.
static void scenario(AssocModel& m, std::int64_t t0) {
    // P25 control channel: identity arrives piecemeal; grants + voice + flags.
    m.begin_stream(1, "p25p1", t0);
    line(m, 1, "Build Version: AW -128-NOTFOUND ", t0 + 5);           // banner
    line(m, 1, "17:30:46 Sync: +P25p1 NAC/CC: 293; RFSS: 004; Site: 012;  TSBK", t0 + 10);
    line(m, 1, " LRA [00] CFVA [3] RFSS[004] SITE [012] SYSID [3A1]", t0 + 20);
    line(m, 1, " CHAN-T [52E6] CHAN-R [50D7] SSC [70] WACN [BEE00]", t0 + 30);
    line(m, 1, " ADJSTS 6 RFSS[004] SITE [013] SYSID [3A1]", t0 + 40);
    line(m, 1, "17:30:47 Sync: +P25p1 NAC/CC: 293;  TSBK", t0 + 50);
    line(m, 1, "P25 TGT: 00000100; SRC: 00012001; NAC: 293; ", t0 + 100);
    line(m, 1, " P25 LCW  Group Call; Emergency", t0 + 120);
    line(m, 1, "P25 TGT: 00013002; SRC: 00012003; NAC: 293; ", t0 + 200);
    line(m, 1, " P25 LCW  Unit to Unit Voice Channel User", t0 + 220);      // private flip
    line(m, 1, " HDU  ALG ID: 0x84 KEY ID: 0x0001 MI: 0x0123456789ABCDEF", t0 + 230);
    // A second P25 stream on the same system hears the TG 100 call (dedup).
    m.begin_stream(2, "p25p1", t0 + 60);
    line(m, 2, "17:30:47 Sync: +P25p1 NAC/CC: 293;  LDU1", t0 + 70);
    line(m, 2, "P25 TGT: 00000100; SRC: 00012001; NAC: 293; ", t0 + 110);
    line(m, 2, "17:30:48 Sync: +P25p1 NAC/CC: 293; LDU2", t0 + 300);
    // DMR direct-mode SMS, as in the real capture (incl. a placeholder CC=00).
    m.begin_stream(3, "dmr", t0 + 400);
    line(m, 3, "19:58:33 Sync: +DMR MS/DM MODE/MONO | Color Code=00 | CSBK", t0 + 405);
    line(m, 3, "19:58:33 Sync: +DMR MS/DM MODE/MONO | Color Code=01 | CSBK", t0 + 410);
    line(m, 3, " Preamble CSBK - Group Data - Source: 123 - Target: 1 ", t0 + 420);
    line(m, 3, "19:58:33 Sync: +DMR MS/DM MODE/MONO | Color Code=01 | DATA ", t0 + 430);
    line(m, 3, " Slot 1 Data Header - Group - Unconfirmed Delivery - Source: 123 Target: 1 ", t0 + 440);
    DsdEvent sms; sms.kind = "message"; sms.slot = "1"; sms.message = "test \x01\x1b[0m\xff \"q\" \\"; // nasty bytes
    sms.raw_line = "DMR short-data PDU (dsd-fme -Z): test";
    m.ingest(3, sms, t0 + 450);
    // A dead stream (banner only) and a sync-only stream that gets pruned.
    m.begin_stream(4, "dstar", t0 + 500);
    line(m, 4, "Decoding DSTAR", t0 + 510);
    m.begin_stream(5, "dmr", t0 + 520);
    for (int k = 0; k < 3; ++k) line(m, 5, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=09 | VC6 ", t0 + 530 + k);
    m.end_stream(5, t0 + 600);
    m.end_stream(4, t0 + 610);
    m.remove_session(4, t0 + 620);
    // A session with a known channel frequency, retuned mid-call.
    m.begin_stream(6, "dmr", t0 + 700, "", 434425000);
    for (int k = 0; k < 2; ++k) line(m, 6, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=02 | VC6 ", t0 + 710 + k);
    line(m, 6, " SLOT 1 TGT=77 SRC=7001 Group Call ", t0 + 720);
    m.retune_stream(6, 438500000, t0 + 800);
    for (int k = 0; k < 2; ++k) line(m, 6, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=02 | VC6 ", t0 + 810 + k);
    line(m, 6, " SLOT 1 TGT=77 SRC=7002 Group Call ", t0 + 820);
    // Later traffic, a gap (new call), a TDMA slot change.
    line(m, 3, " SLOT 1 TGT=9 SRC=3112 Group Call ", t0 + 6000);
    line(m, 3, " SLOT 1 TGT=10 SRC=3115 Group Call ", t0 + 7000);
    m.end_stream(2, t0 + 8000);
    m.remove_session(2, t0 + 8100);
}

int main() {
    std::printf("test_assoc_log\n");
    const fs::path dir = fs::temp_directory_path() / ("assoc_log_test_" + std::to_string(::getpid()));
    fs::remove_all(dir);

    // ---- line codec round-trips any bytes ----
    {
        DsdEvent e;
        e.kind = "call"; e.talkgroup = "100"; e.source_id = "2048"; e.slot = "1"; e.color_code = "0";
        e.alias = "K\xC3\xA9 \xFF"; e.extra = "alg_id=84; key_id=0001"; e.crc_error = "";
        std::string all;
        for (int b = 1; b < 256; ++b) all += static_cast<char>(b);
        e.raw_line = all;
        e.message = "line1\nline2\t\"quoted\" \\back";
        std::map<std::string, std::string> k;
        const std::string l = assoclog::event_line(1234567890123LL, 7, e);
        check(l.find('\n') == std::string::npos, "codec: one physical line per record, even with newlines in the text");
        bool printable = true;
        for (unsigned char ch : l) printable = printable && ch >= 0x20 && ch < 0x7f;
        check(printable, "codec: the record is pure printable ASCII (safe to copy/paste/upload)");
        check(assoclog::parse_line(l, k) && k["op"] == "ev" && k["t"] == "1234567890123" && k["s"] == "7", "codec: op/t/s parse");
        DsdEvent d = assoclog::line_event(k);
        check(d.raw_line == all && d.message == e.message && d.alias == e.alias && d.extra == e.extra &&
              d.kind == e.kind && d.talkgroup == "100" && d.source_id == "2048" && d.color_code == "0" && d.slot == "1",
              "codec: every field, all 255 byte values, round-trips exactly");
    }

    // ---- record a session mix, replay it, get byte-identical explorer output ----
    std::string path;
    {
        AssocModel live;
        live.set_identity("1234abcd5678ef90", "bench");
        live.set_since(500);
        check(live.start_recording(dir.string(), 0, false, 1000), "record: start_recording on an empty model");
        const auto rs = live.recording();
        path = rs.path;
        check(rs.on && !path.empty() && fs::exists(path), "record: file created (" + fs::path(path).filename().string() + ")");
        check(live.to_json(1000).find("\"rec\":{\"on\":true") != std::string::npos, "record: /net.json reports recording on");
        scenario(live, 2000);
        live.clear(9000);                                       // a Clear mid-recording
        line(live, 3, " SLOT 1 TGT=11 SRC=3120 Group Call ", 9500);
        line(live, 3, "19:58:40 Sync: +DMR MS/DM MODE/MONO | Color Code=01 | VC1 ", 9600);
        line(live, 3, "19:58:40 Sync: +DMR MS/DM MODE/MONO | Color Code=01 | VC2 ", 9700);
        for (int k = 0; k < 2; ++k)                             // the retuned stream after the Clear
            line(live, 6, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=02 | VC6 ", 9800 + k);
        line(live, 6, " SLOT 1 TGT=77 SRC=7003 Group Call ", 9900);
        const std::string live_json = live.to_json(10000);
        live.stop_recording(10000);
        check(!live.recording().on && live.recording().last_path == path, "record: stopped; last file remembered");

        const std::string text = slurp_gz(path);
        check(text.rfind("{\"op\":\"header\",\"v\":1,\"t\":1000,\"fresh\":true,\"instance\":\"1234abcd5678ef90\","
                         "\"name\":\"bench\",\"since\":500,\"max_calls\":5000}", 0) == 0,
              "file: starts with a fresh header naming the server run");
        check(text.find("\"why\":\"start\"") != std::string::npos && text.find("\"why\":\"stop\"") != std::string::npos,
              "file: start and stop snapshots present");
        check(text.find("Build Version") != std::string::npos, "file: even inputs the model ignores are recorded");
        check(text.find("\"freq\":434425000") != std::string::npos &&
                  text.find("{\"op\":\"tune\",\"t\":2800,\"s\":6,\"freq\":438500000}") != std::string::npos,
              "file: channel frequencies and retunes are recorded");
        check(recording_start_time(path) == 1000, "file: recording_start_time() reads the header");

        AssocModel replay;
        ReplayResult r;
        std::string err;
        check(replay_log(path, replay, r, std::numeric_limits<std::int64_t>::max(), &err), "replay: reads the file");
        check(r.fresh && r.bad == 0 && !r.truncated && r.clears == 1 && r.removes == 2 && r.begins == 6,
              "replay: header fresh, no bad lines, all lifecycle ops seen");
        const std::string rep_json = replay.to_json(r.stop_t);
        check(families_of(rep_json) == families_of(r.stop_model),
              "replay: explorer output matches the recorded 'stop' snapshot byte for byte");
        check(families_of(rep_json) == families_of(live_json), "replay: ...and the live model itself");
        check(live_json.find("\"key\":\"cc:2@438500000\"") != std::string::npos &&
                  live_json.find("\"freq\":438500000") != std::string::npos,
              "replay: ...including a channel-keyed network after a retune and a Clear");
        check(replay.instance() == live.instance() && replay.name() == "bench" && replay.since() == 9000 &&
              live.since() == 9000,
              "replay: takes on the recorded run's identity; the span restarts at the Clear, as live");
        check(live.import_export(replay.to_export_json(r.stop_t), "replay.json", 10000).status == "skipped",
              "replay: an export of the replay, imported into the server it recorded, is recognised as its own data");
        AssocModel before_clear;                                // the SMS was wiped by the Clear at t=9000
        ReplayResult bc;
        replay_log(path, before_clear, bc, 8999);
        check(before_clear.to_json(8999).find("test \\u0001\\u001b[0m") != std::string::npos,
              "replay: control bytes inside the SMS text survive into the replayed output");

        // Replaying only up to a moment reproduces the state at that moment.
        AssocModel part;
        ReplayResult pr;
        replay_log(path, part, pr, 2300);
        check(pr.t_last <= 2300 && pr.events > 0 && pr.events < r.events, "replay --at: stops at the requested time");
    }

    // ---- recording mid-run: resumed streams; fresh only if cleared first ----
    {
        AssocModel live;
        live.begin_stream(9, "auto", 100);
        line(live, 9, "17:30:46 Sync: +P25p1 NAC/CC: 293;  TSBK", 150);   // auto -> p25
        line(live, 9, "P25 TGT: 00000100; SRC: 00012001; NAC: 293; ", 160);
        check(live.start_recording(dir.string(), 0, false, 200), "mid-run: start without clearing");
        live.stop_recording(300);
        check(slurp_gz(live.recording().last_path).find("\"fresh\":false") != std::string::npos,
              "mid-run: header says not fresh when data already existed");

        check(live.start_recording(dir.string(), 0, true, 1000), "mid-run: start with clear first");
        const std::string p2 = live.recording().path;
        const std::string text0 = slurp_gz(p2);
        line(live, 9, "17:30:50 Sync: +P25p1 NAC/CC: 293;  TSBK", 1100);
        line(live, 9, "17:30:51 Sync: +P25p1 NAC/CC: 293;  TSBK", 1110);
        line(live, 9, "P25 TGT: 00000200; SRC: 00012002; NAC: 293; ", 1200);
        live.stop_recording(1300);
        const std::string text = slurp_gz(p2);
        check(text.find("\"fresh\":true") != std::string::npos, "mid-run: clear-first makes the recording fresh");
        check(text.find("\"resumed\":true") != std::string::npos && text.find("\"family\":\"p25\"") != std::string::npos,
              "mid-run: the running stream is written as resumed, with its inferred family (auto -> p25)");
        AssocModel replay;
        ReplayResult r;
        replay_log(p2, replay, r);
        check(families_of(replay.to_json(r.stop_t)) == families_of(r.stop_model),
              "mid-run: clear-first recording replays to the recorded state exactly");
    }

    // ---- size cap and cut-off files ----
    {
        AssocModel live;
        live.start_recording(dir.string(), 2500, false, 1000);
        const std::string p = live.recording().path;
        scenario(live, 2000);
        check(live.recording().truncated, "cap: recording marks itself truncated at the size cap");
        live.stop_recording(9000);
        AssocModel replay;
        ReplayResult r;
        replay_log(p, replay, r);
        check(r.truncated && r.bad == 0, "cap: replay sees the truncation marker; the file is still valid");

        // An abruptly ended (plain-text, cut mid-line) file still replays.
        const std::string text = slurp_gz(path);
        const fs::path cut = dir / "cut.jsonl";
        std::ofstream(cut) << text.substr(0, text.size() / 2);
        AssocModel rc;
        ReplayResult rr;
        check(replay_log(cut.string(), rc, rr) && rr.events > 0 && rr.bad <= 1,
              "cut-off: a file cut mid-line replays everything before the cut");
    }

    fs::remove_all(dir);
    if (g_failures == 0) {
        std::printf("\nALL ASSOC LOG TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
