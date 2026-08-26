#include "dsdcc_decoder.hpp"
#include <cstdio>
#include <cctype>

// ---------------------------------------------------------------------
// API VERIFICATION NOTES
//
// This file's first revision was written without access to DSDcc (no
// network in that environment) and carried loud LOW CONFIDENCE warnings
// with placeholder calls. It has since been rewritten against DSDcc
// 1.9.0 (commit f27b32d), verified three ways:
//
//   1. Against the installed headers (dsd_decoder.h, dmr.h) — every
//      call below exists with the signature used here.
//   2. Against dsd_main.cpp, dsdccx's own integration loop, which is
//      the upstream-canonical usage: run(sample) per input sample, then
//      poll getAudio1/getAudio2(int&) and resetAudio1/resetAudio2()
//      after consuming.
//   3. End to end against DSDcc's bundled real DMR capture
//      (samples/dmr_it_8.dis, S16LE 48 kHz discriminator audio): this
//      wrapper produces the same decoded audio volume and the same
//      talkgroup/source metadata as dsdccx run on the same file
//      (TG 150607, sources 2222223/2220175, group call, slot #2).
//
// Scorecard vs the original blind guesses, for the curious:
//   - run(short sample) per input sample: guessed exactly right.
//   - Audio via a companion MBE decoder with getAudio/resetAudio:
//     right idea, wrong access path — DSDDecoder exposes it directly as
//     getAudio1/2()/resetAudio1/2(), one pair per TDMA slot.
//   - setDecodeMode(mode, bool): exists, but the guessed second arg of
//     `false` was backwards — the bool is on/off for that mode, so the
//     placeholder would have DISABLED DMR decoding. This is why "looks
//     plausible" isn't "verified".
//   - Metadata getters: the guessed getDMRSlot0Text() didn't exist, but
//     getDMRDecoder().getSlot0Text()/getSlot1Text() do, returning
//     DSDcc's fixed-layout 26-char per-slot status text (see
//     parse_slot_text below for the layout, confirmed against
//     DSDDMR::textVoiceEmbeddedSignalling in dmr.cpp).
// ---------------------------------------------------------------------

#include <dsdcc/dsd_decoder.h>
#include <dsdcc/dmr.h>

namespace dsdsrv {

namespace {

// DSDcc's per-slot status text is a fixed 26-char layout, filled in by
// dmr.cpp (see DSDDMR::processSlotTypePDU and textVoiceEmbeddedSignalling):
//
//   [0]      activity indicator: '*' active / '.' idle (from CACH)
//   [1..2]   color code, 2 digits
//   [4..6]   burst type, 3 chars: "VOX", "IDL", "VLC", "TLC", "CSB", ...
//   [8..15]  source address, 8 digits zero-padded
//   [16]     '>'
//   [17]     'G' (group call) or 'U' (unit-to-unit)
//   [18..25] target address (talkgroup for group calls), 8 digits
//
// e.g. "*04 VOX 02222223>G00150607" = active, CC 4, voice burst,
// source 2222223 calling talkgroup 150607.
//
// Fields are only populated once the corresponding PDUs have decoded;
// until then they're the spaces the buffer was initialized with, so
// every extraction below tolerates blanks.
std::string strip_leading_zeros(const std::string& s) {
    std::size_t i = 0;
    while (i + 1 < s.size() && s[i] == '0') ++i;
    return s.substr(i);
}

std::string digits_at(const char* text, std::size_t off, std::size_t len) {
    std::string out;
    for (std::size_t i = 0; i < len; ++i) {
        char c = text[off + i];
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::string();
        out.push_back(c);
    }
    return strip_leading_zeros(out);
}

struct SlotInfo {
    std::string source;
    std::string target;
    bool group = false;
    bool has_addresses = false;
    std::string burst;      // "VOX", "IDL", ...
    std::string color_code; // decimal, no leading zeros ("04" in the text -> "4")
};

SlotInfo parse_slot_text(const char* text) {
    SlotInfo info;
    info.burst.assign(text + 4, 3);
    info.color_code = digits_at(text, 1, 2);
    if (text[16] == '>') {
        info.source = digits_at(text, 8, 8);
        info.target = digits_at(text, 18, 8);
        info.group = (text[17] == 'G');
        info.has_addresses = !info.source.empty() || !info.target.empty();
    }
    return info;
}

bool sync_present(DSDcc::DSDDecoder::DSDSyncType t) {
    return t != DSDcc::DSDDecoder::DSDSyncNone;
}

// The DMR sync patterns DSDcc distinguishes: BS (base station /
// repeater, "P" in the enum names for positive polarity) vs MS (mobile
// station -- also what direct/simplex mode uses), and data vs voice
// burst. This is the DSDcc equivalent of the "+DMR MS/DM" flavor in
// dsd-fme's per-burst sync lines.
const char* sync_type_name(DSDcc::DSDDecoder::DSDSyncType t) {
    switch (t) {
    case DSDcc::DSDDecoder::DSDSyncDMRDataP:  return "dmr_bs_data";
    case DSDcc::DSDDecoder::DSDSyncDMRDataMS: return "dmr_ms_data";
    case DSDcc::DSDDecoder::DSDSyncDMRVoiceP: return "dmr_bs_voice";
    case DSDcc::DSDDecoder::DSDSyncDMRVoiceMS:return "dmr_ms_voice";
    case DSDcc::DSDDecoder::DSDSyncNXDNP:      return "nxdn";
    case DSDcc::DSDDecoder::DSDSyncNXDNN:      return "nxdn";
    case DSDcc::DSDDecoder::DSDSyncNone:      return "none";
    default:                                  return "other";
    }
}

// Is the current sync an NXDN frame sync? (Used in "auto" mode to pick
// the NXDN metadata path over the DMR slot-text path.)
bool is_nxdn_sync(DSDcc::DSDDecoder::DSDSyncType t) {
    return t == DSDcc::DSDDecoder::DSDSyncNXDNP
        || t == DSDcc::DSDDecoder::DSDSyncNXDNN;
}

} // namespace

DsdccDecoder::DsdccDecoder() = default;
DsdccDecoder::~DsdccDecoder() { stop(); }

bool DsdccDecoder::start(const DsdccConfig& cfg, EventCallback on_event, AudioCallback on_audio) {
    cfg_ = cfg;
    on_event_ = std::move(on_event);
    on_audio_ = std::move(on_audio);

    // DSDcc's input rate is fixed at 48 kHz (README; its symbol timing
    // assumes 10 samples/symbol at DMR's 4800 baud). Feeding any other
    // rate wouldn't error — it would just never sync — so fail loudly
    // here instead.
    if (cfg_.input_sample_rate_hz != 48000) {
        std::fprintf(stderr,
            "dsd-server: DsdccDecoder requires 48000 Hz input (DSDcc's fixed "
            "rate), got %d\n", cfg_.input_sample_rate_hz);
        return false;
    }

    decoder_ = std::make_unique<DSDcc::DSDDecoder>();

    // Suppress DSDcc's default per-frame stderr output (frame info and
    // errorbars) — this runs inside a server, not a terminal UI.
    decoder_->setQuiet();

    // The second argument is on/off for the given mode (verified in
    // dsd_decoder.cpp: DMR case sets m_opts.frame_dmr = on and, when
    // enabling, the 4800 baud data rate). dsdccx passes true for the
    // mode selected on its command line; so do we. The mode string comes
    // from the client's protocol hint (see session.cpp). NOTE: DSDcc's
    // NXDN symbol recovery is fragile on real off-air signals — it locks
    // on clean/synthetic signals but drops sync on real captures where
    // the dsd-fme backend decodes cleanly; prefer that backend for NXDN.
    if (cfg_.mode == "dmr") {
        decoder_->setDecodeMode(DSDcc::DSDDecoder::DSDDecodeDMR, true);
    } else if (cfg_.mode == "nxdn48") {
        decoder_->setDecodeMode(DSDcc::DSDDecoder::DSDDecodeNXDN48, true);
    } else if (cfg_.mode == "nxdn96") {
        decoder_->setDecodeMode(DSDcc::DSDDecoder::DSDDecodeNXDN96, true);
    } else { // "auto" or anything unrecognized
        decoder_->setDecodeMode(DSDcc::DSDDecoder::DSDDecodeAuto, true);
    }

    // mbelib-based voice synthesis is enabled by default in DSDDecoder's
    // constructor (m_mbelibEnable(true)); audio comes out at the MBE
    // decoder's native 8 kHz. setUpsampling(0) makes the "no upsampling"
    // choice explicit rather than relying on the constructor default.
    decoder_->setUpsampling(0);

    last_slot_text_[0].clear();
    last_slot_text_[1].clear();
    last_nxdn_sig_.clear();
    last_sync_ = false;

    running_ = true;
    return true;
}

void DsdccDecoder::stop() {
    if (!running_.exchange(false)) return;
    decoder_.reset();
}

bool DsdccDecoder::write_audio(const int16_t* pcm, std::size_t n) {
    if (!decoder_) return false;

    for (std::size_t i = 0; i < n; ++i) {
        decoder_->run(pcm[i]);

        // Poll for decoded voice after every sample, exactly as
        // dsdccx's main loop does. getAudio1/2 return the MBE decoder's
        // accumulated 8 kHz PCM for TDMA slot #1/#2 respectively (a
        // 20 ms voice frame lands ~160 samples at a time); resetAudio
        // clears the buffer once consumed. Both slots are forwarded on
        // the single audio callback — concurrent voice on both slots of
        // one channel is possible in DMR, but the wire protocol has one
        // audio stream, so they interleave (dsdccx mixes them instead;
        // if that matters for your client, mix here the same way).
        int nb1 = 0;
        short* audio1 = decoder_->getAudio1(nb1);
        if (nb1 > 0) {
            if (on_audio_) on_audio_(audio1, static_cast<std::size_t>(nb1));
            decoder_->resetAudio1();
        }

        int nb2 = 0;
        short* audio2 = decoder_->getAudio2(nb2);
        if (nb2 > 0) {
            if (on_audio_) on_audio_(audio2, static_cast<std::size_t>(nb2));
            decoder_->resetAudio2();
        }
    }

    check_for_state_change();
    return true;
}

void DsdccDecoder::check_for_state_change() {
    if (!decoder_ || !on_event_) return;

    const auto sync_type = decoder_->getSyncType();
    const bool sync = sync_present(sync_type);

    // Sync acquisition/loss is an event of its own (mirrors the
    // subprocess backend, where classify_line tags dsd-fme's "Sync:"
    // lines as kind "sync"). On acquisition, the sync flavor (BS vs
    // MS/direct mode, data vs voice) rides along in extra — that's the
    // part of dsd-fme's "+DMR MS/DM" detail DSDcc also knows. Only the
    // acquiring burst's flavor is reported: within a held sync the type
    // legitimately alternates per 30 ms burst (voice on one slot, data
    // on the other), and one event per burst would be a storm.
    if (sync != last_sync_) {
        last_sync_ = sync;
        DsdEvent ev;
        ev.kind = "sync";
        if (sync) {
            ev.extra = std::string("sync_type=") + sync_type_name(sync_type);
            ev.raw_line = std::string("(dsdcc: sync acquired, ")
                          + sync_type_name(sync_type) + ")";
        } else {
            ev.raw_line = "(dsdcc: sync lost)";
        }
        on_event_(ev);
    }

    // NXDN vs DMR metadata path. The NXDN decoder and the DMR slot-text
    // decoder are separate in DSDcc; feeding the DMR path an NXDN signal
    // would read stale slot text, so dispatch on the configured mode (or,
    // in "auto", on the current sync).
    const bool nxdn = (cfg_.mode == "nxdn48" || cfg_.mode == "nxdn96")
                      || (cfg_.mode == "auto" && is_nxdn_sync(sync_type));
    if (nxdn) {
        check_for_nxdn_state_change();
        return;
    }

    // Per-TDMA-slot state, from DSDcc's fixed-layout status text (see
    // parse_slot_text above). Index 0 = slot #1, 1 = slot #2.
    const char* slot_text[2] = {
        decoder_->getDMRDecoder().getSlot0Text(),
        decoder_->getDMRDecoder().getSlot1Text(),
    };

    for (int s = 0; s < 2; ++s) {
        std::string text(slot_text[s], 26);
        if (text == last_slot_text_[s]) continue; // no change, no event
        last_slot_text_[s] = text;

        SlotInfo info = parse_slot_text(text.c_str());

        DsdEvent ev;
        ev.slot = (s == 0) ? "1" : "2";
        ev.color_code = info.color_code;
        ev.raw_line = "(dsdcc slot" + ev.slot + ") " + text;

        if (info.has_addresses) {
            const bool voice = (s == 0) ? decoder_->getVoice1On() : decoder_->getVoice2On();
            ev.kind = voice ? "voice" : "call";
            ev.talkgroup = info.group ? info.target : "";
            ev.source_id = info.source;
            // For unit-to-unit calls the target isn't a talkgroup;
            // surface it in extra instead so the talkgroup field stays
            // honest.
            if (!info.group && !info.target.empty()) {
                ev.extra = "unit_target=" + info.target;
            }
        } else {
            // No call addresses (DSDcc only learns those from voice
            // embedded signalling) — but the slot-type PDU still told
            // us the burst type and color code, and for control-only
            // traffic (e.g. CSBK bursts) that's ALL the visibility
            // DSDcc has. Surface it instead of dropping it; dsd-fme
            // shows the same information on its per-burst sync lines.
            std::string burst = info.burst;
            while (!burst.empty() && burst.back() == ' ') burst.pop_back();
            while (!burst.empty() && burst.front() == ' ') burst.erase(0, 1);
            if (burst.empty() && info.color_code.empty())
                continue; // text changed but still carries nothing to report
            ev.kind = "burst";
            if (!burst.empty()) ev.extra = "burst=" + burst;
            // "UNK" is DSDcc's marker for a slot-type PDU whose
            // Golay(20,8) FEC failed (dmr.cpp writes "-- UNK") -- the
            // DSDcc-side equivalent of dsd-fme's CRC/FEC ERR flags.
            if (burst == "UNK") ev.crc_error = "1";
        }
        on_event_(ev);
    }
}

void DsdccDecoder::check_for_nxdn_state_change() {
    // Mirror the dsd-fme backend's NXDN field conventions using DSDcc's
    // NXDN accessors, so a client sees the same structured shape from
    // either backend on NXDN. (DSDcc decodes fewer NXDN messages than
    // dsd-fme and its symbol recovery is fragile on real signals -- see
    // PROTOCOL.md -- but when it does decode, the fields line up.)
    const DSDcc::DSDNXDN& n = decoder_->getNXDNDecoder();
    const int ran = n.getRAN();
    const unsigned src = n.getSourceId();
    const unsigned dst = n.getDestinationId();
    const bool group = n.isGroupCall();
    const unsigned loc = n.getLocationId();

    // Collapse the reportable state into one signature; only emit on a
    // change, exactly like the DMR slot-text path, so we don't fire an
    // event per processed block.
    char sig[96];
    std::snprintf(sig, sizeof(sig), "%d/%u/%u/%d/%u", ran, src, dst, group ? 1 : 0, loc);
    if (sig == last_nxdn_sig_) return;
    last_nxdn_sig_ = sig;

    // Nothing worth reporting yet (RAN alone, before any call/site info,
    // is not an event -- it rides on the events below).
    if (src == 0 && loc == 0) return;

    DsdEvent ev;
    ev.kind = "call";
    ev.ran = (ran != 0) ? std::to_string(ran) : "";

    std::vector<std::string> extra;
    if (src != 0) {
        ev.source_id = std::to_string(src);
        if (dst != 0) {
            if (group) ev.talkgroup = std::to_string(dst);
            else       extra.push_back("unit_target=" + std::to_string(dst));
        }
    }
    if (loc != 0) {
        // NXDN 24-bit location ID splits as system code (high 12 bits) /
        // site code (low 12 bits) -- verified against dsd-fme, which
        // prints the same split (e.g. 0x008002 -> "Sys Code: 8 - Site
        // Code 2"). Emit the decomposition plus the raw id, matching the
        // dsd-fme backend's tokens.
        char loch[8];
        std::snprintf(loch, sizeof(loch), "%06X", loc & 0xFFFFFFu);
        extra.push_back("system_code=" + std::to_string((loc >> 12) & 0xFFFu));
        extra.push_back("site_code=" + std::to_string(loc & 0xFFFu));
        extra.push_back(std::string("location_id=") + loch);
    }
    for (std::size_t i = 0; i < extra.size(); ++i) {
        if (i) ev.extra += "; ";
        ev.extra += extra[i];
    }

    char raw[128];
    std::snprintf(raw, sizeof(raw), "(dsdcc nxdn) RAN %d src %u dst %u %s",
                  ran, src, dst, group ? "group" : "unit");
    ev.raw_line = raw;
    on_event_(ev);
}

} // namespace dsdsrv
