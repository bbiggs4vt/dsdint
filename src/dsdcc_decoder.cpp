#include "dsdcc_decoder.hpp"
#include <cstdio>

// ---------------------------------------------------------------------
// API CONFIDENCE NOTES -- read this before debugging a build failure
//
// I could not install/link DSDcc in the environment this was written in
// (no network access), and unlike the liquid-dsp variant, I didn't find
// a single confirmed real-world usage example to check the main ingestion
// call against -- only fragments of dsd_decoder.h, dmr.h, and the
// project README. Confidence, from highest to lowest:
//
// HIGH CONFIDENCE (seen directly in header fragments via search):
//   - DSDcc::DSDDecoder is the top-level class (dsd_decoder.h)
//   - DSDDecoder::DSDDecodeMode enum includes DSDDecodeDMR, DSDDecodeAuto
//   - Decoded audio comes from a companion DSDMBEDecoder object via
//     getAudio(int& nbSamples) -> short*, and resetAudio() to clear it
//   - The library is push-based: you feed it samples, you don't hand it
//     a file descriptor (this is stated directly in the project README)
//
// MEDIUM CONFIDENCE (inferred from a setDecodeMode() mention in release
// notes, and DSDDecoder owning an internal m_dsdSymbol member visible in
// a dsd_decoder.cpp fragment):
//   - There's a DSDDecoder::setDecodeMode(DSDDecodeMode) method
//   - DSDDecoder wraps something like a DSDSymbol object per input sample
//
// LOW CONFIDENCE -- VERIFY THIS FIRST IF THE BUILD FAILS:
//   - The exact method name/signature for pushing one discriminator
//     sample into the decoder. I've written it below as
//     `decoder_->run(sample)` based on the general shape described in
//     DSDcc's README (loop over your own samples, call into the decoder
//     per sample, then check for available output) and a fragment
//     referencing `DSDDecoder::run` in dsd_decoder.cpp, but I have NOT
//     seen this method's actual declaration/signature. It's plausible
//     it takes additional arguments, returns a status code, or is named
//     differently in your installed version.
//   - The metadata getters used in check_for_state_change() below
//     (decoder_->getDMRSlot0Text() etc.) are NOT based on anything I
//     found -- they're placeholders showing the shape of what's needed
//     (read current slot/talkgroup/source/sync state), written so the
//     surrounding event-detection logic has something concrete to call.
//     You will need to replace these with whatever dmr.h / dsd_decoder.h
//     actually expose on your installed version -- open those headers
//     and look for getters on DSDDecoder and DSDDMR before touching
//     anything else in this file.
//
// The overall control flow (push samples -> check for decoded audio ->
// check for state changes -> invoke callbacks) is the part I'd stand
// behind regardless of exact names; it matches DSDcc's own documented
// integration pattern. The specific function names below are a starting
// point to correct against your real headers, not a verified API.
// ---------------------------------------------------------------------

#include <dsdcc/dsd_decoder.h>
#include <dsdcc/dmr.h>
// The "dsdcc/" include prefix above is also a guess -- I don't know
// whether your install exposes these as <dsdcc/dsd_decoder.h> or just
// <dsd_decoder.h>. If the compiler can't find these headers before you
// even get to the method-name issues above, try dropping the prefix
// first and adjust CMakeLists.txt's include path to match wherever
// `make install` actually put them.

namespace dsdsrv {

DsdccDecoder::DsdccDecoder() = default;
DsdccDecoder::~DsdccDecoder() { stop(); }

bool DsdccDecoder::start(const DsdccConfig& cfg, EventCallback on_event, AudioCallback on_audio) {
    // Loud, impossible-to-miss warning: as shipped, the audio and event
    // extraction below are placeholders (see the top-of-file notes) that
    // compile but produce no output -- write_audio() will accept samples
    // and return true without ever invoking on_audio_ or on_event_ until
    // you fill in the real DSDcc calls. This print exists specifically
    // so that doesn't fail silently.
    std::fprintf(stderr,
        "dsd-server: WARNING -- DsdccDecoder is running with placeholder "
        "audio/metadata extraction (see top of dsdcc_decoder.cpp). It will "
        "accept audio but will not produce decoded output until the real "
        "DSDcc API calls are filled in.\n");

    cfg_ = cfg;
    on_event_ = std::move(on_event);
    on_audio_ = std::move(on_audio);

    decoder_ = std::make_unique<DSDcc::DSDDecoder>();

    // See the LOW CONFIDENCE note above for setDecodeMode's exact name;
    // MEDIUM confidence it exists in roughly this shape.
    DSDcc::DSDDecoder::DSDDecodeMode mode = DSDcc::DSDDecoder::DSDDecodeDMR;
    if (cfg_.mode != "dmr") {
        // Only DMR is wired up here since that's this project's target;
        // extend this mapping (DSDDecodeP25P1, DSDDecodeNXDN48, etc. all
        // exist per DSDDecodeMode) if you need other protocols.
        mode = DSDcc::DSDDecoder::DSDDecodeAuto;
    }
    decoder_->setDecodeMode(mode, false /* not sure this second arg is
                                            correct -- some DSDcc setters
                                            take a bool for "auto-fallback"
                                            or similar; verify signature */);

    last_talkgroup_.clear();
    last_source_id_.clear();
    last_slot_.clear();
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
        // LOW CONFIDENCE call -- see the top-of-file notes. This is the
        // one line most likely to need correcting against your actual
        // dsd_decoder.h.
        decoder_->run(pcm[i]);

        // Pull any decoded audio produced by that sample. DSDcc's audio
        // comes from a companion DSDMBEDecoder rather than DSDDecoder
        // itself per the header fragments I found, so the real call is
        // likely something like decoder_->getMBEDecoder().getAudio(n)
        // -- adjust based on how DSDDecoder exposes its DSDMBEDecoder
        // member (if it's public, a getter, or something else).
        int nb_samples = 0;
        const int16_t* audio = nullptr; // placeholder for decoder_->getMBEDecoder().getAudio(nb_samples)
        if (audio && nb_samples > 0 && on_audio_) {
            on_audio_(audio, static_cast<std::size_t>(nb_samples));
            // decoder_->getMBEDecoder().resetAudio(); // clear after consuming, per the header fragment seen
        }
    }

    check_for_state_change();
    return true;
}

void DsdccDecoder::check_for_state_change() {
    if (!decoder_ || !on_event_) return;

    // PLACEHOLDER getters -- replace with whatever dmr.h / dsd_decoder.h
    // actually expose. The intent: read current slot/talkgroup/source/
    // sync state, compare against last_*_, and only fire an event when
    // something actually changed (mirroring why the subprocess backends'
    // classify_line() only matters when a new log line arrives).
    std::string talkgroup;   // = decoder_->getDMR().getTalkgroup() or similar
    std::string source_id;   // = decoder_->getDMR().getSourceId() or similar
    std::string slot;        // = decoder_->getDMR().getSlot() or similar
    bool sync = false;       // = decoder_->getSync() or similar

    if (talkgroup == last_talkgroup_ && source_id == last_source_id_ &&
        slot == last_slot_ && sync == last_sync_) {
        return; // nothing changed, don't spam an event per sample
    }

    last_talkgroup_ = talkgroup;
    last_source_id_ = source_id;
    last_slot_ = slot;
    last_sync_ = sync;

    DsdEvent ev;
    ev.kind = sync ? "sync" : "unknown";
    ev.talkgroup = talkgroup;
    ev.source_id = source_id;
    ev.slot = slot;
    ev.raw_line = "(dsdcc: synthesized from decoder state, not a log line)";
    on_event_(ev);
}

} // namespace dsdsrv
