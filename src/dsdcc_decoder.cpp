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
    std::string burst; // "VOX", "IDL", ...
};

SlotInfo parse_slot_text(const char* text) {
    SlotInfo info;
    info.burst.assign(text + 4, 3);
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
    // mode selected on its command line; so do we.
    if (cfg_.mode == "dmr") {
        decoder_->setDecodeMode(DSDcc::DSDDecoder::DSDDecodeDMR, true);
    } else {
        decoder_->setDecodeMode(DSDcc::DSDDecoder::DSDDecodeAuto, true);
    }

    // mbelib-based voice synthesis is enabled by default in DSDDecoder's
    // constructor (m_mbelibEnable(true)); audio comes out at the MBE
    // decoder's native 8 kHz. setUpsampling(0) makes the "no upsampling"
    // choice explicit rather than relying on the constructor default.
    decoder_->setUpsampling(0);

    last_slot_text_[0].clear();
    last_slot_text_[1].clear();
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

    const bool sync = sync_present(decoder_->getSyncType());

    // Sync acquisition/loss is an event of its own (mirrors the
    // subprocess backend, where classify_line tags dsd-fme's "Sync:"
    // lines as kind "sync").
    if (sync != last_sync_) {
        last_sync_ = sync;
        DsdEvent ev;
        ev.kind = "sync";
        ev.raw_line = sync ? "(dsdcc: sync acquired)" : "(dsdcc: sync lost)";
        on_event_(ev);
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
        if (!info.has_addresses) continue; // slot text changed but carries no call info yet

        const bool voice = (s == 0) ? decoder_->getVoice1On() : decoder_->getVoice2On();

        DsdEvent ev;
        ev.kind = voice ? "voice" : "call";
        ev.talkgroup = info.group ? info.target : "";
        ev.source_id = info.source;
        ev.slot = (s == 0) ? "1" : "2";
        // For unit-to-unit calls the target isn't a talkgroup; surface
        // it in extra instead so the talkgroup field stays honest.
        if (!info.group && !info.target.empty()) {
            ev.extra = "unit_target=" + info.target;
        }
        ev.raw_line = "(dsdcc slot" + ev.slot + ") " + text;
        on_event_(ev);
    }
}

} // namespace dsdsrv
