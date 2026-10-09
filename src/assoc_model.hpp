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

#include "assoc_audio.hpp"
#include "assoc_log.hpp"
#include "assoc_merge.hpp"
#include "assoc_keys.hpp"
#include "audio_quality.hpp"
#include "dsd_backend_types.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

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
    static_assert(kContinueMs == kTwinGapMs, "merged twins follow the live rule");
    // Calls kept per family: max_calls_ (DSD_NET_MAX_CALLS, default 5000;
    // set_max_calls). When full, the oldest call without audio goes first.
    static constexpr std::size_t kMaxRadios = 3000;    // per family
    static constexpr std::size_t kMaxTalkgroups = 1500;
    static constexpr std::size_t kMaxNetworks = 200;
    static constexpr std::size_t kMaxEdgesPerNode = 300;
    static constexpr std::size_t kMaxAliases = 5;
    static constexpr std::size_t kMaxFreqs = 64;       // channels listed per network
    static constexpr std::int64_t kPrerollMs = 1000;   // audio held before its call is decoded
    static constexpr std::size_t kPrerollSamples = 8000;
    // A call's recording starts only with audible audio (peak at least this,
    // about -54 dBFS -- above decoder fade / comfort noise), and is deleted
    // when the call ends if it is shorter than kMinAudioSamples (0.2 s): no
    // empty or blip-only files.
    static constexpr int kAudiblePeak = 64;
    static constexpr std::uint64_t kMinAudioSamples = 1600;

    static std::int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }
    static std::string random_instance() {
        std::random_device rd;
        std::mt19937_64 g((static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^ static_cast<std::uint64_t>(now_ms()));
        char b[24];
        std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(g()));
        return b;
    }
    static bool default_per_receiver() {
        const char* v = std::getenv("DSD_NET_CHANNEL_MERGE");
        return v && std::string(v) == "receiver";
    }
    static std::string default_name() {
        if (const char* n = std::getenv("DSD_SERVER_NAME"); n && *n) return n;
        char h[256] = {0};
        if (gethostname(h, sizeof h - 1) == 0 && h[0]) return h;
        return "dsd-server";
    }

    // A session started a decode pipeline with this protocol label. Resets the
    // stream context (a restart may be a different channel/protocol).
    // `family` overrides the family derived from the label; only a replay of a
    // recording passes it (a stream recorded mid-run may already have resolved
    // "auto" to a concrete family). `freq_hz` is the channel's absolute
    // frequency when the client told us (0 = unknown; see channel_hz()): it
    // keys weak networks by channel instead of by stream.
    void begin_stream(std::uint64_t sid, const std::string& label, std::int64_t now = now_ms(),
                      const std::string& family = std::string(), std::int64_t freq_hz = 0) {
        std::lock_guard<std::mutex> lk(mu_);
        if (rec_.on())
            rec_.write("{\"op\":\"begin\",\"t\":" + std::to_string(now) + ",\"s\":" + std::to_string(sid) +
                       ",\"label\":" + assoclog::q(label) +
                       (freq_hz ? ",\"freq\":" + std::to_string(freq_hz) : std::string()) + "}", now);
        close_session_calls_locked(sid);
        prune_session_locked(sid);      // a restart ends the previous stream
        Ctx& c = sess_[sid];
        c = Ctx{};
        c.label = label;
        c.family = family.empty() ? assoc_family(label) : family;
        c.freq = freq_hz;
        c.running = true;
        c.started_ms = now;
        ++version_;
    }

    // The session's channel moved (the client changed its offset): what it
    // hears from now on is another channel, so its identity starts over (and,
    // as at a stream start, it registers nothing until it decodes traffic).
    void retune_stream(std::uint64_t sid, std::int64_t freq_hz, std::int64_t now = now_ms()) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sess_.find(sid);
        if (it == sess_.end() || it->second.freq == freq_hz) return;
        if (rec_.on())
            rec_.write("{\"op\":\"tune\",\"t\":" + std::to_string(now) + ",\"s\":" + std::to_string(sid) +
                       ",\"freq\":" + std::to_string(freq_hz) + "}", now);
        close_session_calls_locked(sid);
        // The stream carries on at a corrected frequency: what the old one
        // decoded without a call is most likely the mistuned channel -- drop
        // it even if it identified itself.
        prune_session_locked(sid, true);
        Ctx& c = sess_[sid];
        const std::string f = c.family, l = c.label;
        const bool running = c.running;
        c = Ctx{};
        c.family = f;
        c.label = l;
        c.running = running;
        c.freq = freq_hz;
        ++version_;
    }

    // A channel frequency as the explorer keys it: snapped to `step_hz`
    // (default 1250 Hz -- the finest raster that holds both the 6.25 kHz and
    // the 2.5 kHz channel plans, so a nominal channel frequency is unchanged
    // and an offset up to +-625 Hz off still lands on its channel). 0 = unknown.
    static std::int64_t channel_hz(double hz, std::int64_t step_hz = 1250) {
        if (!(hz > 0)) return 0;
        if (step_hz <= 1) return static_cast<std::int64_t>(hz + 0.5);
        return static_cast<std::int64_t>(hz / static_cast<double>(step_hz) + 0.5) * step_hz;
    }
    // Channels-per-receiver merging (see assoc_merge.hpp ChannelMerge): imports
    // keep another receiver's channel-keyed networks apart instead of joining
    // the same frequency + code. Default: join (DSD_NET_CHANNEL_MERGE=receiver
    // to keep apart).
    void set_channels_per_receiver(bool on) {
        std::lock_guard<std::mutex> lk(mu_);
        per_receiver_ = on;
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

    // Drop everything learned so far, and any imports (the page's Clear
    // button). Live stream contexts are kept (their family) but forget their
    // identity and calls.
    void clear(std::int64_t now = now_ms()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (rec_.on()) rec_.write("{\"op\":\"clear\",\"t\":" + std::to_string(now) + "}", now);
        clear_locked(now);
    }
    // Clear, and also delete the call audio files (the forgotten calls' --
    // and any others the audio folder holds). Returns files and bytes deleted.
    std::pair<std::uint64_t, std::uint64_t> clear_with_audio(std::int64_t now = now_ms()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (rec_.on()) rec_.write("{\"op\":\"clear\",\"t\":" + std::to_string(now) + "}", now);
        clear_locked(now);                     // finishes the calls' files first
        ++version_;
        return audio_.remove_all();
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
        if (clear_first) clear_locked(now);
        if (!rec_.start(dir, max_bytes, now)) return false;
        const bool fresh = fam_.empty() && next_call_ == 0;
        rec_.write("{\"op\":\"header\",\"v\":1,\"t\":" + std::to_string(now) +
                   ",\"fresh\":" + (fresh ? "true" : "false") + ",\"instance\":" + assoclog::q(instance_) +
                   ",\"name\":" + assoclog::q(name_) + ",\"since\":" + std::to_string(since_) +
                   ",\"max_calls\":" + std::to_string(max_calls_) + "}", now);
        for (const auto& kv : sess_) {
            if (!kv.second.running) continue;
            rec_.write("{\"op\":\"begin\",\"t\":" + std::to_string(now) + ",\"s\":" + std::to_string(kv.first) +
                       ",\"label\":" + assoclog::q(kv.second.label) + ",\"family\":" + assoclog::q(kv.second.family) +
                       (kv.second.freq ? ",\"freq\":" + std::to_string(kv.second.freq) : std::string()) +
                       ",\"resumed\":true}", now);
        }
        rec_.write("{\"op\":\"snapshot\",\"t\":" + std::to_string(now) + ",\"why\":\"start\",\"model\":" +
                   snapshot_json_locked(now) + "}", now);
        rec_.flush();
        ++version_;
        return true;
    }
    // Stop recording; the file ends with a "stop" snapshot of the model.
    void stop_recording(std::int64_t now = now_ms()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!rec_.on()) return;
        rec_.write("{\"op\":\"snapshot\",\"t\":" + std::to_string(now) + ",\"why\":\"stop\",\"model\":" +
                   snapshot_json_locked(now) + "}", now);
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
        // (The explorer's stream list: decoding vs quiet. Only decoded output
        // counts -- a sync, call, voice... -- not startup banners and the like.)
        if (ev.kind != "unknown" && ev.crc_error != "1") c.heard_ms = now;
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
        if (c.freq && N.freqs.size() < kMaxFreqs) N.freqs.insert(c.freq);

        // 2. Close this stream's calls that went quiet.
        expire_locked(c, F, now);

        const std::string src = norm_id(ev.source_id);
        const std::string tgt = norm_id(ev.talkgroup);
        const std::string slot = ev.slot;
        const bool priv = is_private(up, extra);
        const bool data = ev.kind == "message" || is_data(up);
        const bool enc = is_encrypted(up, extra);
        const bool emerg = ev.emergency == "1";

        // A roster of the site's other channels opens, ends and extends no
        // call. (Taken for one, every roster line -- several a second on a
        // Capacity Plus channel -- made a source-less call and cut the real
        // call on the slot into sub-second pieces, each with a scrap of audio.)
        if (is_roster(up)) return;

        // A DMR terminator (TLC): the talker unkeyed. The repeater then holds
        // the slot for its hang time (a few seconds), repeating the call's ids
        // with every terminator: that keeps the call alive (live, and one call
        // if the talker keys up again) but isn't part of it -- its length ends
        // here. Voice on the slot again resumes it.
        if (ev.kind == "sync" && has(up, "| TLC")) {
            if (Call* k = fresh_call(c, F, slot, now))
                if (!k->tx_end_ms) k->tx_end_ms = k->last_ms;
            return;
        }

        // Unknown lines (identity broadcasts, P25 LCW, ...) never open calls or
        // introduce ids -- they only refine the call in progress on this slot.
        if (ev.kind == "unknown") {
            if (Call* cur = fresh_call(c, F, slot, now)) {
                if (priv) set_private(F, *cur, now);
                if (emerg) cur->emergency = true;
                if (enc) note_encrypted(F, *cur, extra);
                note_service(F, *cur, extra, now);
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
        // (A call header printed on a voice frame -- D-STAR "AMBE ... DST:" --
        // is voice too.)
        if (ev.kind == "voice" || (ev.kind == "sync" && is_voice_sync(up)) ||
            (ev.kind == "call" && has(" " + up, " AMBE "))) {
            cur->voice = true;
            cur->tx_end_ms = 0;                  // talking again (re-keyed in the hang time)
        }
        // Voice-quality: feed this AMBE frame (its b0 pitch class + FEC errors)
        // to the call's analyzer. From the event stream, so it runs whenever
        // voice is decoded -- independent of recording. DSD_NET_QUALITY=0 off.
        if (ev.voice_b0 >= 0 && quality_on_.load(std::memory_order_relaxed))
            call_quality_[cur->id].feed_frame(ev.voice_b0, ev.voice_err);
        if (data) cur->data = true;
        note_service(F, *cur, extra, now);
        if (emerg) cur->emergency = true;
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
        if (enc) note_encrypted(F, *cur, extra);
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
    // Only the copy is made under the lock (decoder threads wait on it); the
    // serializing happens after.
    std::string to_json(std::int64_t now = now_ms()) const {
        std::string head;
        Dataset view;
        {
            std::lock_guard<std::mutex> lk(mu_);
            head = json_head_locked(now);
            view = view_locked(now);
        }
        return head + families_json(view) + "}";
    }

    // Calls kept per protocol (default DSD_NET_MAX_CALLS / 5000). A replay
    // takes the recording's value.
    void set_max_calls(std::size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        max_calls_ = n < 10 ? 10 : n;
    }
    std::size_t max_calls() const {
        std::lock_guard<std::mutex> lk(mu_);
        return max_calls_;
    }
    // Replay of a recording's "keep" op: call `id` of `family` got (or lost)
    // audio, so it rolls off the list last -- as it did live.
    void mark_keep(const std::string& family, std::uint64_t id, bool on) {
        std::lock_guard<std::mutex> lk(mu_);
        auto fit = fam_.find(family);
        if (fit == fam_.end()) return;
        for (auto& k : fit->second.calls) if (k.id == id) { k.keep = on; break; }
    }

    // ---- per-call audio (see assoc_audio.hpp) -----------------------------
    // Record decoded voice per call, into `dir`. Off by default.
    bool start_audio(const std::string& dir, std::uint64_t cap_bytes, std::int64_t max_age_ms,
                     std::int64_t now = now_ms()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (audio_.on()) return true;
        const bool ok = audio_.enable(dir, cap_bytes, max_age_ms, now);
        audio_on_ = ok;
        ++version_;
        return ok;
    }
    void stop_audio() {
        std::lock_guard<std::mutex> lk(mu_);
        audio_on_ = false;
        audio_.disable();
        for (auto& sk : sess_) sk.second.preroll.clear();
        ++version_;
    }
    CallAudioStore::Status audio_status() const {
        std::lock_guard<std::mutex> lk(mu_);
        return audio_.status();
    }
    // A served file's path ("" if `name` isn't one of the store's files).
    std::string audio_path(const std::string& name, std::int64_t now = now_ms()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (name.size() > 2 && name[0] == 'i' && std::isdigit(static_cast<unsigned char>(name[1]))) {
            // An imported call's audio: only a file uploaded for that import.
            const std::uint64_t id = std::strtoull(name.c_str() + 1, nullptr, 10);
            auto it = import_audio_.find(id);
            if (it == import_audio_.end() || !it->second.files.count(name)) return std::string();
            return import_audio_root_ + "/" + std::to_string(id) + "/" + name;
        }
        return audio_.path_for(name, now);
    }

    // Decoded voice from session `sid`'s decoder: `slot` is the DMR TDMA slot
    // the samples belong to (1 or 2), or 0 for a single-channel decoder. They
    // go to the call open on that slot of that stream -- or, for a protocol
    // without TDMA slots, its current call. Audio that arrives just before
    // its call is decoded is held (up to 1 s) and put at the start. Nothing
    // is recorded for an encrypted call, unless the session has a key
    // (`keyed`) and so hears it in the clear. Audio never changes the call
    // records themselves (what a recording replays), only adds its file.
    void audio(std::uint64_t sid, int slot, const int16_t* pcm, std::size_t n, bool keyed = false,
               std::int64_t now = now_ms()) {
        if (!audio_on_.load(std::memory_order_relaxed) || !n) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (!audio_.on()) return;
        auto sit = sess_.find(sid);
        if (sit == sess_.end() || !sit->second.live) return;
        Ctx& c = sit->second;
        auto fit = fam_.find(c.family);
        if (fit == fam_.end()) return;
        Family& F = fit->second;
        Call* k = nullptr;
        if (slot == 1 || slot == 2) {
            k = fresh_call(c, F, std::to_string(slot), now);
            if (!k && slot == 1) k = fresh_call(c, F, std::string(), now);   // slot-less protocol in a stereo mode
        } else {
            k = fresh_call(c, F, std::string(), now);
            for (const char* s : {"1", "2"})
                if (Call* x = fresh_call(c, F, s, now); x && (!k || x->last_ms > k->last_ms)) k = x;
        }
        int peak = 0;
        for (std::size_t i = 0; i < n; ++i) peak = std::max(peak, pcm[i] < 0 ? -static_cast<int>(pcm[i]) : static_cast<int>(pcm[i]));
        const bool silent = peak == 0, audible = peak >= kAudiblePeak;
        // A data-only call (SMS, private data) never gets audio. While only
        // one DMR slot carries voice, dsd-fme copies that voice to both
        // channels and the copy follows whichever slot last printed a line --
        // often a data call on the other slot: it belongs to the (voice) call
        // there. (Silence from an idle slot is just dropped.)
        if (k && k->data && !k->voice) {
            Call* v = nullptr;
            int vslot = slot;
            if (!silent)
                for (const char* s : {"1", "2", ""}) {
                    Call* x = fresh_call(c, F, s, now);
                    if (x && x != k && !(x->data && !x->voice) && (!v || x->last_ms > v->last_ms)) { v = x; vslot = s[0] ? s[0] - '0' : 0; }
                }
            if (!v) return;
            k = v;
            slot = vslot;
        }
        if (!k) {
            if (silent) return;
            Ctx::Preroll& p = c.preroll[slot];
            if (now - p.last > kPrerollMs) { p.pcm.clear(); p.audible = false; }
            p.audible = p.audible || audible;
            p.pcm.insert(p.pcm.end(), pcm, pcm + n);
            if (p.pcm.size() > kPrerollSamples) p.pcm.erase(p.pcm.begin(), p.pcm.end() - kPrerollSamples);
            p.last = now;
            return;
        }
        if (k->no_audio) return;
        if (k->encrypted && !keyed) {
            k->no_audio = true;
            if (!k->audio.empty()) { audio_.discard(k->audio); k->audio.clear(); ++version_; }
            set_keep_locked(c.family, *k, false, now);
            return;
        }
        if (k->audio.empty()) {
            // Held audio: this slot's, and audio from before the decoder
            // knew the slot (slot 0, e.g. DMR direct mode until the first
            // call line) -- oldest first.
            std::vector<Ctx::Preroll*> held;
            for (int ps : {0, slot})
                if (auto pit = c.preroll.find(ps); pit != c.preroll.end() && !pit->second.pcm.empty() &&
                                                   now - pit->second.last <= kPrerollMs &&
                                                   (held.empty() || ps != 0 || slot != 0))
                    held.push_back(&pit->second);
            if (held.size() == 2 && held[0] == held[1]) held.pop_back();
            std::sort(held.begin(), held.end(), [](const Ctx::Preroll* x, const Ctx::Preroll* y) { return x->last < y->last; });
            bool held_audible = false;
            for (const auto* h : held) held_audible = held_audible || h->audible;
            if (!audible && !held_audible) return;                        // no file without audible audio
            const std::string name = CallAudioStore::make_name(k->start_ms, instance_, k->id);
            if (!audio_.open(name, now)) return;
            k->audio = name;
            k->audio_sid = sid;
            k->audio_slot = slot;
            set_keep_locked(c.family, *k, true, now);
            for (Ctx::Preroll* h : held) {
                audio_.append(name, h->pcm.data(), h->pcm.size(), now);
                k->audio_samples += h->pcm.size();
            }
            for (int ps : {0, slot}) { c.preroll[ps].pcm.clear(); c.preroll[ps].audible = false; }
            ++version_;
        } else if (k->audio_sid != sid || (k->audio_slot != slot && k->audio_slot != 0 && slot != 0)) {
            return;                                   // another receiver's / slot's copy of this call: one recording
        } else if (k->audio_slot == 0 && slot != 0) {
            k->audio_slot = slot;                     // the decoder now knows the slot
        }
        audio_.append(k->audio, pcm, n, now);
        k->audio_samples += n;
    }

    // ---- export / import ----------------------------------------------------
    // What the explorer shows (live data plus imports), as a self-describing
    // file that the explorer can open or import again -- format
    // "dsd-net-export", version 1 (see PROTOCOL.md and assoc_merge.hpp).
    std::string to_export_json(std::int64_t now = now_ms()) const {
        std::unique_lock<std::mutex> lk(mu_);
        const Dataset view = view_locked(now);
        const std::string instance = instance_, name = name_;
        lk.unlock();
        return export_json(view, now, instance, name);
    }

    // The association graph as GraphML (Gephi, Cytoscape, yEd, networkx...).
    // Nodes: radios, talkgroups, networks (attribute "type"; "protocol" keeps
    // protocols apart). Edges: radio-talkgroup ("talkgroup", weight = calls),
    // radio-radio ("private", weight = calls), and talkgroup/radio-network
    // membership ("member").
    std::string to_graphml(std::int64_t now = now_ms()) const {
        std::unique_lock<std::mutex> lk(mu_);
        const Dataset view = view_locked(now);
        lk.unlock();
        return graphml(view, now);
    }

    // Import an export (or a merge of exports) as a layer shown on top of the
    // live data. Refuses anything that would count calls twice: this server's
    // own live data, or a partial overlap with an earlier import; a newer
    // export of an already-imported run replaces the older import. Imports
    // are kept until removed or Clear.
    struct ImportResult {
        std::string status;          // imported | replaced | skipped | refused | invalid
        std::string message;
        std::uint64_t id = 0;
    };
    ImportResult import_export(std::string text, const std::string& label, std::int64_t now = now_ms()) {
        ImportResult r;
        Dataset x;
        std::string err;
        ChannelMerge cm;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cm.per_receiver = per_receiver_;
            cm.home = name_;
        }
        if (!dataset_from_export_text(std::move(text), label, x, &err, cm)) { r.status = "invalid"; r.message = err; return r; }
        std::lock_guard<std::mutex> lk(mu_);
        const std::vector<DsSource> live{live_source_locked(now)};
        Verdict v = judge(x, imports_, &live);
        if (v.kind != Verdict::Add) {
            r.status = v.kind == Verdict::Skip ? "skipped" : "refused";
            r.message = v.why;
            return r;
        }
        std::string replaced;
        for (auto it = v.replaces.rbegin(); it != v.replaces.rend(); ++it) {
            replaced = imports_[*it].label + (replaced.empty() ? "" : ", ") + replaced;
            drop_import_audio_locked(imports_[*it].id);
            imports_.erase(imports_.begin() + static_cast<std::ptrdiff_t>(*it));
        }
        x.id = ++next_import_;
        x.exported = x.exported ? x.exported : now;
        ImportAudio ia;
        // Stable call ids for the layer, apart from live ones (and each other).
        for (auto& fk : x.fams) {
            auto& C = fk.second.calls;
            std::stable_sort(C.begin(), C.end(), [](const DsCall& p, const DsCall& q) { return p.start > q.start; });
            cap_calls(C, max_calls_);
            for (std::size_t i = 0; i < C.size(); ++i) C[i].id = x.id * kImportIdStride + (C.size() - i);
            // An imported call's audio isn't on this server yet: nothing to
            // play until its file is uploaded (import_audio). Note which file
            // each call wants, so an upload can be matched to its calls.
            for (auto& c : C) {
                const std::string base = import_audio_base(c.audio);
                if (!base.empty()) ia.want[base].push_back({fk.first, c.id, c.audio_ms});
                c.audio.clear(); c.audio_ms = 0;
            }
        }
        if (!ia.want.empty()) import_audio_[x.id] = std::move(ia);
        r.id = x.id;
        r.status = replaced.empty() ? "imported" : "replaced";
        r.message = replaced.empty() ? std::string("imported") : "imported (replaces " + replaced + ", an older export of the same run)";
        imports_.push_back(std::move(x));
        ++version_;
        return r;
    }
    bool remove_import(std::uint64_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = imports_.begin(); it != imports_.end(); ++it)
            if (it->id == id) { drop_import_audio_locked(id); imports_.erase(it); ++version_; return true; }
        return false;
    }
    void clear_imports() {
        std::lock_guard<std::mutex> lk(mu_);
        drop_all_import_audio_locked();
        if (!imports_.empty()) { imports_.clear(); ++version_; }
    }
    std::size_t import_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return imports_.size();
    }

    // ---- imported calls' audio ----------------------------------------------
    // An "export with audio" zip carries each call's WAV. Import posts the
    // export, then each WAV (import_audio); a file is kept only if a call in
    // that import names it, and is served as "i<import id>_<its name>" from
    // <root>/<import id>/. It goes when the import does (Remove, Clear, a
    // newer export of the same run replacing it). Imports are not kept across
    // a restart, so neither is their audio: the root is emptied when set.
    // Total size is capped (`cap` bytes, across all imports).
    void use_import_audio_dir(const std::string& root, std::uint64_t cap) {
        std::lock_guard<std::mutex> lk(mu_);
        import_audio_root_ = root;
        import_audio_cap_ = cap;
        std::error_code ec;
        if (!root.empty()) std::filesystem::remove_all(root, ec);
    }
    struct AudioUpload {
        std::string status;          // added | have | unknown | full | invalid | failed
        std::string message;
        std::uint64_t calls = 0;     // calls that now play it
    };
    AudioUpload import_audio(std::uint64_t id, const std::string& name, const std::string& data) {
        AudioUpload r;
        const std::string base = import_audio_base(name);
        std::string dir, served, path;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = import_audio_.find(id);
            if (import_audio_root_.empty()) { r.status = "failed"; r.message = "no folder for imported audio"; return r; }
            if (it == import_audio_.end() || base.empty() || !it->second.want.count(base)) {
                r.status = "unknown"; r.message = "no call in this import has that audio"; return r;
            }
            served = "i" + std::to_string(id) + "_" + base;
            if (it->second.files.count(served)) { r.status = "have"; r.message = "already uploaded"; return r; }
            if (data.size() < 44 || data.compare(0, 4, "RIFF") != 0 || data.compare(8, 4, "WAVE") != 0) {
                r.status = "invalid"; r.message = "not a WAV file"; return r;
            }
            if (import_audio_bytes_locked() + data.size() > import_audio_cap_) {
                r.status = "full"; r.message = "the imported-audio space is full"; return r;
            }
            dir = import_audio_root_ + "/" + std::to_string(id);
        }
        // Written outside the lock (a file can be large), then attached.
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        path = dir + "/" + served;
        {
            std::ofstream f(path + ".part", std::ios::binary | std::ios::trunc);
            f.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!f) { std::filesystem::remove(path + ".part", ec); r.status = "failed"; r.message = "could not write the file"; return r; }
        }
        std::filesystem::rename(path + ".part", path, ec);
        if (ec) { std::filesystem::remove(path + ".part", ec); r.status = "failed"; r.message = "could not write the file"; return r; }
        std::lock_guard<std::mutex> lk(mu_);
        auto it = import_audio_.find(id);
        Dataset* d = nullptr;
        for (auto& x : imports_) if (x.id == id) d = &x;
        if (it == import_audio_.end() || !d || it->second.files.count(served)) {   // removed meanwhile / raced
            if (it == import_audio_.end() || !d) std::filesystem::remove(path, ec);
            r.status = it == import_audio_.end() || !d ? "unknown" : "have";
            r.message = r.status == "have" ? "already uploaded" : "the import was removed";
            return r;
        }
        ImportAudio& ia = it->second;
        ia.files[served] = data.size();
        for (const auto& w : ia.want[base]) {
            auto fit = d->fams.find(w.fam);
            if (fit == d->fams.end()) continue;
            for (auto& c : fit->second.calls)
                if (c.id == w.call) { c.audio = served; c.audio_ms = w.ms; ++r.calls; }
        }
        r.status = "added";
        r.message = "added";
        ++version_;
        return r;
    }

    // ---- network merges (NetMerges, assoc_merge.hpp) ------------------------
    // Shared by everyone viewing this server, kept through Clear, and -- with
    // a file (use_merges_file) -- through restarts. The data is never changed:
    // the explorer shows each group as one network, exports carry the rules.
    // Import adds an export's rules for its own networks to that layer.
    //
    // Keep the rules in `path` ({"merges":{...}}): load it now, save on change.
    bool use_merges_file(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        merges_file_ = path;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        mjson::V root;
        if (!mjson::parse(ss.str(), root) || root.t != mjson::V::Obj) return false;
        const mjson::V* mg = root.get("merges");
        if (!mg || mg->t != mjson::V::Obj) return false;
        NetMerges m;
        for (const auto& fk : mg->o)
            if (fk.second.t == mjson::V::Obj)
                for (const auto& kv : fk.second.o)
                    if (kv.second.t == mjson::V::Str) merges_add(m, fk.first, kv.first, kv.second.s);
        merges_ = std::move(m);
        ++version_;
        return true;
    }
    // Show network `from` (and any merged into it) as part of `to`.
    bool merge_networks(const std::string& family, const std::string& from, const std::string& to) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!merges_add(merges_, family, from, to)) return false;
        // A rule an import brought for `from` gives way to this one.
        for (auto& x : imports_) {
            auto f = x.merges.find(family);
            if (f != x.merges.end()) f->second.erase(from);
        }
        ++version_;
        save_merges_locked();
        return true;
    }
    // Undo: `key` is a merged network (it goes back to being its own) or a
    // network others were merged into (they all go back).
    bool unmerge_network(const std::string& family, const std::string& key) {
        std::lock_guard<std::mutex> lk(mu_);
        bool changed = merges_remove(merges_, family, key);
        for (auto& x : imports_) changed = merges_remove(x.merges, family, key) || changed;
        if (!changed) return false;
        ++version_;
        save_merges_locked();
        return true;
    }
    // The rules in effect (the server's own, then the imports').
    NetMerges merges() const {
        std::lock_guard<std::mutex> lk(mu_);
        return view_merges_locked();
    }

    // ---- encryption keyring (assoc_keys.hpp) ----------------------------
    // A per-network decryption keyring the operator feeds to their own
    // decoder (see keys_csv). The values are stored and saved but never put
    // in /net.json or an export: /net.json carries only which key ids are set
    // ("keyed"), and only a deliberate key-list download returns values.
    bool use_keys_file(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        keys_file_ = path;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        mjson::V root;
        if (!mjson::parse(ss.str(), root) || root.t != mjson::V::Obj) return false;
        keys_ = keyring_parse(root);
        bp_.clear();
        if (const mjson::V* b = root.get("bp"); b && b->t == mjson::V::Obj)
            for (const auto& fk : b->o) {
                if (fk.second.t != mjson::V::Obj) continue;
                for (const auto& nk : fk.second.o)
                    if (nk.second.t == mjson::V::Num) {
                        const int n = static_cast<int>(nk.second.n);
                        if (n >= 1 && n <= 255) bp_[fk.first][nk.first] = n;
                    }
            }
        return true;
    }
    bool set_key(const std::string& fam, const std::string& net, const std::string& kid,
                 const std::string& alg, const std::string& value) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!keyring_set(keys_, fam, net, kid, alg, value, now_ms())) return false;
        ++version_;
        save_keys_locked();
        return true;
    }
    bool remove_key(const std::string& fam, const std::string& net, const std::string& kid) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!keyring_remove(keys_, fam, net, kid)) return false;
        ++version_;
        save_keys_locked();
        return true;
    }
    // DMR Basic Privacy: a key NUMBER (1-255, selecting a built-in well-known
    // key), set per network. BP carries no key id and isn't a hex key, so it
    // lives apart from the keyring; it is applied to the server's decoder with
    // dsd-fme's -b, matched by the network's
    // frequency at stream start. The number is not secret.
    bool set_bp(const std::string& fam, const std::string& net, int number) {
        if (fam.empty() || net.empty() || number < 1 || number > 255) return false;
        std::lock_guard<std::mutex> lk(mu_);
        bp_[fam][net] = number;
        ++version_;
        save_keys_locked();
        return true;
    }
    bool remove_bp(const std::string& fam, const std::string& net) {
        std::lock_guard<std::mutex> lk(mu_);
        auto f = bp_.find(fam);
        if (f == bp_.end() || !f->second.erase(net)) return false;
        if (f->second.empty()) bp_.erase(f);
        ++version_;
        save_keys_locked();
        return true;
    }
    // The BP key number for a network of `fam` on `freq_hz` (0 = none). BP has
    // no key id, so a stream is matched to a stored BP entry by the frequency
    // in its network key ("cc:1@440425000"). First match wins.
    int bp_for_freq(const std::string& fam, std::int64_t freq_hz) const {
        if (freq_hz <= 0) return 0;
        std::lock_guard<std::mutex> lk(mu_);
        auto f = bp_.find(fam);
        if (f == bp_.end()) return 0;
        const std::string suffix = "@" + std::to_string(freq_hz);
        for (const auto& nk : f->second)
            if (nk.first.size() >= suffix.size() &&
                nk.first.compare(nk.first.size() - suffix.size(), suffix.size(), suffix) == 0)
                return nk.second;
        return 0;
    }
    // The dsd-fme hex key list (-K) for one network. Returns at least a header.
    std::string keys_csv(const std::string& fam, const std::string& net) const {
        std::lock_guard<std::mutex> lk(mu_);
        return keyring_csv(keys_, fam, net);
    }
    // The dsd-fme hex key list (-K) covering a whole family (or, with fam "",
    // every family), so a decoder can be handed the keyring at start. Returns
    // at least a header.
    std::string keys_csv_family(const std::string& fam) const {
        std::lock_guard<std::mutex> lk(mu_);
        return keyring_csv_family(keys_, fam);
    }
    std::size_t keys_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::size_t n = 0;
        for (const auto& fk : keys_) for (const auto& nk : fk.second) n += nk.second.size();
        return n;
    }
    // How many keys are stored for one family (fam "" = all).
    std::size_t keys_count_family(const std::string& fam) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::size_t n = 0;
        for (const auto& fk : keys_) { if (!fam.empty() && fk.first != fam) continue; for (const auto& nk : fk.second) n += nk.second.size(); }
        return n;
    }

    // This server run's identity in exports (see assoc_merge.hpp). The
    // instance is random per process; the name is DSD_SERVER_NAME or the
    // host name. Tests and replays set them explicitly.
    void set_identity(const std::string& instance, const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!instance.empty()) instance_ = instance;
        name_ = name;
    }
    // Start of the live data's time span (process start, or the last Clear).
    void set_since(std::int64_t since) {
        std::lock_guard<std::mutex> lk(mu_);
        since_ = since;
    }
    std::string instance() const { std::lock_guard<std::mutex> lk(mu_); return instance_; }
    std::string name() const { std::lock_guard<std::mutex> lk(mu_); return name_; }
    std::int64_t since() const { std::lock_guard<std::mutex> lk(mu_); return since_; }

    // A call's audio name from an export, as this server would store it: any
    // "i<n>_" prefixes (an import of an import) dropped, and only the audio
    // store's own safe "call_....wav" form accepted. "" if not.
    static std::string import_audio_base(std::string n) {
        while (n.size() > 2 && n[0] == 'i' && std::isdigit(static_cast<unsigned char>(n[1]))) {
            std::size_t u = 1;
            while (u < n.size() && std::isdigit(static_cast<unsigned char>(n[u]))) ++u;
            if (u >= n.size() || n[u] != '_') break;
            n = n.substr(u + 1);
        }
        return CallAudioStore::valid_name(n) ? n : std::string();
    }
private:
    static constexpr std::uint64_t kImportIdStride = 1000000000ull;

    std::uint64_t import_audio_bytes_locked() const {
        std::uint64_t t = 0;
        for (const auto& ik : import_audio_) for (const auto& f : ik.second.files) t += f.second;
        return t;
    }
    std::string import_audio_json_locked(std::uint64_t id) const {
        auto it = import_audio_.find(id);
        if (it == import_audio_.end()) return std::string();
        return ",\"audio_files\":" + std::to_string(it->second.want.size()) +
               ",\"audio_have\":" + std::to_string(it->second.files.size());
    }
    void drop_import_audio_locked(std::uint64_t id) {
        if (!import_audio_.erase(id) || import_audio_root_.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(import_audio_root_ + "/" + std::to_string(id), ec);
    }
    void drop_all_import_audio_locked() {
        std::vector<std::uint64_t> ids;
        for (const auto& ik : import_audio_) ids.push_back(ik.first);
        for (auto id : ids) drop_import_audio_locked(id);
    }

    // /net.json up to its "families" (the view: live + imports): this run's
    // identity and the import list. Recording snapshots use the live-only
    // form, which replays can reproduce exactly.
    std::string json_head_locked(std::int64_t now) const {
        std::string im = "[";
        for (std::size_t i = 0; i < imports_.size(); ++i) {
            const Dataset& d = imports_[i];
            std::uint64_t nn = 0, nt = 0, nr = 0, nc = 0;
            for (const auto& fk : d.fams) {
                nn += fk.second.networks.size(); nt += fk.second.tgs.size();
                nr += fk.second.radios.size(); nc += fk.second.calls.size();
            }
            if (i) im += ",";
            im += "{\"id\":" + std::to_string(d.id) + ",\"label\":" + assocjson::q(d.label) +
                  ",\"exported\":" + std::to_string(d.exported) + ",\"sources\":" + sources_json(d.sources) +
                  ",\"networks\":" + std::to_string(nn) + ",\"talkgroups\":" + std::to_string(nt) +
                  ",\"radios\":" + std::to_string(nr) + ",\"calls\":" + std::to_string(nc) +
                  import_audio_json_locked(d.id) + "}";
        }
        im += "]";
        return "{\"version\":" + std::to_string(version_) + ",\"now\":" + std::to_string(now) +
               ",\"instance\":" + assocjson::q(instance_) + ",\"name\":" + assocjson::q(name_) +
               ",\"since\":" + std::to_string(since_) + ",\"rec\":" + rec_json_locked() +
               ",\"audio\":" + audio_json_locked() + ",\"max_calls\":" + std::to_string(max_calls_) +
               ",\"map\":" + map_json() + ",\"dev\":" + (dev_tools() ? "true" : "false") +
               ",\"streams\":" + streams_json_locked() + ",\"imports\":" + im +
               ",\"merges\":" + merges_json(view_merges_locked()) +
               ",\"keyed\":" + keyring_loaded_json(keys_) +
               ",\"bp\":" + bp_json_locked() +
               ",\"families\":";
    }
    // The explorer's map: a tile server of your own (DSD_NET_MAP_TILES, a URL
    // template with {z} {x} {y} -- e.g. a local OpenStreetMap tile server for
    // use offline -- and DSD_NET_MAP_ATTRIB, its credit line). The browser
    // fetches the tiles itself; unset, the page offers public ones.
    // DSD_NET_DEV=1: the explorer shows its developer tools (Record) to
    // everyone (otherwise only to a browser opened with /net?dev=1).
    static bool dev_tools() {
        static const bool on = [] {
            const char* v = std::getenv("DSD_NET_DEV");
            return v && (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T' || std::string(v) == "on");
        }();
        return on;
    }
    static std::string map_json() {
        static const std::string j = [] {
            const char* t = std::getenv("DSD_NET_MAP_TILES");
            const char* a = std::getenv("DSD_NET_MAP_ATTRIB");
            const std::string tiles = t ? t : "";
            return "{\"tiles\":" + assocjson::q(tiles) + ",\"attrib\":" + assocjson::q(a ? a : "") + "}";
        }();
        return j;
    }
    // The decode streams running now -- connected, whether or not anything is
    // being decoded: [{"s","fam","label","freq","since","heard","live"}].
    // fam is the protocol ("auto" until detected); heard the last decoded
    // output -- sync, call, voice... (0 = none yet); live whether it has
    // decoded real traffic.
    std::string streams_json_locked() const {
        std::string o = "[";
        bool first = true;
        for (const auto& kv : sess_) {
            const Ctx& c = kv.second;
            if (!c.running) continue;
            o += (first ? "" : ",") + std::string("{\"s\":") + std::to_string(kv.first) + ",\"fam\":" + assocjson::q(c.family) +
                 ",\"label\":" + assocjson::q(c.label) + ",\"freq\":" + std::to_string(c.freq) +
                 ",\"since\":" + std::to_string(c.started_ms) + ",\"heard\":" + std::to_string(c.heard_ms) +
                 ",\"live\":" + (c.live ? "true" : "false") + "}";
            first = false;
        }
        return o + "]";
    }
    std::string audio_json_locked() const {
        const auto a = audio_.status();
        return std::string("{\"on\":") + (a.on ? "true" : "false") + ",\"dir\":" + assocjson::q(a.dir) +
               ",\"bytes\":" + std::to_string(a.bytes) + ",\"cap_bytes\":" + std::to_string(a.cap_bytes) +
               ",\"files\":" + std::to_string(a.files) + ",\"recording\":" + std::to_string(a.open) + "}";
    }
    std::string snapshot_json_locked(std::int64_t now) const {
        return "{\"version\":" + std::to_string(version_) + ",\"now\":" + std::to_string(now) +
               ",\"rec\":" + rec_json_locked() + ",\"families\":" + families_json(dataset_locked(now, false)) + "}";
    }
    DsSource live_source_locked(std::int64_t now) const { return DsSource{instance_, name_, since_, now}; }

    // The live model in plain form. Calls newest first.
    Dataset dataset_locked(std::int64_t now, bool with_audio = true) const {
        Dataset d;
        d.label = "live";
        d.exported = now;
        d.sources.push_back(live_source_locked(now));
        for (const auto& fk : fam_) {
            const Family& F = fk.second;
            DsFamily& D = d.fams[fk.first];
            for (const auto& kv : F.networks) {
                const Network& n = kv.second;
                DsNetwork& o = D.networks[kv.first];
                o.key = n.key; o.label = n.label; o.confidence = n.confidence; o.ids = n.ids; o.sites = n.sites;
                o.freqs = n.freqs;
                o.sessions = n.sessions.size(); o.calls = n.calls; o.first = n.first_ms; o.last = n.last_ms;
                o.keys.insert(n.keys.begin(), n.keys.end());
            }
            for (const auto& kv : F.tgs) {
                const Talkgroup& t = kv.second;
                DsTalkgroup& o = D.tgs[kv.first];
                o.id = t.id; o.networks = t.networks; o.radios.insert(t.radios.begin(), t.radios.end());
                o.calls = t.calls; o.emerg = t.emergencies; o.enc = t.encrypted; o.first = t.first_ms; o.last = t.last_ms;
                o.keys.insert(t.keys.begin(), t.keys.end());
            }
            for (const auto& kv : F.radios) {
                const Radio& r = kv.second;
                DsRadio& o = D.radios[kv.first];
                o.id = r.id; o.aliases = r.aliases; o.networks = r.networks;
                o.tgs.insert(r.tgs.begin(), r.tgs.end()); o.peers.insert(r.peers.begin(), r.peers.end());
                o.calls = r.calls; o.first = r.first_ms; o.last = r.last_ms;
                o.keys.insert(r.keys.begin(), r.keys.end());
                o.pos = r.pos; o.pos_t = r.pos_ms; o.track = r.track;
            }
            D.calls.reserve(F.calls.size());
            for (auto it = F.calls.rbegin(); it != F.calls.rend(); ++it) {
                const Call& k = *it;
                DsCall c;
                c.id = k.id; c.session = k.session; c.streams = k.streams; c.freq = k.freq;
                if (with_audio && !k.audio.empty()) {
                    c.audio = k.audio;
                    c.audio_ms = k.audio_samples * 1000 / CallAudioStore::kRate;
                }
                c.net = k.net; c.site = k.site; c.slot = k.slot; c.src = k.src; c.tgt = k.tgt;
                c.alias = k.alias; c.text = k.text; c.svc = k.svc; c.pos = k.pos;
                c.priv = k.priv; c.voice = k.voice; c.data = k.data; c.emerg = k.emergency; c.enc = k.encrypted;
                c.alg = k.alg; c.kid = k.kid;
                // Voice quality: the finalized verdict, or -- for a still-open
                // call -- a live running one from its active analyzer, so an
                // ongoing bad call shows up before it ends.
                {
                    VoiceQuality::Summary s = k.qual;
                    if (k.open)
                        if (auto qit = call_quality_.find(k.id); qit != call_quality_.end())
                            s = qit->second.summary();
                    if (s.verdict != VoiceQuality::Verdict::Unknown) {
                        c.qual = VoiceQuality::verdict_str(s.verdict);
                        c.qjunk = s.junk;
                        c.qsil = s.sil;
                        c.qerr = s.err_per_frame;
                        c.qframes = s.frames;
                    }
                }
                c.open = k.open && now - k.last_ms <= kContinueMs;
                // Its length ends when the talker unkeyed, not with the
                // repeater's hang time after it.
                c.start = k.start_ms; c.last = k.tx_end_ms ? k.tx_end_ms : k.last_ms;
                D.calls.push_back(std::move(c));
            }
        }
        return d;
    }
    NetMerges view_merges_locked() const {
        NetMerges m = merges_;
        for (const auto& x : imports_) merges_union(m, x.merges);
        return m;
    }
    void save_merges_locked() const {
        if (merges_file_.empty()) return;
        const std::string tmp = merges_file_ + ".tmp";
        std::error_code ec;
        const auto dir = std::filesystem::path(merges_file_).parent_path();
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << "{\"merges\":" << merges_json(merges_) << "}\n";
            if (!f) return;
        }
        std::filesystem::rename(tmp, merges_file_, ec);
    }
    // The keyring holds decryption key values, so write it readable only by
    // the server's own user (0600), never world-readable like other state.
    // {"dmr":{"cc:1@440425000":7,...}} -- BP key numbers per network (not secret).
    std::string bp_json_locked() const {
        using assocjson::q;
        std::ostringstream o;
        o << "{";
        bool ff = true;
        for (const auto& fk : bp_) {
            o << (ff ? "" : ",") << q(fk.first) << ":{"; ff = false;
            bool fn = true;
            for (const auto& nk : fk.second) { o << (fn ? "" : ",") << q(nk.first) << ":" << nk.second; fn = false; }
            o << "}";
        }
        return o.str() + "}";
    }
    void save_keys_locked() const {
        if (keys_file_.empty()) return;
        const std::string tmp = keys_file_ + ".tmp";
        std::error_code ec;
        const auto dir = std::filesystem::path(keys_file_).parent_path();
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << "{\"keys\":" << keyring_json(keys_) << ",\"bp\":" << bp_json_locked() << "}\n";
            if (!f) return;
        }
        std::filesystem::permissions(tmp, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
        std::filesystem::rename(tmp, keys_file_, ec);
    }

    // Live data with the imports merged in.
    Dataset view_locked(std::int64_t now) const {
        Dataset d = dataset_locked(now);
        d.merges = merges_;
        if (imports_.empty()) return d;
        for (const auto& x : imports_) merge_into(d, x);
        finalize_merge(d, false, max_calls_);
        return d;
    }

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
        using assocjson::b;
        using assocjson::q;
        return std::string("{\"on\":") + b(r.on) + ",\"truncated\":" + b(r.truncated) + ",\"file\":" + q(file) +
               ",\"path\":" + q(r.last_path) + ",\"bytes\":" + std::to_string(r.bytes) +
               ",\"file_bytes\":" + std::to_string(r.file_bytes) + "}";
    }
    // Forget everything learned (and imported); live stream contexts keep their
    // protocol and running state. The live data's span restarts at `now`. Call ids restart, so a recording begun after a clear
    // replays to identical output.
    void clear_locked(std::int64_t now) {
        for (auto& fk : fam_)
            for (auto& k : fk.second.calls)
                if (!k.audio.empty()) audio_.finish(k.audio);   // the files stay on disk
        fam_.clear();
        drop_all_import_audio_locked();
        imports_.clear();
        since_ = now;
        for (auto& kv : sess_) {
            Ctx& c = kv.second;
            const std::string f = c.family, l = c.label;
            const bool running = c.running;
            const std::int64_t freq = c.freq, started = c.started_ms, heard = c.heard_ms;
            c = Ctx{};
            c.family = f;
            c.label = l;
            c.running = running;
            c.freq = freq;
            c.started_ms = started;
            c.heard_ms = heard;
        }
        next_call_ = 0;
        ++version_;
    }

    struct Call {
        std::uint64_t id = 0, session = 0;
        std::string net, site, slot, src, tgt, alias, text;
        std::string svc, pos;                           // data service (svc_rank), position report "lat,lon"
        bool priv = false, voice = false, data = false, emergency = false, encrypted = false;
        bool open = true, counted = false;
        std::string alg, kid;                           // encrypted: algorithm / key id named (hex; "" = not seen)
        bool key_noted = false;                         // its key counted on its network, talkgroup, radio
        std::string audio;                              // its audio file ("" = none; see audio())
        std::uint64_t audio_sid = 0;                    // the stream + slot the audio comes from
        int audio_slot = -1;
        std::uint64_t audio_samples = 0;                // its length (8 kHz samples)
        bool keep = false;                              // has audio: rolls off the list last (recorded)
        bool no_audio = false;                          // encrypted: never record
        std::int64_t start_ms = 0, last_ms = 0;
        std::int64_t tx_end_ms = 0;                     // talker unkeyed (DMR terminator; 0 = talking)
        std::uint32_t frames = 0;
        std::uint32_t streams = 1;                      // receivers that heard it (see adopt_twin)
        std::int64_t freq = 0;                          // channel, Hz (0 = unknown)
        VoiceQuality::Summary qual;                     // voice-quality verdict, finalized at close_call
    };
    struct Network {
        std::string key, label, confidence;            // confidence: strong | channel | weak | none
        std::map<std::string, std::string> ids;
        std::set<std::string> sites;
        std::set<std::int64_t> freqs;                  // channels it was heard on, Hz
        std::set<std::uint64_t> sessions;
        std::uint64_t calls = 0;
        std::int64_t first_ms = 0, last_ms = 0;
        std::map<std::string, std::uint32_t> keys;     // encryption keys its calls used: "alg:kid" -> calls
    };
    struct Talkgroup {
        std::string id;
        std::map<std::string, std::uint32_t> radios;   // radio -> calls
        std::set<std::string> networks;
        std::uint64_t calls = 0;
        std::uint32_t emergencies = 0, encrypted = 0;
        std::int64_t first_ms = 0, last_ms = 0;
        std::map<std::string, std::uint32_t> keys;     // "alg:kid" -> calls
    };
    struct Radio {
        std::string id;
        std::vector<std::string> aliases;
        std::map<std::string, std::uint32_t> tgs;      // talkgroup -> calls
        std::map<std::string, std::uint32_t> peers;    // radio -> private calls (either way)
        std::map<std::string, std::uint32_t> keys;     // "alg:kid" -> calls it made
        std::set<std::string> networks;
        std::uint64_t calls = 0;
        std::int64_t first_ms = 0, last_ms = 0;
        std::string pos;                                // its last position report "lat,lon"
        std::int64_t pos_ms = 0;
        std::vector<std::pair<std::int64_t, std::string>> track;  // its position reports (track_add)
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
        bool adjacent = false;                          // the lines being read describe a neighbour site
        bool live = false;                              // has decoded real traffic yet
        bool running = false;                           // pipeline up (begin_stream .. end_stream)
        std::int64_t freq = 0;                          // channel frequency, Hz (0 = unknown)
        std::int64_t started_ms = 0;                    // its pipeline started (begin_stream)
        std::int64_t heard_ms = 0;                      // last decoded output: sync, call, voice... (0 = not yet)
        std::map<std::string, std::uint64_t> active;   // "slot|tgt" -> call id
        std::map<std::string, std::uint64_t> last_slot_call; // slot -> latest call id
        // Weak-anchor values seen but not yet believed: key -> (value, times in a row).
        std::map<std::string, std::pair<std::string, int>> pending;
        // Audio that arrived on a slot before its call was decoded (per audio
        // slot 0/1/2), kept briefly so the call's recording starts with it.
        struct Preroll { std::vector<int16_t> pcm; std::int64_t last = 0; bool audible = false; };
        std::map<int, Preroll> preroll;
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
    // A trunking roster: Capacity Plus channel status ("Bank One F80 Private
    // or Data Call(s) -  LSN 01: TGT 17434; LSN 02: TGT 23043;") lists what
    // the site's other logical channels carry -- it is not a call here.
    static bool is_roster(const std::string& up) { return has(up, "CALL(S)") && has(up, "LSN"); }
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
        // dsd-fme prints a neighbour broadcast as a heading line, then the
        // neighbour's ids on the next line(s): "Adjacent Status Broadcast -
        // Abbreviated" / "LRA [07] RFSS[007] SITE [007] SYSID [015] CHAN-T
        // [0197] SSC [70]". Those ids are the neighbour's until the next
        // message starts (a sync, or another broadcast's heading).
        if (ev.kind == "sync" || has(up, "BROADCAST")) c.adjacent = has(up, "ADJ") || has(up, "NEIGHB");
        std::map<std::string, std::string> got;
        const std::string& fam = c.family;
        if (!ev.color_code.empty() && (fam == "dmr" || fam == "dpmr" || fam == "tetra"))
            got["cc"] = ev.color_code;
        if (!ev.nac.empty() && fam == "p25") got["nac"] = ev.nac;
        if (!ev.ran.empty() && fam == "nxdn") got["ran"] = ev.ran;
        for (const auto& kv : x) {
            if (kv.second.empty()) continue;
            if (fam == "dstar" && kv.first == "rpt1" && upper(kv.second) == "DIRECT") continue;   // simplex, no repeater
            if (is_anchor(fam, kv.first) || is_site_key(fam, kv.first) ||
                (fam == "dmr" && kv.first == "network_type"))
                got[kv.first] = kv.second;
        }
        if (got.empty()) return false;

        // Adjacent/neighbor-site broadcasts describe OTHER sites (possibly of
        // another system): record the site on this stream's network, but
        // absorb nothing -- it must not move this stream's identity or site.
        if (c.adjacent || has(up, "ADJ") || has(up, "NEIGHB")) {
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

    static std::string mhz(std::int64_t hz) { return freq_text(hz); }

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
        if (id.key.empty() && c.freq) {
            // Nothing decoded but the channel is known: the channel is the
            // best identity there is (and the same one for every stream on it).
            id.key = "ch@" + std::to_string(c.freq);
            id.label = "Unidentified \xC2\xB7 " + mhz(c.freq);
            id.conf = "channel";
        } else if (id.key.empty()) {
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
            // one does. With the channel frequency known, a short code on one
            // channel is as good as a conventional repeater's identity: every
            // stream on that frequency hearing that code shares one network.
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
            } else if (c.freq) {
                id.key += "@" + std::to_string(c.freq);
                id.label += " \xC2\xB7 " + mhz(c.freq);
                id.conf = "channel";
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
                // An unidentified bucket of a known channel is refined by any
                // identity heard on that channel, however many streams fed it.
                const bool channel_unknown = c.freq && prev == "ch@" + std::to_string(c.freq);
                if (mine || refines || channel_unknown) merge_network(F, prev, id.key);
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
        for (std::int64_t f : a.freqs) if (z.freqs.size() < kMaxFreqs) z.freqs.insert(f);
        for (const auto& kv : a.ids) z.ids.insert(kv);
        z.sessions.insert(a.sessions.begin(), a.sessions.end());
        z.calls += a.calls;
        for (const auto& kv : a.keys) key_add(z.keys, kv.first, kv.second);
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

    // What a decoder line says about the call it belongs to: the data
    // service (kept if more specific than what the call has) and a position
    // report (on the call, and as the sending radio's last position).
    static void note_service(Family& F, Call& k, const std::map<std::string, std::string>& extra, std::int64_t now) {
        auto s = extra.find("svc");
        if (s != extra.end() && svc_rank(s->second) > svc_rank(k.svc)) k.svc = s->second;
        auto g = extra.find("gps");
        if (g == extra.end() || g->second.empty()) return;
        k.pos = g->second;
        auto r = F.radios.find(k.src);
        if (r != F.radios.end()) {
            r->second.pos = g->second;
            r->second.pos_ms = now;
            track_add(r->second.track, now, g->second);
        }
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
        // (No source: a call whose talker wasn't decoded -- the target only.)
        Radio* r = nullptr;
        if (!k.src.empty()) {
            r = &ensure_radio(F, k.src, now);
            r->last_ms = now;
            ++r->calls;
            if (!k.net.empty()) r->networks.insert(k.net);
        }
        if (k.priv) {
            if (r) {
                bump(r->peers, k.tgt);
                Radio& p = ensure_radio(F, k.tgt, now);
                p.last_ms = now;
                bump(p.peers, k.src);
                if (!k.net.empty()) p.networks.insert(k.net);
            }
        } else {
            if (r) bump(r->tgs, k.tgt);
            Talkgroup& t = ensure_tg(F, k.tgt, now);
            t.last_ms = std::max(t.last_ms, now);
            ++t.calls;
            if (r) bump(t.radios, k.src);
            if (!k.net.empty()) t.networks.insert(k.net);
        }
        auto nit = F.networks.find(k.net);
        if (nit != F.networks.end()) ++nit->second.calls;
        note_enc_counts(F, k);
    }

    // An encrypted call: flag it, and take the algorithm / key id it names
    // (the first seen; normalised: uppercase hex, no leading zeros, at least
    // two digits -- "0x0001" and "01" are one key).
    void note_encrypted(Family& F, Call& k, const std::map<std::string, std::string>& x) {
        k.encrypted = true;
        auto hexid = [](std::string v) {
            v = upper(v);
            const std::size_t nz = v.find_first_not_of('0');
            v = nz == std::string::npos ? std::string() : v.substr(nz);
            while (v.size() < 2) v = "0" + v;
            return v;
        };
        if (k.kid.empty()) {
            auto ki = x.find("key_id");
            if (ki != x.end() && !ki->second.empty()) {
                k.kid = hexid(ki->second);
                auto ai = x.find("alg_id");
                if (ai != x.end() && !ai->second.empty()) k.alg = hexid(ai->second);
            }
        }
        note_enc_counts(F, k);
    }
    // Once a call is counted: the key it used, on its network, talkgroup
    // and talker (once per call). (Its talkgroup's encrypted-call count is
    // taken when it closes: close_call.)
    void note_enc_counts(Family& F, Call& k) {
        if (!k.counted || k.kid.empty() || k.key_noted) return;
        k.key_noted = true;
        const std::string key = enc_key(k.alg, k.kid);
        if (auto n = F.networks.find(k.net); n != F.networks.end()) key_add(n->second.keys, key);
        if (!k.priv) if (auto t = F.tgs.find(k.tgt); t != F.tgs.end()) key_add(t->second.keys, key);
        if (auto r = F.radios.find(k.src); r != F.radios.end()) key_add(r->second.keys, key);
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
            if (k.key_noted) bump(tit->second.keys, enc_key(k.alg, k.kid), -1);
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
        if (twin.kid.empty()) { twin.alg = dup.alg; twin.kid = dup.kid; }
        if (twin.alias.empty()) twin.alias = dup.alias;
        if (!dup.text.empty() && twin.text.find(dup.text) == std::string::npos)
            twin.text = twin.text.empty() ? dup.text : twin.text + " | " + dup.text;
        if (twin.slot.empty()) twin.slot = dup.slot;
        if (twin.site.empty()) twin.site = dup.site;
        if (svc_rank(dup.svc) > svc_rank(twin.svc)) twin.svc = dup.svc;
        if (twin.pos.empty()) twin.pos = dup.pos;
        twin.start_ms = std::min(twin.start_ms, dup.start_ms);
        twin.last_ms = std::max(twin.last_ms, dup.last_ms);
        ++twin.streams;
        // One recording per call: the twin's, else the duplicate's.
        if (!dup.audio.empty()) {
            if (twin.audio.empty()) {
                twin.audio = dup.audio; twin.audio_sid = dup.audio_sid; twin.audio_slot = dup.audio_slot;
                twin.audio_samples = dup.audio_samples;
            } else {
                audio_.discard(dup.audio);
            }
            dup.audio.clear();
        }
        twin.keep = twin.keep || dup.keep;            // (from the flag: a replay has no files)
        const std::uint64_t dup_id = dup.id, twin_id = twin.id;
        for (auto& kv : c.active) if (kv.second == dup_id) kv.second = twin_id;
        for (auto& kv : c.last_slot_call) if (kv.second == dup_id) kv.second = twin_id;
        c.last_slot_call[slot] = twin_id;
        // Erasing from a deque can invalidate every reference into it, so
        // re-find the twin by id afterwards.
        for (auto it = F.calls.begin(); it != F.calls.end(); ++it)
            if (it->id == dup_id) { F.calls.erase(it); break; }
        call_quality_.erase(dup_id);   // the twin keeps its own analyzer
        Call* t = find_call(F, twin_id);
        if (t) note_enc_counts(F, *t);                // the key the duplicate knew
        return t;
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
        if (F.calls.size() >= max_calls_) {
            // Roll off the oldest call -- but calls with audio last.
            auto victim = F.calls.begin();
            for (auto it = F.calls.begin(); it != F.calls.end(); ++it)
                if (!it->keep) { victim = it; break; }
            close_call(F, *victim);
            F.calls.erase(victim);
        }
        Call k;
        k.id = ++next_call_;
        k.session = sid;
        k.slot = slot;
        k.net = c.net;
        k.site = c.site;
        k.freq = c.freq;
        k.start_ms = k.last_ms = now;
        F.calls.push_back(std::move(k));
        c.last_slot_call[slot] = F.calls.back().id;
        return &F.calls.back();
    }
    // A call got / lost audio: it rolls off the list last. Recorded, so a
    // replay keeps the same calls.
    void set_keep_locked(const std::string& family, Call& k, bool on, std::int64_t now) {
        if (k.keep == on) return;
        k.keep = on;
        if (rec_.on())
            rec_.write("{\"op\":\"keep\",\"t\":" + std::to_string(now) + ",\"f\":" + assoclog::q(family) +
                       ",\"c\":" + std::to_string(k.id) + ",\"on\":" + (on ? "1" : "0") + "}", now);
    }
    void set_keep_locked(Family& F, Call& k, bool on) {
        for (const auto& fk : fam_)
            if (&fk.second == &F) { set_keep_locked(fk.first, k, on, k.last_ms); return; }
    }
    void close_call(Family& F, Call& k) {
        if (!k.open) return;
        k.open = false;
        // Finalize the voice-quality verdict from this call's active analyzer.
        if (auto qit = call_quality_.find(k.id); qit != call_quality_.end()) {
            k.qual = qit->second.summary();
            call_quality_.erase(qit);
        }
        if (!k.audio.empty()) {
            // Flagged encrypted late, turned out to carry only data, or too
            // short to be worth a file: delete.
            if (k.encrypted || (k.data && !k.voice) || k.audio_samples < kMinAudioSamples) {
                audio_.discard(k.audio);
                k.audio.clear();
                k.audio_samples = 0;
                set_keep_locked(F, k, false);
            } else {
                audio_.finish(k.audio);
            }
        }
        // A voice call whose talker was never decoded (D-STAR heard after its
        // header: "SRC: ... INTERRUPTED") still went to its target: counted
        // for it (and its network) when it ends -- without a radio.
        if (!k.counted && k.src.empty() && !k.tgt.empty() && k.voice && !k.data) count_call(F, k, k.last_ms);
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
    // no call counted on it, no call record referring to it -- and that only
    // ever meant something while the stream lasted: an "Unidentified" bucket
    // (noise, a wrong protocol's syncs) or a short code scoped to the stream
    // ("Color Code 4 · stream 3"). Unless another stream is still on it (that
    // stream's own end decides). A network that identified itself for good --
    // a system id (a control channel's WACN/SYS), or a code on a known channel
    // -- is real knowledge and stays, calls or not: a quiet control channel
    // whose client reconnects every few seconds must not make its protocol
    // vanish and reappear. Radios and talkgroups known only through a dropped
    // network go with it, and a protocol left empty disappears.
    // `retune`: the stream moved channel (retune_stream) -- identified
    // call-less networks of the old channel go too.
    //
    // A partial identity the stream had before it heard the rest -- P25 "SYS
    // 006" before the WACN, or a NAC before the system id -- is covered by
    // the network that has it all (every network id of it there, the same):
    // nothing is lost dropping it. (A short code is only covered by a system
    // carrying it on P25, where the NAC resolves to its system.)
    static bool covered_locked(const Family& F, const std::string& fam, const std::string& key, const Network& n) {
        std::vector<std::pair<std::string, std::string>> mine;
        for (const auto& id : n.ids) if (is_anchor(fam, id.first)) mine.push_back(id);
        if (mine.empty() || (n.confidence != "strong" && fam != "p25")) return false;
        for (const auto& o : F.networks) {
            if (o.first == key || o.second.confidence != "strong") continue;
            bool all = true;
            for (const auto& id : mine) {
                auto it = o.second.ids.find(id.first);
                if (it == o.second.ids.end() || it->second != id.second) { all = false; break; }
            }
            if (all) return true;
        }
        return false;
    }
    void prune_session_locked(std::uint64_t sid, bool retune = false) {
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
            if (!retune && (n.confidence == "strong" || (n.confidence == "channel" && !n.ids.empty())) &&
                !covered_locked(F, fam, kv.first, n))
                continue;                        // identified: kept
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

    mutable std::mutex mu_;
    std::map<std::string, Family> fam_;
    std::map<std::uint64_t, Ctx> sess_;
    std::uint64_t version_ = 0;
    std::uint64_t next_call_ = 0;
    AssocRecorder rec_;
    // identity / provenance (see assoc_merge.hpp) and imported layers
    std::string instance_ = random_instance(), name_ = default_name();
    std::int64_t since_ = now_ms();
    std::vector<Dataset> imports_;
    std::uint64_t next_import_ = 0;
    // Imported calls' audio (import_audio): per import, the files its calls
    // name ("call_....wav" -> the calls) and the ones uploaded (served name -> bytes).
    struct AudioWant { std::string fam; std::uint64_t call = 0, ms = 0; };
    struct ImportAudio {
        std::map<std::string, std::vector<AudioWant>> want;
        std::map<std::string, std::uint64_t> files;
    };
    std::map<std::uint64_t, ImportAudio> import_audio_;
    std::string import_audio_root_;                    // "" = imported audio not kept
    std::uint64_t import_audio_cap_ = 0;
    NetMerges merges_;                                 // network merges (shared; kept through Clear)
    std::string merges_file_;                          // where they are kept ("" = memory only)
    KeyRing keys_;                                     // per-network decryption keyring (values never leave in /net.json/exports)
    std::map<std::string, std::map<std::string, int>> bp_;  // DMR Basic Privacy key numbers: fam -> net -> 1..255 (not secret)
    std::string keys_file_;                            // where it is kept ("" = memory only)
    bool per_receiver_ = default_per_receiver();
    std::size_t max_calls_ = max_calls_setting();
    CallAudioStore audio_;
    std::atomic<bool> audio_on_{false};
    // Voice-quality (intelligibility) analysis, independent of recording:
    // runs whenever voice is decoded unless DSD_NET_QUALITY=0. The active
    // per-call analyzers live here, keyed by call id; a call's verdict is
    // finalized onto its Call record (Call::qual) when it closes.
    std::atomic<bool> quality_on_{quality_default()};
    std::map<std::uint64_t, VoiceQuality> call_quality_;

    static bool quality_default() {
        const char* v = std::getenv("DSD_NET_QUALITY");
        return !(v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F'));
    }
};

} // namespace dsdsrv
