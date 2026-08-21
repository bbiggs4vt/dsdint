// dsdcc_decoder.hpp
//
// In-process alternative to DsdProcess (dsd_process.hpp), built on DSDcc
// (https://github.com/f4exb/dsdcc) instead of spawning a dsd-fme/DSD
// subprocess. This is a structurally different integration than the
// subprocess backends: DSDcc is a C++ library designed to have samples
// pushed into it directly, with decoded audio pulled back out via a
// getter — there's no child process, no pipes, no UDP audio port.
//
// PLEASE READ BEFORE USING THIS FILE
//
// This was written without access to DSDcc's actual headers (no network
// in the environment it was built in) — everything here is based on
// fragments of dsd_decoder.h, dmr.h, and the project README surfaced via
// web search, not a complete reading of the API. Confidence varies a lot
// by piece, and it's spelled out precisely in dsdcc_decoder.cpp because
// the uncertainty is concentrated in one specific call (how you actually
// feed samples into DSDDecoder) rather than spread evenly across the
// file. Read the top-of-file comment in dsdcc_decoder.cpp before relying
// on this — in particular, the exact per-sample ingestion method name is
// a placeholder, not a verified API call.
//
// What IS a solid design decision regardless of the exact API names:
// since this is in-process (no subprocess, no separate reader threads),
// start()/write_audio()/stop() all run synchronously on whatever thread
// calls them — for this project, that's Session's demod worker thread.
// Event/audio callbacks therefore also fire on that thread, not a
// dedicated reader thread like DsdProcess's. session.cpp already handles
// this correctly without any changes: its callbacks always post onto the
// WebSocket's strand executor before touching ws_, regardless of which
// thread invoked them (see start_pipeline() in session.cpp) -- that
// pattern was written generically enough to cover both backends.

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
    // DSDcc's DSDDecoder::DSDDecodeMode enum includes DSDDecodeDMR,
    // DSDDecodeAuto, etc. (confirmed from dsd_decoder.h). Kept as a
    // string here for symmetry with DsdProcessConfig::mode_flag and to
    // keep this header decoupled from DSDcc's actual enum — mapped to
    // the real enum value in dsdcc_decoder.cpp.
    std::string mode = "dmr";

    // Sample rate of the discriminator PCM you'll be pushing via
    // write_audio(). DSDcc's DSDRate enum (DSDRate2400/4800/9600)
    // appears to describe the protocol's symbol rate rather than the
    // input audio sample rate specifically -- verify against
    // dsd_decoder.h how these two concepts relate for your version
    // before assuming this field maps directly onto DSDRate.
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

    // Constructs and configures the underlying DSDDecoder. Unlike
    // DsdProcess::start(), there's no subprocess to fail to spawn, so
    // this should essentially always return true; it's still bool for
    // interface symmetry with DsdProcess (see dsd_backend_selector.hpp).
    bool start(const DsdccConfig& cfg, EventCallback on_event, AudioCallback on_audio);

    // Pushes PCM samples into the decoder and synchronously invokes
    // on_event_/on_audio_ for anything that comes out as a result --
    // NOT asynchronous like DsdProcess::write_audio(), which just wrote
    // to a pipe and let a separate reader thread handle the response.
    bool write_audio(const int16_t* pcm, std::size_t n);

    void stop();
    bool running() const { return running_.load(); }

private:
    // Feeds decoder state changes (sync, talkgroup, slot, etc.) to
    // on_event_ when they differ from what was last reported, so we're
    // not emitting an event on every single processed sample. See the
    // detailed caveats in dsdcc_decoder.cpp about exactly which getters
    // this polls and how confident I am in each one.
    void check_for_state_change();

    std::unique_ptr<DSDcc::DSDDecoder> decoder_;
    DsdccConfig cfg_;
    EventCallback on_event_;
    AudioCallback on_audio_;
    std::atomic<bool> running_{false};

    // Last-reported state, for change detection in check_for_state_change().
    std::string last_talkgroup_;
    std::string last_source_id_;
    std::string last_slot_;
    bool last_sync_ = false;
};

} // namespace dsdsrv
