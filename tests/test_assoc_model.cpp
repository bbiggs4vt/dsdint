// test_assoc_model.cpp
//
// Unit tests for AssocModel (src/assoc_model.hpp), the network explorer's
// call / talkgroup / radio / network association model. Real dsd-fme line
// formats are run through the real classifier (classify_dsd_fme_line) and fed
// in with explicit timestamps, then the model's JSON (/net.json) is parsed back
// -- so every check also proves the JSON is well-formed.

#include "../src/assoc_model.hpp"
#include "../src/dsd_process.hpp"

#include <cctype>
#include <filesystem>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <unistd.h>

using namespace dsdsrv;

static int g_failures = 0;
static void check(bool c, const std::string& what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what.c_str());
    if (!c) ++g_failures;
}

// ---- minimal JSON reader (enough for the model's output) ----
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::map<std::string, J> o;
    const J& operator[](const std::string& k) const { static J nul; auto it = o.find(k); return it == o.end() ? nul : it->second; }
    const J& at(std::size_t i) const { static J nul; return i < a.size() ? a[i] : nul; }
    std::size_t size() const { return t == Arr ? a.size() : t == Obj ? o.size() : 0; }
    bool has(const std::string& k) const { return o.count(k) != 0; }
};
struct Parser {
    const std::string& s;
    std::size_t i = 0;
    bool ok = true;
    explicit Parser(const std::string& x) : s(x) {}
    void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
    J str() {
        J j; j.t = J::Str;
        if (i >= s.size() || s[i] != '"') { ok = false; return j; }
        ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') {
                ++i;
                char e = s[i];
                if (e == 'u') { j.s += '?'; i += 5; continue; }
                j.s += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
                ++i;
            } else {
                j.s += s[i++];
            }
        }
        if (i >= s.size()) ok = false;
        ++i;
        return j;
    }
    J val() {
        ws();
        J j;
        if (i >= s.size()) { ok = false; return j; }
        char c = s[i];
        if (c == '{') {
            j.t = J::Obj; ++i; ws();
            if (i < s.size() && s[i] == '}') { ++i; return j; }
            for (;;) {
                ws(); J k = str(); ws();
                if (i >= s.size() || s[i] != ':') { ok = false; return j; }
                ++i;
                j.o[k.s] = val(); ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return j; }
                ok = false; return j;
            }
        }
        if (c == '[') {
            j.t = J::Arr; ++i; ws();
            if (i < s.size() && s[i] == ']') { ++i; return j; }
            for (;;) {
                j.a.push_back(val()); ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return j; }
                ok = false; return j;
            }
        }
        if (c == '"') return str();
        if (s.compare(i, 4, "true") == 0) { i += 4; j.t = J::Bool; j.b = true; return j; }
        if (s.compare(i, 5, "false") == 0) { i += 5; j.t = J::Bool; return j; }
        if (s.compare(i, 4, "null") == 0) { i += 4; return j; }
        std::size_t st = i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '.' ||
                                s[i] == 'e' || s[i] == 'E' || s[i] == '+')) ++i;
        if (st == i) { ok = false; return j; }
        j.t = J::Num; j.n = std::stod(s.substr(st, i - st));
        return j;
    }
};
static J parse(const std::string& s, bool* ok = nullptr) {
    Parser p(s);
    J j = p.val();
    p.ws();
    if (ok) *ok = p.ok && p.i == s.size();
    return j;
}
static const J* find(const J& arr, const char* key, const std::string& val) {
    for (const auto& e : arr.a) if (e[key].s == val) return &e;
    return nullptr;
}
static bool contains(const J& arr, const std::string& v) {
    for (const auto& e : arr.a) if (e.s == v) return true;
    return false;
}

// Feed a real dsd-fme line through the classifier into the model.
static void line(AssocModel& m, std::uint64_t sid, const std::string& l, std::int64_t t) {
    m.ingest(sid, classify_dsd_fme_line(l), t);
}
static J snap(const AssocModel& m, std::int64_t t, bool* ok = nullptr) { return parse(m.to_json(t), ok); }

// Feed `n` AMBE voice frames of a given b0 pitch class into an open call on
// slot "1" of session `sid`, one per ms from `t`. b0: 86 speech, 124 silence,
// 121 erasure, 127 tone (real codeword classes).
static void feed_frames(AssocModel& m, std::uint64_t sid, int b0, int n, std::int64_t t) {
    DsdEvent vf; vf.kind = "voice"; vf.slot = "1"; vf.voice_b0 = b0; vf.voice_err = 0;
    for (int i = 0; i < n; ++i) m.ingest(sid, vf, t + i);
}

int main() {
    std::printf("test_assoc_model\n");

    // ---- family mapping ----
    check(assoc_family("p25p1") == "p25" && assoc_family("p25p2") == "p25", "p25p1/p25p2 -> p25 family");
    check(assoc_family("nxdn48") == "nxdn" && assoc_family("tetrakit") == "tetra", "nxdn48 -> nxdn, tetrakit -> tetra");
    check(assoc_family("edacs_esk") == "edacs" && assoc_family("provoice") == "edacs", "edacs*/provoice -> edacs");
    check(assoc_family("pocsag1200").empty() && assoc_family("flex").empty(), "paging is not modelled");

    // ---- DMR: call stitching, weak (per-stream) network, voice extension ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        line(m, 1, "19:54:55 Sync: +DMR  slot1  [SLOT2] | Color Code=04 | VC6 ", 1000);
        line(m, 1, " SLOT 2 TGT=19535 SRC=2222223 Group Call  ", 1100);
        line(m, 1, "19:54:55 Sync: +DMR  slot1  [SLOT2] | Color Code=04 | VC1 ", 1400);
        line(m, 1, " SLOT 2 TGT=19535 SRC=2222223 Group Call  ", 1800);
        line(m, 1, "19:54:56 Sync: +DMR  slot1  [SLOT2] | Color Code=04 | VC2 ", 2500);
        bool ok = false;
        J j = snap(m, 2600, &ok);
        check(ok, "DMR: /net.json is well-formed");
        const J& F = j["families"]["dmr"];
        check(F["calls"].size() == 1, "DMR: repeated call lines + voice syncs stitch into ONE call");
        const J& c = F["calls"].at(0);
        check(c["src"].s == "2222223" && c["tgt"].s == "19535" && c["slot"].s == "2", "DMR: call src/tgt/slot");
        check(c["voice"].b && !c["priv"].b && c["open"].b, "DMR: voice group call, still open");
        check(c["last"].n == 2500, "DMR: a voice sync extends the call (last = 2500)");
        const J* tg = find(F["talkgroups"], "id", "19535");
        check(tg && (*tg)["radios"]["2222223"].n == 1 && (*tg)["calls"].n == 1, "DMR: TG 19535 <- radio 2222223 (1 call)");
        const J* r = find(F["radios"], "id", "2222223");
        check(r && (*r)["tgs"]["19535"].n == 1, "DMR: radio 2222223 -> TG 19535");
        check(F["networks"].size() == 1 && F["networks"].at(0)["key"].s == "cc:4@s1" &&
              F["networks"].at(0)["confidence"].s == "weak", "DMR: color code is a WEAK, per-stream network");
        check(c["net"].s == "cc:4@s1", "DMR: call attributed to the stream's network");

        // After a gap longer than the continuation window: a new call.
        line(m, 1, " SLOT 2 TGT=19535 SRC=2222223 Group Call  ", 9000);
        J j2 = snap(m, 9000);
        const J& F2 = j2["families"]["dmr"];
        check(F2["calls"].size() == 2, "DMR: same TG/source after >4 s gap is a new call");
        check((*find(F2["talkgroups"], "id", "19535"))["radios"]["2222223"].n == 2, "DMR: association count 2");
        check(!F2["calls"].at(1)["open"].b, "DMR: the earlier call is closed");

        // New talker on the same talkgroup = new call.
        line(m, 1, " SLOT 2 TGT=19535 SRC=3000001 Group Call  ", 9500);
        J j3 = snap(m, 9500);
        check(j3["families"]["dmr"]["calls"].size() == 3, "DMR: a different source on the same TG starts a new call");
        check((*find(j3["families"]["dmr"]["talkgroups"], "id", "19535"))["radios"].size() == 2, "DMR: TG 19535 now has 2 radios");
    }

    // ---- DMR TDMA: a new call on a slot closes the previous one there ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        line(m, 1, " SLOT 1 TGT=100 SRC=1 Group Call ", 1000);
        line(m, 1, " SLOT 1 TGT=200 SRC=2 Group Call ", 2000);
        J j = snap(m, 2000);
        const J& C = j["families"]["dmr"]["calls"];
        check(C.size() == 2 && C.at(0)["tgt"].s == "200" && C.at(0)["open"].b && !C.at(1)["open"].b,
              "TDMA: new call on slot 1 closes the previous slot-1 call");
    }

    // ---- private (unit-to-unit) call -> radio<->radio edge, no talkgroup ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        line(m, 1, " SLOT 1 TGT=1234 SRC=5678 Private Call ", 1000);
        J j = snap(m, 1000);
        const J& F = j["families"]["dmr"];
        check(F["calls"].at(0)["priv"].b, "private: call flagged private");
        check(!find(F["talkgroups"], "id", "1234"), "private: target is NOT recorded as a talkgroup");
        const J* a = find(F["radios"], "id", "5678");
        const J* b = find(F["radios"], "id", "1234");
        check(a && b && (*a)["peers"]["1234"].n == 1 && (*b)["peers"]["5678"].n == 1, "private: peer edge both ways");
    }

    // ---- a call counted as group that turns out to be unit-to-unit (P25 LCW) ----
    {
        AssocModel m;
        m.begin_stream(1, "p25p1", 0);
        line(m, 1, "2023/10/02 10:23:18 P25 TGT: 00000077; SRC: 00000055; NAC: 293; ", 1000);
        J j0 = snap(m, 1000);
        check(find(j0["families"]["p25"]["talkgroups"], "id", "77") != nullptr, "flip: first counted as TG 77");
        // An id-less LCW line naming the service (it classifies as "voice", so
        // clients see it; a suppressed LCW line reaches the model the same way
        // via on_suppressed).
        DsdEvent lcw = classify_dsd_fme_line(" P25 LCW  Unit to Unit Voice Channel User");
        check(lcw.talkgroup.empty() && lcw.source_id.empty(), "flip: the LCW line carries no ids of its own");
        m.ingest(1, lcw, 1200);
        J j = snap(m, 1200);
        const J& F = j["families"]["p25"];
        check(!find(F["talkgroups"], "id", "77"), "flip: TG 77 removed once the call is known to be private");
        const J* r = find(F["radios"], "id", "55");
        check(r && (*r)["peers"]["77"].n == 1 && (*r)["tgs"].size() == 0, "flip: association moved to the peer edge");
    }

    // ---- P25: identity arriving piecemeal on one stream merges into one network ----
    {
        AssocModel m;
        m.begin_stream(2, "p25p1", 0);
        line(m, 2, "17:30:46 Sync: +P25p1 NAC/CC: 717; RFSS: 001; Site: 097;  TSBK", 1000);
        line(m, 2, "2023/10/02 10:23:18 P25 TGT: 00000100; SRC: 00002048; NAC: 717; ", 1100);
        J j0 = snap(m, 1100);
        check(j0["families"]["p25"]["networks"].at(0)["key"].s == "nac:717@s2", "P25: bare NAC -> weak per-stream network first");
        // System ID (strong), then WACN (refines it) -- both on suppressed lines.
        DsdEvent sys = classify_dsd_fme_line(" LRA [00] CFVA [3] RFSS[001] SITE [097] SYSID [715]");
        check(sys.kind == "unknown" && sys.extra.find("system_id=715") != std::string::npos, "P25: SYSID line is unknown with system_id");
        m.ingest(2, sys, 1200);
        line(m, 2, " CHAN-T [52E6] CHAN-R [50D7] SSC [70] WACN [BEE0A]", 1300);
        line(m, 2, "2023/10/02 10:23:19 P25 TGT: 00000200; SRC: 00002050; NAC: 717; ", 1400);
        J j = snap(m, 1400);
        const J& F = j["families"]["p25"];
        check(F["networks"].size() == 1, "P25: NAC -> SYS 715 -> WACN/SYS merged into ONE network (no fragments)");
        const J& n = F["networks"].at(0);
        check(n["key"].s == "wacn:BEE0A/sys:715" && n["confidence"].s == "strong", "P25: strong WACN+SYSID identity");
        check(n["ids"]["nac"].s == "717", "P25: the NAC is kept as an attribute of the strong network");
        check(contains(n["sites"], "RFSS 1 \xC2\xB7 Site 97"), "P25: site RFSS 1 / Site 97 recorded");
        check(n["calls"].n == 2, "P25: both calls (before and after identification) counted on it");
        bool all = true;
        for (const auto& c : F["calls"].a) all = all && c["net"].s == "wacn:BEE0A/sys:715";
        check(all, "P25: the early call was re-attributed to the identified network");

        // A second stream that only ever hears NAC 717 resolves to that network.
        m.begin_stream(3, "p25p1", 0);
        line(m, 3, "17:30:47 Sync: +P25p1 NAC/CC: 717;  LDU1", 1990);
        line(m, 3, "2023/10/02 10:24:00 P25 TGT: 00000100; SRC: 00002099; NAC: 717; ", 2000);
        J j2 = snap(m, 2000);
        const J& F2 = j2["families"]["p25"];
        check(F2["networks"].size() == 1 && F2["networks"].at(0)["sessions"].n == 2,
              "P25: a NAC-only stream resolves to the known WACN/SYS network carrying that NAC");
        check((*find(F2["talkgroups"], "id", "100"))["radios"].size() == 2, "P25: TG 100 now has radios from both streams");

        // Adjacent-site broadcast: recorded as a site, identity/site of the stream untouched.
        DsdEvent adj = classify_dsd_fme_line(" ADJSTS 6 RFSS[008] SITE [009] SYSID [999]");
        m.ingest(2, adj, 2100);
        J j3 = snap(m, 2100);
        const J& n3 = j3["families"]["p25"]["networks"].at(0);
        check(j3["families"]["p25"]["networks"].size() == 1 && n3["key"].s == "wacn:BEE0A/sys:715",
              "P25: neighbour broadcast (other SYSID) does not move the stream's identity");
        check(contains(n3["sites"], "RFSS 8 \xC2\xB7 Site 9"), "P25: neighbour site recorded on the network");
    }

    // ---- two receivers hearing the SAME call on a shared network = one call ----
    {
        AssocModel m;
        m.begin_stream(1, "p25p1", 0);   // control channel with full identity
        m.begin_stream(2, "p25p1", 0);   // voice channel: only the NAC is heard
        line(m, 1, "17:30:46 Sync: +P25p1 NAC/CC: 293; RFSS: 004; Site: 012;  TSBK", 900);
        m.ingest(1, classify_dsd_fme_line(" LRA [00] CFVA [3] RFSS[004] SITE [012] SYSID [3A1]"), 910);
        line(m, 1, " CHAN-T [52E6] CHAN-R [50D7] SSC [70] WACN [BEE00]", 920);
        line(m, 2, "17:30:46 Sync: +P25p1 NAC/CC: 293;  TSBK", 930);
        const char* c = "2023/10/02 10:23:18 P25 TGT: 00000100; SRC: 00012001; NAC: 293; ";
        line(m, 1, c, 1000);                                   // the grant
        line(m, 2, c, 1050);                                   // the same call on the voice channel
        line(m, 2, "17:31:02 Sync: +P25p1 NAC/CC: 293; LDU1", 1200);
        line(m, 2, " P25 LCW  Group Call; Emergency", 1300);   // flag seen only by stream 2
        J j = snap(m, 1300);
        const J& F = j["families"]["p25"];
        check(F["calls"].size() == 1, "dedup: grant + voice-channel copy of one call = ONE call record");
        check(F["calls"].at(0)["streams"].n == 2, "dedup: the call records that 2 receivers heard it");
        check(F["calls"].at(0)["emerg"].b, "dedup: later events from either stream still refine the call");
        check((*find(F["talkgroups"], "id", "100"))["radios"]["12001"].n == 1, "dedup: association counted once, not twice");

        // A genuinely new call later from the same radio is still a new call.
        line(m, 2, c, 9000);
        check(snap(m, 9000)["families"]["p25"]["calls"].size() == 2, "dedup: a later call is not swallowed");
    }
    {
        // Per-stream weak networks never dedupe: two CC-1 repeaters are not one system.
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        m.begin_stream(2, "dmr", 0);
        line(m, 1, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=01 | VC6 ", 900);
        line(m, 2, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=01 | VC6 ", 900);
        line(m, 1, " SLOT 1 TGT=9 SRC=3112 Group Call ", 1000);
        line(m, 2, " SLOT 1 TGT=9 SRC=3112 Group Call ", 1000);
        J j = snap(m, 1000);
        check(j["families"]["dmr"]["calls"].size() == 2, "dedup: never across per-stream (weak) networks");
    }

    // ---- control channel: concurrent grants on slot "" stay separate ----
    {
        AssocModel m;
        m.begin_stream(1, "p25p1", 0);
        const char* a = "2023/10/02 10:23:18 P25 TGT: 00000100; SRC: 00000001; NAC: 293; ";
        const char* b = "2023/10/02 10:23:18 P25 TGT: 00000200; SRC: 00000002; NAC: 293; ";
        line(m, 1, a, 1000); line(m, 1, b, 1100); line(m, 1, a, 1900); line(m, 1, b, 2000); line(m, 1, a, 2900);
        J j = snap(m, 2900);
        const J& C = j["families"]["p25"]["calls"];
        check(C.size() == 2 && C.at(0)["open"].b && C.at(1)["open"].b, "control channel: two interleaved grants = 2 concurrent calls");
    }

    // ---- DMR weak ids from two streams are NOT merged; shared TG/radio is the evidence ----
    {
        AssocModel m;
        m.begin_stream(4, "dmr", 0);
        m.begin_stream(5, "dmr", 0);
        for (int k = 0; k < 2; ++k) {
            line(m, 4, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=04 | VC6 ", 1000 + k);
            line(m, 5, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=04 | VC6 ", 1000 + k);
        }
        line(m, 4, " SLOT 1 TGT=9 SRC=3112 Group Call ", 1100);
        line(m, 5, " SLOT 1 TGT=9 SRC=3112 Group Call ", 1100);
        J j = snap(m, 1200);
        const J& F = j["families"]["dmr"];
        check(F["networks"].size() == 2, "weak ids: two CC-4 streams stay two networks (CC is not an identity)");
        check((*find(F["talkgroups"], "id", "9"))["networks"].size() == 2, "weak ids: TG 9 is seen on both -> cross-network evidence");
        check((*find(F["radios"], "id", "3112"))["networks"].size() == 2, "weak ids: radio 3112 is seen on both");
    }

    // ---- retune: a different color code on the same stream = a different network ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        line(m, 1, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=04 | VC6 ", 1000);
        line(m, 1, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=04 | VC1 ", 1050);
        line(m, 1, " SLOT 1 TGT=10 SRC=1 Group Call ", 1100);
        line(m, 1, "19:55:55 Sync: +DMR  slot1  [SLOT1] | Color Code=07 | VC6 ", 9000);
        line(m, 1, "19:55:55 Sync: +DMR  slot1  [SLOT1] | Color Code=07 | VC1 ", 9050);
        line(m, 1, " SLOT 1 TGT=20 SRC=2 Group Call ", 9100);
        J j = snap(m, 9100);
        const J& F = j["families"]["dmr"];
        check(F["networks"].size() == 2, "retune: CC 4 -> CC 7 yields a second network, not a merge");
        check((*find(F["talkgroups"], "id", "10"))["networks"].at(0).s == "cc:4@s1" &&
              (*find(F["talkgroups"], "id", "20"))["networks"].at(0).s == "cc:7@s1", "retune: each TG on its own network");
    }

    // ---- weak-anchor hysteresis (from the real voice capture's lines) ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        DsdEvent ph = classify_dsd_fme_line("19:50:50 Sync: +DMR MS/DM MODE/MONO | Color Code=00 | VC* ");
        check(ph.color_code == "0", "hysteresis: dsd-fme's placeholder line reads as CC 0");
        m.ingest(1, ph, 1000);                                       // one-off placeholder
        line(m, 1, " SLOT 1 TGT=1 SRC=123 Group Call ", 1050);
        for (int k = 0; k < 4; ++k)
            line(m, 1, "19:50:50 Sync: +DMR MS/DM MODE/MONO | Color Code=01 | VC* ", 1100 + 100 * k);
        J j = snap(m, 1500);
        const J& F = j["families"]["dmr"];
        check(F["networks"].size() == 1 && F["networks"].at(0)["key"].s == "cc:1@s1",
              "hysteresis: a one-off 'Color Code=00' doesn't split the stream; it is CC 1");
        check(F["calls"].at(0)["net"].s == "cc:1@s1" && F["networks"].at(0)["calls"].n == 1,
              "hysteresis: the early call is on CC 1 and the network's counter agrees");
        // Alternating noise never flips an established identity.
        const char* n7 = "19:50:51 Sync: +DMR MS/DM MODE/MONO | Color Code=07 | VC* ";
        const char* n1 = "19:50:51 Sync: +DMR MS/DM MODE/MONO | Color Code=01 | VC* ";
        line(m, 1, n7, 2000); line(m, 1, n1, 2100); line(m, 1, n7, 2200); line(m, 1, n1, 2300);
        J j2 = snap(m, 2300);
        check(j2["families"]["dmr"]["networks"].size() == 1, "hysteresis: alternating CC noise never flips the network");
    }

    // ---- CRC-failed events never create ids ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        DsdEvent bad = classify_dsd_fme_line(" SLOT 1 TGT=999 SRC=888 Group Call (CRC ERR)");
        check(bad.crc_error == "1", "crc: line is CRC-flagged");
        m.ingest(1, bad, 1000);
        J j = snap(m, 1000);
        check(!j["families"]["dmr"].has("radios") || j["families"]["dmr"]["radios"].size() == 0, "crc: no radio 888 / TG 999 from a failed frame");
    }

    // ---- Capacity Plus: the channel-status roster is not a call (real lines, 460.175 MHz) ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        std::int64_t t = 1000;
        for (int f = 0; f < 6; ++f) {                 // one TDMA frame pair, repeated
            line(m, 1, "18:20:50 Sync: +DMR   slot1  [slot2] | Color Code=05 | CSBK", t);
            line(m, 1, " Capacity Plus Channel Status - FL: 1 TS: 1 RS: 0 - Rest LSN: 4 - Final Block", t);
            line(m, 1, f % 2 ? " Bank One F80 Private or Data Call(s) -  LSN 01: TGT 17434; LSN 02: TGT 23043;"
                             : " Bank One F80 Private or Data Call(s) -  LSN 04: TGT 20504;", t);
            line(m, 1, "18:20:50 Sync: +DMR  [SLOT1]  slot2  | Color Code=05 | VC6 ", t + 30);
            if (f == 0) line(m, 1, " SLOT 1 TGT=12 SRC=23021 FLCO=0x00 FID=0x10 SVC=0x20 Group TXI Call  ", t + 30);
            line(m, 1, " AMBE A33E43756BB380 err = [0] [0] ", t + 30);
            t += 90;
        }
        J j = snap(m, t);
        const J& F = j["families"]["dmr"];
        check(F["calls"].size() == 1, "cap+: roster lines (other LSNs' traffic) make no calls of their own");
        const J& c = F["calls"].at(0);
        check(c["src"].s == "23021" && c["tgt"].s == "12" && !c["priv"].b && c["voice"].b,
              "cap+: the slot's real call (23021 -> TG 12, voice) is one call, not cut by the roster");
    }

    // ---- P25 neighbour broadcast: ids on the line after the heading (real lines, 380.475 MHz) ----
    {
        AssocModel m;
        m.begin_stream(1, "p25p1", 0, "", 380475000);
        std::int64_t t = 1000;
        for (int cycle = 0; cycle < 3; ++cycle, t += 1000) {
            line(m, 1, "15:01:53 Sync: +P25p1 WACN: 580A0; SYS: 006; NAC/CC: 00D; RFSS: 008; Site: 008;  TSBK", t);
            line(m, 1, " RFSS Status Broadcast - Implicit", t + 1);
            line(m, 1, "  LRA [08] SYSID [006] RFSS ID [008] SITE ID [008] CHAN [0026] SSC [70]", t + 2);
            line(m, 1, "15:01:53 Sync: +P25p1 WACN: 580A0; SYS: 006; NAC/CC: 00D; RFSS: 008; Site: 008;  TSBK", t + 100);
            line(m, 1, " Adjacent Status Broadcast - Abbreviated", t + 101);
            line(m, 1, "  LRA [07] RFSS[007] SITE [007] SYSID [015] CHAN-T [0197] SSC [70]", t + 102);
            line(m, 1, " Up to Date (Correct) Valid RFSS Connection Active", t + 103);
            line(m, 1, "  Frequency [385.087500] MHz", t + 104);
            line(m, 1, "2026/10/07 15:01:53 P25 TGT: 00000101; SRC: 06746109; NAC: 00D; ", t + 200 + cycle);
        }
        J j = snap(m, t);
        const J& N = j["families"]["p25"]["networks"];
        check(N.size() == 1 && N.at(0)["key"].s == "wacn:580A0/sys:006",
              "p25 adjacent: the neighbour's SYSID (on the line after the heading) doesn't become this stream's network");
        check(contains(N.at(0)["sites"], "RFSS 7 \xC2\xB7 Site 7"), "p25 adjacent: the neighbour is recorded as a site of the network");
        const J& C = j["families"]["p25"]["calls"];
        bool all = C.size() > 0;
        for (std::size_t i = 0; i < C.size(); ++i) all = all && C.at(i)["net"].s == "wacn:580A0/sys:006";
        check(all, "p25 adjacent: calls stay on the stream's own network");
    }

    // ---- DMR hang time: the call ends when the talker unkeys (real lines, 460.575 MHz) ----
    {
        auto talk = [](AssocModel& m, std::int64_t& t, int bursts) {
            for (int f = 0; f < bursts; ++f, t += 60) {
                line(m, 1, "16:49:03 Sync: +DMR   slot1  [slot2] | Color Code=05 | VC2 ", t);
                line(m, 1, " SLOT 2 TGT=13 SRC=9112 FLCO=0x00 FID=0x10 SVC=0x20 Group TXI Call  ", t);
            }
        };
        auto hang = [](AssocModel& m, std::int64_t& t, int bursts) {   // terminators, ids repeated
            for (int f = 0; f < bursts; ++f, t += 100) {
                line(m, 1, "16:49:05 Sync: +DMR   slot1  [slot2] | Color Code=05 | TLC  ", t);
                line(m, 1, " SLOT 2 TGT=13 SRC=9112 FLCO=0x00 FID=0x10 SVC=0x20 Group TXI Call  ", t);
            }
        };
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        std::int64_t t = 1000;
        talk(m, t, 50);                                // 3 s of voice: 1000 .. 3940
        const std::int64_t unkey = t - 60;
        hang(m, t, 30);                                // 3 s of hang time
        J j = snap(m, t);
        const J& F = j["families"]["dmr"];
        check(F["calls"].size() == 1, "hang time: one call");
        const J& c = F["calls"].at(0);
        check(c["start"].n == 1000 && c["last"].n == static_cast<double>(unkey),
              "hang time: the call's length ends when the talker unkeyed, not with the repeater's terminators");
        check(c["open"].b, "hang time: still open while the repeater holds the slot");
        talk(m, t, 10);                                // keys up again within the hang time
        J j2 = snap(m, t);
        check(j2["families"]["dmr"]["calls"].size() == 1 && j2["families"]["dmr"]["calls"].at(0)["last"].n == static_cast<double>(t - 60),
              "hang time: talking again within it resumes the same call");
    }

    // ---- the real DMR SMS sequence (direct mode): one data call carrying the text ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        line(m, 1, "19:58:33 Sync: +DMR MS/DM MODE/MONO | Color Code=01 | CSBK", 1000);
        DsdEvent pre = classify_dsd_fme_line(" Preamble CSBK - Group Data - Source: 123 - Target: 1 ");
        check(pre.talkgroup == "1" && pre.source_id == "123", "SMS: classifier reads 'Target: 1' as the destination");
        m.ingest(1, pre, 1050);
        line(m, 1, "19:58:33 Sync: +DMR MS/DM MODE/MONO | Color Code=01 | DATA ", 1100);
        line(m, 1, " Slot 1 Data Header - Group - Unconfirmed Delivery - Source: 123 Target: 1 ", 1150);
        line(m, 1, "19:58:33 Sync: +DMR MS/DM MODE/MONO | Color Code=01 | R12U ", 1200);
        line(m, 1, "19:58:34 Sync: +DMR MS/DM MODE/MONO | Color Code=01 | R12U ", 1300);
        DsdEvent msg;                      // what DmrPduTextCarry emits
        msg.kind = "message"; msg.slot = "1"; msg.message = "test";
        m.ingest(1, msg, 1400);
        J j = snap(m, 1400);
        const J& F = j["families"]["dmr"];
        check(F["calls"].size() == 1, "SMS: unslotted preamble + 'Slot 1' header + text = ONE call");
        const J& c = F["calls"].at(0);
        check(c["src"].s == "123" && c["tgt"].s == "1" && c["slot"].s == "1", "SMS: 123 -> TG 1 on slot 1");
        check(c["data"].b && !c["voice"].b && c["text"].s == "test", "SMS: data call carrying the text \"test\"");
    }

    // ---- TETRA (events as the TETRA backend produces them) ----
    {
        AssocModel m;
        m.begin_stream(1, "tetra", 0);
        DsdEvent s; s.kind = "sync"; s.color_code = "17"; s.extra = "mcc=00ea; mnc=004e; la=6163; func=NETINFO1";
        s.raw_line = "TETMON_begin FUNC:NETINFO1 CCODE:17 MCC:00ea MNC:004e LA:6163 RX:1 TETMON_end";
        m.ingest(1, s, 1000);
        DsdEvent c; c.kind = "call"; c.talkgroup = "1001"; c.source_id = "2001"; c.extra = "encr=1";
        c.raw_line = "TETMON_begin FUNC:DSETUPDEC SSI:1001 SSI2:2001 ENCR:1 TETMON_end";
        m.ingest(1, c, 1100);
        J j = snap(m, 1100);
        const J& F = j["families"]["tetra"];
        const J& n = F["networks"].at(0);
        check(n["key"].s == "mcc:00ea/mnc:004e" && n["confidence"].s == "strong", "TETRA: MCC/MNC strong network");
        check(contains(n["sites"], "LA 6163"), "TETRA: location area as the site");
        check(F["calls"].at(0)["enc"].b, "TETRA: encrypted call flagged");
    }

    // ---- encryption: each call's algorithm + key id; keys counted per network,
    //      talkgroup and radio (once per call; "0x0042" and "42" are one key) ----
    {
        AssocModel m;
        m.begin_stream(1, "p25p1", 0);
        line(m, 1, "2023/10/02 10:23:18 P25 TGT: 00000100; SRC: 00002048; NAC: 293; ", 1000);
        line(m, 1, " HDU  ALG ID: 0x84 KEY ID: 0x0042 MI: 0x0123456789ABCDEF ENC", 1100);
        line(m, 1, " LDU2 ALG ID: 0x84 KEY ID: 0x0042 MI: 0x0123456789ABCDEF ENC", 1300);
        line(m, 1, "2023/10/02 10:23:28 P25 TGT: 00000100; SRC: 00002049; NAC: 293; ", 11000);
        line(m, 1, " HDU  ALG ID: 0x84 KEY ID: 0x42 MI: 0x0123456789ABCDEF ENC", 11100);
        line(m, 1, "2023/10/02 10:23:38 P25 TGT: 00000200; SRC: 00002049; NAC: 293; ", 21000);
        line(m, 1, " HDU  ALG ID: 0xAA KEY ID: 0x0007 MI: 0x0123456789ABCDEF ENC", 21100);
        line(m, 1, "2023/10/02 10:23:48 P25 TGT: 00000300; SRC: 00002050; NAC: 293; ", 31000);
        J j = snap(m, 45000);
        const J& F = j["families"]["p25"];
        const J* c1 = nullptr; const J* c3 = nullptr;
        for (const auto& c : F["calls"].a) { if (c["src"].s == "2048") c1 = &c; if (c["src"].s == "2050") c3 = &c; }
        check(c1 && (*c1)["enc"].b && (*c1)["alg"].s == "84" && (*c1)["kid"].s == "42",
              "enc: the call carries its algorithm and key id (normalised: 0x0042 -> 42)");
        check(c3 && !(*c3)["enc"].b && (*c3)["kid"].s.empty(), "enc: a clear call has no key");
        const J* t1 = find(F["talkgroups"], "id", "100");
        const J* t2 = find(F["talkgroups"], "id", "200");
        check(t1 && (*t1)["keys"]["84:42"].n == 2 && (*t1)["enc"].n == 2,
              "enc: talkgroup 100 -- two encrypted calls, both on key 84:42 (counted once per call)");
        check(t2 && (*t2)["keys"]["AA:07"].n == 1, "enc: talkgroup 200 uses ADP key 07");
        const J* r49 = find(F["radios"], "id", "2049");
        check(r49 && (*r49)["keys"]["84:42"].n == 1 && (*r49)["keys"]["AA:07"].n == 1, "enc: the radio's keys, per call it made");
        const J* r50 = find(F["radios"], "id", "2050");
        check(r50 && (*r50)["keys"].o.empty(), "enc: a radio with only clear calls has no keys");
        const J& n = F["networks"].at(0);
        check(n["keys"]["84:42"].n == 2 && n["keys"]["AA:07"].n == 1, "enc: the network's keys");
        // An export carries them, and importing it gives them back.
        Dataset d;
        std::string err;
        check(dataset_from_export_text(m.to_export_json(45000), "x", d, &err) &&
                  d.fams["p25"].tgs["100"].keys["84:42"] == 2 && d.fams["p25"].calls.size() == 4,
              "enc: keys survive export -> import");
        bool kid_ok = false;
        for (const auto& c : d.fams["p25"].calls) if (c.src == "2048") kid_ok = c.alg == "84" && c.kid == "42";
        check(kid_ok, "enc: a call's key survives export -> import");
    }

    // ---- emergency flag is tallied on the talkgroup when the call closes ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        line(m, 1, " SLOT 1 TGT=100 SRC=2048 Group Emergency", 1000);
        m.end_stream(1);
        J j = snap(m, 1000);
        const J& F = j["families"]["dmr"];
        check(F["calls"].at(0)["emerg"].b && !F["calls"].at(0)["open"].b, "emergency: flagged, closed at end_stream");
        check((*find(F["talkgroups"], "id", "100"))["emerg"].n == 1, "emergency: tallied on TG 100");
    }

    // ---- auto mode: family inferred from real traffic, not the banner ----
    {
        AssocModel m;
        m.begin_stream(1, "auto", 0);
        line(m, 1, "Decoding DMR BS/MS Simplex", 900);        // banner (unknown) -> ignored
        line(m, 1, "17:30:46 Sync: +P25p1 NAC/CC: 293;  TSBK", 1000);
        line(m, 1, "2023/10/02 10:23:18 P25 TGT: 00000100; SRC: 00002048; NAC: 293; ", 1100);
        J j = snap(m, 1100);
        check(j["families"].has("p25") && !j["families"].has("dmr"), "auto: inferred P25 from the sync line, not the DMR banner");
    }

    // ---- streams that never decode anything leave no trace ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        // dsd-fme's startup banner reaches the model via on_suppressed...
        for (const char* b : {"Build Version: AW -128-NOTFOUND ", "MBElib Version: 1.3.0",
                              "Decoding DMR BS/MS Simplex", "Audio In Device: -"})
            line(m, 1, b, 1000);
        // ...and noise yields at most CRC-failed garbage.
        line(m, 1, " SLOT 1 TGT=999 SRC=888 Group Call (CRC ERR)", 1100);
        check(snap(m, 1200)["families"].size() == 0,
              "dead stream: banner + CRC-failed noise create no tab, network or radio");
        m.end_stream(1);
        m.remove_session(1);
        check(snap(m, 1300)["families"].size() == 0, "dead stream: still nothing after it ends");
    }
    {
        // Identity heard before the first traffic is kept, so the network comes
        // out identified -- with no "Unidentified" bucket ever created.
        AssocModel m;
        m.begin_stream(1, "p25p1", 0);
        m.ingest(1, classify_dsd_fme_line(" LRA [00] CFVA [3] RFSS[004] SITE [012] SYSID [3A1]"), 900);
        m.ingest(1, classify_dsd_fme_line(" CHAN-T [52E6] CHAN-R [50D7] SSC [70] WACN [BEE00]"), 950);
        check(snap(m, 950)["families"].size() == 0, "pre-traffic: identity broadcasts alone don't register the stream");
        line(m, 1, "17:30:46 Sync: +P25p1 NAC/CC: 293; RFSS: 004; Site: 012;  TSBK", 1000);
        J j = snap(m, 1000);
        const J& N = j["families"]["p25"]["networks"];
        check(N.size() == 1 && N.at(0)["key"].s == "wacn:BEE00/sys:3A1",
              "pre-traffic: first sync registers the stream already identified as WACN/SYS");
    }
    {
        // After Clear, a still-running stream re-enters on its next real traffic.
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        line(m, 1, " SLOT 1 TGT=5 SRC=6 Group Call ", 1000);
        m.clear();
        line(m, 1, "Decoding DMR BS/MS Simplex", 1100);
        check(snap(m, 1100)["families"].size() == 0, "clear: banner-type lines don't re-register a cleared stream");
        line(m, 1, " SLOT 1 TGT=5 SRC=6 Group Call ", 1200);
        check(snap(m, 1200)["families"].has("dmr"), "clear: real traffic does");
    }

    // ---- when a stream ends, networks that never carried a call and only
    //      meant something for that stream (unidentified, stream-scoped) are
    //      dropped; identified ones (system id, code on a known channel) stay ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        for (int k = 0; k < 3; ++k) line(m, 1, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=04 | VC6 ", 1000 + k);
        check(snap(m, 1100)["families"]["dmr"]["networks"].size() == 1, "prune: a sync-only network is shown while the stream runs");
        m.end_stream(1);
        check(snap(m, 1200)["families"].size() == 0, "prune: ...and dropped (with its empty protocol) when the stream ends");
    }
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        for (int k = 0; k < 2; ++k) line(m, 1, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=04 | VC6 ", 1000 + k);
        line(m, 1, " SLOT 1 TGT=9 SRC=3112 Group Call ", 1100);
        m.end_stream(1);
        m.remove_session(1);
        J j = snap(m, 1200);
        check(j["families"]["dmr"]["networks"].size() == 1 && j["families"]["dmr"]["calls"].size() == 1,
              "prune: a network with calls is kept after its stream ends and disconnects");
    }
    {
        // Retune leftover: CC 4 (sync only), then CC 7 with a call. At the end
        // only the CC 4 bucket goes.
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        for (int k = 0; k < 2; ++k) line(m, 1, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=04 | VC6 ", 1000 + k);
        for (int k = 0; k < 2; ++k) line(m, 1, "19:55:55 Sync: +DMR  slot1  [SLOT1] | Color Code=07 | VC6 ", 9000 + k);
        line(m, 1, " SLOT 1 TGT=20 SRC=2 Group Call ", 9100);
        check(snap(m, 9100)["families"]["dmr"]["networks"].size() == 2, "prune: both buckets present mid-stream");
        m.end_stream(1);
        J j = snap(m, 9200);
        check(j["families"]["dmr"]["networks"].size() == 1 && j["families"]["dmr"]["networks"].at(0)["key"].s == "cc:7@s1",
              "prune: the call-less CC 4 bucket is dropped, CC 7 (with a call) kept");
    }
    {
        // A quiet control channel (a system id, no calls) whose client
        // reconnects: its network -- and protocol -- stay when the streams end.
        AssocModel m;
        m.begin_stream(1, "p25p1", 0);
        m.begin_stream(2, "p25p1", 0);
        for (std::uint64_t sid : {1, 2}) {
            line(m, sid, "17:30:46 Sync: +P25p1 NAC/CC: 293; RFSS: 004; Site: 012;  TSBK", 900);
            m.ingest(sid, classify_dsd_fme_line(" LRA [00] CFVA [3] RFSS[004] SITE [012] SYSID [3A1]"), 910);
            line(m, sid, "17:30:47 Sync: +P25p1 NAC/CC: 293;  TSBK", 920);
        }
        m.end_stream(1);
        check(snap(m, 1000)["families"]["p25"]["networks"].size() == 1, "prune: kept while another stream is still on it");
        m.end_stream(2);
        m.remove_session(1);
        m.remove_session(2);
        check(snap(m, 1100)["families"]["p25"]["networks"].size() == 1,
              "prune: a call-less network with a system id stays after every stream on it ends");
    }
    {
        // Partial identities a short session had (a NAC, a SYS without the
        // WACN) are dropped when the full system network covers them.
        AssocModel m;
        m.begin_stream(1, "p25p1", 0, "", 380475000);                       // the full identity
        line(m, 1, "15:01:40 Sync: +P25p1 WACN: 580A0; SYS: 006; NAC/CC: 00D; RFSS: 008; Site: 008;  TSBK", 1000);
        line(m, 1, "15:01:40 Sync: +P25p1 WACN: 580A0; SYS: 006; NAC/CC: 00D; RFSS: 008; Site: 008;  TSBK", 1010);
        m.begin_stream(2, "p25p1", 0, "", 380475000);                       // a NAC only, then gone
        line(m, 2, "15:01:42 Sync: +P25p1 NAC/CC: 00D;  TSBK", 1100);
        line(m, 2, "15:01:42 Sync: +P25p1 NAC/CC: 00D;  TSBK", 1110);
        m.remove_session(2);
        m.begin_stream(3, "p25p1", 0, "", 380487500);                       // SYS without WACN, then gone
        line(m, 3, "15:01:42 Sync: +P25p1 NAC/CC: 00D;  TSBK", 1200);
        line(m, 3, "15:01:42 Sync: +P25p1 NAC/CC: 00D;  TSBK", 1210);
        line(m, 3, " RFSS Status Broadcast - Implicit", 1220);
        line(m, 3, "  LRA [08] SYSID [006] RFSS ID [008] SITE ID [008] CHAN [0026] SSC [70]", 1230);
        m.remove_session(3);
        m.remove_session(1);
        J j = snap(m, 1300);
        const J& N = j["families"]["p25"]["networks"];
        check(N.size() == 1 && N.at(0)["key"].s == "wacn:580A0/sys:006",
              "prune: a NAC-only / SYS-only leftover is dropped -- the full system network covers it -- and that one stays");
    }
    {
        // A code on a known channel stays too; an unidentified channel bucket goes.
        AssocModel m;
        m.begin_stream(1, "dmr", 0, "", 460175000);
        for (int k = 0; k < 3; ++k) line(m, 1, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=05 | VC6 ", 1000 + k);
        m.begin_stream(2, "dmr", 0, "", 453075000);
        line(m, 2, "19:54:55 Sync: +DMR  slot1  [SLOT1] | VC6 ", 1000);
        check(snap(m, 1100)["families"]["dmr"]["networks"].size() == 2, "prune: channel CC 5 and an unidentified channel, mid-stream");
        m.remove_session(1);
        m.remove_session(2);
        J j = snap(m, 1200);
        check(j["families"]["dmr"]["networks"].size() == 1 && j["families"]["dmr"]["networks"].at(0)["key"].s == "cc:5@460175000",
              "prune: a code on a known channel stays; the unidentified channel goes");
    }
    {
        // Radios known only through a dropped network go with it; a radio
        // with calls elsewhere just loses the dropped network.
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        m.begin_stream(2, "dmr", 0);
        for (int k = 0; k < 2; ++k) {
            line(m, 1, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=01 | VC6 ", 1000 + k);
            line(m, 2, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=02 | VC6 ", 1000 + k);
        }
        line(m, 1, " SLOT 1 TGT=9 SRC=500 Group Call ", 1100);                   // a real call on stream 1
        DsdEvent reg; reg.kind = "sync"; reg.slot = "1"; reg.source_id = "500";   // radio 500 merely seen on stream 2
        m.ingest(2, reg, 1150);
        DsdEvent lone = reg; lone.source_id = "777";                              // radio 777 only ever on stream 2
        m.ingest(2, lone, 1160);
        m.end_stream(2);
        J j = snap(m, 1200);
        const J& F = j["families"]["dmr"];
        check(F["networks"].size() == 1, "prune: stream 2's call-less network dropped");
        check(!find(F["radios"], "id", "777"), "prune: a radio known only through it is dropped");
        const J* r = find(F["radios"], "id", "500");
        check(r && (*r)["networks"].size() == 1 && (*r)["networks"].at(0).s == "cc:1@s1",
              "prune: a radio with calls elsewhere is kept, minus the dropped network");
    }
    {
        // A restart (new start on the same session) ends the previous stream too.
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        for (int k = 0; k < 2; ++k) line(m, 1, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=04 | VC6 ", 1000 + k);
        m.begin_stream(1, "dmr", 0);
        check(snap(m, 1100)["families"].size() == 0, "prune: restarting a session drops its call-less network");
    }

    // ---- paging and streams without begin_stream are ignored ----
    {
        AssocModel m;
        m.begin_stream(1, "pocsag", 0);
        DsdEvent p; p.kind = "page"; p.talkgroup = "1234567"; p.message = "hello";
        m.ingest(1, p, 1000);
        m.ingest(99, classify_dsd_fme_line(" SLOT 1 TGT=100 SRC=2048 Group Call "), 1000);
        check(snap(m, 1000)["families"].size() == 0, "paging / unregistered streams are not modelled");
    }

    // ---- bounded memory + clear() ----
    {
        AssocModel m;
        check(m.max_calls() == kDefaultMaxCalls, "bounded: 5000 calls per protocol by default (DSD_NET_MAX_CALLS)");
        m.set_max_calls(400);
        m.set_since(0);
        m.begin_stream(1, "dmr", 0);
        for (int i = 0; i < 450; ++i)
            line(m, 1, " SLOT 1 TGT=" + std::to_string(1000 + i) + " SRC=7 Group Call ", 1000 + i * 5000LL);
        J j = snap(m, 1000 + 450 * 5000LL);
        check(j["families"]["dmr"]["calls"].size() == 400 && j["max_calls"].n == 400, "bounded: calls list capped at max_calls");
        check((*find(j["families"]["dmr"]["radios"], "id", "7"))["tgs"].size() == AssocModel::kMaxEdgesPerNode,
              "bounded: per-radio talkgroup edges capped");
        m.clear();
        check(snap(m, 0)["families"].size() == 0, "clear(): everything forgotten");
        line(m, 1, " SLOT 1 TGT=5 SRC=6 Group Call ", 1);
        check(snap(m, 1)["families"].has("dmr"), "clear(): live stream keeps working afterwards");
    }

    // ---- export: native JSON (re-openable) and GraphML ----
    {
        AssocModel m;
        m.begin_stream(1, "p25p1", 0);
        line(m, 1, "17:30:46 Sync: +P25p1 NAC/CC: 293; RFSS: 004; Site: 012;  TSBK", 900);
        m.ingest(1, classify_dsd_fme_line(" LRA [00] CFVA [3] RFSS[004] SITE [012] SYSID [3A1]"), 910);
        line(m, 1, " CHAN-T [52E6] CHAN-R [50D7] SSC [70] WACN [BEE00]", 920);
        line(m, 1, "P25 TGT: 00000100; SRC: 00012001; NAC: 293; ", 1000);
        line(m, 1, "P25 TGT: 00000200; SRC: 00012001; NAC: 293; ", 6000);
        line(m, 1, "P25 TGT: 00013002; SRC: 00012003; NAC: 293; ", 12000);
        line(m, 1, " P25 LCW  Unit to Unit Voice Channel User", 12100);
        m.begin_stream(2, "dmr", 0);
        for (int k = 0; k < 2; ++k) line(m, 2, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=03 | VC1 ", 13000 + k);
        DsdEvent al; al.kind = "call"; al.slot = "1"; al.talkgroup = "9"; al.source_id = "3112";
        al.alias = "K\xC3\xA9<b>&\"x\x01\xFF";                     // valid UTF-8 e-acute, markup, ctrl, bad byte
        m.ingest(2, al, 13100);

        bool ok = false;
        const std::string ex = m.to_export_json(14000);
        J e = parse(ex, &ok);
        check(ok, "export: the JSON export is well-formed");
        check(e["format"].s == "dsd-net-export" && e["format_version"].n == 1 && e["exported"].n == 14000 &&
              e["now"].n == 14000 && e["source"].s == "dsd-server", "export: self-describing header (format, version, time)");
        const std::string live = m.to_json(14000);
        check(ex.substr(ex.find("\"families\":")) == live.substr(live.find("\"families\":")),
              "export: 'families' is exactly what /net.json serves (so the explorer can open it)");

        const std::string g = m.to_graphml(14000);
        auto count = [&](const std::string& needle) {
            std::size_t n = 0;
            for (std::size_t p = g.find(needle); p != std::string::npos; p = g.find(needle, p + 1)) ++n;
            return n;
        };
        check(g.rfind("<?xml version=\"1.0\" encoding=\"UTF-8\"?>", 0) == 0 && g.find("</graphml>") != std::string::npos,
              "graphml: XML declaration and closing tag");
        // nodes: p25 = 1 network + 2 TGs + 3 radios; dmr = 1 network + 1 TG + 1 radio
        check(count("<node id=") == 9, "graphml: one node per network / talkgroup / radio (9)");
        check(count(">talkgroup</data>") == 3 + 3 && count(">private</data>") == 1,
              "graphml: radio-talkgroup edges and the private-call edge");
        check(g.find("<node id=\"p25:r:12001\">") != std::string::npos && g.find("<node id=\"p25:t:100\">") != std::string::npos,
              "graphml: node ids keep protocols apart (p25:r:12001, p25:t:100)");
        check(g.find("K\xC3\xA9&lt;b&gt;&amp;&quot;x??") != std::string::npos,
              "graphml: markup escaped, valid UTF-8 kept, control + invalid bytes replaced (well-formed XML)");
        check(g.find("WACN BEE00 \xC2\xB7 SYS 3A1") != std::string::npos, "graphml: network labels carried");
    }

    // ---- known channel frequency: weak codes keyed by channel, not stream ----
    {
        check(AssocModel::channel_hz(434425000.0) == 434425000 && AssocModel::channel_hz(434425500.0) == 434425000 &&
                  AssocModel::channel_hz(451006250.0) == 451006250 && AssocModel::channel_hz(154452400.0) == 154452500 &&
                  AssocModel::channel_hz(0) == 0 && AssocModel::channel_hz(434425123.0, 1) == 434425123,
              "channel_hz: snaps to 1.25 kHz (6.25 kHz and 2.5 kHz plans unchanged, +-625 Hz absorbed)");
        check(freq_text(434425000) == "434.4250 MHz" && freq_text(451006250) == "451.00625 MHz" &&
                  freq_text(154452500) == "154.4525 MHz",
              "freq_text: four decimals, more only when needed");

        const std::int64_t F1 = 434425000, F2 = 438500000;
        const char* cc1 = "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=01 | VC6 ";
        AssocModel m;
        m.begin_stream(1, "dmr", 0, "", F1);
        m.begin_stream(2, "dmr", 0, "", F1);       // a second receiver session on the same channel
        m.begin_stream(3, "dmr", 0, "", F2);       // same color code, another channel
        m.begin_stream(4, "dmr", 0);               // same color code, frequency unknown
        for (std::uint64_t sid : {1, 2, 3, 4})
            for (int k = 0; k < 2; ++k) line(m, sid, cc1, 1000 + k);
        for (std::uint64_t sid : {1, 2, 3, 4}) line(m, sid, " SLOT 1 TGT=9 SRC=3112 Group Call ", 1100 + sid);
        J j = snap(m, 1200);
        const J& F = j["families"]["dmr"];
        const J* n1 = find(F["networks"], "key", "cc:1@434425000");
        check(n1 && (*n1)["confidence"].s == "channel" && (*n1)["label"].s == "Color Code 1 \xC2\xB7 434.4250 MHz" &&
                  (*n1)["sessions"].n == 2 && (*n1)["freqs"].size() == 1 && (*n1)["freqs"].at(0).n == F1,
              "channel: two sessions on 434.425 MHz with CC 1 are ONE network ('Color Code 1 · 434.4250 MHz')");
        check(find(F["networks"], "key", "cc:1@438500000") && find(F["networks"], "key", "cc:1@s4") && F["networks"].size() == 3,
              "channel: the same code on another channel, or on an unknown one, stays separate");
        check(n1 && (*n1)["calls"].n == 1 && F["calls"].size() == 3,
              "channel: the call both sessions on the channel heard is one call (deduplicated, counted once)");
        const J* c1 = nullptr;
        for (const auto& c : F["calls"].a) if (c["net"].s == "cc:1@434425000") c1 = &c;
        check(c1 && (*c1)["freq"].n == F1 && (*c1)["streams"].n == 2, "channel: calls carry their frequency");

        // Reconnect: a new session on the same channel lands in the same network.
        m.end_stream(1, 2000);
        m.remove_session(1, 2000);
        m.begin_stream(9, "dmr", 3000, "", F1);
        for (int k = 0; k < 2; ++k) line(m, 9, cc1, 3100 + k);
        line(m, 9, " SLOT 1 TGT=9 SRC=4000 Group Call ", 3200);
        J j2 = snap(m, 3300);
        const J* n9 = find(j2["families"]["dmr"]["networks"], "key", "cc:1@434425000");
        check(n9 && (*n9)["calls"].n == 2 && j2["families"]["dmr"]["networks"].size() == 3,
              "channel: a reconnect on the same channel continues the same network (no new bucket)");

        // Nothing decoded but traffic on a known channel: the channel is the identity.
        m.begin_stream(10, "dmr", 4000, "", 446006250);
        line(m, 10, " SLOT 1 TGT=5 SRC=6 Group Call ", 4100);
        J j3 = snap(m, 4200);
        const J* nu = find(j3["families"]["dmr"]["networks"], "key", "ch@446006250");
        check(nu && (*nu)["confidence"].s == "channel" && (*nu)["label"].s == "Unidentified \xC2\xB7 446.00625 MHz",
              "channel: an unidentified stream on a known channel is keyed by the channel");

        // Calls before the code is confirmed (as on the real capture): two
        // sessions fill the channel's "Unidentified" bucket, which becomes
        // the color-code network once the code is believed.
        {
            AssocModel q;
            q.begin_stream(1, "dmr", 0, "", F1);
            q.begin_stream(2, "dmr", 0, "", F1);
            for (std::uint64_t sid : {1, 2}) line(q, sid, " SLOT 1 TGT=1 SRC=123 Group Call ", 1000 + sid);
            check(find(snap(q, 1100)["families"]["dmr"]["networks"], "key", "ch@434425000") != nullptr,
                  "channel: traffic before any code -> the channel's Unidentified bucket");
            for (std::uint64_t sid : {1, 2})
                for (int k = 0; k < 2; ++k) line(q, sid, cc1, 1200 + k);
            q.end_stream(1, 2000);
            q.end_stream(2, 2000);
            J jq = snap(q, 2100);
            const J& NQ = jq["families"]["dmr"]["networks"];
            check(NQ.size() == 1 && NQ.at(0)["key"].s == "cc:1@434425000" && NQ.at(0)["calls"].n == 1 &&
                      NQ.at(0)["sessions"].n == 2,
                  "channel: ...which becomes 'Color Code 1' once the code is believed, even when shared by two sessions");
        }

        // Retune: the stream's identity starts over on the new channel.
        m.retune_stream(9, F2, 5000);
        for (int k = 0; k < 2; ++k) line(m, 9, cc1, 5100 + k);
        line(m, 9, " SLOT 1 TGT=9 SRC=4001 Group Call ", 5200);
        J j4 = snap(m, 5300);
        const J* n2 = find(j4["families"]["dmr"]["networks"], "key", "cc:1@438500000");
        check(n2 && (*n2)["calls"].n == 2 && (*find(j4["families"]["dmr"]["networks"], "key", "cc:1@434425000"))["calls"].n == 2,
              "retune: after a retune the stream's calls go to the new channel's network");

        // GraphML lists a network's channels.
        check(m.to_graphml(5300).find("<data key=\"frequencies\">438.5000 MHz</data>") != std::string::npos,
              "graphml: networks carry their channel frequencies");
    }
    {
        // P25: a NAC heard on a known channel still resolves to the strong
        // system carrying that NAC; a retune prunes a call-less network.
        AssocModel m;
        m.begin_stream(1, "p25p1", 0, "", 851012500);
        line(m, 1, "17:30:46 Sync: +P25p1 NAC/CC: 717; RFSS: 001; Site: 097;  TSBK", 1000);
        m.ingest(1, classify_dsd_fme_line(" LRA [00] CFVA [3] RFSS[001] SITE [097] SYSID [715]"), 1010);
        line(m, 1, " CHAN-T [52E6] CHAN-R [50D7] SSC [70] WACN [BEE0A]", 1020);
        line(m, 1, "P25 TGT: 00000100; SRC: 00002048; NAC: 717; ", 1100);
        m.begin_stream(2, "p25p1", 0, "", 852012500);
        line(m, 2, "17:30:47 Sync: +P25p1 NAC/CC: 717;  LDU1", 1200);
        line(m, 2, "P25 TGT: 00000100; SRC: 00002099; NAC: 717; ", 1210);
        J j = snap(m, 1300);
        const J& P = j["families"]["p25"];
        check(P["networks"].size() == 1 && P["networks"].at(0)["key"].s == "wacn:BEE0A/sys:715" &&
                  P["networks"].at(0)["freqs"].size() == 2,
              "channel/P25: a NAC-only stream on another channel joins the WACN/SYS system; both channels listed");
        m.begin_stream(3, "dmr", 0, "", 434425000);
        for (int k = 0; k < 2; ++k) line(m, 3, "19:54:55 Sync: +DMR  slot1  [SLOT1] | Color Code=04 | VC6 ", 2000 + k);
        m.retune_stream(3, 434450000, 2100);
        check(snap(m, 2200)["families"].has("dmr") == false, "retune: a call-less network of the old channel is dropped");
    }

    // ---- D-STAR: header on AMBE lines (payload logging), blank SRC, DIRECT ----
    {
        AssocModel m;
        m.begin_stream(1, "dstar", 0, "", 429998750);
        const std::string hdr = " AMBE F094B64EF43600 err = [0] [0]  RPT 2: DIRECT   RPT 1: DIRECT   DST: CQCQCQ   ";
        for (int k = 0; k < 10; ++k) line(m, 1, hdr + "SRC:              INTERRUPTED", 1000 + 100 * k);
        m.end_stream(1, 3000);
        m.remove_session(1, 3000);
        J j = snap(m, 3100);
        check(j["families"].has("dstar"), "D-STAR: a call heard without its talker keeps the network after the stream ends");
        const J& D = j["families"]["dstar"];
        check(D["calls"].size() == 1 && D["calls"].at(0)["tgt"].s == "CQCQCQ" && D["calls"].at(0)["src"].s.empty(),
              "D-STAR: blank SRC -> one call to CQCQCQ, no source");
        const J* tg = find(D["talkgroups"], "id", "CQCQCQ");
        check(tg && (*tg)["calls"].n == 1 && (*tg)["radios"].size() == 0, "D-STAR: CQCQCQ counted (1 call), no radio");
        check(D["radios"].size() == 0, "D-STAR: no radio made up for the missing source");
        check(D["networks"].size() == 1 && D["networks"].at(0)["key"].s.find("DIRECT") == std::string::npos,
              "D-STAR: RPT 1: DIRECT (simplex) is not a repeater network");

        m.begin_stream(2, "dstar", 4000, "", 429998750);
        for (int k = 0; k < 5; ++k) line(m, 2, hdr + "SRC: N0CALL  /ID51", 4000 + 100 * k);
        m.end_stream(2, 5000);
        J j2 = snap(m, 5100);
        const J& D2 = j2["families"]["dstar"];
        const J* tg2 = find(D2["talkgroups"], "id", "CQCQCQ");
        check(tg2 && (*tg2)["calls"].n == 2 && (*tg2)["radios"].size() == 1, "D-STAR: a call with its talker adds the radio");
    }

    // ---- data services and position reports ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0, "", 460175000);
        DmrSlotCarry carry;
        auto L = [&](const char* l, std::int64_t t) { DsdEvent e = classify_dsd_fme_line(l); carry.apply(e); m.ingest(1, e, t); };
        L("21:21:28 Sync: +DMR  [SLOT1]  slot2  | Color Code=05 | DATA ", 1000);
        L("Slot 1 Data Header - Indiv - Confirmed Delivery - Response Requested - Source: 64250 Target: 13416", 1100);
        L("Slot 1 Data Header - Extended - SAP 01 [Moto NET] - MFID 10 [Moto]", 1150);
        L(" DST(MNIS): 00013416; MNIS ARS;   ???: C5E5", 1200);
        L("Slot 1 Data Header - Indiv - Response Packet - Source: 13416 Target: 64250", 1400);
        J j = snap(m, 1500);
        const J& F = j["families"]["dmr"];
        const J* ars = nullptr; const J* ack = nullptr;
        for (const auto& c : F["calls"].a) { if (c["src"].s == "64250") ars = &c; if (c["src"].s == "13416") ack = &c; }
        check(ars && (*ars)["svc"].s == "ars", "svc: the gateway's packet is labelled ARS (the MNIS line after its header)");
        check(ack && (*ack)["svc"].s == "ack", "svc: the reply is an ACK");

        AssocModel p;
        p.begin_stream(2, "p25p1", 0, "", 851012500);
        auto P = [&](const char* l, std::int64_t t) { p.ingest(2, classify_dsd_fme_line(l), t); };
        P("17:31:49 Sync: +P25p1 NAC/CC: 293; LDU1", 1000);
        P(" TG: 100; SRC: 6745697; ", 1050);
        P(" LCW MFID90 (Moto) GPS: Lat: 39.03494 N Lon: -76.98460 W (39.03494, -76.98460) Current Fix;", 1100);
        J jp = snap(p, 1200);
        const J& PF = jp["families"]["p25"];
        check(PF["calls"].size() == 1 && PF["calls"].at(0)["pos"].s == "39.03494,-76.98460", "gps: the position is on the call");
        const J* rad = find(PF["radios"], "id", "6745697");
        check(rad && (*rad)["pos"].s == "39.03494,-76.98460" && (*rad)["pos_t"].n == 1100, "gps: and is the radio's last position");
        // Each new position is added to the radio's track; a repeat moves
        // the last fix's time on.
        P(" LCW MFID90 (Moto) GPS: Lat: 39.03478 N Lon: -76.98450 W (39.03478, -76.98450) Current Fix;", 1150);
        P(" LCW MFID90 (Moto) GPS: Lat: 39.03478 N Lon: -76.98450 W (39.03478, -76.98450) Current Fix;", 1180);
        J jt = snap(p, 1190);
        const J* tr = find(jt["families"]["p25"]["radios"], "id", "6745697");
        check(tr && (*tr)["track"].size() == 2 && (*tr)["track"].at(0).at(1).s == "39.03494,-76.98460" &&
                  (*tr)["track"].at(1).at(1).s == "39.03478,-76.98450" && (*tr)["track"].at(1).at(0).n == 1180,
              "track: two distinct fixes, oldest first; a repeat only moves the time on");
        std::vector<std::pair<std::int64_t, std::string>> cap;
        for (int i = 0; i < 130; ++i) track_add(cap, i, std::to_string(i) + ".0,0.0");
        check(cap.size() == kMaxTrack && cap.front().first == 30 && cap.back().first == 129, "track: capped, the oldest go first");
        // An export keeps both.
        Dataset d;
        std::string err;
        check(dataset_from_export_text(p.to_export_json(1300), "x", d, &err) &&
                  d.fams["p25"].calls.size() == 1 && d.fams["p25"].calls[0].pos == "39.03478,-76.98450" &&
                  d.fams["p25"].radios["6745697"].pos == "39.03478,-76.98450" &&
                  d.fams["p25"].radios["6745697"].track.size() == 2,
              "export: positions and the track survive an export / import");
        // An export names each call's audio file: a merge keeps the name (the
        // file view plays it from an "export with audio" zip); a live import
        // drops it -- the file isn't on this server.
        std::string ex = p.to_export_json(1300);
        const std::string at = "\"pos\":\"39.03478,-76.98450\"";
        const std::size_t pp = ex.find(at, ex.find("\"calls\":["));
        check(pp != std::string::npos, "export: the call to tag with audio");
        ex.insert(pp, "\"audio\":\"call_1_x_1.wav\",\"audio_ms\":2000,");
        Dataset da;
        check(dataset_from_export_text(ex, "x", da, &err) && da.fams["p25"].calls.size() == 1 &&
                  da.fams["p25"].calls[0].audio == "call_1_x_1.wav" && da.fams["p25"].calls[0].audio_ms == 2000,
              "export: a call's audio file name is read back");
        AssocModel live;
        live.set_identity("0123456789abcdef", "other");
        live.import_export(ex, "x.json", 2000);
        check(live.to_json(2000).find("call_1_x_1.wav") == std::string::npos, "import: an imported call has no audio to play");

        // ...until Import uploads the zip's WAV for it (import_audio): then it
        // plays, served as "i<id>_<name>" from the import's own folder, which
        // goes with the import.
        namespace fs = std::filesystem;
        const std::string root = (fs::temp_directory_path() / ("dsd_imp_audio_" + std::to_string(::getpid()))).string();
        fs::create_directories(root + "/stale");
        AssocModel ia;
        ia.set_identity("0123456789abcdef", "other");
        ia.use_import_audio_dir(root, 1000);
        check(!fs::exists(root + "/stale"), "import audio: the folder is emptied when set (imports don't outlive a restart)");
        const auto ir = ia.import_export(ex, "x.json", 2000);
        check(ir.id == 1 && ia.to_json(2000).find("\"audio_files\":1,\"audio_have\":0") != std::string::npos,
              "import audio: the import lists 1 recording wanted, none uploaded yet");
        std::string wav = "RIFF" + std::string(4, '\0') + "WAVEfmt " + std::string(32, '\0');   // 44-byte header
        wav += std::string(100, '\x01');
        check(ia.import_audio(1, "call_9_x_9.wav", wav).status == "unknown", "import audio: a file no call names is refused");
        check(ia.import_audio(1, "../call_1_x_1.wav", wav).status == "unknown", "import audio: a path is refused");
        check(ia.import_audio(7, "call_1_x_1.wav", wav).status == "unknown", "import audio: an unknown import is refused");
        check(ia.import_audio(1, "call_1_x_1.wav", std::string(100, 'x')).status == "invalid", "import audio: a non-WAV is refused");
        const auto up = ia.import_audio(1, "call_1_x_1.wav", wav);
        check(up.status == "added" && up.calls == 1, "import audio: the WAV is kept and attached to its call");
        const std::string js = ia.to_json(2100);
        check(js.find("\"audio\":\"i1_call_1_x_1.wav\",\"audio_ms\":2000") != std::string::npos &&
                  js.find("\"audio_have\":1") != std::string::npos,
              "import audio: the call now names its served file (with its length)");
        const std::string ap = ia.audio_path("i1_call_1_x_1.wav");
        check(!ap.empty() && fs::file_size(ap) == wav.size(), "import audio: /net/audio serves the uploaded file");
        check(ia.audio_path("i1_call_9_x_9.wav").empty() && ia.audio_path("i1_../x.wav").empty(),
              "import audio: nothing else under the import is served");
        check(ia.import_audio(1, "call_1_x_1.wav", wav).status == "have", "import audio: a repeat upload is a no-op");
        check(ia.to_export_json(2100).find("i1_call_1_x_1.wav") != std::string::npos,
              "import audio: an export of the view names the imported audio (so export with audio carries it on)");
        check(AssocModel::import_audio_base("i3_i12_call_1_x_1.wav") == "call_1_x_1.wav" &&
                  AssocModel::import_audio_base("i3_notacall.wav").empty(),
              "import audio: an import of an import maps back to the original file name");
        ia.remove_import(1);
        check(ia.audio_path("i1_call_1_x_1.wav").empty() && !fs::exists(ap), "import audio: removing the import deletes its audio");
        // The cap (1000 bytes here) across all imports.
        const auto ir2 = ia.import_export(ex, "x.json", 2200);
        check(ir2.id == 2 && ia.import_audio(2, "call_1_x_1.wav", wav + std::string(1000, '\x01')).status == "full",
              "import audio: past the cap a file is refused (full)");
        check(ia.import_audio(2, "call_1_x_1.wav", wav).status == "added", "import audio: within the cap it is kept");
        ia.clear_imports();
        check(!fs::exists(root + "/2"), "import audio: Remove all deletes every import's audio");
        fs::remove_all(root);
    }

    // ---- JSON escaping of decoder-derived text ----
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        line(m, 1, " SLOT 1 TGT=1 SRC=2 Group Call ", 1000);
        DsdEvent msg; msg.kind = "message"; msg.slot = "1"; msg.message = "quote\" back\\slash <b>\x01";
        m.ingest(1, msg, 1100);
        bool ok = false;
        J j = snap(m, 1100, &ok);
        check(ok && j["families"]["dmr"]["calls"].at(0)["text"].s.find("quote\" back\\slash <b>") == 0,
              "JSON: quotes / backslashes / control chars escaped, text round-trips");
    }

    // ---- voice quality: the AMBE-frame verdict reaches a call's JSON ----
    // From the event stream, so it runs with no recording (quality_on_ default
    // on). Distributions match the real captures (docs/AUDIO_QUALITY_CHECK.md).
    // Clear: ~90% speech + ~10% silence -> good.
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        line(m, 1, " SLOT 1 TGT=9 SRC=3112 Group Call ", 1000);
        feed_frames(m, 1, 86, 226, 1001);                 // speech
        feed_frames(m, 1, 124, 26, 1230);                 // silence
        J j = snap(m, 1300);
        const J& c = j["families"]["dmr"]["calls"].at(0);
        check(c["q"].s == "good", "quality: clear call (10% silence, 0 junk) -> good");
        check(c.has("qj") && c.has("qs") && c.has("qn"), "quality: junk/silence/frame diagnostics present");
    }
    // Encrypted counting call: ~85% speech, ~0% silence, ~15% junk -> unusable.
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        line(m, 1, " SLOT 1 TGT=9 SRC=3112 Group Call ", 1000);
        feed_frames(m, 1, 86, 199, 1001);                 // speech
        feed_frames(m, 1, 121, 22, 1201);                 // erasure
        feed_frames(m, 1, 127, 12, 1224);                 // tone
        feed_frames(m, 1, 124, 1, 1237);                  // one silence
        J j = snap(m, 1300);
        const J& c = j["families"]["dmr"]["calls"].at(0);
        check(c["q"].s == "unusable", "quality: encrypted/garbled call (15% junk) -> unusable");
        check(c["qj"].n > 0.13, "quality: junk fraction ~0.15 in JSON");
    }
    // Encrypted continuous call: low junk but ~0% silence -> marginal.
    {
        AssocModel m;
        m.begin_stream(1, "dmr", 0);
        line(m, 1, " SLOT 1 TGT=9 SRC=3112 Group Call ", 1000);
        feed_frames(m, 1, 86, 257, 1001);                 // all speech, no pauses
        feed_frames(m, 1, 121, 1, 1259);                  // 1 erasure (junk ~0.4%)
        J j = snap(m, 1300);
        const J& c = j["families"]["dmr"]["calls"].at(0);
        check(c["q"].s == "marginal", "quality: continuous (no silence) call -> marginal");
    }

    if (g_failures == 0) {
        std::printf("\nALL ASSOC MODEL TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
