// test_assoc_audio.cpp
//
// Per-call audio for the network explorer (AssocModel::audio, assoc_audio.hpp):
// decoded voice goes to the right call (per TDMA slot, per stream), audio heard
// just before its call is decoded is kept, silence and encrypted calls are not
// recorded, two receivers' copies of one call make one recording, the WAV files
// are valid, the disk cap holds, only the store's own files are served, and
// audio never leaks into what a recording replays or an export imports.
// Each test block feeds samples of a distinct value, then reads the files back.

#include "../src/assoc_replay.hpp"
#include "../src/dsd_process.hpp"

#include <unistd.h>
#include <zlib.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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
static void tone(AssocModel& m, std::uint64_t sid, int slot, int16_t v, std::int64_t t, std::size_t n = 160,
                 bool keyed = false) {
    std::vector<int16_t> pcm(n, v);
    m.audio(sid, slot, pcm.data(), pcm.size(), keyed, t);
}
static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
static std::uint32_t le32(const std::string& s, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[at])) | static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 2])) << 16 | static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 3])) << 24;
}
// The samples of a WAV file (checks the header first).
static bool wav(const std::string& path, std::vector<int16_t>& out) {
    const std::string s = slurp(path);
    if (s.size() < 44 || s.compare(0, 4, "RIFF") || s.compare(8, 8, "WAVEfmt ") || s.compare(36, 4, "data")) return false;
    const std::uint32_t data = le32(s, 40);
    if (le32(s, 4) != 36 + data || data != s.size() - 44 || le32(s, 24) != 8000) return false;
    out.resize(data / 2);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<int16_t>(static_cast<unsigned char>(s[44 + 2 * i]) | static_cast<unsigned char>(s[45 + 2 * i]) << 8);
    return true;
}
static bool all_eq(const std::vector<int16_t>& v, int16_t x) {
    for (int16_t s : v) if (s != x) return false;
    return !v.empty();
}
// The model's calls (family f) as (src, audio file) pairs.
struct C { std::string src, tgt, audio; std::uint64_t ms; };
static std::vector<C> calls(const AssocModel& m, const char* f, std::int64_t now) {
    mjson::V j;
    mjson::parse(m.to_json(now), j);
    std::vector<C> out;
    const mjson::V* fs = j.get("families");
    const mjson::V* F = fs ? fs->get(f) : nullptr;
    const mjson::V* cs = F ? F->get("calls") : nullptr;
    if (cs) for (const auto& c : cs->a) out.push_back({c.str("src"), c.str("tgt"), c.str("audio"), static_cast<std::uint64_t>(c.num("audio_ms"))});
    return out;
}
static const C* by_src(const std::vector<C>& v, const std::string& src) {
    for (const auto& c : v) if (c.src == src) return &c;
    return nullptr;
}

static const char* kCC = "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=04 | VC6 ";

int main() {
    std::printf("test_assoc_audio\n");
    const fs::path dir = fs::temp_directory_path() / ("assoc_audio_test_" + std::to_string(::getpid()));
    fs::remove_all(dir);

    // ---- off by default: nothing recorded ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        for (int k = 0; k < 2; ++k) line(m, 1, kCC, 1000 + k);
        line(m, 1, " SLOT 1 TGT=9 SRC=100 Group Call ", 1100);
        tone(m, 1, 1, 1111, 1120);
        check(!m.audio_status().on && calls(m, "dmr", 1200).at(0).audio.empty() && !fs::exists(dir),
              "off: audio is not recorded unless enabled");
    }

    const auto file = [&](const std::string& sub, const std::string& name) { return (dir / sub / name).string(); };

    // ---- DMR: two calls at once, one per slot; each gets only its slot ----
    {
        AssocModel m;
        m.set_identity("abcdef0123456789", "t");
        check(m.start_audio((dir / "a").string(), 1 << 30, 0, 1000) && m.audio_status().on, "enable: audio store on");
        m.begin_stream(1, "dmr", 0);
        for (int k = 0; k < 2; ++k) line(m, 1, kCC, 1000 + k);
        line(m, 1, " SLOT 1 TGT=9 SRC=100 Group Call ", 1100);
        line(m, 1, " SLOT 2 TGT=10 SRC=200 Group Call ", 1105);
        for (int f = 0; f < 50; ++f) {                        // 1 s of each slot
            tone(m, 1, 1, 1111, 1120 + 20 * f);
            tone(m, 1, 2, 2222, 1120 + 20 * f);
        }
        auto cs = calls(m, "dmr", 2200);
        const C* pa = by_src(cs, "100");
        const C* pb = by_src(cs, "200");
        const C A = pa ? *pa : C{}, B = pb ? *pb : C{};       // copies: `cs` is reassigned below
        const C* a = pa ? &A : nullptr;
        const C* b = pb ? &B : nullptr;
        check(a && b && !a->audio.empty() && !b->audio.empty() && a->audio != b->audio,
              "slots: each of two concurrent calls has its own audio file");
        check(a && a->ms == 1000 && b && b->ms == 1000, "slots: /net.json gives each call's audio length (1.000 s)");
        std::vector<int16_t> s1, s2;
        check(a && b && wav(m.audio_path(a->audio, 1LL << 40), s1) && wav(m.audio_path(b->audio, 1LL << 40), s2),
              "wav: valid RIFF/WAVE, 8 kHz, sizes current while the call is still open");
        check(all_eq(s1, 1111) && s1.size() == 8000 && all_eq(s2, 2222) && s2.size() == 8000,
              "slots: slot 1's call holds only slot-1 audio, slot 2's only slot-2 audio");
        check(a && a->audio.find("abcdef01") != std::string::npos && CallAudioStore::valid_name(a->audio),
              "names: call_<start>_<instance>_<id>.wav");

        // The call ends (a new call on slot 1 closes it); later audio goes to the new call.
        line(m, 1, " SLOT 1 TGT=11 SRC=300 Group Call ", 2300);
        tone(m, 1, 1, 3333, 2320);
        cs = calls(m, "dmr", 2400);
        const C* c = by_src(cs, "300");
        std::vector<int16_t> s3, s1b;
        check(c && wav(m.audio_path(c->audio, 1LL << 40), s3) && all_eq(s3, 3333) && wav(m.audio_path(a->audio, 1LL << 40), s1b) && s1b.size() == 8000,
              "slots: a new call on the slot gets the audio from then on; the earlier file is final");
        check(by_src(cs, "100") && by_src(cs, "100")->ms == 1000, "slots: a finished call still reports its audio length");

        // A slot whose call went quiet long ago doesn't get stray audio.
        tone(m, 1, 2, 4444, 9000);
        std::vector<int16_t> s2b;
        check(wav(m.audio_path(b->audio, 1LL << 40), s2b) && s2b.size() == 8000, "slots: audio long after a call ended is not added to it");
        m.end_stream(1, 9100);
    }

    // ---- data calls: no audio; dsd-fme's copy of the other slot's voice goes there ----
    {
        AssocModel m;
        m.start_audio((dir / "d").string(), 1 << 30, 0, 1000);
        m.begin_stream(1, "dmr", 0);
        for (int k = 0; k < 2; ++k) line(m, 1, kCC, 1000 + k);
        // A data call alone: audible audio on its slot makes no file.
        line(m, 1, " Slot 1 Data Header - Group - Unconfirmed Delivery - Source: 123 Target: 1 ", 1100);
        for (int f = 0; f < 25; ++f) tone(m, 1, 1, 1500, 1110 + 20 * f);
        auto cs = calls(m, "dmr", 1700);
        check(by_src(cs, "123") && by_src(cs, "123")->audio.empty(), "data: a data-only call gets no audio file");
        // Voice on slot 2 while slot 1 carries data: dsd-fme copies the voice to
        // both channels and it arrives as slot 1 -- it belongs to the voice call.
        line(m, 1, " SLOT 2 TGT=10 SRC=200 Group Call ", 1750);
        for (int f = 0; f < 25; ++f) tone(m, 1, 2, 2222, 1760 + 20 * f);
        line(m, 1, " Slot 1 Data Header - Group - Unconfirmed Delivery - Source: 124 Target: 1 ", 2260);
        for (int f = 0; f < 25; ++f) tone(m, 1, 1, 2222, 2270 + 20 * f);
        for (int f = 0; f < 5; ++f) tone(m, 1, 1, 0, 2770 + 20 * f);       // an idle slot's silence: dropped
        cs = calls(m, "dmr", 2900);
        const C* v = by_src(cs, "200");
        const C* d = by_src(cs, "124");
        std::vector<int16_t> sv;
        check(d && d->audio.empty(), "data: the data call on the other slot gets none of the voice");
        check(v && wav(m.audio_path(v->audio, 1LL << 40), sv) && all_eq(sv, 2222) && sv.size() == 8000,
              "data: the voice copied onto the data call's slot goes to the voice call (1.000 s, nothing lost)");
        m.end_stream(1, 3000);
    }

    // ---- audio that arrives just before its call is decoded ----
    {
        AssocModel m;
        m.start_audio((dir / "b").string(), 1 << 30, 0, 1000);
        m.begin_stream(1, "dmr", 0);
        for (int k = 0; k < 2; ++k) line(m, 1, kCC, 1000 + k);   // stream is live, no call yet
        tone(m, 1, 1, 0, 1010);                                   // silence: never kept
        for (int f = 0; f < 10; ++f) tone(m, 1, 1, 500, 1020 + 20 * f);
        line(m, 1, " SLOT 1 TGT=9 SRC=100 Group Call ", 1250);
        for (int f = 0; f < 5; ++f) tone(m, 1, 1, 600, 1260 + 20 * f);
        const C* c = nullptr;
        auto cs = calls(m, "dmr", 1400);
        c = by_src(cs, "100");
        std::vector<int16_t> s;
        bool ok = c && wav(m.audio_path(c->audio, 1LL << 40), s) && s.size() == 15 * 160;
        for (std::size_t i = 0; ok && i < s.size(); ++i) ok = s[i] == (i < 1600 ? 500 : 600);
        check(ok, "pre-roll: the 200 ms heard before the call line start the call's recording, in order");

        // Silence alone on a call makes no file.
        line(m, 1, " SLOT 2 TGT=10 SRC=200 Group Call ", 1500);
        for (int f = 0; f < 5; ++f) tone(m, 1, 2, 0, 1510 + 20 * f);
        cs = calls(m, "dmr", 1700);
        check(by_src(cs, "200") && by_src(cs, "200")->audio.empty(), "silence: a call with only silent audio has no file");
        // Pre-roll older than a second is dropped.
        m.end_stream(1, 2000);
        m.begin_stream(2, "dmr", 3000);
        for (int k = 0; k < 2; ++k) line(m, 2, kCC, 3000 + k);
        tone(m, 2, 1, 700, 3010);
        line(m, 2, " SLOT 1 TGT=9 SRC=101 Group Call ", 5000);
        tone(m, 2, 1, 800, 5010);
        cs = calls(m, "dmr", 5100);
        std::vector<int16_t> s2;
        check(by_src(cs, "101") && wav(m.audio_path(by_src(cs, "101")->audio, 1LL << 40), s2) && all_eq(s2, 800),
              "pre-roll: audio from more than a second before the call isn't attached to it");
    }

    // ---- no empty or blip-only files ----
    {
        AssocModel m;
        m.start_audio((dir / "q").string(), 1 << 30, 0, 1000);
        m.begin_stream(1, "dmr", 0);
        for (int k = 0; k < 2; ++k) line(m, 1, kCC, 1000 + k);
        // Near-silence (decoder fade / comfort noise, peak < 64) on a call: no file.
        line(m, 1, " SLOT 1 TGT=9 SRC=100 Group Call ", 1100);
        for (int f = 0; f < 50; ++f) tone(m, 1, 1, 20, 1110 + 20 * f);
        auto cs = calls(m, "dmr", 2200);
        check(by_src(cs, "100") && by_src(cs, "100")->audio.empty(), "audible: a call with only near-silent audio gets no file");
        // A 0.1 s blip: recorded while the call runs, deleted when it ends.
        line(m, 1, " SLOT 2 TGT=10 SRC=200 Group Call ", 3000);
        for (int f = 0; f < 5; ++f) tone(m, 1, 2, 3000, 3010 + 20 * f);
        cs = calls(m, "dmr", 3200);
        const std::string blip = by_src(cs, "200") ? by_src(cs, "200")->audio : std::string();
        m.end_stream(1, 3300);
        cs = calls(m, "dmr", 3400);
        check(!blip.empty() && by_src(cs, "200")->audio.empty() && !fs::exists(file("q", blip)) && m.audio_status().files == 0,
              "min length: a recording under 0.2 s is deleted when its call ends (no stub files left)");
        // Every file left in the directory is a real recording.
        bool real = true;
        for (const auto& e : fs::directory_iterator(dir / "q")) real = real && fs::file_size(e.path()) > 44 + 2 * 1600;
        check(real, "min length: nothing but recordings of at least 0.2 s on disk");
    }

    // ---- slot not yet known (DMR direct mode before the first call line) ----
    {
        AssocModel m;
        m.start_audio((dir / "u").string(), 1 << 30, 0, 1000);
        m.begin_stream(1, "dmr", 0);
        line(m, 1, "19:50:50 Sync: +DMR MS/DM MODE/MONO | Color Code=01 | VC* ", 1000);
        line(m, 1, "19:50:50 Sync: +DMR MS/DM MODE/MONO | Color Code=01 | VC* ", 1010);
        for (int f = 0; f < 10; ++f) tone(m, 1, 0, 400, 1020 + 20 * f);    // slot unknown, no call yet
        line(m, 1, " SLOT 1 TGT=9 SRC=100 Group Call ", 1230);
        for (int f = 0; f < 5; ++f) tone(m, 1, 0, 500, 1240 + 20 * f);     // still unknown: same stream's call
        for (int f = 0; f < 10; ++f) tone(m, 1, 1, 600, 1340 + 20 * f);    // now slot 1
        auto cs = calls(m, "dmr", 1600);
        std::vector<int16_t> s;
        bool ok = by_src(cs, "100") && wav(m.audio_path(by_src(cs, "100")->audio, 1LL << 40), s) && s.size() == 25 * 160;
        for (std::size_t i = 0; ok && i < s.size(); ++i) ok = s[i] == (i < 1600 ? 400 : i < 2400 ? 500 : 600);
        check(ok, "slot unknown: audio from before the decoder knew the slot is kept, in order, then the slot's own");
    }

    // ---- a full list rolls off calls without audio first (and replays the same) ----
    {
        AssocModel m;
        m.set_identity("99aa88bb77cc66dd", "rx");
        m.set_since(500);
        m.set_max_calls(20);
        m.start_audio((dir / "k").string(), 1 << 30, 0, 1000);
        m.start_recording(dir.string(), 0, false, 1000);
        const std::string rpath = m.recording().path;
        m.begin_stream(1, "dmr", 1000);
        for (int k = 0; k < 2; ++k) line(m, 1, kCC, 1001 + k);
        std::int64_t t = 2000;
        auto call = [&](int src, bool with_audio) {
            line(m, 1, " SLOT 1 TGT=9 SRC=" + std::to_string(src) + " Group Call ", t);
            if (with_audio) for (int f = 0; f < 15; ++f) tone(m, 1, 1, 2000, t + 10 + 20 * f);   // 0.3 s
            t += 1000;
        };
        for (int i = 0; i < 10; ++i) call(100 + i, true);       // 10 calls with audio
        for (int i = 0; i < 30; ++i) call(200 + i, false);      // then 30 without
        auto cs = calls(m, "dmr", t);
        int with = 0, without = 0;
        for (const auto& c : cs) (c.audio.empty() ? without : with)++;
        check(cs.size() == 20 && with == 10 && by_src(cs, "100") && by_src(cs, "229") && !by_src(cs, "219"),
              "priority: a full list keeps every call with audio (even the oldest) and drops the oldest without");
        for (int i = 0; i < 15; ++i) call(300 + i, true);       // more audio calls than room
        cs = calls(m, "dmr", t);
        with = 0;
        for (const auto& c : cs) if (!c.audio.empty()) ++with;
        check(cs.size() == 20 && with == 20 && by_src(cs, "314") && !by_src(cs, "100") && !by_src(cs, "229"),
              "priority: once only calls with audio are left, the oldest of those go");
        m.stop_recording(t);
        AssocModel r;
        ReplayResult rr;
        replay_log(rpath, r, rr);
        check(rr.fresh && r.max_calls() == 20 && families_of(r.to_json(t)) == families_of(rr.stop_model),
              "priority: the recording notes which calls had audio, so a replay drops the same calls");
    }
    {
        std::vector<DsCall> C(10);
        for (int i = 0; i < 10; ++i) { C[i].id = 10 - i; C[i].start = 100 - i; }   // newest first
        C[7].audio = "a.wav"; C[8].audio = "b.wav"; C[9].audio = "c.wav";       // the three oldest
        cap_calls(C, 5);
        check(C.size() == 5 && C[0].id == 10 && C[1].id == 9 && C[2].audio == "a.wav" && C[4].audio == "c.wav",
              "priority: a merged / imported view keeps calls with audio the same way");
    }

    // ---- dsd-fme stereo: a lone slot's voice is copied to both channels ----
    {
        std::vector<std::pair<int, std::size_t>> got;
        std::vector<int16_t> scratch;
        auto out = [&](int slot, const int16_t*, std::size_t n) { got.push_back({slot, n}); };
        std::vector<int16_t> dup(320), split(320);
        for (std::size_t i = 0; i < 160; ++i) { dup[2 * i] = dup[2 * i + 1] = static_cast<int16_t>(i); split[2 * i] = 100; split[2 * i + 1] = 200; }
        route_stereo_slots(dup.data(), dup.size(), 2, scratch, out);
        check(got.size() == 1 && got[0].first == 2 && got[0].second == 160,
              "stereo: identical channels (dsd-fme copying one slot's voice) go once, to the active slot");
        got.clear();
        route_stereo_slots(dup.data(), dup.size(), 0, scratch, out);
        check(got.size() == 1 && got[0].first == 0, "stereo: ...or as slot-unknown when no slot is active yet");
        got.clear();
        route_stereo_slots(split.data(), split.size(), 1, scratch, out);
        check(got.size() == 2 && got[0].first == 1 && got[1].first == 2, "stereo: different channels (voice on both slots) go to their own slots");
    }

    // ---- encrypted calls: never recorded (unless the session has the key) ----
    {
        AssocModel m;
        m.start_audio((dir / "c").string(), 1 << 30, 0, 1000);
        m.begin_stream(1, "p25p1", 0);
        line(m, 1, "17:30:46 Sync: +P25p1 NAC/CC: 293;  LDU1", 1000);
        line(m, 1, "P25 TGT: 00000100; SRC: 00012001; NAC: 293; ", 1100);
        line(m, 1, " HDU  ALG ID: 0x84 KEY ID: 0x0001 MI: 0x0123456789ABCDEF", 1110);
        for (int f = 0; f < 5; ++f) tone(m, 1, 0, 900, 1120 + 20 * f);
        auto cs = calls(m, "p25", 1300);
        check(by_src(cs, "12001") && by_src(cs, "12001")->audio.empty(), "encrypted: no audio file");

        // Flagged encrypted only after audio started: the file is deleted.
        m.begin_stream(2, "p25p1", 2000);
        line(m, 2, "17:30:46 Sync: +P25p1 NAC/CC: 294;  LDU1", 2000);
        line(m, 2, "P25 TGT: 00000200; SRC: 00012002; NAC: 294; ", 2100);
        tone(m, 2, 0, 901, 2110);
        cs = calls(m, "p25", 2150);
        const std::string early = by_src(cs, "12002") ? by_src(cs, "12002")->audio : std::string();
        line(m, 2, " HDU  ALG ID: 0x84 KEY ID: 0x0001 MI: 0x0123456789ABCDEF", 2120);
        tone(m, 2, 0, 901, 2130);
        cs = calls(m, "p25", 2200);
        check(!early.empty() && by_src(cs, "12002")->audio.empty() && !fs::exists(file("c", early)) &&
                  m.audio_path(early, 2200).empty(),
              "encrypted: a call found to be encrypted after its audio began loses the file");

        // With the key, the session hears it in the clear: recorded.
        m.begin_stream(3, "p25p1", 3000);
        line(m, 3, "17:30:46 Sync: +P25p1 NAC/CC: 295;  LDU1", 3000);
        line(m, 3, "P25 TGT: 00000300; SRC: 00012003; NAC: 295; ", 3100);
        line(m, 3, " HDU  ALG ID: 0x84 KEY ID: 0x0001 MI: 0x0123456789ABCDEF", 3110);
        tone(m, 3, 0, 902, 3120, 2000, true);   // >= kMinAudioSamples, so only the encrypted path can discard it
        cs = calls(m, "p25", 3200);
        check(by_src(cs, "12003") && !by_src(cs, "12003")->audio.empty(), "encrypted: recorded when the session has the key");

        // ... and that decrypted audio must SURVIVE the call closing -- the
        // discard-for-encrypted at close_call must not throw away audio we
        // decoded with the key.
        const std::string dec_file = by_src(cs, "12003")->audio;
        line(m, 3, "P25 TGT: 00000300; SRC: 00099999; NAC: 295; ", 3400);   // a new talker closes the call
        cs = calls(m, "p25", 3500);
        const C* dec = by_src(cs, "12003");
        check(dec && dec->audio == dec_file && fs::exists(file("c", dec_file)),
              "encrypted: a keyed (decrypted) call keeps its audio after it closes");
    }

    // ---- one call heard by two receivers: one recording ----
    {
        AssocModel m;
        m.start_audio((dir / "d").string(), 1 << 30, 0, 1000);
        for (std::uint64_t sid : {1, 2}) {
            m.begin_stream(sid, "p25p1", 0);
            line(m, sid, "17:30:46 Sync: +P25p1 NAC/CC: 717; RFSS: 001; Site: 097;  TSBK", 1000);
            m.ingest(sid, classify_dsd_fme_line(" LRA [00] CFVA [3] RFSS[001] SITE [097] SYSID [715]"), 1010);
            line(m, sid, " CHAN-T [52E6] CHAN-R [50D7] SSC [70] WACN [BEE0A]", 1020);
        }
        line(m, 1, "P25 TGT: 00000100; SRC: 00002048; NAC: 717; ", 1100);
        tone(m, 1, 0, 111, 1110);
        line(m, 2, "P25 TGT: 00000100; SRC: 00002048; NAC: 717; ", 1120);   // the same call on stream 2
        for (int f = 0; f < 5; ++f) { tone(m, 2, 0, 222, 1130 + 20 * f); tone(m, 1, 0, 111, 1130 + 20 * f); }
        auto cs = calls(m, "p25", 1300);
        std::vector<int16_t> s;
        check(cs.size() == 1 && !cs[0].audio.empty() && wav(m.audio_path(cs[0].audio, 1LL << 40), s) && all_eq(s, 111) && s.size() == 6 * 160,
              "twins: one call, one recording (the first receiver's), no interleaving");
    }

    // ---- single-channel protocols: mono audio goes to the stream's call ----
    {
        AssocModel m;
        m.start_audio((dir / "e").string(), 1 << 30, 0, 1000);
        // dsd-fme sends single-channel protocols as mono ("slot 0"); DSDcc
        // decodes them into its slot-1 buffer. Either reaches the call.
        const std::pair<std::uint64_t, int> streams[] = {{1, 0}, {2, 1}};
        for (const auto& st : streams) {
            const std::uint64_t sid = st.first;
            m.begin_stream(sid, "nxdn48", 0);
            line(m, sid, "12:00:01 Sync: +NXDN48 RAN 01 VOICE", 1000);
            line(m, sid, " VCALL - TGT: 00000300 SRC: 0000432" + std::to_string(sid), 1100);
            tone(m, sid, st.second, static_cast<int16_t>(1230 + sid), 1110);
            tone(m, sid, st.second, static_cast<int16_t>(1230 + sid), 1130);
        }
        auto cs = calls(m, "nxdn", 1200);
        std::vector<int16_t> s0, s1;
        check(by_src(cs, "4321") && wav(m.audio_path(by_src(cs, "4321")->audio, 1LL << 40), s0) && s0.size() == 320 && all_eq(s0, 1231) &&
                  by_src(cs, "4322") && wav(m.audio_path(by_src(cs, "4322")->audio, 1LL << 40), s1) && s1.size() == 320 && all_eq(s1, 1232),
              "mono: a single-channel protocol's audio (dsd-fme's mono, DSDcc's slot 1) goes to its call");
    }

    // ---- serving: only the store's own files ----
    {
        AssocModel m;
        m.start_audio((dir / "f").string(), 1 << 30, 0, 1000);
        m.begin_stream(1, "dmr", 0);
        for (int k = 0; k < 2; ++k) line(m, 1, kCC, 1000 + k);
        line(m, 1, " SLOT 1 TGT=9 SRC=100 Group Call ", 1100);
        tone(m, 1, 1, 77, 1110);
        const std::string name = calls(m, "dmr", 1200).at(0).audio;
        std::ofstream((dir / "f" / "notes.txt").string()) << "x";
        check(!m.audio_path(name, 1200).empty(), "serve: a call's file resolves");
        bool none = true;
        for (const char* bad : {"../f/notes.txt", "notes.txt", "call_1_x_1.wav", "call_../../etc/passwd.wav",
                                "/etc/passwd", "call_1_x_1.wav/..", ""})
            none = none && m.audio_path(bad, 1200).empty();
        check(none, "serve: other names, unknown files and path tricks are refused");
    }

    // ---- disk cap: oldest finished files go first, files being written never ----
    {
        AssocModel m;
        m.start_audio((dir / "g").string(), 20000, 0, 1000);     // ~1.2 s of audio
        m.begin_stream(1, "dmr", 0);
        for (int k = 0; k < 2; ++k) line(m, 1, kCC, 1000 + k);
        std::vector<std::string> names;
        for (int i = 0; i < 4; ++i) {
            const std::int64_t t = 2000 + 1000 * i;
            line(m, 1, " SLOT 1 TGT=9 SRC=" + std::to_string(100 + i) + " Group Call ", t);
            for (int f = 0; f < 25; ++f) tone(m, 1, 1, static_cast<int16_t>(1000 + i), t + 10 + 20 * f);   // 0.5 s = 8 KB
            names.push_back(by_src(calls(m, "dmr", t + 600), std::to_string(100 + i))->audio);
        }
        const auto st = m.audio_status();
        check(st.bytes <= 20000 && !fs::exists(file("g", names[0])) && fs::exists(file("g", names[3])) &&
                  m.audio_path(names[0], 6000).empty(),
              "cap: total stays under the cap; the oldest file was deleted, the newest kept");
        // A restart finds the files already there and counts them.
        m.stop_audio();
        AssocModel m2;
        m2.start_audio((dir / "g").string(), 20000, 0, 7000);
        check(m2.audio_status().files == st.files && !m2.audio_path(names[3], 7000).empty(),
              "restart: earlier runs' files are counted against the cap and still play");
    }

    // ---- audio never reaches a recording's snapshots or an import ----
    {
        AssocModel m;
        m.set_identity("1122334455667788", "rx");
        m.set_since(500);
        m.start_audio((dir / "h").string(), 1 << 30, 0, 1000);
        m.start_recording(dir.string(), 0, false, 1000);
        const std::string rpath = m.recording().path;
        m.begin_stream(1, "dmr", 1000);
        for (int k = 0; k < 2; ++k) line(m, 1, kCC, 1001 + k);
        line(m, 1, " SLOT 1 TGT=9 SRC=100 Group Call ", 1100);
        for (int f = 0; f < 5; ++f) tone(m, 1, 1, 5500, 1110 + 20 * f);
        const std::string live = m.to_json(1300);
        m.stop_recording(1300);
        AssocModel r;
        ReplayResult rr;
        replay_log(rpath, r, rr);
        check(live.find("\"audio\":\"call_") != std::string::npos && rr.stop_model.find("\"audio\":\"call_") == std::string::npos &&
                  families_of(r.to_json(1300)) == families_of(rr.stop_model),
              "replay: snapshots leave audio out, so a replay still matches byte for byte");
        const std::string ex = m.to_export_json(1300);
        Dataset d;
        dataset_from_export_text(ex, "x", d, nullptr);
        // The export names the file (an "export with audio" zip carries it,
        // and a merge keeps the name); an import into another server's live
        // view doesn't point at a file that server hasn't got.
        AssocModel other;
        other.set_identity("99aabbccddeeff00", "elsewhere");
        other.import_export(ex, "x.json", 1300);
        check(ex.find("\"audio\":\"call_") != std::string::npos && d.fams["dmr"].calls.size() == 1 &&
                  d.fams["dmr"].calls[0].audio.rfind("call_", 0) == 0 && other.to_json(1300).find("\"audio\":\"call_") == std::string::npos,
              "export: says which calls had audio (kept by a merge); an import elsewhere doesn't point at files it hasn't got");
        // Clear finishes the open files; they stay playable.
        const std::string name = calls(m, "dmr", 1300).at(0).audio;
        m.clear(1400);
        std::vector<int16_t> s;
        check(wav(m.audio_path(name, 1LL << 40), s) && s.size() == 800 && !m.audio_path(name, 1500).empty(),
              "clear: the explorer forgets the call; its finished file stays (until the cap)");
        // Clear with audio (the page asks): the files are deleted from disk.
        const std::string fpath = m.audio_path(name, 1600);
        const auto gone = m.clear_with_audio(1700);
        check(gone.first == 1 && gone.second >= 44 + 1600 && !std::filesystem::exists(fpath) && m.audio_path(name, 1800).empty() &&
                  m.audio_status().files == 0 && m.audio_status().bytes == 0,
              "clear with audio: the call audio files are deleted, and the store is empty");
    }

    fs::remove_all(dir);
    if (g_failures) {
        std::printf("\n%d CHECK(S) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nALL ASSOC AUDIO TESTS PASSED\n");
    return 0;
}
