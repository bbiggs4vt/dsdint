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
#include <cstdio>
#include <map>
#include <string>
#include <vector>

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
        m.begin_stream(1, "dmr", 0);
        for (int i = 0; i < 450; ++i)
            line(m, 1, " SLOT 1 TGT=" + std::to_string(1000 + i) + " SRC=7 Group Call ", 1000 + i * 5000LL);
        J j = snap(m, 1000 + 450 * 5000LL);
        check(j["families"]["dmr"]["calls"].size() == AssocModel::kMaxCalls, "bounded: calls ring capped at kMaxCalls");
        check((*find(j["families"]["dmr"]["radios"], "id", "7"))["tgs"].size() == AssocModel::kMaxEdgesPerNode,
              "bounded: per-radio talkgroup edges capped");
        m.clear();
        check(snap(m, 0)["families"].size() == 0, "clear(): everything forgotten");
        line(m, 1, " SLOT 1 TGT=5 SRC=6 Group Call ", 1);
        check(snap(m, 1)["families"].has("dmr"), "clear(): live stream keeps working afterwards");
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

    if (g_failures == 0) {
        std::printf("\nALL ASSOC MODEL TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
