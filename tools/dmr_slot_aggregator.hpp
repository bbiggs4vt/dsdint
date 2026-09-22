// dmr_slot_aggregator.hpp
//
// Client-side per-slot call aggregation for DMR (the only 2-slot TDMA
// protocol on this server's FM/DSD chain). Feed it the flat event fields
// you already parse out of each {"type":"event", ...} JSON frame, plus the
// time the frame arrived; it groups them into per-slot call sessions and
// fires start/update/end callbacks. No networking, no JSON, std-only C++17.
//
// GATING: only use this for DMR. Wire events carry no protocol field, so
// the client must decide from the protocol it requested in `start`; use
// is_timeslotted_protocol() below.

#pragma once

#include <string>
#include <vector>
#include <array>
#include <map>
#include <optional>
#include <functional>
#include <chrono>
#include <cctype>
#include <cstdint>

namespace dsdclient {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// The flat wire fields the aggregator uses (a subset of the event frame).
struct Event {
    std::string kind;        // voice | sync | call | message | burst | unknown
    std::string talkgroup;
    std::string source_id;
    std::string slot;        // "1" | "2" | ""
    std::string color_code;
    std::string emergency;   // "1" | ""
    std::string alias;
    std::string crc_error;   // "1" | ""
    std::string message;     // SMS / short-data
    std::string extra;       // "; "-joined key=value tokens
    std::string raw;
};

// Only DMR is 2-slot TDMA here. Mirrors the server's forgiving hint
// matching (case / separators ignored) for the one protocol that has slots.
inline bool is_timeslotted_protocol(std::string p) {
    std::string k;
    for (char c : p) {
        if (c == ' ' || c == '_' || c == '-') continue;
        k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return k == "dmr";
}

// One aggregated call session on one slot.
struct Call {
    int slot = 0;                              // 1 or 2
    std::string talkgroup;                     // group TG, "" if none/private
    std::string source;                        // source radio id
    std::string unit_target;                   // set => private (unit-to-unit)
    bool group = true;                         // group vs private
    std::string color_code;
    std::string alias;                         // talker alias (arrives late)
    bool emergency = false;
    TimePoint start;
    TimePoint last_activity;
    std::uint32_t events = 0;                  // events attributed to this call
    std::uint32_t voice_events = 0;            // 'voice'-kind events (DSDcc)
    std::uint32_t crc_errors = 0;              // FEC/CRC-flagged events
    std::vector<std::string> messages;         // decoded SMS / short-data
    std::map<std::string, std::string> extra;  // merged key=value tokens

    double duration_s() const {
        return std::chrono::duration<double>(last_activity - start).count();
    }
};

class DmrSlotAggregator {
public:
    // hang: close a call this long after its last activity when no explicit
    // terminator was seen. DMR voice bursts land ~every 60 ms per slot, and
    // terminators are often lost on marginal signal, so ~1 s is safe.
    explicit DmrSlotAggregator(std::chrono::milliseconds hang = std::chrono::milliseconds(1000))
        : hang_(hang) {}

    std::function<void(const Call&)> on_call_start;
    std::function<void(const Call&)> on_call_update;
    std::function<void(const Call&)> on_call_end;

    // Feed one event with its arrival time.
    void on_event(const Event& ev, TimePoint now = Clock::now()) {
        tick(now);                             // opportunistic timeout sweep

        int slot = ev.slot == "1" ? 1 : (ev.slot == "2" ? 2 : 0);
        if (slot) { last_slot_ = slot; apply(slot, ev, now); return; }

        // slot == "": sync + trunking/control are channel-wide, not a slot.
        if (is_channel_wide(ev)) return;

        // Metadata without a slot (alias / SMS / emergency lines): attribute
        // by matching the payload's source to an active call; else fall back
        // to the last slot that had activity, but only if a call is open.
        int target = 0;
        if (!ev.source_id.empty()) {
            for (int s : {1, 2})
                if (call_(s) && call_(s)->source == ev.source_id) target = s;
        }
        if (!target && last_slot_ && call_(last_slot_)) target = last_slot_;
        if (target) apply(target, ev, now);
    }

    // Call periodically (e.g. every 250 ms) so calls end on hang-time even
    // when no terminator or further events arrive.
    void tick(TimePoint now = Clock::now()) {
        for (int s : {1, 2})
            if (call_(s) && now - call_(s)->last_activity > hang_) close(s);
    }

    const std::optional<Call>& active(int slot) const { return slots_[idx(slot)].call; }
    const std::vector<Call>& history() const { return history_; }

private:
    struct SlotState { std::optional<Call> call; };
    std::array<SlotState, 2> slots_;
    std::vector<Call> history_;
    int last_slot_ = 0;
    std::chrono::milliseconds hang_;

    static int idx(int slot) { return slot == 2 ? 1 : 0; }
    std::optional<Call>& call_(int slot) { return slots_[idx(slot)].call; }

    static std::map<std::string, std::string> parse_extra(const std::string& extra) {
        std::map<std::string, std::string> out;
        std::size_t i = 0;
        while (i <= extra.size()) {
            std::size_t semi = extra.find(';', i);
            std::string tok = extra.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
            std::size_t a = tok.find_first_not_of(" \t");
            std::size_t b = tok.find_last_not_of(" \t");
            if (a != std::string::npos) {
                tok = tok.substr(a, b - a + 1);
                std::size_t eq = tok.find('=');
                if (eq != std::string::npos) out[tok.substr(0, eq)] = tok.substr(eq + 1);
            }
            if (semi == std::string::npos) break;
            i = semi + 1;
        }
        return out;
    }

    static bool is_channel_wide(const Event& ev) {
        if (ev.kind == "sync") return true;
        static const char* ctrl[] = {"network_type", "network_id", "site_id",
                                     "rest_channel", "lcn", "rfss"};
        for (const char* k : ctrl)
            if (ev.extra.find(k) != std::string::npos) return true;
        return false;
    }

    void apply(int slot, const Event& ev, TimePoint now) {
        auto ex = parse_extra(ev.extra);

        // Identity event: a call/voice frame that names a party. A change of
        // talkgroup or source on this slot rolls over to a new call.
        const bool identity = (ev.kind == "call" || ev.kind == "voice") &&
                              (!ev.talkgroup.empty() || !ev.source_id.empty() || ex.count("unit_target"));
        if (identity && call_(slot)) {
            const bool changed = (!ev.talkgroup.empty() && ev.talkgroup != call_(slot)->talkgroup) ||
                                 (!ev.source_id.empty() && ev.source_id != call_(slot)->source);
            if (changed) close(slot);
        }
        if (identity && !call_(slot)) {
            Call c; c.slot = slot; c.start = now; c.last_activity = now;
            call_(slot) = c;
        }
        if (!call_(slot)) return;              // stray burst with no open call

        Call& c = *call_(slot);
        const bool is_new = (c.events == 0);
        c.last_activity = now;
        c.events++;
        if (ev.kind == "voice") c.voice_events++;
        if (ev.crc_error == "1") c.crc_errors++;
        if (!ev.talkgroup.empty()) c.talkgroup = ev.talkgroup;
        if (!ev.source_id.empty()) c.source = ev.source_id;
        if (!ev.color_code.empty()) c.color_code = ev.color_code;
        if (!ev.alias.empty()) c.alias = ev.alias;
        if (ev.emergency == "1") c.emergency = true;
        if (!ev.message.empty()) c.messages.push_back(ev.message);
        for (auto& kv : ex) c.extra[kv.first] = kv.second;
        if (auto it = ex.find("unit_target"); it != ex.end()) { c.unit_target = it->second; c.group = false; }

        if (is_new) { if (on_call_start) on_call_start(c); }
        else        { if (on_call_update) on_call_update(c); }

        const bool terminated = (ex.count("burst") && ex["burst"] == "TLC") ||
                                ev.raw.find("Terminator") != std::string::npos;
        if (terminated) close(slot);
    }

    void close(int slot) {
        auto& st = slots_[idx(slot)];
        if (!st.call) return;
        Call done = *st.call;
        st.call.reset();
        history_.push_back(done);
        if (on_call_end) on_call_end(history_.back());
    }
};

} // namespace dsdclient
