// tetrapol_process.hpp
//
// Manages one tetrapol-kit `tetrapol_dump` child process for the TETRAPOL
// decode path. It is the TETRAPOL analog of TetraProcess / DsdProcess and
// mirrors their lifecycle (start / feed / stop / running) and fork/exec/pipe
// machinery, emitting the shared DsdEvent so session.cpp consumes TETRAPOL
// like any other backend.
//
// It is a hybrid of the two existing subprocess backends:
//
//  * Input is the demodulated BITSTREAM (like TetraProcess), one unpacked bit
//    per byte, fed to the child's stdin. The GMSK front end (tetrapol_demod.*)
//    turns IQ into bits and the frame sync (tetrapol_frame_sync.*) is upstream;
//    tetrapol_dump reads those raw bits from stdin (its only input; it takes
//    just "-i FILE", defaulting to stdin) and does the differential decode /
//    descramble / framing itself. write_bits() is the feed method.
//
//  * Events arrive over STDOUT as text (like DsdProcess), NOT over UDP.
//    tetrapol_dump logs each decoded TSDU as a multi-line indented tree
//    (a `CODOP=0x.. (NAME)` header + tab-indented field lines). Because a
//    message spans many lines, a stateful TetrapolParser (tetrapol_output.*)
//    accumulates the field lines and emits one DsdEvent per message; that is
//    the one structural difference from the one-line-per-event DSD parser.
//
// Voice: TETRAPOL traffic audio needs the proprietary vocoder and is not
// decoded by tetrapol-kit; there is no audio path here (unlike DsdProcess).
//
// STATUS: the subprocess machinery is exercised end to end by
// tests/test_tetrapol_process.cpp against a small fake tetrapol_dump stub
// (tests/tetrapol_fake_dump.cpp) -- fork/exec, the stdin bit pipe, and the
// stdout parse/forward path -- without needing the real binary. The parser's
// field mapping is source-pinned to tetrapol-kit's printf strings but NOT yet
// validated on a live TETRAPOL signal. Linux-specific (fork/exec/pipe).

#pragma once

#include "dsd_backend_types.hpp"
#include "tetrapol_output.hpp"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>
#include <mutex>

namespace dsdsrv {

struct TetrapolProcessConfig {
    // The tetrapol-kit decoder. Found on PATH by default.
    std::string tetrapol_dump_path = "tetrapol_dump";

    // Extra raw args appended verbatim (rarely needed; tetrapol_dump takes
    // only "-i FILE", and stdin is the default input we rely on).
    std::vector<std::string> extra_args;

    // Forward kind:"unknown" events (CODOPs the parser doesn't map to
    // sync/call). Default false: suppress them, exactly as the other backends
    // suppress their unmapped/noise lines. Recognized events always forward.
    bool forward_unknown = false;

    // Send the child's stderr (tetrapol-kit's own debug logging) to the
    // server's stderr instead of /dev/null. Default false to keep logs clean;
    // set true when debugging the child. (stdout is always captured -- it is
    // the event stream.)
    bool inherit_child_log = false;
};

// Emit filter for the TETRAPOL backend, mirroring the other backends'
// *_forward_event. Declared in tetrapol_output.hpp; re-exported here so
// callers that include only this header see it.
using dsdsrv::tetrapol_forward_event;

class TetrapolProcess {
public:
    using EventCallback = std::function<void(const DsdEvent&)>;

    TetrapolProcess() = default;
    ~TetrapolProcess();

    TetrapolProcess(const TetrapolProcess&) = delete;
    TetrapolProcess& operator=(const TetrapolProcess&) = delete;

    // Forks tetrapol_dump wired to a stdin pipe (bits in) and a stdout pipe
    // (text out), and starts the stdout reader thread. Returns false (child
    // reaped, everything undone) on fork/exec/pipe failure.
    bool start(const TetrapolProcessConfig& cfg, EventCallback on_event);

    // Relays demodulated bits (one unpacked bit per byte, 0x00/0x01) to the
    // child's stdin. Safe to call from one producer thread while the reader
    // thread runs. Returns false if the pipe is closed / write failed.
    bool write_bits(const unsigned char* bits, std::size_t n);

    // Closes stdin (EOF to tetrapol_dump), waits for exit, joins the reader
    // thread. Safe to call multiple times.
    void stop();

    bool running() const { return running_.load(); }

private:
    void stdout_reader_loop();
    std::vector<std::string> build_argv() const;

    TetrapolProcessConfig cfg_;
    EventCallback on_event_;
    TetrapolParser parser_;

    pid_t child_pid_ = -1;
    int stdin_fd_ = -1;   // write end, our side
    int stdout_fd_ = -1;  // read end, our side

    std::thread stdout_thread_;
    std::atomic<bool> running_{false};
    std::mutex write_mutex_;
};

} // namespace dsdsrv
