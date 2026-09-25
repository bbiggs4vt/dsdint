// dsd_process.hpp
//
// Manages one dsd-fme child process: feeds it raw discriminator PCM on
// stdin, reads its textual event log from stdout/stderr, and (optionally)
// listens on a local UDP port for the decoded voice PCM that dsd-fme
// streams out via its "-o udp:host:port" output.
//
// STATUS: verified against real dsd-fme (lwvmobile/dsd-fme, commit
// 198f0ea) built from source and run on a real DMR capture. The original
// guessed flags were partly wrong and are fixed here -- see build_argv()
// in dsd_process.cpp for the verified command line, and DSD-FME
// VERIFICATION NOTES there for exactly what was checked. Other
// versions/forks may still differ; `dsd-fme -h` remains the authority
// for yours.
//
// This class is Linux-specific (uses fork/exec/pipe/POSIX sockets).

#pragma once

#include "dsd_backend_types.hpp"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>
#include <mutex>

namespace dsdsrv {

// Classify one cleaned dsd-fme log line (ANSI/CR already stripped) into a
// structured DsdEvent. Free function -- it depends on nothing but the
// line text -- so it is unit-tested directly against real dsd-fme output
// (see tests/test_dsd_fme_parse.cpp). Handles both DMR and NXDN line
// formats.
DsdEvent classify_dsd_fme_line(const std::string& line);

// Carries dsd-fme's per-burst physical TDMA slot forward onto the lines that
// follow it. dsd-fme decodes one burst at a time and prints the slot only on
// that burst's "Sync:" line (as "[slot1]"/"[SLOT2]"); the CSBK / call /
// trunking metadata lines it prints for the same burst carry no slot marker,
// so classify_dsd_fme_line leaves their slot "". Because those lines belong to
// the slot named by the most recent sync, this remembers it and stamps it onto
// them -- giving clients a reliable per-event slot instead of each having to
// re-derive it from event order. Fed events in stdout order; state is the last
// explicit slot seen. It is self-gating: until a "[slotN]" marker appears (as
// on single-slot protocols) nothing is stamped. Stateful and single-threaded
// (the stdout reader), so no locking; unit-tested via tests/test_dsd_fme_parse.
struct DmrSlotCarry {
    std::string current;   // last explicit slot seen ("1"/"2"); "" until first

    void apply(DsdEvent& ev) {
        if (ev.slot == "1" || ev.slot == "2") { current = ev.slot; return; }
        if (current.empty()) return;
        // Only stamp slot-bearing traffic kinds; leave channel-wide/unknown
        // lines unslotted.
        if (ev.kind == "voice" || ev.kind == "call" ||
            ev.kind == "message" || ev.kind == "burst") {
            ev.slot = current;
        }
    }
};

struct DsdProcessConfig {
    std::string dsd_fme_path = "dsd-fme";
    // Discriminator audio format we'll write to dsd-fme's stdin.
    // dsd-fme reads stdin ("-i -") as raw S16LE mono at its wav sample
    // rate, which defaults to 48000 (verified in dsd_audio.c's
    // openAudioInDevice); 48000 is also what FmDemodulator produces.
    int input_sample_rate_hz = 48000;
    // Decoder mode letter, passed as "-f <letter>". VERIFIED against
    // real dsd-fme: "s" = DMR TDMA BS/MS simplex (the DMR mode), "a" =
    // auto-detect. The old default here was "d", on the guess that d
    // meant DMR -- in real dsd-fme "-fd" is D-STAR, so that default
    // would have silently decoded the wrong protocol.
    std::string mode_flag = "s";
    // Extra raw args appended verbatim (e.g. {"-T"} for trunking, or
    // {"-C", "451000000"} for a control channel). Kept separate from the
    // fixed flags above so callers don't have to rebuild the base command.
    std::vector<std::string> extra_args;

    // If nonzero, dsd-fme is told to stream decoded voice PCM out via
    // UDP to 127.0.0.1:<udp_audio_port> ("-o udp:127.0.0.1:<port>"),
    // which we then read locally. 0 = disabled (no audio relay, events
    // only) -- "-o null" is passed instead, because dsd-fme's default
    // output is PulseAudio and it EXITS at startup when no Pulse daemon
    // is reachable (verified; typical for a server).
    //
    // Payload is raw PCM, no header. With mode_flag "s" (DMR stereo
    // mode) real dsd-fme sends 8000 Hz STEREO interleaved int16 -- TDMA
    // slot 1 on the left channel, slot 2 on the right (640-byte packets
    // = 20 ms).
    uint16_t udp_audio_port = 0;

    // Collapse that stereo stream to a single 8 kHz MONO stream before it
    // reaches on_audio (so the subprocess backend matches the in-process
    // DSDcc backend's mono output). The mono channel auto-follows the
    // active TDMA slot -- picked from the slot the decoder is currently
    // reporting voice/call activity on (slot 1 -> left, slot 2 -> right).
    // While no slot is known (nothing decoded yet, or concurrent voice on
    // both slots) it falls back to an (L+R) downmix. Default true; set
    // false to relay dsd-fme's raw stereo interleave unchanged.
    bool mono_follow_slot = true;

    // Forward dsd-fme's unrecognized (kind:"unknown") log lines as events.
    // Default false: suppress them. dsd-fme prints a large startup block —
    // an ASCII-art banner, version/build lines, and a device/config dump
    // ("Build Version: …", "MBElib Version: …", "Decoding DMR BS/MS
    // Simplex", "UDP Blaster Output: …", "Audio In Device: …", …) — none of
    // which match a decode pattern, so they all classify as "unknown" and
    // otherwise reach the client as noise. Set true to forward them anyway
    // (useful when developing the classifier: an unrecognized line you
    // expected to parse still surfaces with its raw text).
    bool forward_unknown = false;
};

// Emit filter for the subprocess backend: whether a classified event should
// be forwarded to the client. Everything with a recognized kind is always
// forwarded; kind:"unknown" lines (dsd-fme's banner/config noise, or any
// line the classifier didn't match) are forwarded only when forward_unknown
// is set. Free function so it is unit-testable alongside classify.
bool dsd_fme_forward_event(const DsdEvent& ev, bool forward_unknown);

// Collapse one interleaved 8 kHz stereo buffer (L,R,L,R,...) to a single
// mono channel for the given active TDMA slot: slot 1 -> left, slot 2 ->
// right, anything else (0 = unknown / both) -> an (L+R) downmix. Writes
// nsamp/2 samples into `out` (resized to fit) and returns that count; a
// stray trailing sample from an odd nsamp is dropped so L/R phase can't
// slip. Free function so the deinterleave is unit-tested without a live
// dsd-fme (see tests/test_dsd_fme_parse.cpp).
std::size_t stereo_to_mono_for_slot(const int16_t* pcm, std::size_t nsamp,
                                    int slot, std::vector<int16_t>& out);

class DsdProcess {
public:
    using EventCallback = std::function<void(const DsdEvent&)>;
    using AudioCallback = std::function<void(const int16_t* pcm, std::size_t n)>;

    DsdProcess() = default;
    ~DsdProcess();

    DsdProcess(const DsdProcess&) = delete;
    DsdProcess& operator=(const DsdProcess&) = delete;

    // Spawns the child process and starts the background reader threads.
    // Returns false (with errno set) on fork/exec/pipe failure.
    bool start(const DsdProcessConfig& cfg, EventCallback on_event, AudioCallback on_audio);

    // Writes discriminator PCM samples to the child's stdin. Safe to call
    // from one producer thread while reader threads run in the background.
    // Returns false if the pipe is closed / write failed.
    bool write_audio(const int16_t* pcm, std::size_t n);

    // Closes stdin (signals EOF to dsd-fme), waits for the process to
    // exit, and joins reader threads. Safe to call multiple times.
    void stop();

    bool running() const { return running_.load(); }

private:
    void stdout_reader_loop();
    void udp_reader_loop();
    void publish_active_slot(const DsdEvent& ev);
    std::vector<std::string> build_argv() const;
    DsdEvent classify_line(const std::string& line) const { return classify_dsd_fme_line(line); }

    DsdProcessConfig cfg_;
    EventCallback on_event_;
    AudioCallback on_audio_;

    pid_t child_pid_ = -1;
    int stdin_fd_ = -1;   // write end, our side
    int stdout_fd_ = -1;  // read end, our side
    int udp_fd_ = -1;

    std::thread stdout_thread_;
    std::thread udp_thread_;
    std::atomic<bool> running_{false};
    std::mutex write_mutex_;

    // Active TDMA slot for mono_follow_slot, published by the stdout
    // reader (which classifies dsd-fme's slot-bearing lines) and read by
    // the UDP reader (which picks the matching stereo channel). 0 = not
    // yet known -> downmix. Atomic because the two reader threads touch it.
    std::atomic<int> active_slot_{0};
    // Carries dsd-fme's per-burst slot onto its unmarked follow-on lines.
    // Touched only by the stdout reader thread, so it needs no lock.
    DmrSlotCarry slot_carry_;
};

} // namespace dsdsrv
