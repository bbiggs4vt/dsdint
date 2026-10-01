// assoc_model.hpp
//
// Network / call / radio association model behind the "network explorer"
// page (/net). Every decoded event from every session is fed in (structured,
// before it is serialized for the client); the model stitches the per-frame
// event stream into calls and accumulates who-talks-on-what:
//
//   * networks   -- identified from the identity tokens a stream reveals
//                   (P25 WACN+SysID, NXDN system code, TETRA MCC/MNC, DMR
//                   network id; or a weak id such as a bare NAC / color code
//                   / RAN), with their sites;
//   * talkgroups -- which radios were heard on them, on which networks;
//   * radios     -- which talkgroups they used, private-call peers, aliases,
//                   which networks they were heard on;
//   * calls      -- one record per call: source -> target (group or private),
//                   voice or data/SMS, slot, emergency, encrypted, duration.
//
// Everything is kept per protocol FAMILY (dmr, p25, nxdn, tetra, ...) and
// never linked across families -- a P25 radio id and a DMR radio id are
// unrelated numbers.
//
// Network attribution: identity tokens arrive on different lines than calls
// (P25's WACN/SysID come from network-status broadcasts, DMR's color code from
// sync lines; call lines carry only src/tgt). So each session stream keeps a
// context of the identity it has seen and each call is attributed to that
// stream's current network. When a stream upgrades from a weak id to a strong
// one, the weak (or not-yet-identified) bucket it created is merged into the
// strong network; a stream that only ever sees a weak id is resolved to a
// known strong network carrying that same weak id, when exactly one does.
//
// Thread-safety: one mutex. ingest() is called from decoder reader threads,
// begin/end_stream from session strands, to_json() from the HTTP handler.
// Memory is bounded (calls ring, radio/talkgroup/network caps with LRU
// eviction); everything is in-memory and resets on restart or clear().

#pragma once

#include "assoc_log.hpp"
#include "dsd_backend_types.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace dsdsrv {

// Protocol family for a session's protocol label (protocol_hint_label()).
// "" = not a digital-voice family we model (paging); "auto" = infer per event.
inline std::string assoc_family(const std::string& label) {
    if (label == "dmr") return "dmr";
    if (label == "p25p1" || label == "p25p2" || label == "p25") return "p25";
    if (label == "nxdn48" || label == "nxdn96" || label == "nxdn") return "nxdn";
    if (label == "dpmr") return "dpmr";
    if (label == "dstar") return "dstar";
    if (label == "ysf") return "ysf";
    if (label == "tetra" || label == "tetrakit") return "tetra";
    if (label == "provoice" || label.rfind("edacs", 0) == 0) return "edacs";
    if (label == "x2tdma") return "x2tdma";
    if (label == "auto") return "auto";
    if (label == "pager-auto" || label.rfind("pocsag", 0) == 0 || label == "flex") return "";
    return label;
}

class AssocModel {
public:
    // ---- tunables -------------------------------------------------------
    static constexpr std::int64_t kContinueMs = 4000; // gap that still continues a call
    static constexpr std::size_t kMaxCalls = 400;      // per family
    static constexpr std::size_t kMaxRadios = 3000;    // per family
    static constexpr std::size_t kMaxTalkgroups = 1500;
    static constexpr std::size_t kMaxNetworks = 200;
    static constexpr std::size_t kMaxEdgesPerNode = 300;
    static constexpr std::size_t kMaxAliases = 5;

    static std::int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // A session started a decode pipeline with this protocol label. Resets the
    // stream context (a restart may be a different channel/protocol).
    // `family` overrides the family derived from the label; only a replay of a
    // recording passes it (a stream recorded mid-run may already have resolved
    // "auto" to a concrete family).
    void begin_stream(std::uint64_t sid, const std::string& label, std::int64_t now = now_ms(),
                      const std::string& family = std::string()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (rec_.on())
            rec_.write("{\"op\":\"begin\",\"t\":" + std::to_string(now) + ",\"s\":" + std::to_string(sid) +
                       ",\"label\":" + assoclog::q(label) + "}", now);
        close_session_calls_locked(sid);
        prune_session_locked(sid);      // a restart ends the previous stream
        Ctx& c = sess_[sid];
        c = Ctx{};
        c.label = label;
        c.family = family.empty() ? assoc_family(label) : family;
        c.running = true;
        ++version_;
    }

    // The session's pipeline stopped: close its open calls (keep nothing open).
    void end_stream(std::uint64_t sid, std::int64_t now = now_ms()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (rec_.on())
            rec_.write("{\"op\":\"end\",\"t\":" + std::to_string(now) + ",\"s\":" + std::to_string(sid) + "}", now);
        close_session_calls_locked(sid);
        auto it = sess_.find(sid);
        if (it != sess_.end()) { it->second.running = false; it->second.active.clear(); }
        prune_session_locked(sid);
        ++version_;
    }

    // The session disconnected.
    void remove_session(std::uint64_t sid, std::int64_t now = now_ms()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (rec_.on())
            rec_.write("{\"op\":\"remove\",\"t\":" + std::to_string(now) + ",\"s\":" + std::to_string(sid) + "}", now);
        close_session_calls_locked(sid);
        prune_session_locked(sid);
        sess_.erase(sid);
        ++version_;
    }

    // Drop everything learned so far (the page's Clear button). Live stream
    // contexts are kept (their family) but forget their identity and calls.
    void clear(std::int64_t now = now_ms()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (rec_.on()) rec_.write("{\"op\":\"clear\",\"t\":" + std::to_string(now) + "}", now);
        clear_locked();
    }

    // ---- recording (see assoc_log.hpp) ------------------------------------
    struct RecStatus {
        bool on = false, truncated = false;
        std::string path, last_path;            // current file ("" when off); current or most recent
        std::uint64_t bytes = 0, file_bytes = 0; // uncompressed written; compressed size on disk
    };
    // Start recording every input to a new file in `dir`. clear_first wipes
    // the model first so the recording starts "fresh" and replays exactly.
    // Streams already running are written as resumed "begin" lines. Returns
    // false if the file can't be created. No-op if already recording.
    bool start_recording(const std::string& dir, std::uint64_t max_bytes, bool clear_first,
                         std::int64_t now = now_ms()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (rec_.on()) return true;
        if (clear_first) clear_locked();
        if (!rec_.start(dir, max_bytes, now)) return false;
        const bool fresh = fam_.empty() && next_call_ == 0;
        rec_.write("{\"op\":\"header\",\"v\":1,\"t\":" + std::to_string(now) +
                   ",\"fresh\":" + (fresh ? "true" : "false") + "}", now);
        for (const auto& kv : sess_) {
            if (!kv.second.running) continue;
            rec_.write("{\"op\":\"begin\",\"t\":" + std::to_string(now) + ",\"s\":" + std::to_string(kv.first) +
                       ",\"label\":" + assoclog::q(kv.second.label) + ",\"family\":" + assoclog::q(kv.second.family) +
                       ",\"resumed\":true}", now);
        }
        rec_.write("{\"op\":\"snapshot\",\"t\":" + std::to_string(now) + ",\"why\":\"start\",\"model\":" +
                   to_json_locked(now) + "}", now);
        rec_.flush();
        ++version_;
        return true;
    }
    // Stop recording; the file ends with a "stop" snapshot of the model.
    void stop_recording(std::int64_t now = now_ms()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!rec_.on()) return;
        rec_.write("{\"op\":\"snapshot\",\"t\":" + std::to_string(now) + ",\"why\":\"stop\",\"model\":" +
                   to_json_locked(now) + "}", now);
        rec_.stop();
        ++version_;
    }
    void flush_recording() {
        std::lock_guard<std::mutex> lk(mu_);
        rec_.flush();
    }
    RecStatus recording() const {
        std::lock_guard<std::mutex> lk(mu_);
        return rec_status_locked();
    }

    std::uint64_t version() const {
        std::lock_guard<std::mutex> lk(mu_);
        return version_;
    }

    // Feed one decoded event from session `sid`.
    void ingest(std::uint64_t sid, const DsdEvent& ev, std::int64_t now = now_ms()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (rec_.on()) rec_.write(assoclog::event_line(now, sid, ev), now);  // every input, before any filtering
        auto sit = sess_.find(sid);
        if (sit == sess_.end()) return;          // no begin_stream (e.g. pager) -> ignore
        Ctx& c = sit->second;
        if (c.family.empty()) return;
        if (c.family == "auto") {
            if (ev.kind == "unknown") return;    // banner lines say "DMR" etc.; wait for real traffic
            std::string f = infer_family(ev.raw_line);
            if (f.empty()) return;
            c.family = f;
        }
        if (ev.crc_error == "1") return;         // failed FEC/CRC: ids are unreliable

        const std::string up = upper(ev.raw_line);
        const auto extra = parse_extra(ev.extra);

        // A stream enters the model only once it decodes real traffic (a sync,
        // call, voice, data...). Until then it creates nothing -- no protocol
        // tab, no "Unidentified" network -- so an empty channel, noise, or the
        // wrong protocol leaves no trace (dsd-fme's startup banner alone would
        // otherwise register the stream). Identity broadcasts heard before the
        // first traffic are kept on the stream, so its network comes out
        // already identified.
        if (!c.live) {
            if (ev.kind == "unknown") {
                absorb_identity(c, ev, extra, up, nullptr);
                return;
            }
            c.live = true;
        }
        Family& F = fam_[c.family];
        ++version_;

        // 1. Network identity for this stream.
        if (absorb_identity(c, ev, extra, up, &F) || c.net.empty() || !F.networks.count(c.net))
            resolve_network(c, sid, F, now);
        Network& N = F.networks[c.net];
        N.last_ms = now;
        if (!N.first_ms) N.first_ms = now;
        N.sessions.insert(sid);
        if (!c.site.empty()) N.sites.insert(c.site);

        // 2. Close this stream's calls that went quiet.
        expire_locked(c, F, now);

        const std::string src = norm_id(ev.source_id);
        const std::string tgt = norm_id(ev.talkgroup);
        const std::string slot = ev.slot;
        const bool priv = is_private(up, extra);
        const bool data = ev.kind == "message" || is_data(up);
        const bool enc = is_encrypted(up, extra);
        const bool emerg = ev.emergency == "1";

        // Unknown lines (identity broadcasts, P25 LCW, ...) never open calls or
        // introduce ids -- they only refine the call in progress on this slot.
        if (ev.kind == "unknown") {
            if (Call* cur = fresh_call(c, F, slot, now)) {
                if (priv) set_private(F, *cur, now);
                if (emerg) cur->emergency = true;
                if (enc) cur->encrypted = true;
            }
            return;
        }

        if (!src.empty()) touch_radio(F, src, c.net, ev.alias, now);

        const bool callish = ev.kind == "call" || ev.kind == "voice" ||
                             ev.kind == "message" || ev.kind == "burst";
        const bool extends = callish || (ev.kind == "sync" && is_traffic_sync(up));
        if (!extends) return;

        // 3. Find the call this event belongs to. Keyed by (slot, target) so a
        //    control channel interleaving several grants keeps them apart;
        //    events without a target fall back to the slot's latest call.
        Call* cur = nullptr;
        if (!tgt.empty()) {
            auto ait = c.active.find(slot + "|" + tgt);
            if (ait != c.active.end()) cur = find_call(F, ait->second);
            if (cur && (!cur->open || now - cur->last_ms > kContinueMs)) cur = nullptr;
            if (!cur) {
                // The same call seen with and without a slot marker (DMR direct
                // mode prints the preamble CSBK unslotted but the data header
                // as "Slot 1"): adopt the fresh slot-less call and give it the
                // slot -- but never merge two different real slots.
                std::string adopt_key;
                for (const auto& kv : c.active) {
                    const std::size_t bar = kv.first.rfind('|');
                    if (bar == std::string::npos || kv.first.compare(bar + 1, std::string::npos, tgt) != 0) continue;
                    const std::string kslot = kv.first.substr(0, bar);
                    if (!kslot.empty() && !slot.empty()) continue;
                    Call* k = find_call(F, kv.second);
                    if (!k || !k->open || now - k->last_ms > kContinueMs) continue;
                    if (!src.empty() && !k->src.empty() && k->src != src) continue;
                    cur = k;
                    adopt_key = kv.first;
                    break;
                }
                if (cur && cur->slot.empty() && !slot.empty()) {
                    c.active.erase(adopt_key);
                    cur->slot = slot;
                    c.active[slot + "|" + tgt] = cur->id;
                }
            }
            if (!cur) {
                // A target-less call already open on this slot (e.g. it began
                // with "Source: 123" only) adopts this target.
                Call* last = fresh_call(c, F, slot, now);
                if (last && last->tgt.empty() && (src.empty() || last->src.empty() || last->src == src))
                    cur = last;
            }
        } else {
            cur = fresh_call(c, F, slot, now);
        }
        if (cur && !src.empty() && !cur->src.empty() && src != cur->src) {
            close_call(F, *cur);                 // same target, new talker -> new call
            cur = nullptr;
        }
        if (!cur) {
            if (!callish) return;                // a voice sync alone doesn't start a call
            if (src.empty() && tgt.empty() && ev.kind != "message") return;
            cur = open_call(c, F, sid, slot, now);
        }

        // 4. Fold this event into the call.
        if (cur->src.empty()) cur->src = src;
        if (cur->tgt.empty() && !tgt.empty()) {
            cur->tgt = tgt;
            c.active[slot + "|" + tgt] = cur->id;
        }
        if (ev.kind == "voice" || (ev.kind == "sync" && is_voice_sync(up))) cur->voice = true;
        if (data) cur->data = true;
        if (emerg) cur->emergency = true;
        if (enc) cur->encrypted = true;
        if (!ev.alias.empty()) cur->alias = ev.alias;
        if (!ev.message.empty() && cur->text.find(ev.message) == std::string::npos)
            cur->text = cur->text.empty() ? ev.message : cur->text + " | " + ev.message;
        // Network attribution is fixed once the association is counted (a
        // merge relabels it consistently; a mid-call retune must not move a
        // call whose counters already landed on the old network).
        if (!cur->counted) {
            cur->net = c.net;
            if (!c.site.empty()) cur->site = c.site;
        }
        cur->last_ms = now;
        ++cur->frames;
        c.last_slot_call[slot] = cur->id;
        if (priv) set_private(F, *cur, now);
        if (!cur->counted && !cur->src.empty() && !cur->tgt.empty()) {
            // Two receivers often hear the SAME call (a P25 control channel's
            // grant and the voice channel; two sites of one system). On the
            // same shared network, the same source -> target while the other
            // stream's call is still running is physically one call: adopt it
            // instead of recording (and counting) it twice.
            if (Call* twin = find_twin(F, *cur, now)) cur = adopt_twin(c, F, *cur, *twin, slot);
            if (!cur->counted) count_call(F, *cur, now);
        }
    }

    // Serialize the whole model for /net.json. Calls come newest first.
    std::string to_json(std::int64_t now = now_ms()) const {
        std::lock_guard<std::mutex> lk(mu_);
        return to_json_locked(now);
    }

private:
    std::string to_json_locked(std::int64_t now) const {
        std::ostringstream o;
        o << "{\"version\":" << version_ << ",\"now\":" << now << ",\"rec\":" << rec_json_locked()
          << ",\"families\":{";
        bool firstf = true;
        for (const auto& fk : fam_) {
            const Family& F = fk.second;
            if (!firstf) o << ",";
            firstf = false;
            o << q(fk.first) << ":{";

            o << "\"networks\":[";
            bool first = true;
            for (const auto& nk : F.networks) {
                const Network& n = nk.second;
                if (!first) o << ",";
                first = false;
                o << "{\"key\":" << q(n.key) << ",\"label\":" << q(n.label)
                  << ",\"confidence\":" << q(n.confidence) << ",\"ids\":" << obj(n.ids)
                  << ",\"sites\":" << arr(n.sites) << ",\"sessions\":" << n.sessions.size()
                  << ",\"calls\":" << n.calls << ",\"first\":" << n.first_ms
                  << ",\"last\":" << n.last_ms << "}";
            }
            o << "],\"talkgroups\":[";
            first = true;
            for (const auto& tk : F.tgs) {
                const Talkgroup& t = tk.second;
                if (!first) o << ",";
                first = false;
                o << "{\"id\":" << q(t.id) << ",\"networks\":" << arr(t.networks)
                  << ",\"radios\":" << counts(t.radios) << ",\"calls\":" << t.calls
                  << ",\"emerg\":" << t.emergencies << ",\"enc\":" << t.encrypted
                  << ",\"first\":" << t.first_ms << ",\"last\":" << t.last_ms << "}";
            }
            o << "],\"radios\":[";
            first = true;
            for (const auto& rk : F.radios) {
                const Radio& r = rk.second;
                if (!first) o << ",";
                first = false;
                o << "{\"id\":" << q(r.id) << ",\"aliases\":" << arr(r.aliases)
                  << ",\"tgs\":" << counts(r.tgs) << ",\"peers\":" << counts(r.peers)
                  << ",\"networks\":" << arr(r.networks) << ",\"calls\":" << r.calls
                  << ",\"first\":" << r.first_ms << ",\"last\":" << r.last_ms << "}";
            }
            o << "],\"calls\":[";
            first = true;
            for (auto it = F.calls.rbegin(); it != F.calls.rend(); ++it) {
                const Call& k = *it;
                if (!first) o << ",";
                first = false;
                const bool open = k.open && now - k.last_ms <= kContinueMs;
                o << "{\"id\":" << k.id << ",\"session\":" << k.session << ",\"net\":" << q(k.net)
                  << ",\"site\":" << q(k.site) << ",\"slot\":" << q(k.slot)
                  << ",\"src\":" << q(k.src) << ",\"tgt\":" << q(k.tgt)
                  << ",\"alias\":" << q(k.alias) << ",\"text\":" << q(k.text)
                  << ",\"priv\":" << b(k.priv) << ",\"voice\":" << b(k.voice)
                  << ",\"data\":" << b(k.data) << ",\"emerg\":" << b(k.emergency)
                  << ",\"enc\":" << b(k.encrypted) << ",\"open\":" << b(open)
                  << ",\"streams\":" << k.streams
                  << ",\"start\":" << k.start_ms << ",\"last\":" << k.last_ms << "}";
            }
            o << "]}";
        }
        o << "}}";
        return o.str();
    }

private:
    RecStatus rec_status_locked() const {
        RecStatus r;
        r.on = rec_.on();
        r.truncated = rec_.truncated();
        r.path = rec_.path();
        r.last_path = rec_.last_path();
        r.bytes = rec_.bytes();
        r.file_bytes = rec_.file_bytes();
        return r;
    }
    std::string rec_json_locked() const {
        const RecStatus r = rec_status_locked();
        const std::string file = r.last_path.empty() ? std::string()
                                                     : std::filesystem::path(r.last_path).filename().string();
        return std::string("{\"on\":") + b(r.on) + ",\"truncated\":" + b(r.truncated) + ",\"file\":" + q(file) +
               ",\"path\":" + q(r.last_path) + ",\"bytes\":" + std::to_string(r.bytes) +
               ",\"file_bytes\":" + std::to_string(r.file_bytes) + "}";
    }
    // Forget everything learned; live stream contexts keep their protocol and
    // running state. Call ids restart, so a recording begun after a clear
    // replays to identical output.
    void clear_locked() {
        fam_.clear();
        for (auto& kv : sess_) {
            Ctx& c = kv.second;
            const std::string f = c.family, l = c.label;
            const bool running = c.running;
            c = Ctx{};
            c.family = f;
            c.label = l;
            c.running = running;
        }
        next_call_ = 0;
        ++version_;
    }

    struct Call {
        std::uint64_t id = 0, session = 0;
        std::string net, site, slot, src, tgt, alias, text;
        bool priv = false, voice = false, data = false, emergency = false, encrypted = false;
        bool open = true, counted = false;
        std::int64_t start_ms = 0, last_ms = 0;
        std::uint32_t frames = 0;
        std::uint32_t streams = 1;                      // receivers that heard it (see adopt_twin)
    };
    struct Network {
        std::string key, label, confidence;            // confidence: strong | weak | none
        std::map<std::string, std::string> ids;
        std::set<std::string> sites;
        std::set<std::uint64_t> sessions;
        std::uint64_t calls = 0;
        std::int64_t first_ms = 0, last_ms = 0;
    };
    struct Talkgroup {
        std::string id;
        std::map<std::string, std::uint32_t> radios;   // radio -> calls
        std::set<std::string> networks;
        std::uint64_t calls = 0;
        std::uint32_t emergencies = 0, encrypted = 0;
        std::int64_t first_ms = 0, last_ms = 0;
    };
    struct Radio {
        std::string id;
        std::vector<std::string> aliases;
        std::map<std::string, std::uint32_t> tgs;      // talkgroup -> calls
        std::map<std::string, std::uint32_t> peers;    // radio -> private calls (either way)
        std::set<std::string> networks;
        std::uint64_t calls = 0;
        std::int64_t first_ms = 0, last_ms = 0;
    };
    struct Family {
        std::map<std::string, Network> networks;
        std::map<std::string, Talkgroup> tgs;
        std::map<std::string, Radio> radios;
        std::deque<Call> calls;                        // oldest at front
    };
    struct Ctx {                                       // one per session stream
        std::string family;
        std::string label;                             // protocol label from begin_stream
        std::map<std::string, std::string> ids;        // identity tokens seen
        std::string net, site;
        bool strong = false;
        bool live = false;                              // has decoded real traffic yet
        bool running = false;                           // pipeline up (begin_stream .. end_stream)
        std::map<std::string, std::uint64_t> active;   // "slot|tgt" -> call id
        std::map<std::string, std::uint64_t> last_slot_call; // slot -> latest call id
        // Weak-anchor values seen but not yet believed: key -> (value, times in a row).
        std::map<std::string, std::pair<std::string, int>> pending;
    };

    // ---- string helpers -------------------------------------------------
    static std::string upper(const std::string& s) {
        std::string o(s);
        for (auto& ch : o) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return o;
    }
    static bool has(const std::string& up, const char* needle) { return up.find(needle) != std::string::npos; }

    static std::map<std::string, std::string> parse_extra(const std::string& extra) {
        std::map<std::string, std::string> m;
        std::size_t p = 0;
        while (p < extra.size()) {
            std::size_t e = extra.find(';', p);
            if (e == std::string::npos) e = extra.size();
            std::string tok = extra.substr(p, e - p);
            std::size_t a = tok.find_first_not_of(' '), z = tok.find_last_not_of(' ');
            if (a != std::string::npos) {
                tok = tok.substr(a, z - a + 1);
                std::size_t eq = tok.find('=');
                if (eq != std::string::npos && eq > 0) m[tok.substr(0, eq)] = tok.substr(eq + 1);
            }
            p = e + 1;
        }
        return m;
    }

    // Strip leading zeros; reject empty / all-zero / non-numeric-garbage ids.
    // Callsign-style ids (D-STAR/YSF) are kept as-is (trimmed).
    static std::string norm_id(const std::string& s) {
        std::size_t a = s.find_first_not_of(' '), z = s.find_last_not_of(' ');
        if (a == std::string::npos) return std::string();
        std::string t = s.substr(a, z - a + 1);
        bool digits = !t.empty() && std::all_of(t.begin(), t.end(),
                                                [](unsigned char ch) { return std::isdigit(ch); });
        if (digits) {
            std::size_t nz = t.find_first_not_of('0');
            return nz == std::string::npos ? std::string() : t.substr(nz);
        }
        if (t.find_first_of("*?") != std::string::npos) return std::string();
        return t;
    }

    static std::string infer_family(const std::string& raw) {
        const std::string up = upper(raw);
        if (has(up, "P25")) return "p25";
        if (has(up, "NXDN")) return "nxdn";
        if (has(up, "DPMR")) return "dpmr";
        if (has(up, "DSTAR") || has(up, "D-STAR")) return "dstar";
        if (has(up, "YSF")) return "ysf";
        if (has(up, "EDACS") || has(up, "PROVOICE")) return "edacs";
        if (has(up, "X2-TDMA") || has(up, "X2TDMA")) return "x2tdma";
        if (has(up, "DMR")) return "dmr";
        return std::string();
    }

    static bool is_private(const std::string& up, const std::map<std::string, std::string>& x) {
        auto ut = x.find("unit_target");
        if (ut != x.end() && ut->second != "0" && !ut->second.empty()) return true;
        return has(up, "PRIVATE") || has(up, "UNIT TO UNIT") || has(up, "UNIT-TO-UNIT") ||
               has(up, "INDIV") || has(up, "I-CALL") || has(up, "U2U");
    }
    static bool is_data(const std::string& up) {
        return has(up, " DATA") || has(up, "SMS") || has(up, "UDT") || has(up, "SHORT DATA") ||
               has(up, "TEXT:");
    }
    static bool is_encrypted(const std::string& up, const std::map<std::string, std::string>& x) {
        auto a = x.find("alg_id");
        if (a != x.end()) {
            std::string v = upper(a->second);
            std::size_t nz = v.find_first_not_of('0');
            if (nz != std::string::npos && v.substr(nz) != "80") return true; // 0x80 / 0 = clear
        }
        auto e = x.find("encr");
        if (e != x.end() && !e->second.empty() && e->second != "0") return true;
        return has(up, "ENCRYPTED");
    }
    // Sync lines that belong to call traffic (extend a call) vs idle/control.
    static bool is_voice_sync(const std::string& up) {
        return has(up, "| VC") || has(up, "VOICE") || has(up, "LDU") || has(up, "HDU") ||
               has(up, "V/D");
    }
    static bool is_traffic_sync(const std::string& up) {
        if (has(up, "IDLE")) return false;
        return is_voice_sync(up) || has(up, "| DATA") || has(up, "R12") || has(up, "R34") ||
               has(up, "R1_") || has(up, "RTCH");
    }

    // ---- network identity -----------------------------------------------
    // Token keys whose change means "a different network" (reset the stream's
    // identity) vs keys that only locate a site within it.
    static bool is_anchor(const std::string& fam, const std::string& k) {
        if (fam == "p25") return k == "wacn" || k == "system_id" || k == "nac";
        if (fam == "dmr") return k == "network_id" || k == "cc";
        if (fam == "nxdn") return k == "system_code" || k == "ran";
        if (fam == "tetra") return k == "mcc" || k == "mnc" || k == "cc";
        if (fam == "dpmr") return k == "cc";
        if (fam == "dstar") return k == "rpt1";
        if (fam == "ysf") return k == "downlink";
        if (fam == "edacs") return k == "system_id";
        return false;
    }
    static bool is_weak_anchor(const std::string& k) { return k == "cc" || k == "nac" || k == "ran"; }
    static bool is_site_key(const std::string& fam, const std::string& k) {
        if (fam == "p25") return k == "rfss" || k == "site_id";
        if (fam == "dmr") return k == "site_id";
        if (fam == "nxdn") return k == "site_code" || k == "location_id";
        if (fam == "tetra") return k == "la";
        return false;
    }

    // Returns true when the stream's identity changed.
    bool absorb_identity(Ctx& c, const DsdEvent& ev, const std::map<std::string, std::string>& x,
                         const std::string& up, Family* F) {
        std::map<std::string, std::string> got;
        const std::string& fam = c.family;
        if (!ev.color_code.empty() && (fam == "dmr" || fam == "dpmr" || fam == "tetra"))
            got["cc"] = ev.color_code;
        if (!ev.nac.empty() && fam == "p25") got["nac"] = ev.nac;
        if (!ev.ran.empty() && fam == "nxdn") got["ran"] = ev.ran;
        for (const auto& kv : x) {
            if (kv.second.empty()) continue;
            if (is_anchor(fam, kv.first) || is_site_key(fam, kv.first) ||
                (fam == "dmr" && kv.first == "network_type"))
                got[kv.first] = kv.second;
        }
        if (got.empty()) return false;

        // Adjacent/neighbor-site broadcasts describe OTHER sites (possibly of
        // another system): record the site on this stream's network, but
        // absorb nothing -- it must not move this stream's identity or site.
        if (has(up, "ADJ") || has(up, "NEIGHB")) {
            std::string s = site_label(fam, got);
            if (!s.empty() && !c.net.empty() && F) {
                auto nit = F->networks.find(c.net);
                if (nit != F->networks.end()) nit->second.sites.insert(s);
            }
            return false;
        }

        // Weak anchors (short codes printed on every burst: DMR color code,
        // P25 NAC, NXDN RAN) are only believed once seen twice in a row --
        // dsd-fme prints placeholders (e.g. "Color Code=00" at the start of a
        // voice superframe, before the embedded CC is decoded) that would
        // otherwise split one stream into two networks. Seeing the established
        // value again cancels a pending change, so alternating noise never
        // flips the identity. Strong ids come from decoded control messages
        // and are taken as-is.
        for (auto it = got.begin(); it != got.end();) {
            if (!is_weak_anchor(it->first)) { ++it; continue; }
            auto est = c.ids.find(it->first);
            if (est != c.ids.end() && est->second == it->second) {
                c.pending.erase(it->first);
                ++it;
                continue;
            }
            auto& p = c.pending[it->first];
            if (p.first == it->second) ++p.second;
            else p = {it->second, 1};
            if (p.second >= 2) { c.pending.erase(it->first); ++it; }
            else it = got.erase(it);
        }
        if (got.empty()) return false;

        // A different value for a network anchor means this stream is now on
        // another network (retune): start its identity over, then absorb.
        for (const auto& kv : got) {
            auto it = c.ids.find(kv.first);
            if (it != c.ids.end() && it->second != kv.second && is_anchor(fam, kv.first)) {
                c.ids.clear();
                c.strong = false;
                c.net.clear();
                c.site.clear();
                break;
            }
        }
        bool changed = false;
        for (const auto& kv : got) {
            auto it = c.ids.find(kv.first);
            if (it != c.ids.end() && it->second == kv.second) continue;
            c.ids[kv.first] = kv.second;
            changed = true;
        }
        if (changed) c.site = site_label(fam, c.ids);
        return changed;
    }

    static std::string site_label(const std::string& fam, const std::map<std::string, std::string>& ids) {
        auto g = [&](const char* k) { auto it = ids.find(k); return it == ids.end() ? std::string() : it->second; };
        if (fam == "p25") {
            std::string r = g("rfss"), s = g("site_id");
            if (!r.empty() && !s.empty()) return "RFSS " + r + " \xC2\xB7 Site " + s;
            if (!s.empty()) return "Site " + s;
            return std::string();
        }
        if (fam == "dmr") { std::string s = g("site_id"); return s.empty() ? s : "Site " + s; }
        if (fam == "nxdn") {
            std::string s = g("site_code"), l = g("location_id");
            if (!s.empty()) return "Site " + s + (l.empty() ? std::string() : " \xC2\xB7 Loc " + l);
            return l.empty() ? l : "Loc " + l;
        }
        if (fam == "tetra") { std::string la = g("la"); return la.empty() ? la : "LA " + la; }
        return std::string();
    }

    struct NetId { std::string key, label, conf, weak_key, weak_val; };
    static NetId derive(const std::string& fam, const std::map<std::string, std::string>& ids) {
        auto g = [&](const char* k) { auto it = ids.find(k); return it == ids.end() ? std::string() : it->second; };
        const std::string dot = " \xC2\xB7 ";
        NetId n;
        if (fam == "p25") {
            std::string w = g("wacn"), s = g("system_id"), nac = g("nac");
            if (!w.empty() && !s.empty()) n = {"wacn:" + w + "/sys:" + s, "WACN " + w + dot + "SYS " + s, "strong", "", ""};
            else if (!s.empty()) n = {"sys:" + s, "SYS " + s, "strong", "", ""};
            else if (!nac.empty()) n = {"nac:" + nac, "NAC " + nac, "weak", "nac", nac};
        } else if (fam == "dmr") {
            std::string id = g("network_id"), t = g("network_type"), cc = g("cc");
            if (!id.empty()) n = {"net:" + id, "Network " + id + (t.empty() ? "" : " (" + t + ")"), "strong", "", ""};
            else if (!cc.empty()) n = {"cc:" + cc, "Color Code " + cc, "weak", "cc", cc};
        } else if (fam == "nxdn") {
            std::string s = g("system_code"), ran = g("ran");
            if (!s.empty()) n = {"sys:" + s, "System " + s, "strong", "", ""};
            else if (!ran.empty()) n = {"ran:" + ran, "RAN " + ran, "weak", "ran", ran};
        } else if (fam == "tetra") {
            std::string mcc = g("mcc"), mnc = g("mnc"), cc = g("cc");
            if (!mcc.empty() && !mnc.empty()) n = {"mcc:" + mcc + "/mnc:" + mnc, "MCC " + mcc + dot + "MNC " + mnc, "strong", "", ""};
            else if (!cc.empty()) n = {"cc:" + cc, "Colour Code " + cc, "weak", "cc", cc};
        } else if (fam == "dpmr") {
            std::string cc = g("cc");
            if (!cc.empty()) n = {"cc:" + cc, "Channel Code " + cc, "weak", "cc", cc};
        } else if (fam == "dstar") {
            std::string r = g("rpt1");
            if (!r.empty()) n = {"rpt:" + r, "Repeater " + r, "weak", "rpt1", r};
        } else if (fam == "ysf") {
            std::string d = g("downlink");
            if (!d.empty()) n = {"dl:" + d, "Downlink " + d, "weak", "downlink", d};
        } else if (fam == "edacs") {
            std::string s = g("system_id");
            if (!s.empty()) n = {"sys:" + s, "System " + s, "strong", "", ""};
        }
        return n;
    }

    void resolve_network(Ctx& c, std::uint64_t sid, Family& F, std::int64_t now) {
        NetId id = derive(c.family, c.ids);
        const std::string prev = c.net;
        bool strong = id.conf == "strong";
        if (id.key.empty()) {
            id.key = "unknown:s" + std::to_string(sid);
            id.label = "Unidentified \xC2\xB7 stream " + std::to_string(sid);
            id.conf = "none";
        } else if (id.conf == "weak") {
            // A weak id is NOT a network identity: a DMR color code has 16
            // values, an NXDN RAN 64 -- two unrelated repeaters routinely share
            // one. So a weak bucket is scoped to this stream; talkgroups and
            // radios it shares with other buckets then show up as evidence of a
            // link (the explorer's Links view) instead of being silently merged.
            // Only P25's 12-bit NAC is distinctive enough to resolve to a known
            // strong (WACN/SysID) network carrying the same NAC, when exactly
            // one does.
            std::string match;
            int hits = 0;
            if (c.family == "p25") {
                for (const auto& kv : F.networks) {
                    const Network& n = kv.second;
                    if (n.confidence != "strong") continue;
                    auto it = n.ids.find(id.weak_key);
                    if (it != n.ids.end() && it->second == id.weak_val) { match = kv.first; ++hits; }
                }
            }
            if (hits == 1) {
                id.key = match;
            } else {
                id.key += "@s" + std::to_string(sid);
                id.label += " \xC2\xB7 stream " + std::to_string(sid);
            }
        }
        Network& N = ensure_network(F, id.key, now);
        if (N.label.empty() || (strong && N.confidence != "strong")) {
            N.label = id.label;
            N.confidence = id.conf;
        }
        for (const auto& kv : c.ids) N.ids[kv.first] = kv.second;
        // Upgrade: this stream learned more about its identity (a retune would
        // have cleared c.net first, so prev -> new here is a refinement, e.g.
        // unidentified -> NAC -> SYS 715 -> WACN BEE0A/SYS 715). Fold the
        // bucket it filled before into the network it now belongs to when only
        // this stream fed that bucket, or when the new identity strictly
        // refines it (every anchor id of the old bucket is present, unchanged)
        // -- then other streams on it are moved along too.
        if (!prev.empty() && prev != id.key) {
            auto pit = F.networks.find(prev);
            if (pit != F.networks.end()) {
                const Network& pn = pit->second;
                const bool mine = pn.sessions.empty() ||
                                  (pn.sessions.size() == 1 && *pn.sessions.begin() == sid);
                bool refines = false;
                for (const auto& kv : pn.ids) {
                    if (!is_anchor(c.family, kv.first)) continue;
                    auto it = c.ids.find(kv.first);
                    if (it == c.ids.end() || it->second != kv.second) { refines = false; break; }
                    refines = true;
                }
                if (mine || refines) merge_network(F, prev, id.key);
            }
        }
        c.net = id.key;
        c.strong = strong;
    }

    Network& ensure_network(Family& F, const std::string& key, std::int64_t now) {
        auto it = F.networks.find(key);
        if (it != F.networks.end()) return it->second;
        if (F.networks.size() >= kMaxNetworks) evict_oldest(F.networks);
        Network& n = F.networks[key];
        n.key = key;
        n.first_ms = n.last_ms = now;
        return n;
    }

    void merge_network(Family& F, const std::string& from, const std::string& to) {
        auto fi = F.networks.find(from), ti = F.networks.find(to);
        if (fi == F.networks.end() || ti == F.networks.end()) return;
        Network& a = fi->second;
        Network& z = ti->second;
        z.sites.insert(a.sites.begin(), a.sites.end());
        for (const auto& kv : a.ids) z.ids.insert(kv);
        z.sessions.insert(a.sessions.begin(), a.sessions.end());
        z.calls += a.calls;
        if (a.first_ms && (!z.first_ms || a.first_ms < z.first_ms)) z.first_ms = a.first_ms;
        z.last_ms = std::max(z.last_ms, a.last_ms);
        auto relabel = [&](std::set<std::string>& s) { if (s.erase(from)) s.insert(to); };
        for (auto& kv : F.tgs) relabel(kv.second.networks);
        for (auto& kv : F.radios) relabel(kv.second.networks);
        for (auto& k : F.calls) if (k.net == from) k.net = to;
        for (auto& kv : sess_) if (kv.second.net == from) kv.second.net = to;
        F.networks.erase(fi);
    }

    // ---- entities -------------------------------------------------------
    template <class M>
    static void evict_oldest(M& m) {
        if (m.empty()) return;
        auto victim = m.begin();
        for (auto it = m.begin(); it != m.end(); ++it)
            if (it->second.last_ms < victim->second.last_ms) victim = it;
        m.erase(victim);
    }

    Radio& ensure_radio(Family& F, const std::string& id, std::int64_t now) {
        auto it = F.radios.find(id);
        if (it != F.radios.end()) return it->second;
        if (F.radios.size() >= kMaxRadios) evict_oldest(F.radios);
        Radio& r = F.radios[id];
        r.id = id;
        r.first_ms = r.last_ms = now;
        return r;
    }
    Talkgroup& ensure_tg(Family& F, const std::string& id, std::int64_t now) {
        auto it = F.tgs.find(id);
        if (it != F.tgs.end()) return it->second;
        if (F.tgs.size() >= kMaxTalkgroups) evict_oldest(F.tgs);
        Talkgroup& t = F.tgs[id];
        t.id = id;
        t.first_ms = t.last_ms = now;
        return t;
    }

    void touch_radio(Family& F, const std::string& id, const std::string& net,
                     const std::string& alias, std::int64_t now) {
        Radio& r = ensure_radio(F, id, now);
        r.last_ms = now;
        if (!net.empty()) r.networks.insert(net);
        if (!alias.empty() && std::find(r.aliases.begin(), r.aliases.end(), alias) == r.aliases.end() &&
            r.aliases.size() < kMaxAliases)
            r.aliases.push_back(alias);
    }

    static void bump(std::map<std::string, std::uint32_t>& m, const std::string& k, int d = 1) {
        auto it = m.find(k);
        if (it == m.end()) {
            if (d > 0 && m.size() < kMaxEdgesPerNode) m[k] = static_cast<std::uint32_t>(d);
            return;
        }
        if (d < 0 && it->second <= static_cast<std::uint32_t>(-d)) { m.erase(it); return; }
        it->second = static_cast<std::uint32_t>(static_cast<int>(it->second) + d);
    }

    // Both ends of the call are known: record the association once per call.
    void count_call(Family& F, Call& k, std::int64_t now) {
        k.counted = true;
        Radio& r = ensure_radio(F, k.src, now);
        r.last_ms = now;
        ++r.calls;
        if (!k.net.empty()) r.networks.insert(k.net);
        if (k.priv) {
            bump(r.peers, k.tgt);
            Radio& p = ensure_radio(F, k.tgt, now);
            p.last_ms = now;
            bump(p.peers, k.src);
            if (!k.net.empty()) p.networks.insert(k.net);
        } else {
            bump(r.tgs, k.tgt);
            Talkgroup& t = ensure_tg(F, k.tgt, now);
            t.last_ms = now;
            ++t.calls;
            bump(t.radios, k.src);
            if (!k.net.empty()) t.networks.insert(k.net);
        }
        auto nit = F.networks.find(k.net);
        if (nit != F.networks.end()) ++nit->second.calls;
    }

    // A call already counted as group turned out to be unit-to-unit: move its
    // association from the talkgroup to the radio-radio peer edge.
    void set_private(Family& F, Call& k, std::int64_t now) {
        if (k.priv) return;
        k.priv = true;
        if (!k.counted) return;
        auto rit = F.radios.find(k.src);
        if (rit != F.radios.end()) bump(rit->second.tgs, k.tgt, -1);
        auto tit = F.tgs.find(k.tgt);
        if (tit != F.tgs.end()) {
            bump(tit->second.radios, k.src, -1);
            if (tit->second.calls) --tit->second.calls;
            if (tit->second.calls == 0 && tit->second.radios.empty()) F.tgs.erase(tit);
        }
        if (rit != F.radios.end()) bump(rit->second.peers, k.tgt);
        Radio& p = ensure_radio(F, k.tgt, now);
        p.last_ms = now;
        bump(p.peers, k.src);
        if (!k.net.empty()) p.networks.insert(k.net);
    }

    // ---- calls ----------------------------------------------------------
    // Another stream's open call that is the same call as `k`. Per-stream
    // (weak / unidentified) network keys never match across streams, so this
    // only dedupes on a genuinely shared network.
    static Call* find_twin(Family& F, const Call& k, std::int64_t now) {
        for (auto it = F.calls.rbegin(); it != F.calls.rend(); ++it) {
            Call& o = *it;
            if (o.id == k.id || o.session == k.session || !o.open) continue;
            if (now - o.last_ms > kContinueMs) continue;
            if (o.net == k.net && o.src == k.src && o.tgt == k.tgt && o.priv == k.priv) return &o;
        }
        return nullptr;
    }
    // Fold the just-completed duplicate `dup` into `twin`, point this stream's
    // call slots at the twin, and drop the duplicate. Returns the twin.
    Call* adopt_twin(Ctx& c, Family& F, Call& dup, Call& twin, const std::string& slot) {
        twin.voice = twin.voice || dup.voice;
        twin.data = twin.data || dup.data;
        twin.emergency = twin.emergency || dup.emergency;
        twin.encrypted = twin.encrypted || dup.encrypted;
        if (twin.alias.empty()) twin.alias = dup.alias;
        if (!dup.text.empty() && twin.text.find(dup.text) == std::string::npos)
            twin.text = twin.text.empty() ? dup.text : twin.text + " | " + dup.text;
        if (twin.slot.empty()) twin.slot = dup.slot;
        if (twin.site.empty()) twin.site = dup.site;
        twin.start_ms = std::min(twin.start_ms, dup.start_ms);
        twin.last_ms = std::max(twin.last_ms, dup.last_ms);
        ++twin.streams;
        const std::uint64_t dup_id = dup.id, twin_id = twin.id;
        for (auto& kv : c.active) if (kv.second == dup_id) kv.second = twin_id;
        for (auto& kv : c.last_slot_call) if (kv.second == dup_id) kv.second = twin_id;
        c.last_slot_call[slot] = twin_id;
        // Erasing from a deque can invalidate every reference into it, so
        // re-find the twin by id afterwards.
        for (auto it = F.calls.begin(); it != F.calls.end(); ++it)
            if (it->id == dup_id) { F.calls.erase(it); break; }
        return find_call(F, twin_id);
    }
    static Call* find_call(Family& F, std::uint64_t id) {
        for (auto it = F.calls.rbegin(); it != F.calls.rend(); ++it)
            if (it->id == id) return &*it;
        return nullptr;
    }
    Call* fresh_call(Ctx& c, Family& F, const std::string& slot, std::int64_t now) {
        auto it = c.last_slot_call.find(slot);
        if (it == c.last_slot_call.end()) return nullptr;
        Call* k = find_call(F, it->second);
        if (!k || !k->open || now - k->last_ms > kContinueMs) return nullptr;
        return k;
    }
    Call* open_call(Ctx& c, Family& F, std::uint64_t sid, const std::string& slot, std::int64_t now) {
        // A TDMA slot carries one call at a time: a new call on it ends the
        // previous one. (Slot "" -- e.g. a P25 control channel interleaving
        // grants for several talkgroups -- may hold concurrent calls.)
        if (!slot.empty())
            if (Call* prev = fresh_call(c, F, slot, now)) close_call(F, *prev);
        if (F.calls.size() >= kMaxCalls) {
            close_call(F, F.calls.front());
            F.calls.pop_front();
        }
        Call k;
        k.id = ++next_call_;
        k.session = sid;
        k.slot = slot;
        k.net = c.net;
        k.site = c.site;
        k.start_ms = k.last_ms = now;
        F.calls.push_back(std::move(k));
        c.last_slot_call[slot] = F.calls.back().id;
        return &F.calls.back();
    }
    void close_call(Family& F, Call& k) {
        if (!k.open) return;
        k.open = false;
        if (!k.counted || k.priv) return;
        auto t = F.tgs.find(k.tgt);
        if (t == F.tgs.end()) return;
        if (k.emergency) ++t->second.emergencies;
        if (k.encrypted) ++t->second.encrypted;
    }
    void expire_locked(Ctx& c, Family& F, std::int64_t now) {
        for (auto it = c.active.begin(); it != c.active.end();) {
            Call* k = find_call(F, it->second);
            if (!k || !k->open || now - k->last_ms > kContinueMs) {
                if (k) close_call(F, *k);
                it = c.active.erase(it);
            } else {
                ++it;
            }
        }
    }
    // A stream ended: drop each network it fed that never carried a call --
    // no call counted on it, no call record referring to it -- unless another
    // stream is still on it (that stream's own end decides). Radios and
    // talkgroups known only through a dropped network go with it, and a
    // protocol left empty disappears. Networks with calls are kept.
    void prune_session_locked(std::uint64_t sid) {
        auto sit = sess_.find(sid);
        if (sit == sess_.end()) return;
        const std::string fam = sit->second.family;
        auto fit = fam_.find(fam);
        if (fit == fam_.end()) return;
        Family& F = fit->second;
        std::vector<std::string> dead;
        for (const auto& kv : F.networks) {
            const Network& n = kv.second;
            if (n.calls || !n.sessions.count(sid)) continue;
            bool in_use = false;
            for (const auto& sk : sess_)
                if (sk.first != sid && sk.second.running && sk.second.family == fam && sk.second.net == kv.first) {
                    in_use = true;
                    break;
                }
            if (in_use) continue;
            bool has_call = false;
            for (const auto& k : F.calls) if (k.net == kv.first) { has_call = true; break; }
            if (!has_call) dead.push_back(kv.first);
        }
        if (dead.empty()) return;
        for (const auto& key : dead) {
            F.networks.erase(key);
            for (auto it = F.radios.begin(); it != F.radios.end();) {
                Radio& r = it->second;
                r.networks.erase(key);
                if (r.networks.empty() && !r.calls && r.tgs.empty() && r.peers.empty()) it = F.radios.erase(it);
                else ++it;
            }
            for (auto it = F.tgs.begin(); it != F.tgs.end();) {
                Talkgroup& t = it->second;
                t.networks.erase(key);
                if (t.networks.empty() && !t.calls && t.radios.empty()) it = F.tgs.erase(it);
                else ++it;
            }
            if (sit->second.net == key) sit->second.net.clear();
        }
        if (F.networks.empty() && F.radios.empty() && F.tgs.empty() && F.calls.empty()) fam_.erase(fit);
        ++version_;
    }

    void close_session_calls_locked(std::uint64_t sid) {
        auto sit = sess_.find(sid);
        if (sit == sess_.end()) return;
        auto fit = fam_.find(sit->second.family);
        if (fit == fam_.end()) return;
        for (auto& k : fit->second.calls)
            if (k.session == sid) close_call(fit->second, k);
    }

    // ---- JSON helpers ---------------------------------------------------
    static std::string q(const std::string& s) {
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
    static const char* b(bool v) { return v ? "true" : "false"; }
    template <class C>
    static std::string arr(const C& c) {
        std::string o = "[";
        bool first = true;
        for (const auto& s : c) { if (!first) o += ","; first = false; o += q(s); }
        return o + "]";
    }
    static std::string obj(const std::map<std::string, std::string>& m) {
        std::string o = "{";
        bool first = true;
        for (const auto& kv : m) { if (!first) o += ","; first = false; o += q(kv.first) + ":" + q(kv.second); }
        return o + "}";
    }
    static std::string counts(const std::map<std::string, std::uint32_t>& m) {
        std::string o = "{";
        bool first = true;
        for (const auto& kv : m) {
            if (!first) o += ",";
            first = false;
            o += q(kv.first) + ":" + std::to_string(kv.second);
        }
        return o + "}";
    }

    mutable std::mutex mu_;
    std::map<std::string, Family> fam_;
    std::map<std::uint64_t, Ctx> sess_;
    std::uint64_t version_ = 0;
    std::uint64_t next_call_ = 0;
    AssocRecorder rec_;
};

} // namespace dsdsrv
