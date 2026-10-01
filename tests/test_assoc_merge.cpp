// test_assoc_merge.cpp
//
// Unit tests for merging network-explorer data (src/assoc_merge.hpp): several
// exports merged into one view (Open... with several files, net-merge), and
// exports imported into a live model (Import...). Real dsd-fme lines feed real
// models with explicit timestamps; their exports are merged and the results
// parsed back. The rules under test: ids and strong network keys join across
// sources, weak network keys stay per server run, and nothing is ever counted
// twice (provenance: skip / replace / refuse).

#include "../src/assoc_model.hpp"
#include "../src/dsd_process.hpp"

#include <zlib.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace dsdsrv;

static int g_failures = 0;
static void check(bool c, const std::string& what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what.c_str());
    if (!c) ++g_failures;
}

static void line(AssocModel& m, std::uint64_t sid, const std::string& l, std::int64_t t) {
    m.ingest(sid, classify_dsd_fme_line(l), t);
}
static std::string pad8(const std::string& id) { return std::string(8 - std::min<std::size_t>(8, id.size()), '0') + id; }

// One P25 stream on WACN BEE0A / SYS 715 (a strong identity) with a call.
static void p25_call(AssocModel& m, std::uint64_t sid, std::int64_t t, const std::string& tg, const std::string& src) {
    m.begin_stream(sid, "p25p1", t);
    line(m, sid, "17:30:46 Sync: +P25p1 NAC/CC: 717; RFSS: 001; Site: 097;  TSBK", t + 10);
    m.ingest(sid, classify_dsd_fme_line(" LRA [00] CFVA [3] RFSS[001] SITE [097] SYSID [715]"), t + 20);
    line(m, sid, " CHAN-T [52E6] CHAN-R [50D7] SSC [70] WACN [BEE0A]", t + 30);
    line(m, sid, "2023/10/02 10:23:18 P25 TGT: " + pad8(tg) + "; SRC: " + pad8(src) + "; NAC: 717; ", t + 40);
    m.end_stream(sid, t + 100);
}
// One DMR stream on color code 4 (a weak, per-stream identity) with a call.
static void dmr_call(AssocModel& m, std::uint64_t sid, std::int64_t t, const std::string& tg, const std::string& src) {
    m.begin_stream(sid, "dmr", t);
    line(m, sid, "19:54:55 Sync: +DMR  slot1  [SLOT2] | Color Code=04 | VC6 ", t + 10);
    line(m, sid, "19:54:55 Sync: +DMR  slot1  [SLOT2] | Color Code=04 | VC1 ", t + 20);
    line(m, sid, " SLOT 2 TGT=" + tg + " SRC=" + src + " Group Call  ", t + 30);
    line(m, sid, "19:54:56 Sync: +DMR  slot1  [SLOT2] | Color Code=04 | VC2 ", t + 40);
    m.end_stream(sid, t + 100);
}

static mjson::V parse(const std::string& s, bool* ok = nullptr) {
    mjson::V v;
    const bool good = mjson::parse(s, v);
    if (ok) *ok = good;
    return v;
}
static const mjson::V& fam(const mjson::V& root, const char* f) {
    static mjson::V nul;
    const mjson::V* fs = root.get("families");
    const mjson::V* x = fs ? fs->get(f) : nullptr;
    return x ? *x : nul;
}
static const mjson::V& arr(const mjson::V& f, const char* k) {
    static mjson::V nul;
    const mjson::V* a = f.get(k);
    return a ? *a : nul;
}
static const mjson::V* by(const mjson::V& a, const char* key, const std::string& val) {
    for (const auto& e : a.a) if (e.str(key) == val) return &e;
    return nullptr;
}
static std::set<std::string> keys_of(const mjson::V& a, const char* key) {
    std::set<std::string> s;
    for (const auto& e : a.a) s.insert(e.str(key));
    return s;
}
static std::string families_part(const std::string& j) {
    const std::size_t p = j.find("\"families\":");
    return p == std::string::npos ? std::string() : j.substr(p);
}
static std::string gzip(const std::string& s) {
    z_stream z{};
    deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::string out(deflateBound(&z, s.size()) + 64, '\0');
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(s.data()));
    z.avail_in = static_cast<uInt>(s.size());
    z.next_out = reinterpret_cast<Bytef*>(&out[0]);
    z.avail_out = static_cast<uInt>(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}
static std::string status_of(const std::vector<MergeReport>& r, const std::string& label) {
    for (const auto& x : r) if (x.label == label) return x.status;
    return "-";
}

int main() {
    std::printf("test_assoc_merge\n");

    // Two receivers ("alpha", "bravo"): the same P25 system (strong id) and
    // DMR color code 4 (weak) -- possibly different DMR systems.
    AssocModel A, B;
    A.set_identity("aaaaaaaa11111111", "alpha");
    B.set_identity("bbbbbbbb22222222", "bravo");
    A.set_since(1000);
    B.set_since(1000);
    p25_call(A, 1, 2000, "100", "2048");
    dmr_call(A, 2, 3000, "19535", "2222223");
    const std::string a1 = A.to_export_json(5000);           // alpha, early
    p25_call(A, 3, 6000, "200", "2049");
    const std::string a2 = A.to_export_json(9000);           // alpha, later (contains a1)
    p25_call(B, 1, 2500, "100", "3000");
    dmr_call(B, 2, 3500, "19535", "2222223");
    const std::string b1 = B.to_export_json(8000);

    // ---- export carries the run's provenance ----
    {
        bool ok = false;
        mjson::V e = parse(a2, &ok);
        const mjson::V* s = e.get("sources");
        check(ok && e.str("format") == "dsd-net-export" && e.str("instance") == "aaaaaaaa11111111" &&
                  e.str("name") == "alpha",
              "export: format, instance and name");
        check(s && s->a.size() == 1 && s->a[0].str("instance") == "aaaaaaaa11111111" && s->a[0].num("since") == 1000 &&
                  s->a[0].num("through") == 9000,
              "export: one source -- this run, since 1000 through the export time");
    }

    // ---- merging two receivers ----
    {
        std::vector<MergeReport> rep;
        Dataset d = merge_exports({{"a2.json", a2}, {"b1.json", b1}}, rep);
        check(status_of(rep, "a2.json") == "merged" && status_of(rep, "b1.json") == "merged", "merge: both files merged");
        bool ok = false;
        mjson::V j = parse(export_json(d, 9000, "", "merged"), &ok);
        check(ok, "merge: merged export is well-formed JSON");
        const mjson::V& P = fam(j, "p25");
        check(arr(P, "networks").a.size() == 1 && arr(P, "networks").a[0].str("key") == "wacn:BEE0A/sys:715",
              "merge: the same strong P25 system from both receivers is ONE network");
        check(arr(P, "networks").a[0].num("calls") == 3 && arr(P, "networks").a[0].num("sessions") == 3,
              "merge: ...carrying both receivers' calls and streams");
        const mjson::V* tg = by(arr(P, "talkgroups"), "id", "100");
        check(tg && tg->get("radios") && tg->get("radios")->o.size() == 2 && tg->num("calls") == 2,
              "merge: TG 100 has radios heard by either receiver (2048 by alpha, 3000 by bravo)");
        const mjson::V& D = fam(j, "dmr");
        const auto dk = keys_of(arr(D, "networks"), "key");
        check(dk.size() == 2 && dk.count("cc:4@s2~aaaaaaaa") && dk.count("cc:4@s2~bbbbbbbb"),
              "merge: weak DMR 'color code 4' stays apart per receiver run (qualified keys)");
        const mjson::V* n = by(arr(D, "networks"), "key", "cc:4@s2~bbbbbbbb");
        check(n && n->str("label").find("\xC2\xB7 bravo") != std::string::npos, "merge: ...and labelled with the receiver name");
        const mjson::V* t = by(arr(D, "talkgroups"), "id", "19535");
        const mjson::V* r = by(arr(D, "radios"), "id", "2222223");
        check(t && t->get("networks")->a.size() == 2 && r && r->num("calls") == 2 && r->get("networks")->a.size() == 2,
              "merge: TG 19535 / radio 2222223 link the two DMR networks (that is how sources connect)");
        bool calls_net_ok = true;
        for (const auto& c : arr(D, "calls").a) calls_net_ok = calls_net_ok && dk.count(c.str("net"));
        check(calls_net_ok, "merge: calls point at the qualified network keys");
        const mjson::V& PC = arr(P, "calls");
        bool desc = true, open = false;
        std::set<double> ids;
        for (std::size_t i = 0; i < PC.a.size(); ++i) {
            ids.insert(PC.a[i].num("id"));
            open = open || PC.a[i].boolean("open");
            if (i) desc = desc && PC.a[i - 1].num("start") >= PC.a[i].num("start");
        }
        check(PC.a.size() == 3 && desc && ids.size() == 3 && !open, "merge: calls newest first, unique ids, none 'open'");
        const mjson::V* src = j.get("sources");
        check(src && src->a.size() == 2, "merge: the result lists both sources");

        // Re-merging the merged file with its parts adds nothing.
        std::vector<MergeReport> rep2;
        const std::string m = export_json(d, 9000, "", "merged");
        Dataset d2 = merge_exports({{"m.json", m}, {"a2.json", a2}, {"b1.json", b1}}, rep2);
        check(status_of(rep2, "a2.json") == "skipped" && status_of(rep2, "b1.json") == "skipped" &&
                  families_json(d2) == families_json(d),
              "merge: a merged file re-merged with its own parts -> parts skipped, same result");
        Dataset d3 = merge_exports({{"a2.json", a2}, {"m.json", m}}, rep2);
        check(families_json(d3) == families_json(d), "merge: a part first, then the merged file -> the merge replaces it");

        const std::string g = graphml(d, 9000);
        check(g.find("<graphml") != std::string::npos && g.find("p25:n:wacn:BEE0A/sys:715") != std::string::npos &&
                  g.find("dmr:n:cc:4@s2~aaaaaaaa") != std::string::npos,
              "merge: GraphML of the merged data has the joined and the qualified networks");
    }

    // ---- provenance: same run exported twice, partial overlaps, Clear ----
    {
        std::vector<MergeReport> rep;
        Dataset d = merge_exports({{"a1.json", a1}, {"a2.json", a2}}, rep);
        Dataset only = merge_exports({{"a2.json", a2}}, rep);
        check(status_of(rep, "a1.json") == "replaced" && families_json(d) == families_json(only),
              "provenance: an older export of the same run is replaced by the newer one (not added)");
        rep.clear();
        d = merge_exports({{"a2.json", a2}, {"a1.json", a1}}, rep);
        check(status_of(rep, "a1.json") == "skipped" && families_json(d) == families_json(only),
              "provenance: an older export after the newer one is skipped");
        rep.clear();
        merge_exports({{"a2.json", a2}, {"copy.json", a2}}, rep);
        check(status_of(rep, "copy.json") == "skipped", "provenance: the same file twice is merged once");

        // alpha+bravo merged early, then alpha's later export: it overlaps the
        // merge's alpha part but isn't contained either way -> refused.
        rep.clear();
        Dataset m1 = merge_exports({{"a1.json", a1}, {"b1.json", b1}}, rep);
        const std::string ms = export_json(m1, 8000, "", "merged");
        rep.clear();
        merge_exports({{"m1.json", ms}, {"a2.json", a2}}, rep);
        check(status_of(rep, "a2.json") == "refused", "provenance: a partial overlap (would double-count) is refused");

        // After a Clear the run's new data is a disjoint span: merges fine.
        AssocModel C;
        C.set_identity("cccccccc33333333", "charlie");
        C.set_since(1000);
        p25_call(C, 1, 2000, "100", "4000");
        const std::string c1 = C.to_export_json(3000);
        C.clear(4000);
        p25_call(C, 1, 5000, "100", "4001");
        const std::string c2 = C.to_export_json(6000);
        rep.clear();
        Dataset dc = merge_exports({{"c1.json", c1}, {"c2.json", c2}}, rep);
        mjson::V j = parse(export_json(dc, 6000, "", ""));
        check(status_of(rep, "c2.json") == "merged" && arr(fam(j, "p25"), "radios").a.size() == 2,
              "provenance: before and after a Clear (disjoint spans of one run) both merge");
    }

    // ---- inputs that aren't exports; gzip; exports without provenance ----
    {
        std::vector<MergeReport> rep;
        merge_exports({{"rec.jsonl", "{\"op\":\"header\",\"v\":1,\"t\":1}\n"}, {"x.json", "{\"hello\":1}"},
                       {"bad.json", "{\"families\":"}, {"gz.json.gz", gzip(b1)}},
                      rep);
        check(status_of(rep, "rec.jsonl") == "invalid" && rep[0].message.find("recording") != std::string::npos,
              "inputs: a recording is rejected with a pointer to net-replay --export");
        check(status_of(rep, "x.json") == "invalid" && status_of(rep, "bad.json") == "invalid",
              "inputs: other JSON / broken JSON rejected");
        check(status_of(rep, "gz.json.gz") == "merged", "inputs: a gzip-compressed export is accepted");

        // An export from before provenance existed: no instance / sources.
        std::string legacy = a1;
        const std::size_t p = legacy.find(",\"instance\"");
        const std::size_t q = legacy.find(",\"families\"");
        legacy = legacy.substr(0, p) + legacy.substr(q);
        rep.clear();
        Dataset d = merge_exports({{"old.json", legacy}, {"old-copy.json", legacy}}, rep);
        check(status_of(rep, "old.json") == "merged" && status_of(rep, "old-copy.json") == "skipped",
              "legacy: an export without provenance can still not be merged twice");
        mjson::V lj = parse("{\"families\":" + families_json(d) + "}");
        const auto dk = keys_of(arr(fam(lj, "dmr"), "networks"), "key");
        check(dk.size() == 1 && dk.begin()->find("~anon-") != std::string::npos,
              "legacy: its weak networks are qualified with a content-derived identity");
    }

    // ---- importing into a live model ----
    {
        AssocModel L;
        L.set_identity("dddddddd44444444", "delta");
        L.set_since(1000);
        p25_call(L, 1, 2000, "300", "5000");
        const std::string before = L.to_json(9500);
        const std::string own = L.to_export_json(9400);

        auto r = L.import_export(a2, "a2.json", 9500);
        check(r.status == "imported" && r.id == 1 && L.import_count() == 1, "import: an export of another receiver is imported");
        bool ok = false;
        mjson::V j = parse(L.to_json(9500), &ok);
        const mjson::V* im = j.get("imports");
        check(ok && im && im->a.size() == 1 && im->a[0].str("label") == "a2.json" && im->a[0].num("calls") == 3 &&
                  j.str("instance") == "dddddddd44444444" && j.str("name") == "delta",
              "import: /net.json lists it (label, counts) next to this run's identity");
        const mjson::V& P = fam(j, "p25");
        check(arr(P, "networks").a.size() == 1 && arr(P, "networks").a[0].num("calls") == 3 &&
                  arr(P, "radios").a.size() == 3,
              "import: live and imported data are shown merged (one P25 system, all radios)");
        check(by(arr(fam(j, "dmr"), "networks"), "key", "cc:4@s2~aaaaaaaa") != nullptr,
              "import: the imported weak DMR network is qualified (can't merge with a local CC 4)");
        bool live_ids_kept = by(arr(P, "calls"), "src", "5000") && by(arr(P, "calls"), "src", "5000")->num("id") == 1;
        bool imp_ids = by(arr(P, "calls"), "src", "2048") && by(arr(P, "calls"), "src", "2048")->num("id") > 1e9;
        check(live_ids_kept && imp_ids, "import: live call ids unchanged; imported calls get their own id range");

        check(L.import_export(a2, "again.json", 9600).status == "skipped", "import: the same export again -> skipped");
        check(L.import_export(a1, "a1.json", 9600).status == "skipped", "import: an older export of that run -> skipped");
        check(L.import_export(own, "own.json", 9600).status == "skipped",
              "import: this server's own export -> skipped (it is the live data)");
        // L's export now includes the import: everything in it is shown already.
        check(L.import_export(L.to_export_json(9600), "own2.json", 9600).status == "skipped",
              "import: this server's export that includes its imports -> skipped");
        auto rb = L.import_export(gzip(b1), "b1.json.gz", 9700);
        check(rb.status == "imported" && rb.id == 2, "import: a second receiver (gzip) -> imported");
        auto rr = L.import_export("{\"op\":\"header\",\"v\":1}", "rec", 9700);
        check(rr.status == "invalid" && !rr.message.empty(), "import: a recording -> invalid, with the reason");

        // A newer export of an imported run replaces it.
        AssocModel B2;
        B2.set_identity("bbbbbbbb22222222", "bravo");
        B2.set_since(1000);
        p25_call(B2, 1, 2500, "100", "3000");
        dmr_call(B2, 2, 3500, "19535", "2222223");
        p25_call(B2, 3, 8500, "100", "3001");
        auto rn = L.import_export(B2.to_export_json(9000), "b-later.json", 9800);
        mjson::V j2 = parse(L.to_json(9800));
        check(rn.status == "replaced" && j2.get("imports")->a.size() == 2 && by(*j2.get("imports"), "label", "b-later.json") &&
                  !by(*j2.get("imports"), "label", "b1.json.gz"),
              "import: a newer export of an imported run replaces the older import");

        // A file mixing this run's live span with others -> refused.
        L.clear_imports();
        std::vector<MergeReport> rep;
        Dataset mix = merge_exports({{"own.json", own}, {"b1.json", b1}}, rep);
        auto rm = L.import_export(export_json(mix, 9400, "", "merged"), "mix.json", 9900);
        check(rm.status == "refused", "import: a merge that contains part of the live data -> refused (would double-count)");

        // Remove restores the live view exactly.
        L.import_export(a2, "a2.json", 9500);
        auto id = L.import_export(b1, "b1.json", 9500).id;
        check(L.remove_import(id) && !L.remove_import(999), "import: remove by id");
        L.clear_imports();
        check(families_part(L.to_json(9500)) == families_part(before),
              "import: with the imports removed the view is byte-identical to the live data alone");

        // Clear drops imports too; then a pre-Clear export of this run is
        // history and imports fine.
        L.import_export(a2, "a2.json", 9500);
        L.clear(10000);
        check(L.import_count() == 0, "import: Clear also clears imports");
        auto rp = L.import_export(own, "pre-clear.json", 10100);
        check(rp.status == "imported", "import: this run's pre-Clear export imports after a Clear (disjoint)");
        L.clear(10200);

        // Export of live + imports -> a third server shows the same networks
        // as net-merge of the parts.
        p25_call(L, 1, 10300, "300", "5000");
        dmr_call(L, 2, 10400, "19535", "7777");
        L.import_export(a2, "a2.json", 11000);
        L.import_export(b1, "b1.json", 11000);
        const std::string lx = L.to_export_json(11000);
        mjson::V lxj = parse(lx);
        check(lxj.get("sources") && lxj.get("sources")->a.size() == 3, "import: an export of live + imports lists all three sources");
        AssocModel Z;
        Z.set_identity("eeeeeeee55555555", "echo");
        Z.set_since(1000);
        check(Z.import_export(lx, "delta.json", 12000).status == "imported", "import: that export imports elsewhere");
        mjson::V zj = parse(Z.to_json(12000));
        rep.clear();
        const std::string lown = [] {   // L's own data alone, exported separately
            AssocModel t;
            t.set_identity("dddddddd44444444", "delta");
            t.set_since(10200);
            p25_call(t, 1, 10300, "300", "5000");
            dmr_call(t, 2, 10400, "19535", "7777");
            return t.to_export_json(11000);
        }();
        Dataset parts = merge_exports({{"l.json", lown}, {"a2.json", a2}, {"b1.json", b1}}, rep);
        mjson::V pj = parse("{\"families\":" + families_json(parts) + "}");
        bool same = true;
        for (const char* f : {"p25", "dmr"})
            for (const char* k : {"networks", "talkgroups", "radios"}) {
                same = same && keys_of(arr(fam(zj, f), k), k[0] == 'n' ? "key" : "id") ==
                                   keys_of(arr(fam(pj, f), k), k[0] == 'n' ? "key" : "id");
                same = same && arr(fam(zj, f), "calls").a.size() == arr(fam(pj, f), "calls").a.size();
            }
        check(same, "import: chained (import, export, import elsewhere) == net-merge of the original parts");
        const mjson::V* dmr4 = by(arr(fam(zj, "dmr"), "networks"), "key", "cc:4@s2~dddddddd");
        check(dmr4 && dmr4->str("label").find("delta") != std::string::npos,
              "import: the exporting server's own weak networks get its name when imported elsewhere");
    }

    // ---- one call heard by two receivers is one call ----
    {
        AssocModel X, Y;
        X.set_identity("1111111100000000", "x-ray");
        Y.set_identity("2222222200000000", "yankee");
        X.set_since(1000);
        Y.set_since(1000);
        p25_call(X, 1, 2000, "100", "2048");      // same system, same call, 300 ms apart
        p25_call(Y, 7, 2300, "100", "2048");
        p25_call(Y, 7, 30000, "100", "2048");     // the same parties again much later: another call
        dmr_call(X, 2, 3000, "9", "77");          // DMR on weak color codes: can't tell -> kept apart
        dmr_call(Y, 2, 3000, "9", "77");
        std::vector<MergeReport> rep;
        Dataset d = merge_exports({{"x", X.to_export_json(40000)}, {"y", Y.to_export_json(40000)}}, rep);
        const DsFamily& P = d.fams["p25"];
        const DsNetwork& n = P.networks.begin()->second;
        const DsTalkgroup& t = P.tgs.at("100");
        const DsRadio& r = P.radios.at("2048");
        check(P.calls.size() == 2 && P.calls.back().streams == 2 && P.calls.front().streams == 1,
              "twins: the call both receivers heard is listed once (heard on 2 streams); the later one separately");
        check(n.calls == 2 && t.calls == 2 && r.calls == 2 && t.radios.at("2048") == 2 && r.tgs.at("100") == 2,
              "twins: ...and counted once on the network, talkgroup and radio");
        check(P.calls.back().start == 2040 && n.sessions == 2, "twins: earliest start kept; both streams still listed");
        check(d.fams["dmr"].calls.size() == 2 && d.fams["dmr"].tgs.at("9").calls == 2,
              "twins: same parties on weak, per-receiver networks are not assumed to be one call");
        AssocModel L;
        L.set_identity("3333333300000000", "live");
        L.set_since(1000);
        p25_call(L, 1, 2100, "100", "2048");
        L.import_export(X.to_export_json(40000), "x", 40000);
        mjson::V j = parse(L.to_json(40000));
        const mjson::V& C = arr(fam(j, "p25"), "calls");
        check(C.a.size() == 1 && C.a[0].num("id") == 1 && C.a[0].num("streams") == 2,
              "twins: an imported call the live data also heard folds into the live call (live id kept)");
    }

    // ---- channel networks (short code + known frequency) across receivers ----
    {
        const std::int64_t F1 = 434425000;
        auto dmr_on = [](AssocModel& m, std::uint64_t sid, std::int64_t t, std::int64_t f, const std::string& tg,
                         const std::string& src) {
            m.begin_stream(sid, "dmr", t, "", f);
            line(m, sid, "19:54:55 Sync: +DMR  slot1  [SLOT2] | Color Code=04 | VC6 ", t + 10);
            line(m, sid, "19:54:55 Sync: +DMR  slot1  [SLOT2] | Color Code=04 | VC1 ", t + 20);
            line(m, sid, " SLOT 2 TGT=" + tg + " SRC=" + src + " Group Call  ", t + 30);
            m.end_stream(sid, t + 100);
        };
        AssocModel X, Y, X2;
        X.set_identity("1212121200000000", "north");
        Y.set_identity("3434343400000000", "south");
        X2.set_identity("5656565600000000", "north");     // north again, after a restart
        for (AssocModel* m : {&X, &Y, &X2}) m->set_since(1000);
        dmr_on(X, 1, 2000, F1, "9", "77");          // both hear the same call on 434.425
        dmr_on(Y, 5, 2050, F1, "9", "77");
        dmr_on(Y, 6, 9000, F1, "9", "78");
        dmr_on(X2, 2, 3000, F1, "9", "79");
        const std::string x = X.to_export_json(20000), y = Y.to_export_json(20000), x2 = X2.to_export_json(20000);

        std::vector<MergeReport> rep;
        Dataset d = merge_exports({{"x", x}, {"y", y}, {"x2", x2}}, rep);
        const DsFamily& D = d.fams["dmr"];
        check(D.networks.size() == 1 && D.networks.count("cc:4@434425000") &&
                  D.networks.at("cc:4@434425000").confidence == "channel" &&
                  D.networks.at("cc:4@434425000").freqs == std::set<std::int64_t>{F1},
              "channel merge: the same frequency + color code from both receivers (and a restart) is ONE network");
        check(D.calls.size() == 3 && D.networks.at("cc:4@434425000").calls == 3 && D.tgs.at("9").calls == 3 &&
                  D.calls.back().streams == 2 && D.calls.back().freq == F1,
              "channel merge: the call both receivers heard is one call (and calls keep their frequency)");

        ChannelMerge per;
        per.per_receiver = true;
        rep.clear();
        Dataset p = merge_exports({{"x", x}, {"y", y}, {"x2", x2}}, rep, per);
        const DsFamily& P = p.fams["dmr"];
        check(P.networks.size() == 2 && P.networks.count("cc:4@434425000~north") && P.networks.count("cc:4@434425000~south") &&
                  P.networks.at("cc:4@434425000~north").calls == 2 &&
                  P.networks.at("cc:4@434425000~south").label == "Color Code 4 \xC2\xB7 434.4250 MHz \xC2\xB7 south",
              "per receiver: each receiver keeps its own channel network; one receiver's runs still join");
        check(P.calls.size() == 4, "per receiver: no cross-receiver call folding either");

        // Import into a live "north" in per-receiver mode: north's own earlier
        // run joins the live channel network; south's stays apart.
        AssocModel L;
        L.set_identity("7878787800000000", "north");
        L.set_since(15000);
        L.set_channels_per_receiver(true);
        dmr_on(L, 1, 16000, F1, "9", "80");
        check(L.import_export(x, "x", 20000).status == "imported" && L.import_export(y, "y", 20000).status == "imported",
              "per receiver: imports accepted");
        mjson::V j = parse(L.to_json(20000));
        const auto keys = keys_of(arr(fam(j, "dmr"), "networks"), "key");
        check(keys.size() == 2 && keys.count("cc:4@434425000") && keys.count("cc:4@434425000~south"),
              "per receiver: the live receiver's own earlier data joins its live channel network");
        L.set_channels_per_receiver(false);
        L.clear_imports();
        L.import_export(y, "y", 20000);
        mjson::V j2 = parse(L.to_json(20000));
        const mjson::V* net = by(arr(fam(j2, "dmr"), "networks"), "key", "cc:4@434425000");
        check(arr(fam(j2, "dmr"), "networks").a.size() == 1 && net && net->num("calls") == 3,
              "default: another receiver's channel network joins the live one");
    }

    // ---- merged calls are bounded ----
    {
        auto big = [](const std::string& inst, std::int64_t t0) {
            Dataset d;
            d.sources.push_back(DsSource{inst, inst, 0, t0 + 10000});
            DsFamily& F = d.fams["dmr"];
            for (int i = 0; i < 700; ++i) {
                DsCall c;
                c.id = static_cast<std::uint64_t>(700 - i);
                c.src = "1";
                c.tgt = "9";
                c.start = t0 + 700 - i;
                c.last = c.start;
                F.calls.push_back(c);
            }
            return export_json(d, t0 + 10000, inst, inst);
        };
        std::vector<MergeReport> rep;
        Dataset d = merge_exports({{"x", big("x1", 100000)}, {"y", big("y1", 200000)}}, rep);
        const auto& C = d.fams["dmr"].calls;
        check(C.size() == kMergedCallsPerFamily && C.front().start == 200700 && C.front().id == C.size() && C.back().id == 1,
              "bounds: merged calls capped per protocol, newest kept, renumbered");
    }

    if (g_failures) {
        std::printf("\n%d CHECK(S) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nALL ASSOC MERGE TESTS PASSED\n");
    return 0;
}
