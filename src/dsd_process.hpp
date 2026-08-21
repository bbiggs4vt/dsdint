// dsd_process.hpp
//
// Manages one dsd-fme child process: feeds it raw discriminator PCM on
// stdin, reads its textual event log from stdout, and (optionally) listens
// on a local UDP port for the decoded voice PCM that dsd-fme streams out
// via its "-U host:port" option.
//
// IMPORTANT: dsd-fme's exact command-line flags and stdout log format have
// changed across versions/forks. The flags below (input from stdin, DMR
// frame sync, UDP audio output) reflect the common case but you should
// verify against `dsd-fme -h` for the version you have installed and
// adjust build_argv() / the event regexes in dsd_process.cpp accordingly.
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

struct DsdProcessConfig {
    std::string dsd_fme_path = "dsd-fme";
    // Discriminator audio format we'll write to dsd-fme's stdin.
    int input_sample_rate_hz = 48000;
    // DMR frame sync mode. See `dsd-fme -h`: typically "-f d" or similar.
    std::string mode_flag = "d";
    // Extra raw args appended verbatim (e.g. {"-T"} for trunking, or
    // {"-C", "451000000"} for a control channel). Kept separate from the
    // fixed flags above so callers don't have to rebuild the base command.
    std::vector<std::string> extra_args;

    // If non-empty, dsd-fme is told to stream decoded voice PCM out via
    // UDP to 127.0.0.1:<udp_audio_port>, which we then read locally.
    // 0 = disabled (no audio relay, events only).
    uint16_t udp_audio_port = 0;
};

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
    std::vector<std::string> build_argv() const;
    DsdEvent classify_line(const std::string& line) const;

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
};

} // namespace dsdsrv
