// dsdcc_decoder.hpp
//
// In-process alternative to DsdProcess (dsd_process.hpp), built on DSDcc
// (https://github.com/f4exb/dsdcc) instead of spawning a dsd-fme/DSD
// subprocess. This is a structurally different integration than the
// subprocess backends: DSDcc is a C++ library designed to have samples
// pushed into it directly, with decoded audio pulled back out via a
// getter — there's no child process, no pipes, no UDP audio port.
//
// STATUS: verified against DSDcc 1.9.0 (commit f27b32d) — the API calls
// in dsdcc_decoder.cpp were checked against the real dsd_decoder.h/dmr.h
// headers and against dsd_main.cpp (dsdccx's own integration loop), and
// the whole path was exercised end to end with DSDcc's bundled real DMR
// capture (samples/dmr_it_8.dis): decoded audio and correct
// talkgroup/source metadata come out. This file's earlier revision was
// written blind (no network) with placeholder API calls; that caveat no
// longer applies. See dsdcc_decoder.cpp's top comment for what was
// verified and how.
//
// Input contract: DSDcc takes discriminator audio as S16LE at a FIXED
// 48000 Hz (stated in its README and assumed throughout its symbol
// timing: 10 samples/symbol at 4800 baud). start() rejects any other
// input_sample_rate_hz rather than silently producing garbage. This
// matches what Session feeds it — FmDemodulator outputs 48 kHz PCM.
//
// Output: decoded voice PCM at 8000 Hz mono (DSDcc's DSDMBEDecoder
// native rate; upsampling deliberately off). Note this differs from
// nothing in the wire protocol — the audio frame tag doesn't carry a
// rate — but clients that assumed dsd-fme's rate get the same 8 kHz
// here, since dsd-fme's UDP output is typically 8 kHz too (see
// session.hpp's protocol notes).
//
// Threading: since this is in-process (no subprocess, no separate
// reader threads), start()/write_audio()/stop() all run synchronously
// on whatever thread calls them — for this project, that's Session's
// demod worker thread. Event/audio callbacks therefore also fire on
// that thread, not a dedicated reader thread like DsdProcess's.
// session.cpp already handles this correctly without any changes: its
// callbacks always post onto the WebSocket's strand executor before
// touching ws_, regardless of which thread invoked them (see
// start_pipeline() in session.cpp).

#pragma once

#include "dsd_backend_types.hpp"

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <memory>
#include <atomic>

// Forward-declare DSDcc's decoder type so this header doesn't force
// every includer to have DSDcc's headers on their include path; the
// real #include lives in dsdcc_decoder.cpp.
namespace DSDcc { class DSDDecoder; }

namespace dsdsrv {

struct DsdccConfig {
    // "dmr" maps to DSDDecodeDMR; anything else falls back to
    // DSDDecodeAuto (DSDcc's own auto frame-type detection). Kept as a
    // string for symmetry with DsdProcessConfig::mode_flag and to keep
    // this header decoupled from DSDcc's enum.
    std::string mode = "dmr";

    // Sample rate of the discriminator PCM you'll be pushing via
    // write_audio(). DSDcc's input rate is FIXED at 48000 Hz (see the
    // top-of-file comment); start() fails on any other value. The field
    // exists so the caller states its assumption explicitly instead of
    // the mismatch surfacing as silent decode garbage. (DSDcc's DSDRate
    // enum is the protocol baud rate — 2400/4800/9600 — not this; DMR's
    // 4800 baud is set internally by setDecodeMode.)
    int input_sample_rate_hz = 48000;
};

class DsdccDecoder {
public:
    using EventCallback = std::function<void(const DsdEvent&)>;
    using AudioCallback = std::function<void(const int16_t* pcm, std::size_t n)>;

    DsdccDecoder();
    ~DsdccDecoder();

    DsdccDecoder(const DsdccDecoder&) = delete;
    DsdccDecoder& operator=(const DsdccDecoder&) = delete;

    // Constructs and configures the underlying DSDDecoder. Returns
    // false only on a config the decoder can't honor (currently: an
    // input sample rate other than 48000); there's no subprocess to
    // fail to spawn, unlike DsdProcess::start().
    bool start(const DsdccConfig& cfg, EventCallback on_event, AudioCallback on_audio);

    // Pushes PCM samples into the decoder and synchronously invokes
    // on_event_/on_audio_ for anything that comes out as a result --
    // NOT asynchronous like DsdProcess::write_audio(), which just wrote
    // to a pipe and let a separate reader thread handle the response.
    bool write_audio(const int16_t* pcm, std::size_t n);

    void stop();
    bool running() const { return running_.load(); }

private:
    // Reads decoder state (sync, per-slot talkgroup/source/voice) and
    // fires on_event_ only when something changed since last reported,
    // so we're not emitting an event per processed sample. Called once
    // per write_audio() block.
    void check_for_state_change();

    std::unique_ptr<DSDcc::DSDDecoder> decoder_;
    DsdccConfig cfg_;
    EventCallback on_event_;
    AudioCallback on_audio_;
    std::atomic<bool> running_{false};

    // Last-reported state for change detection: DSDcc's 26-char status
    // text per TDMA slot (index 0 = slot #1, 1 = slot #2 — DSDcc's
    // slot0light/slot1light naming), plus overall sync presence.
    std::string last_slot_text_[2];
    bool last_sync_ = false;
};

} // namespace dsdsrv
