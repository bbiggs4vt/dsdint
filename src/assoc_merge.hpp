// assoc_merge.hpp
//
// Merging network-explorer data from several sources: the live model, exports
// imported into it ("Import..."), and several export files viewed together
// ("Open..." with more than one file, or tools/net_merge.cpp).
//
// A Dataset is the explorer's data in plain form (what an export holds). This
// file has the one serializer every explorer view uses (families_json), the
// GraphML writer, the merge, and the provenance rules that keep a merge from
// counting the same calls twice.
//
// Provenance. Every export records, per server run ("instance"), the time span
// its data covers ([since, through]: since the run started or was last
// cleared, through the export time). The model is cumulative within a run, so
// two exports of one run with overlapping spans hold the same calls. Merging
// therefore:
//   * skips a dataset whose spans are all inside what is already merged;
//   * replaces an earlier import that the new dataset fully contains (a newer
//     export of the same run);
//   * refuses a partial overlap (it would double-count), with the reason;
//   * merges freely data from different runs, or from disjoint spans of one
//     run (e.g. before and after a Clear).
// Exports without provenance (none should exist) get a content-derived
// identity, so at least an identical file can't be merged twice.
//
// Identities. Talkgroup and radio ids are global within a protocol, and strong
// network keys (P25 WACN/SysID, DMR network id, NXDN system code, TETRA
// MCC/MNC) name the same system everywhere -- those merge across sources,
// which is how sources link up. "Channel" keys -- a short code on a known
// frequency ("cc:1@434425000": color code 1 on 434.425 MHz) -- name a
// conventional channel: they merge across runs and, by default, across
// receivers too (ChannelMerge::per_receiver keeps each receiver's apart, for
// receivers far enough apart to hear different systems on one frequency).
// Weak / unidentified keys without a frequency ("cc:1@s3": color code 1 on
// stream 3) are only unique within one server run, so an imported one is
// qualified with its run ("cc:1@s3~1a2b3c4d") and labelled with the source's
// name.
//
// Network merges (NetMerges) are the user's "these are one network" rules --
// e.g. one channel heard at two frequency offsets. They travel with the data
// (exports, imports, merges) but never change it: readers show each group as
// one network, and only GraphML gets them applied (apply_merges).

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

namespace dsdsrv {

// ---- JSON / XML text helpers shared by every explorer serializer ----------
namespace assocjson {

inline std::string q(const std::string& s) {
    std::string o = "\"";
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char u[8];
                    std::snprintf(u, sizeof u, "\\u%04x", ch);
                    o += u;
                } else {
                    o += static_cast<char>(ch);
                }
        }
    }
    return o + "\"";
}
inline const char* b(bool v) { return v ? "true" : "false"; }
template <class C>
inline std::string nums(const C& c) {
    std::string o = "[";
    bool first = true;
    for (const auto& n : c) { if (!first) o += ","; first = false; o += std::to_string(n); }
    return o + "]";
}
template <class C>
inline std::string arr(const C& c) {
    std::string o = "[";
    bool first = true;
    for (const auto& s : c) { if (!first) o += ","; first = false; o += q(s); }
    return o + "]";
}
inline std::string obj(const std::map<std::string, std::string>& m) {
    std::string o = "{";
    bool first = true;
    for (const auto& kv : m) { if (!first) o += ","; first = false; o += q(kv.first) + ":" + q(kv.second); }
    return o + "}";
}
template <class M>
inline std::string counts(const M& m) {
    std::string o = "{";
    bool first = true;
    for (const auto& kv : m) { if (!first) o += ","; first = false; o += q(kv.first) + ":" + std::to_string(kv.second); }
    return o + "}";
}
inline std::string iso(std::int64_t ms) {
    if (!ms) return std::string();
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}
// XML escaping that also guarantees well-formed UTF-8: decoder output can
// hold control bytes and invalid UTF-8 (e.g. Latin-1 aliases), which strict
// XML readers reject -- each such byte becomes '?'.
inline std::string xesc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '&': o += "&amp;"; break;
                case '<': o += "&lt;"; break;
                case '>': o += "&gt;"; break;
                case '"': o += "&quot;"; break;
                case '\'': o += "&apos;"; break;
                default: o += (c < 0x20 && c != '\t' && c != '\n' && c != '\r') ? '?' : static_cast<char>(c);
            }
            ++i;
            continue;
        }
        int n = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        bool ok = n && c >= 0xc2 && i + n <= s.size();
        for (int k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xc0) == 0x80;
        if (ok) { o.append(s, i, static_cast<std::size_t>(n)); i += static_cast<std::size_t>(n); }
        else { o += '?'; ++i; }
    }
    return o;
}

} // namespace assocjson

// "434.4250 MHz" / "451.00625 MHz": at least four decimals, more only when the
// channel needs them.
inline std::string freq_text(std::int64_t hz) {
    if (hz <= 0) return std::string();
    char b[40];
    std::snprintf(b, sizeof b, "%lld.%06lld", static_cast<long long>(hz / 1000000), static_cast<long long>(hz % 1000000));
    std::string t(b);
    while (t.size() > t.find('.') + 5 && t.back() == '0') t.pop_back();
    return t + " MHz";
}

// ---- minimal nested JSON reader (for uploaded exports) --------------------
namespace mjson {

struct V {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<V> a;
    std::vector<std::pair<std::string, V>> o;   // keeps key order
    const V* get(const std::string& k) const {
        if (t != Obj) return nullptr;
        for (const auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string str(const std::string& k) const { const V* v = get(k); return v && v->t == Str ? v->s : std::string(); }
    double num(const std::string& k) const { const V* v = get(k); return v && v->t == Num ? v->n : 0; }
    bool boolean(const std::string& k) const { const V* v = get(k); return v && v->t == Bool && v->b; }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}
    bool parse(V& out, std::string& err) {
        if (!value(out, 0)) { err = err_.empty() ? "malformed JSON" : err_; return false; }
        ws();
        if (i_ != s_.size()) { err = "trailing data after JSON at byte " + std::to_string(i_); return false; }
        return true;
    }

private:
    void ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
    bool fail(const char* m) { if (err_.empty()) err_ = std::string(m) + " at byte " + std::to_string(i_); return false; }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xc0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3f)); }
        else if (cp < 0x10000) {
            o += static_cast<char>(0xe0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            o += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            o += static_cast<char>(0xf0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3f)); o += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    bool hex4(unsigned& cp) {
        if (i_ + 4 > s_.size()) return false;
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s_[i_++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& o) {
        if (i_ >= s_.size() || s_[i_] != '"') return fail("expected string");
        ++i_;
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c != '\\') { o += c; continue; }
            if (i_ >= s_.size()) return fail("bad escape");
            char e = s_[i_++];
            switch (e) {
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xd800 && cp < 0xdc00 && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        std::size_t save = i_;
                        i_ += 2;
                        unsigned lo;
                        if (hex4(lo) && lo >= 0xdc00 && lo < 0xe000) cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        else i_ = save;
                    }
                    utf8(o, cp);
                    break;
                }
                default: o += e;
            }
        }
        if (i_ >= s_.size()) return fail("unterminated string");
        ++i_;
        return true;
    }
    bool value(V& v, int depth) {
        if (depth > 64) return fail("nested too deeply");
        ws();
        if (i_ >= s_.size()) return fail("unexpected end");
        char c = s_[i_];
        if (c == '{') {
            v.t = V::Obj;
            ++i_;
            ws();
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i_ >= s_.size() || s_[i_] != ':') return fail("expected ':'");
                ++i_;
                v.o.emplace_back(std::move(k), V());
                if (!value(v.o.back().second, depth + 1)) return false;
                ws();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.t = V::Arr;
            ++i_;
            ws();
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            for (;;) {
                v.a.emplace_back();
                if (!value(v.a.back(), depth + 1)) return false;
                ws();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.t = V::Str; return str(v.s); }
        if (s_.compare(i_, 4, "true") == 0) { v.t = V::Bool; v.b = true; i_ += 4; return true; }
        if (s_.compare(i_, 5, "false") == 0) { v.t = V::Bool; i_ += 5; return true; }
        if (s_.compare(i_, 4, "null") == 0) { i_ += 4; return true; }
        const char* begin = s_.c_str() + i_;
        char* end = nullptr;
        double d = std::strtod(begin, &end);
        if (end == begin) return fail("unexpected character");
        v.t = V::Num;
        v.n = d;
        i_ += static_cast<std::size_t>(end - begin);
        return true;
    }
    const std::string& s_;
    std::size_t i_ = 0;
    std::string err_;
};

inline bool parse(const std::string& text, V& out, std::string* err = nullptr) {
    std::string e;
    Parser p(text);
    if (p.parse(out, e)) return true;
    if (err) *err = e;
    return false;
}

} // namespace mjson

// ---- the explorer's data in plain form ------------------------------------
struct DsNetwork {
    std::string key, label, confidence;
    std::map<std::string, std::string> ids;
    std::set<std::string> sites;
    std::set<std::int64_t> freqs;
    std::uint64_t sessions = 0, calls = 0;
    std::int64_t first = 0, last = 0;
    std::map<std::string, std::uint64_t> keys;   // encryption keys its calls used: "alg:kid" -> calls (enc_key)
};
struct DsTalkgroup {
    std::string id;
    std::map<std::string, std::uint64_t> radios;
    std::set<std::string> networks;
    std::uint64_t calls = 0, emerg = 0, enc = 0;
    std::int64_t first = 0, last = 0;
    std::map<std::string, std::uint64_t> keys;   // "alg:kid" -> calls
};
struct DsRadio {
    std::string id;
    std::vector<std::string> aliases;
    std::map<std::string, std::uint64_t> tgs, peers;
    std::set<std::string> networks;
    std::map<std::string, std::uint64_t> keys;   // "alg:kid" -> calls
    std::uint64_t calls = 0;
    std::int64_t first = 0, last = 0;
    std::string pos;                           // last position report "lat,lon" ("" = none)
    std::int64_t pos_t = 0;                    // when (ms)
    std::vector<std::pair<std::int64_t, std::string>> track;  // its position reports, oldest first (time, "lat,lon")
};
// Position history kept per radio (distinct fixes, newest kept).
constexpr std::size_t kMaxTrack = 100;
// Add a fix to a track: the same position as the last one only moves its
// time on; the oldest go once it is full.
inline void track_add(std::vector<std::pair<std::int64_t, std::string>>& tr, std::int64_t t, const std::string& pos) {
    if (!tr.empty() && tr.back().second == pos) { tr.back().first = std::max(tr.back().first, t); return; }
    tr.emplace_back(t, pos);
    if (tr.size() > kMaxTrack) tr.erase(tr.begin(), tr.begin() + static_cast<std::ptrdiff_t>(tr.size() - kMaxTrack));
}
// How specific a data call's service label is: a later, more specific one
// replaces it (an announcement, then the packet, then the MNIS service).
inline int svc_rank(const std::string& s) {
    return s.empty() ? 0 : s == "preamble" ? 1 : s == "ack" ? 2 : s == "data" ? 3 : 4;
}
// An encryption key as the explorer counts it: "alg:kid" (hex, as the
// protocol numbers them; alg "" when only the key id was seen).
inline std::string enc_key(const std::string& alg, const std::string& kid) { return alg + ":" + kid; }
// Keys kept per network / talkgroup / radio (a system uses a handful).
constexpr std::size_t kMaxKeys = 32;
template <class M>
inline void key_add(M& m, const std::string& k, typename M::mapped_type n = 1) {
    auto it = m.find(k);
    if (it != m.end()) it->second += n;
    else if (m.size() < kMaxKeys) m.emplace(k, n);
}
struct DsCall {
    std::uint64_t id = 0, session = 0, streams = 1;
    std::string net, site, slot, src, tgt, alias, text;
    std::string svc;                            // a data call's service: preamble, ack, data, ars, lrrp, mnis:80, ...
    std::string pos;                            // a position report sent during the call "lat,lon"
    bool priv = false, voice = false, data = false, emerg = false, enc = false, open = false;
    std::string alg, kid;                       // encrypted: the algorithm and key id it named ("" = not seen)
    std::int64_t start = 0, last = 0;
    std::int64_t freq = 0;
    std::string audio;                          // its audio file on this server, if any (see /net/audio/; an import's once uploaded)
    std::uint64_t audio_ms = 0;
};
struct DsFamily {
    std::map<std::string, DsNetwork> networks;
    std::map<std::string, DsTalkgroup> tgs;
    std::map<std::string, DsRadio> radios;
    std::vector<DsCall> calls;                 // in output order (newest first)
};
// One server run's contribution: its data from `since` through `through`.
struct DsSource {
    std::string instance, name;
    std::int64_t since = 0, through = 0;
};
// Network merges: the explorer's "these are one network" rules (e.g. one
// channel heard at two frequency offsets), per protocol: network key -> the
// key it is shown under. Rules, not a merge of the data: the networks stay
// apart underneath, so a merge can always be undone. Always flat -- a target
// is never itself merged into another.
using NetMerges = std::map<std::string, std::map<std::string, std::string>>;
constexpr std::size_t kMaxMergesPerFamily = 500;

// The key a network is shown under.
inline std::string merge_root(const NetMerges& m, const std::string& fam, const std::string& key) {
    auto f = m.find(fam);
    if (f == m.end()) return key;
    auto it = f->second.find(key);
    return it == f->second.end() ? key : it->second;
}
// Show `from` (and whatever is merged into it) under `to`'s network. False
// when nothing changes (already one network, or the rule list is full).
inline bool merges_add(NetMerges& m, const std::string& fam, const std::string& from, const std::string& to) {
    if (fam.empty() || from.empty() || to.empty()) return false;
    const std::string r = merge_root(m, fam, to);
    if (r == from || merge_root(m, fam, from) == r) return false;
    auto& F = m[fam];
    if (!F.count(from) && F.size() >= kMaxMergesPerFamily) return false;
    F[from] = r;
    for (auto& kv : F) if (kv.second == from) kv.second = r;
    return true;
}
// Undo a merge: a merged network goes back to being its own; a network others
// were merged into lets all of them go.
inline bool merges_remove(NetMerges& m, const std::string& fam, const std::string& key) {
    auto f = m.find(fam);
    if (f == m.end()) return false;
    auto& F = f->second;
    bool changed = F.erase(key) > 0;
    if (!changed) {
        for (auto it = F.begin(); it != F.end();) {
            if (it->second == key) { it = F.erase(it); changed = true; }
            else ++it;
        }
    }
    if (F.empty()) m.erase(f);
    return changed;
}
inline std::size_t merges_count(const NetMerges& m) {
    std::size_t n = 0;
    for (const auto& f : m) n += f.second.size();
    return n;
}

struct Dataset {
    std::uint64_t id = 0;                      // import id (live model layers)
    std::string label;                         // file name, or "live"
    std::int64_t exported = 0;
    std::vector<DsSource> sources;
    std::map<std::string, DsFamily> fams;
    NetMerges merges;                          // network merges (see NetMerges)
};

// How many calls the explorer keeps per protocol (the live model's list and
// a merged view): DSD_NET_MAX_CALLS, default 5000.
constexpr std::size_t kDefaultMaxCalls = 5000;
inline std::size_t max_calls_setting() {
    const char* v = std::getenv("DSD_NET_MAX_CALLS");
    const unsigned long n = (v && v[0]) ? std::strtoul(v, nullptr, 10) : kDefaultMaxCalls;
    return n < 10 ? 10 : static_cast<std::size_t>(n);
}
constexpr std::size_t kMergedCallsPerFamily = kDefaultMaxCalls;
// Two receivers' records of one call: same network, source, target and kind,
// no further apart in time than this (= AssocModel::kContinueMs, the live
// model's rule for one call heard on two streams).
constexpr std::int64_t kTwinGapMs = 4000;

// The per-protocol JSON the explorer renders (/net.json "families").
inline std::string families_json(const Dataset& d) {
    using namespace assocjson;
    std::ostringstream o;
    o << "{";
    bool firstf = true;
    for (const auto& fk : d.fams) {
        const DsFamily& F = fk.second;
        if (!firstf) o << ",";
        firstf = false;
        o << q(fk.first) << ":{";
        o << "\"networks\":[";
        bool first = true;
        for (const auto& nk : F.networks) {
            const DsNetwork& n = nk.second;
            if (!first) o << ",";
            first = false;
            o << "{\"key\":" << q(n.key) << ",\"label\":" << q(n.label) << ",\"confidence\":" << q(n.confidence)
              << ",\"ids\":" << obj(n.ids) << ",\"sites\":" << arr(n.sites) << ",\"freqs\":" << nums(n.freqs)
              << ",\"sessions\":" << n.sessions
              << ",\"calls\":" << n.calls << ",\"first\":" << n.first << ",\"last\":" << n.last;
            if (!n.keys.empty()) o << ",\"keys\":" << counts(n.keys);
            o << "}";
        }
        o << "],\"talkgroups\":[";
        first = true;
        for (const auto& tk : F.tgs) {
            const DsTalkgroup& t = tk.second;
            if (!first) o << ",";
            first = false;
            o << "{\"id\":" << q(t.id) << ",\"networks\":" << arr(t.networks) << ",\"radios\":" << counts(t.radios)
              << ",\"calls\":" << t.calls << ",\"emerg\":" << t.emerg << ",\"enc\":" << t.enc
              << ",\"first\":" << t.first << ",\"last\":" << t.last;
            if (!t.keys.empty()) o << ",\"keys\":" << counts(t.keys);
            o << "}";
        }
        o << "],\"radios\":[";
        first = true;
        for (const auto& rk : F.radios) {
            const DsRadio& r = rk.second;
            if (!first) o << ",";
            first = false;
            o << "{\"id\":" << q(r.id) << ",\"aliases\":" << arr(r.aliases) << ",\"tgs\":" << counts(r.tgs)
              << ",\"peers\":" << counts(r.peers) << ",\"networks\":" << arr(r.networks) << ",\"calls\":" << r.calls
              << ",\"first\":" << r.first << ",\"last\":" << r.last;
            if (!r.keys.empty()) o << ",\"keys\":" << counts(r.keys);
            if (!r.pos.empty()) o << ",\"pos\":" << q(r.pos) << ",\"pos_t\":" << r.pos_t;
            if (!r.track.empty()) {
                o << ",\"track\":[";
                for (std::size_t i = 0; i < r.track.size(); ++i)
                    o << (i ? "," : "") << "[" << r.track[i].first << "," << q(r.track[i].second) << "]";
                o << "]";
            }
            o << "}";
        }
        o << "],\"calls\":[";
        first = true;
        for (const DsCall& k : F.calls) {
            if (!first) o << ",";
            first = false;
            o << "{\"id\":" << k.id << ",\"session\":" << k.session << ",\"net\":" << q(k.net)
              << ",\"site\":" << q(k.site) << ",\"freq\":" << k.freq << ",\"slot\":" << q(k.slot)
              << ",\"src\":" << q(k.src)
              << ",\"tgt\":" << q(k.tgt) << ",\"alias\":" << q(k.alias) << ",\"text\":" << q(k.text)
              << ",\"priv\":" << b(k.priv) << ",\"voice\":" << b(k.voice) << ",\"data\":" << b(k.data)
              << ",\"emerg\":" << b(k.emerg) << ",\"enc\":" << b(k.enc) << ",\"open\":" << b(k.open)
              << ",\"streams\":" << k.streams << ",\"start\":" << k.start << ",\"last\":" << k.last;
            if (!k.audio.empty()) o << ",\"audio\":" << q(k.audio) << ",\"audio_ms\":" << k.audio_ms;
            if (!k.svc.empty()) o << ",\"svc\":" << q(k.svc);
            if (!k.pos.empty()) o << ",\"pos\":" << q(k.pos);
            if (!k.kid.empty()) o << ",\"alg\":" << q(k.alg) << ",\"kid\":" << q(k.kid);
            o << "}";
        }
        o << "]}";
    }
    o << "}";
    return o.str();
}

inline std::string sources_json(const std::vector<DsSource>& ss) {
    using namespace assocjson;
    std::string o = "[";
    for (std::size_t i = 0; i < ss.size(); ++i) {
        if (i) o += ",";
        o += "{\"instance\":" + q(ss[i].instance) + ",\"name\":" + q(ss[i].name) + ",\"since\":" +
             std::to_string(ss[i].since) + ",\"through\":" + std::to_string(ss[i].through) + "}";
    }
    return o + "]";
}

// {"dmr":{"cc:1@436627500":"cc:1@436625000"}, ...}
inline std::string merges_json(const NetMerges& m) {
    using namespace assocjson;
    std::string o = "{";
    bool firstf = true;
    for (const auto& f : m) {
        if (f.second.empty()) continue;
        o += (firstf ? "" : ",") + q(f.first) + ":" + obj(f.second);
        firstf = false;
    }
    return o + "}";
}
// The rules about networks the dataset holds (either end -- the other may be
// in another receiver's export): what an export keeps.
inline NetMerges merges_held(const Dataset& d) {
    NetMerges out;
    for (const auto& f : d.merges) {
        auto fi = d.fams.find(f.first);
        if (fi == d.fams.end()) continue;
        for (const auto& kv : f.second)
            if (fi->second.networks.count(kv.first) || fi->second.networks.count(kv.second)) out[f.first].insert(kv);
    }
    return out;
}

// A self-describing export ("dsd-net-export" v1) of a dataset.
inline std::string export_json(const Dataset& d, std::int64_t now, const std::string& instance,
                               const std::string& name) {
    using namespace assocjson;
    const NetMerges mg = merges_held(d);
    return "{\"format\":\"dsd-net-export\",\"format_version\":1,\"source\":\"dsd-server\",\"instance\":" + q(instance) +
           ",\"name\":" + q(name) + ",\"exported\":" + std::to_string(now) + ",\"now\":" + std::to_string(now) +
           ",\"sources\":" + sources_json(d.sources) +
           (mg.empty() ? std::string() : ",\"merges\":" + merges_json(mg)) + ",\"families\":" + families_json(d) + "}";
}

// 64-bit FNV-1a, for content-derived identities.
inline std::string fnv_hex(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

// A gzip-compressed upload (export.json.gz) is accepted as well; anything else
// passes through. Returns false if the data is gzip but corrupt or would
// inflate past `max_out`.
inline bool gunzip_if_needed(std::string& data, std::size_t max_out = std::size_t(512) << 20) {
    if (data.size() < 2 || static_cast<unsigned char>(data[0]) != 0x1f || static_cast<unsigned char>(data[1]) != 0x8b)
        return true;
    z_stream z{};
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return false;
    std::string out;
    char buf[1 << 16];
    z.next_in = reinterpret_cast<Bytef*>(&data[0]);
    z.avail_in = static_cast<uInt>(data.size());
    int rc;
    do {
        z.next_out = reinterpret_cast<Bytef*>(buf);
        z.avail_out = sizeof buf;
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&z); return false; }
        out.append(buf, sizeof buf - z.avail_out);
        if (out.size() > max_out) { inflateEnd(&z); return false; }
        if (rc == Z_OK && z.avail_in == 0 && z.avail_out != 0) break;    // truncated input
    } while (rc != Z_STREAM_END);
    inflateEnd(&z);
    if (rc != Z_STREAM_END) return false;
    data.swap(out);
    return true;
}

namespace detail {
inline std::uint64_t u(const mjson::V& v, const char* k) { double d = v.num(k); return d > 0 ? static_cast<std::uint64_t>(d) : 0; }
inline std::int64_t i64(const mjson::V& v, const char* k) { return static_cast<std::int64_t>(v.num(k)); }
inline void strset(const mjson::V* a, std::set<std::string>& out) {
    if (a && a->t == mjson::V::Arr) for (const auto& e : a->a) if (e.t == mjson::V::Str) out.insert(e.s);
}
inline void countmap(const mjson::V* o, std::map<std::string, std::uint64_t>& out) {
    if (o && o->t == mjson::V::Obj)
        for (const auto& kv : o->o) if (kv.second.t == mjson::V::Num && kv.second.n > 0) out[kv.first] += static_cast<std::uint64_t>(kv.second.n);
}
inline int rank(const std::string& conf) {
    return conf == "strong" ? 3 : conf == "channel" ? 2 : conf == "weak" ? 1 : 0;
}
} // namespace detail

// How channel-keyed networks (a short code on a known frequency) from
// different receivers merge: joined (default), or kept per receiver -- then
// they are qualified with the receiver's name, except the `home` receiver's
// own (the live server importing its own earlier data).
struct ChannelMerge {
    bool per_receiver = false;
    std::string home;
};

// Build a dataset from a parsed export. Local (weak / unidentified) network
// keys of the export's own run are qualified with that run, so they can never
// collide with another run's. Returns false with *err on a non-export.
inline bool dataset_from_export(const mjson::V& root, const std::string& label, Dataset& d, std::string* err,
                                const ChannelMerge& cm = ChannelMerge()) {
    using detail::u;
    using detail::i64;
    if (root.t != mjson::V::Obj) { if (err) *err = "not a JSON object"; return false; }
    if (root.get("op")) { if (err) *err = "this is a recording (raw decoder input), not an export -- convert it with net-replay --export"; return false; }
    const std::string fmt = root.str("format");
    const mjson::V* fams = root.get("families");
    if (!fams || fams->t != mjson::V::Obj || (!fmt.empty() && fmt != "dsd-net-export")) {
        if (err) *err = "not a dsd-server explorer export (expected format \"dsd-net-export\")";
        return false;
    }
    d = Dataset{};
    d.label = label;
    d.exported = i64(root, "exported") ? i64(root, "exported") : i64(root, "now");
    for (const auto& fk : fams->o) {
        const mjson::V& F = fk.second;
        if (F.t != mjson::V::Obj) continue;
        DsFamily& df = d.fams[fk.first];
        if (const mjson::V* a = F.get("networks"); a && a->t == mjson::V::Arr)
            for (const auto& e : a->a) {
                DsNetwork n;
                n.key = e.str("key");
                if (n.key.empty()) continue;
                n.label = e.str("label");
                n.confidence = e.str("confidence");
                if (const mjson::V* ids = e.get("ids"); ids && ids->t == mjson::V::Obj)
                    for (const auto& kv : ids->o) if (kv.second.t == mjson::V::Str) n.ids[kv.first] = kv.second.s;
                detail::strset(e.get("sites"), n.sites);
                if (const mjson::V* fq = e.get("freqs"); fq && fq->t == mjson::V::Arr)
                    for (const auto& x : fq->a) if (x.t == mjson::V::Num && x.n > 0) n.freqs.insert(static_cast<std::int64_t>(x.n));
                n.sessions = u(e, "sessions"); n.calls = u(e, "calls"); n.first = i64(e, "first"); n.last = i64(e, "last");
                detail::countmap(e.get("keys"), n.keys);
                df.networks[n.key] = std::move(n);
            }
        if (const mjson::V* a = F.get("talkgroups"); a && a->t == mjson::V::Arr)
            for (const auto& e : a->a) {
                DsTalkgroup t;
                t.id = e.str("id");
                if (t.id.empty()) continue;
                detail::strset(e.get("networks"), t.networks);
                detail::countmap(e.get("radios"), t.radios);
                t.calls = u(e, "calls"); t.emerg = u(e, "emerg"); t.enc = u(e, "enc");
                detail::countmap(e.get("keys"), t.keys);
                t.first = i64(e, "first"); t.last = i64(e, "last");
                df.tgs[t.id] = std::move(t);
            }
        if (const mjson::V* a = F.get("radios"); a && a->t == mjson::V::Arr)
            for (const auto& e : a->a) {
                DsRadio r;
                r.id = e.str("id");
                if (r.id.empty()) continue;
                if (const mjson::V* al = e.get("aliases"); al && al->t == mjson::V::Arr)
                    for (const auto& x : al->a) if (x.t == mjson::V::Str) r.aliases.push_back(x.s);
                detail::countmap(e.get("tgs"), r.tgs);
                detail::countmap(e.get("peers"), r.peers);
                detail::strset(e.get("networks"), r.networks);
                detail::countmap(e.get("keys"), r.keys);
                r.calls = u(e, "calls"); r.first = i64(e, "first"); r.last = i64(e, "last");
                r.pos = e.str("pos"); r.pos_t = i64(e, "pos_t");
                if (const mjson::V* tr = e.get("track"); tr && tr->t == mjson::V::Arr)
                    for (const auto& x : tr->a)
                        if (x.t == mjson::V::Arr && x.a.size() == 2 && x.a[0].t == mjson::V::Num && x.a[1].t == mjson::V::Str)
                            track_add(r.track, static_cast<std::int64_t>(x.a[0].n), x.a[1].s);
                df.radios[r.id] = std::move(r);
            }
        if (const mjson::V* a = F.get("calls"); a && a->t == mjson::V::Arr)
            for (const auto& e : a->a) {
                DsCall c;
                c.id = u(e, "id"); c.session = u(e, "session"); c.streams = std::max<std::uint64_t>(1, u(e, "streams"));
                c.net = e.str("net"); c.site = e.str("site"); c.slot = e.str("slot"); c.src = e.str("src");
                c.tgt = e.str("tgt"); c.alias = e.str("alias"); c.text = e.str("text");
                c.svc = e.str("svc"); c.pos = e.str("pos");
                // Its audio file's name (the audio itself stays with the server
                // that recorded it, or travels in an "export with audio" zip).
                c.audio = e.str("audio"); c.audio_ms = u(e, "audio_ms");
                c.priv = e.boolean("priv"); c.voice = e.boolean("voice"); c.data = e.boolean("data");
                c.emerg = e.boolean("emerg"); c.enc = e.boolean("enc");
                c.alg = e.str("alg"); c.kid = e.str("kid");
                c.open = false;                                  // history, not live
                c.start = i64(e, "start"); c.last = i64(e, "last"); c.freq = i64(e, "freq");
                df.calls.push_back(std::move(c));
            }
    }

    // Provenance: the export's own run, plus whatever it had imported.
    std::string instance = root.str("instance"), name = root.str("name");
    if (const mjson::V* s = root.get("sources"); s && s->t == mjson::V::Arr)
        for (const auto& e : s->a) {
            DsSource src;
            src.instance = e.str("instance");
            src.name = e.str("name");
            src.since = i64(e, "since");
            src.through = i64(e, "through");
            if (!src.instance.empty()) d.sources.push_back(src);
        }
    if (d.sources.empty()) {
        if (instance.empty()) {                                  // no provenance at all
            instance = "anon-" + fnv_hex(families_json(d)).substr(0, 12);
            if (name.empty()) name = label;
        }
        d.sources.push_back(DsSource{instance, name, 0, d.exported});
    }

    // Qualify this run's local network keys (already-qualified ones contain
    // '~'): stream-scoped ones with the run, channel ones with the receiver
    // when channels are kept per receiver.
    std::map<std::string, std::map<std::string, std::string>> remaps;     // family -> old key -> new
    const std::string tag = instance.substr(0, std::min<std::size_t>(8, instance.size()));
    const std::string who = name.empty() ? tag : name;
    if (!instance.empty()) {
        for (auto& fk : d.fams) {
            DsFamily& F = fk.second;
            std::map<std::string, std::string> remap;
            std::map<std::string, DsNetwork> nets;
            for (auto& nk : F.networks) {
                DsNetwork n = nk.second;
                const bool local = n.key.find('~') == std::string::npos;
                if (local && n.confidence == "channel") {
                    if (cm.per_receiver && who != cm.home) {
                        remap[n.key] = n.key + "~" + who;
                        n.key += "~" + who;
                        n.label += " \xC2\xB7 " + who;
                    }
                } else if (local && n.confidence != "strong") {
                    remap[n.key] = n.key + "~" + tag;
                    n.key += "~" + tag;
                    n.label += " \xC2\xB7 " + who;
                }
                nets.emplace(n.key, std::move(n));
            }
            F.networks = std::move(nets);
            if (remap.empty()) continue;
            remaps[fk.first] = remap;
            auto fix = [&](std::set<std::string>& s) {
                std::set<std::string> o;
                for (const auto& k : s) { auto it = remap.find(k); o.insert(it == remap.end() ? k : it->second); }
                s = std::move(o);
            };
            for (auto& kv : F.tgs) fix(kv.second.networks);
            for (auto& kv : F.radios) fix(kv.second.networks);
            for (auto& c : F.calls) { auto it = remap.find(c.net); if (it != remap.end()) c.net = it->second; }
        }
    }

    // Its network merges, under the keys as qualified above. A rule may name
    // a network the export doesn't hold (another receiver's channel offset,
    // say): that key is qualified as it would be if it were held -- and a
    // rule about a stream-scoped network of some other run is dropped (no
    // telling which network it meant).
    if (const mjson::V* mg = root.get("merges"); mg && mg->t == mjson::V::Obj)
        for (const auto& fk : mg->o) {
            auto fi = d.fams.find(fk.first);
            if (fi == d.fams.end() || fk.second.t != mjson::V::Obj) continue;
            const auto& rm = remaps[fk.first];
            const auto& nets = fi->second.networks;
            auto key = [&](const std::string& k) -> std::string {
                if (auto it = rm.find(k); it != rm.end()) return it->second;
                if (nets.count(k) || k.find('~') != std::string::npos) return k;
                const std::size_t at = k.rfind('@');
                if (k.rfind("unknown:", 0) == 0 || (at != std::string::npos && at + 1 < k.size() && k[at + 1] == 's'))
                    return std::string();                            // stream-scoped
                if (at != std::string::npos && cm.per_receiver && !instance.empty() && who != cm.home)
                    return k + "~" + who;                            // a channel, kept per receiver
                return k;
            };
            for (const auto& kv : fk.second.o) {
                if (kv.second.t != mjson::V::Str) continue;
                const std::string from = key(kv.first), to = key(kv.second.s);
                if (from.empty() || to.empty() || (!nets.count(from) && !nets.count(to))) continue;
                merges_add(d.merges, fk.first, from, to);
            }
        }
    return true;
}

inline bool dataset_from_export_text(std::string text, const std::string& label, Dataset& d, std::string* err,
                                     const ChannelMerge& cm = ChannelMerge()) {
    if (!gunzip_if_needed(text)) { if (err) *err = "corrupt gzip data"; return false; }
    if (text.compare(0, 16, "{\"op\":\"header\",") == 0) {
        if (err) *err = "this is a recording (raw decoder input), not an export -- convert it with net-replay --export";
        return false;
    }
    mjson::V root;
    std::string e;
    if (!mjson::parse(text, root, &e)) { if (err) *err = "not valid JSON (" + e + ")"; return false; }
    return dataset_from_export(root, label, d, err, cm);
}

namespace detail {
inline void dec(std::map<std::string, std::uint64_t>& m, const std::string& k) {
    auto it = m.find(k);
    if (it == m.end()) return;
    if (it->second <= 1) m.erase(it);
    else --it->second;
}
inline void dec(std::uint64_t& n) { if (n) --n; }
// `dup` (already added to F's counters) is the same call as one already in
// the merge: take back what counting it added. Mirrors AssocModel::count_call.
inline void uncount(DsFamily& F, const DsCall& dup) {
    const std::string key = dup.kid.empty() ? std::string() : enc_key(dup.alg, dup.kid);
    auto r = F.radios.find(dup.src);
    if (r != F.radios.end()) dec(r->second.calls);
    if (r != F.radios.end() && !key.empty()) dec(r->second.keys, key);
    if (dup.priv) {
        if (r != F.radios.end()) dec(r->second.peers, dup.tgt);
        auto p = F.radios.find(dup.tgt);
        if (p != F.radios.end()) dec(p->second.peers, dup.src);
    } else {
        if (r != F.radios.end()) dec(r->second.tgs, dup.tgt);
        auto t = F.tgs.find(dup.tgt);
        if (t != F.tgs.end()) {
            dec(t->second.calls);
            dec(t->second.radios, dup.src);
            if (dup.emerg) dec(t->second.emerg);
            if (dup.enc) dec(t->second.enc);
            if (!key.empty()) dec(t->second.keys, key);
        }
    }
    auto n = F.networks.find(dup.net);
    if (n != F.networks.end()) dec(n->second.calls);
    if (n != F.networks.end() && !key.empty()) dec(n->second.keys, key);
}
inline void fold_call(DsCall& twin, const DsCall& dup) {
    twin.voice = twin.voice || dup.voice;
    twin.data = twin.data || dup.data;
    twin.emerg = twin.emerg || dup.emerg;
    twin.enc = twin.enc || dup.enc;
    if (twin.kid.empty()) { twin.alg = dup.alg; twin.kid = dup.kid; }
    if (twin.alias.empty()) twin.alias = dup.alias;
    if (!dup.text.empty() && twin.text.find(dup.text) == std::string::npos)
        twin.text = twin.text.empty() ? dup.text : twin.text + " | " + dup.text;
    if (twin.slot.empty()) twin.slot = dup.slot;
    if (twin.site.empty()) twin.site = dup.site;
    if (svc_rank(dup.svc) > svc_rank(twin.svc)) twin.svc = dup.svc;
    if (twin.pos.empty()) twin.pos = dup.pos;
    if (twin.audio.empty()) { twin.audio = dup.audio; twin.audio_ms = dup.audio_ms; }
    twin.start = std::min(twin.start, dup.start);
    twin.last = std::max(twin.last, dup.last);
    twin.streams += dup.streams;
}
} // namespace detail

// Fold `from` into `into` (same protocols merge; ids and strong network keys
// join; counts add up). A call both sides heard -- same network, parties and
// time -- is kept once (heard on more streams), and counted once.
// Add another dataset's network merges to `into`'s, where `into` has no say
// on that network yet (its own rules win).
inline void merges_union(NetMerges& into, const NetMerges& from) {
    for (const auto& f : from)
        for (const auto& kv : f.second) {
            auto fi = into.find(f.first);
            if (fi != into.end() && fi->second.count(kv.first)) continue;
            merges_add(into, f.first, kv.first, kv.second);
        }
}

inline void merge_into(Dataset& into, const Dataset& from) {
    auto lo = [](std::int64_t a, std::int64_t b) { return !a ? b : !b ? a : std::min(a, b); };
    merges_union(into.merges, from.merges);
    for (const auto& fk : from.fams) {
        DsFamily& T = into.fams[fk.first];
        const DsFamily& F = fk.second;
        for (const auto& kv : F.networks) {
            auto it = T.networks.find(kv.first);
            if (it == T.networks.end()) { T.networks.insert(kv); continue; }
            DsNetwork& n = it->second;
            const DsNetwork& m = kv.second;
            if (detail::rank(m.confidence) > detail::rank(n.confidence)) { n.confidence = m.confidence; n.label = m.label; }
            for (const auto& i : m.ids) n.ids.insert(i);
            n.sites.insert(m.sites.begin(), m.sites.end());
            n.freqs.insert(m.freqs.begin(), m.freqs.end());
            n.sessions += m.sessions; n.calls += m.calls;
            for (const auto& x : m.keys) key_add(n.keys, x.first, x.second);
            n.first = lo(n.first, m.first); n.last = std::max(n.last, m.last);
        }
        for (const auto& kv : F.tgs) {
            auto it = T.tgs.find(kv.first);
            if (it == T.tgs.end()) { T.tgs.insert(kv); continue; }
            DsTalkgroup& t = it->second;
            const DsTalkgroup& m = kv.second;
            for (const auto& r : m.radios) t.radios[r.first] += r.second;
            t.networks.insert(m.networks.begin(), m.networks.end());
            t.calls += m.calls; t.emerg += m.emerg; t.enc += m.enc;
            for (const auto& x : m.keys) key_add(t.keys, x.first, x.second);
            t.first = lo(t.first, m.first); t.last = std::max(t.last, m.last);
        }
        for (const auto& kv : F.radios) {
            auto it = T.radios.find(kv.first);
            if (it == T.radios.end()) { T.radios.insert(kv); continue; }
            DsRadio& r = it->second;
            const DsRadio& m = kv.second;
            for (const auto& a : m.aliases)
                if (r.aliases.size() < 8 && std::find(r.aliases.begin(), r.aliases.end(), a) == r.aliases.end()) r.aliases.push_back(a);
            for (const auto& x : m.tgs) r.tgs[x.first] += x.second;
            for (const auto& x : m.peers) r.peers[x.first] += x.second;
            for (const auto& x : m.keys) key_add(r.keys, x.first, x.second);
            r.networks.insert(m.networks.begin(), m.networks.end());
            r.calls += m.calls;
            r.first = lo(r.first, m.first); r.last = std::max(r.last, m.last);
            if (!m.pos.empty() && m.pos_t >= r.pos_t) { r.pos = m.pos; r.pos_t = m.pos_t; }
            if (!m.track.empty()) {                    // both receivers' fixes, in time order
                auto all = r.track;
                all.insert(all.end(), m.track.begin(), m.track.end());
                std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                r.track.clear();
                for (const auto& f : all) track_add(r.track, f.first, f.second);
            }
        }
        // Calls: fold twins (only against calls already in the merge, i.e.
        // from other sources -- each source deduped its own streams).
        std::multimap<std::string, std::size_t> by_tgt;
        for (std::size_t i = 0; i < T.calls.size(); ++i)
            if (!T.calls[i].src.empty() && !T.calls[i].tgt.empty()) by_tgt.emplace(T.calls[i].tgt, i);
        // (Networks merged into one count as one: two receivers can hear one
        // channel at different frequency offsets.)
        auto root = [&](const std::string& k) { return merge_root(into.merges, fk.first, k); };
        for (const DsCall& c : F.calls) {
            DsCall* twin = nullptr;
            if (!c.src.empty() && !c.tgt.empty() && !c.net.empty()) {
                auto range = by_tgt.equal_range(c.tgt);
                for (auto it = range.first; it != range.second && !twin; ++it) {
                    DsCall& o = T.calls[it->second];
                    if ((o.net == c.net || root(o.net) == root(c.net)) && o.src == c.src && o.priv == c.priv && c.start <= o.last + kTwinGapMs &&
                        o.start <= c.last + kTwinGapMs)
                        twin = &o;
                }
            }
            if (twin) { detail::fold_call(*twin, c); detail::uncount(T, c); }
            else T.calls.push_back(c);
        }
    }
    into.sources.insert(into.sources.end(), from.sources.begin(), from.sources.end());
}

// Bound a newest-first call list to `cap`: the oldest calls WITHOUT audio go
// first, then (if still over) the oldest of the rest -- as the live model.
inline void cap_calls(std::vector<DsCall>& C, std::size_t cap) {
    if (C.size() <= cap) return;
    std::size_t over = C.size() - cap;
    std::vector<bool> drop(C.size(), false);
    for (std::size_t i = C.size(); i-- > 0 && over;)
        if (C[i].audio.empty()) { drop[i] = true; --over; }
    for (std::size_t i = C.size(); i-- > 0 && over;)
        if (!drop[i]) { drop[i] = true; --over; }
    std::vector<DsCall> keep;
    keep.reserve(cap);
    for (std::size_t i = 0; i < C.size(); ++i) if (!drop[i]) keep.push_back(std::move(C[i]));
    C.swap(keep);
}
// After merging: calls newest first, bounded, and (unless the layers already
// have distinct ids) renumbered.
inline void finalize_merge(Dataset& d, bool renumber = true, std::size_t cap = kMergedCallsPerFamily) {
    for (auto& fk : d.fams) {
        auto& C = fk.second.calls;
        std::stable_sort(C.begin(), C.end(), [](const DsCall& a, const DsCall& b) { return a.start > b.start; });
        cap_calls(C, cap);
        if (renumber)
            for (std::size_t i = 0; i < C.size(); ++i) C[i].id = C.size() - i;
    }
}

// ---- provenance --------------------------------------------------------------
inline bool src_contains(const DsSource& a, const DsSource& b) {
    return a.instance == b.instance && a.since <= b.since && b.through <= a.through;
}
inline bool src_overlaps(const DsSource& a, const DsSource& b) {
    if (a.instance != b.instance) return false;
    if (a.since == b.since && a.through == b.through) return true;          // incl. zero-length spans
    return a.since < b.through && b.since < a.through;
}
inline bool covered(const std::vector<DsSource>& xs, const std::vector<DsSource>& by) {
    for (const auto& x : xs) {
        bool in = false;
        for (const auto& y : by) if (src_contains(y, x)) { in = true; break; }
        if (!in) return false;
    }
    return true;
}
// "alpha 10-01 12:00-12:30Z" (the end's date only when it differs).
inline std::string span_text(const DsSource& s) {
    auto fmt = [](std::int64_t ms) {
        if (!ms) return std::string("start");
        std::time_t t = static_cast<std::time_t>(ms / 1000);
        std::tm tm{};
        gmtime_r(&t, &tm);
        char b[24];
        std::strftime(b, sizeof b, "%m-%d %H:%M", &tm);
        return std::string(b);
    };
    const std::string a = fmt(s.since), z = fmt(s.through);
    const std::string zz = a.size() == 11 && z.size() == 11 && a.compare(0, 5, z, 0, 5) == 0 ? z.substr(6) : z;
    return (s.name.empty() ? s.instance.substr(0, 8) : s.name) + " " + a + "\xE2\x80\x93" + zz + "Z";
}

struct Verdict {
    enum Kind { Add, Skip, Refuse } kind = Add;
    std::vector<std::size_t> replaces;      // indices into the layer list
    std::string why;
};

// May dataset `x` join `layers` (and, for the live model, `live`) without
// counting anything twice?
inline Verdict judge(const Dataset& x, const std::vector<Dataset>& layers, const std::vector<DsSource>* live) {
    Verdict v;
    // Everything in it is already shown (e.g. an export of this server that
    // included its own imports, or a merge of files already merged)?
    std::vector<DsSource> all;
    if (live) all = *live;
    for (const auto& L : layers) all.insert(all.end(), L.sources.begin(), L.sources.end());
    if (!x.sources.empty() && covered(x.sources, all)) {
        v.kind = Verdict::Skip;
        v.why = "already included";
        bool in_live = live && covered(x.sources, *live);
        for (const auto& L : layers) if (!in_live && covered(x.sources, L.sources)) { v.why += " in " + L.label; break; }
        if (in_live) v.why = "it is this server's own live data (already shown)";
        return v;
    }
    if (live) {
        bool ov = false;
        for (const auto& a : x.sources) for (const auto& l : *live) ov = ov || src_overlaps(a, l);
        if (ov) {
            v.kind = Verdict::Refuse;
            v.why = "it overlaps this server's live data -- importing it would count the same calls twice (Clear first to import it)";
            return v;
        }
    }
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Dataset& L = layers[i];
        bool ov = false;
        for (const auto& a : x.sources) for (const auto& l : L.sources) ov = ov || src_overlaps(a, l);
        if (!ov) continue;
        if (covered(L.sources, x.sources)) { v.replaces.push_back(i); continue; }
        v.kind = Verdict::Refuse;
        v.why = "it partly overlaps " + L.label + " (same server run, overlapping time) -- merging would count the same calls twice";
        v.replaces.clear();
        return v;
    }
    return v;
}

struct MergeReport {
    std::string label, status, message;     // status: merged | replaced | skipped | refused | invalid
};

// Merge several export files (no live model): the stateless "Open..." of
// several files and tools/net_merge.cpp. Inputs are taken in order.
inline Dataset merge_exports(const std::vector<std::pair<std::string, std::string>>& files,
                             std::vector<MergeReport>& report, const ChannelMerge& cm = ChannelMerge()) {
    std::vector<Dataset> layers;
    for (const auto& f : files) {
        Dataset x;
        std::string err;
        if (!dataset_from_export_text(f.second, f.first, x, &err, cm)) {
            report.push_back({f.first, "invalid", err});
            continue;
        }
        Verdict v = judge(x, layers, nullptr);
        if (v.kind == Verdict::Skip) { report.push_back({f.first, "skipped", v.why}); continue; }
        if (v.kind == Verdict::Refuse) { report.push_back({f.first, "refused", v.why}); continue; }
        std::string replaced;
        for (auto it = v.replaces.rbegin(); it != v.replaces.rend(); ++it) {
            replaced += (replaced.empty() ? "" : ", ") + layers[*it].label;
            for (auto& r : report) if (r.label == layers[*it].label && r.status != "invalid") {
                r.status = "replaced";
                r.message = "superseded by " + f.first + " (a newer export of the same run)";
            }
            layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(*it));
        }
        report.push_back({f.first, "merged", replaced.empty() ? std::string("merged") : "merged (replaces " + replaced + ")"});
        layers.push_back(std::move(x));
    }
    Dataset out;
    out.label = "merged";
    for (const auto& l : layers) { merge_into(out, l); out.exported = std::max(out.exported, l.exported); }
    finalize_merge(out, true, max_calls_setting());
    return out;
}

// Apply the network merges to the data itself: each group becomes one
// network under its target's key (and label, when the target is there). For
// outputs that can't show the rules -- GraphML.
inline void apply_merges(Dataset& d) {
    auto lo = [](std::int64_t a, std::int64_t b) { return !a ? b : !b ? a : std::min(a, b); };
    for (auto& fk : d.fams) {
        auto mi = d.merges.find(fk.first);
        if (mi == d.merges.end() || mi->second.empty()) continue;
        const auto& R = mi->second;
        auto root = [&](const std::string& k) { auto it = R.find(k); return it == R.end() ? k : it->second; };
        DsFamily& F = fk.second;
        std::map<std::string, DsNetwork> nets;
        for (int pass = 0; pass < 2; ++pass)               // targets first: theirs are the labels
            for (const auto& nk : F.networks) {
                const std::string r = root(nk.first);
                if ((r == nk.first) != (pass == 0)) continue;
                const DsNetwork& m = nk.second;
                auto it = nets.find(r);
                if (it == nets.end()) { DsNetwork n = m; n.key = r; nets.emplace(r, std::move(n)); continue; }
                DsNetwork& n = it->second;
                if (detail::rank(m.confidence) > detail::rank(n.confidence)) n.confidence = m.confidence;
                for (const auto& i : m.ids) n.ids.insert(i);
                n.sites.insert(m.sites.begin(), m.sites.end());
                n.freqs.insert(m.freqs.begin(), m.freqs.end());
                n.sessions += m.sessions; n.calls += m.calls;
                for (const auto& x : m.keys) key_add(n.keys, x.first, x.second);
                n.first = lo(n.first, m.first); n.last = std::max(n.last, m.last);
            }
        F.networks = std::move(nets);
        auto fix = [&](std::set<std::string>& s) {
            std::set<std::string> o;
            for (const auto& k : s) o.insert(root(k));
            s = std::move(o);
        };
        for (auto& kv : F.tgs) fix(kv.second.networks);
        for (auto& kv : F.radios) fix(kv.second.networks);
        for (auto& c : F.calls) c.net = root(c.net);
    }
    d.merges.clear();
}

// ---- GraphML (Gephi, Cytoscape, yEd, networkx) ------------------------------
inline std::string graphml(const Dataset& in, std::int64_t now) {
    using namespace assocjson;
    Dataset merged;
    if (!in.merges.empty()) { merged = in; apply_merges(merged); }
    const Dataset& d = in.merges.empty() ? in : merged;
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<graphml xmlns=\"http://graphml.graphdrawing.org/xmlns\" "
         "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
         "xsi:schemaLocation=\"http://graphml.graphdrawing.org/xmlns "
         "http://graphml.graphdrawing.org/xmlns/1.0/graphml.xsd\">\n"
         "  <!-- dsd-server network explorer export, " << iso(now) << " -->\n";
    const char* nkeys[][3] = {{"type", "type", "string"}, {"protocol", "protocol", "string"},
                              {"label", "label", "string"}, {"calls", "calls", "long"},
                              {"aliases", "aliases", "string"}, {"networks", "networks", "string"},
                              {"radios", "radios", "long"}, {"talkgroups", "talkgroups", "long"},
                              {"emergencies", "emergencies", "long"}, {"encrypted", "encrypted", "long"},
                              {"confidence", "confidence", "string"}, {"identifiers", "identifiers", "string"},
                              {"sites", "sites", "string"}, {"frequencies", "frequencies", "string"},
                              {"first_seen", "first_seen", "string"}, {"last_seen", "last_seen", "string"}};
    for (const auto& k : nkeys)
        o << "  <key id=\"" << k[0] << "\" for=\"node\" attr.name=\"" << k[1] << "\" attr.type=\"" << k[2] << "\"/>\n";
    o << "  <key id=\"etype\" for=\"edge\" attr.name=\"type\" attr.type=\"string\"/>\n"
         "  <key id=\"weight\" for=\"edge\" attr.name=\"weight\" attr.type=\"double\"/>\n"
         "  <key id=\"ecalls\" for=\"edge\" attr.name=\"calls\" attr.type=\"long\"/>\n"
         "  <graph id=\"dsd-net\" edgedefault=\"undirected\">\n";
    auto dt = [&](const char* k, const std::string& v) { o << "      <data key=\"" << k << "\">" << xesc(v) << "</data>\n"; };
    auto dn = [&](const char* k, std::uint64_t v) { o << "      <data key=\"" << k << "\">" << v << "</data>\n"; };
    std::uint64_t eid = 0;
    auto edge = [&](const std::string& a, const std::string& z, const char* type, std::uint64_t calls) {
        o << "    <edge id=\"e" << ++eid << "\" source=\"" << xesc(a) << "\" target=\"" << xesc(z) << "\">\n";
        dt("etype", type);
        o << "      <data key=\"weight\">" << (calls ? calls : 1) << "</data>\n";
        if (calls) dn("ecalls", calls);
        o << "    </edge>\n";
    };
    for (const auto& fk : d.fams) {
        const std::string& fam = fk.first;
        const DsFamily& F = fk.second;
        auto nid = [&](const char* t, const std::string& id) { return fam + ":" + t + ":" + id; };
        auto netlabels = [&](const std::set<std::string>& keys) {
            std::string out;
            for (const auto& k : keys) {
                auto it = F.networks.find(k);
                out += (out.empty() ? "" : "; ") + (it == F.networks.end() ? k : it->second.label);
            }
            return out;
        };
        for (const auto& kv : F.networks) {
            const DsNetwork& n = kv.second;
            std::string ids, sites, freqs;
            for (const auto& i : n.ids) ids += (ids.empty() ? "" : "; ") + i.first + "=" + i.second;
            for (const auto& st : n.sites) sites += (sites.empty() ? "" : "; ") + st;
            for (std::int64_t f : n.freqs) freqs += (freqs.empty() ? "" : "; ") + freq_text(f);
            o << "    <node id=\"" << xesc(nid("n", n.key)) << "\">\n";
            dt("type", "network"); dt("protocol", fam); dt("label", n.label); dn("calls", n.calls);
            dt("confidence", n.confidence); dt("identifiers", ids); dt("sites", sites);
            if (!freqs.empty()) dt("frequencies", freqs);
            dt("first_seen", iso(n.first)); dt("last_seen", iso(n.last));
            o << "    </node>\n";
        }
        for (const auto& kv : F.tgs) {
            const DsTalkgroup& t = kv.second;
            o << "    <node id=\"" << xesc(nid("t", t.id)) << "\">\n";
            dt("type", "talkgroup"); dt("protocol", fam); dt("label", "TG " + t.id); dn("calls", t.calls);
            dn("radios", t.radios.size()); dn("emergencies", t.emerg); dn("encrypted", t.enc);
            dt("networks", netlabels(t.networks)); dt("first_seen", iso(t.first)); dt("last_seen", iso(t.last));
            o << "    </node>\n";
        }
        for (const auto& kv : F.radios) {
            const DsRadio& r = kv.second;
            std::string al;
            for (const auto& a : r.aliases) al += (al.empty() ? "" : " / ") + a;
            o << "    <node id=\"" << xesc(nid("r", r.id)) << "\">\n";
            dt("type", "radio"); dt("protocol", fam); dt("label", al.empty() ? r.id : r.id + " (" + al + ")");
            dn("calls", r.calls); dt("aliases", al); dn("talkgroups", r.tgs.size());
            dt("networks", netlabels(r.networks)); dt("first_seen", iso(r.first)); dt("last_seen", iso(r.last));
            o << "    </node>\n";
        }
        for (const auto& kv : F.radios) {
            const DsRadio& r = kv.second;
            for (const auto& t : r.tgs)
                if (F.tgs.count(t.first)) edge(nid("r", r.id), nid("t", t.first), "talkgroup", t.second);
            for (const auto& pr : r.peers)
                if (r.id < pr.first && F.radios.count(pr.first)) edge(nid("r", r.id), nid("r", pr.first), "private", pr.second);
            for (const auto& k : r.networks)
                if (F.networks.count(k)) edge(nid("r", r.id), nid("n", k), "member", 0);
        }
        for (const auto& kv : F.tgs)
            for (const auto& k : kv.second.networks)
                if (F.networks.count(k)) edge(nid("t", kv.first), nid("n", k), "member", 0);
    }
    o << "  </graph>\n</graphml>\n";
    return o.str();
}

} // namespace dsdsrv
