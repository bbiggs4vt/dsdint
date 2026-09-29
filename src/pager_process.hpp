// pager_process.hpp
//
// One multimon-ng child process per session. We write 16-bit native-endian
// mono PCM at 22050 Hz to its stdin (`-t raw -`) and read its stdout, one
// JSON object per line (`--json`: upstream multimon-ng newer than the 1.3.0
// release that Debian/Ubuntu package; see README "Paging"). Each complete
// stdout line is handed to the line
// callback from a dedicated reader thread. stderr is inherited so decoder
// diagnostics land in the server's own log.
//
// Process hygiene (same scheme as DsdProcess, plus fd cleanup):
//   - pipes are O_CLOEXEC so concurrent sessions' children never inherit
//     each other's pipe ends (which would block EOF delivery on stop);
//   - an exec-status pipe makes a missing binary a start() failure instead
//     of a phantom "started";
//   - SIGPIPE is ignored process-wide so a crashed child turns writes into
//     EPIPE rather than killing the server.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace dsdsrv {

struct MultimonConfig {
    // Binary to exec (searched on PATH). Defaults from $MULTIMON_NG, else
    // "multimon-ng".
    std::string path;
    // Demodulators to enable, e.g. {"POCSAG1200","FLEX_NEXT"}.
    std::vector<std::string> demods;
    // POCSAG text interpretation: "" / "auto" (multimon-ng guesses, and may
    // print more than one rendering of an ambiguous message), or "alpha",
    // "numeric", "skyper" to force one.
    std::string pocsag_mode;
    // Prefix the command with `stdbuf -oL` (coreutils) to force line-buffered
    // stdout. multimon-ng's JSON writer never flushes (its FLEX_NEXT output
    // in particular), so on a pipe decoded pages would sit in a 4 KiB stdio
    // buffer until it filled or the session stopped -- minutes of latency on
    // a quiet channel. Defaults to on when stdbuf is on PATH, unless
    // $PAGER_NO_STDBUF is set.
    bool line_buffered = default_line_buffered();

    static bool default_line_buffered();
};

std::string default_multimon_path();

// Build the argv (exposed for tests).
std::vector<std::string> build_multimon_argv(const MultimonConfig& cfg);

class MultimonProcess {
public:
    using LineCallback = std::function<void(const std::string& line)>;
    // Called (on the reader thread) if the child's stdout closes on its own,
    // i.e. the decoder exited without stop() having been called.
    using ExitCallback = std::function<void()>;

    MultimonProcess() = default;
    ~MultimonProcess() { stop(); }
    MultimonProcess(const MultimonProcess&) = delete;
    MultimonProcess& operator=(const MultimonProcess&) = delete;

    bool start(const MultimonConfig& cfg, LineCallback on_line, ExitCallback on_exit = nullptr);
    // Blocking write of PCM samples to the decoder; false if the child is gone.
    bool write_audio(const int16_t* pcm, std::size_t n);
    // Closes stdin (multimon-ng flushes and exits at EOF), waits briefly,
    // then SIGTERMs if needed. Every line the child printed has been
    // delivered to the callback by the time this returns.
    void stop();

    bool running() const { return running_.load(); }

private:
    void reader_loop();

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    pid_t child_pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    std::mutex write_mutex_;
    std::thread reader_;
    LineCallback on_line_;
    ExitCallback on_exit_;
};

} // namespace dsdsrv
