#include "dsd_process.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cerrno>
#include <regex>
#include <sstream>
#include <iostream>

namespace dsdsrv {

DsdProcess::~DsdProcess() { stop(); }

std::vector<std::string> DsdProcess::build_argv() const {
    // NOTE: verify these flags against `dsd-fme -h` for your installed
    // build. This targets the common dsd-fme CLI shape:
    //   dsd-fme -i - -f d [-U 127.0.0.1:PORT] [extra_args...]
    std::vector<std::string> argv;
    argv.push_back(cfg_.dsd_fme_path);
    argv.push_back("-i");
    argv.push_back("-");            // read raw discriminator audio from stdin
    argv.push_back("-f");
    argv.push_back(cfg_.mode_flag); // e.g. "d" for DMR
    if (cfg_.udp_audio_port != 0) {
        argv.push_back("-U");
        argv.push_back("127.0.0.1:" + std::to_string(cfg_.udp_audio_port));
    }
    for (const auto& a : cfg_.extra_args) argv.push_back(a);
    return argv;
}

bool DsdProcess::start(const DsdProcessConfig& cfg, EventCallback on_event, AudioCallback on_audio) {
    cfg_ = cfg;
    on_event_ = std::move(on_event);
    on_audio_ = std::move(on_audio);

    int stdin_pipe[2];  // [0]=read (child), [1]=write (us)
    int stdout_pipe[2]; // [0]=read (us), [1]=write (child)
    if (pipe(stdin_pipe) != 0) return false;
    if (pipe(stdout_pipe) != 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        return false;
    }

    // Optional UDP socket for decoded voice audio, bound before fork so we
    // know it's ready by the time the child starts sending to it.
    udp_fd_ = -1;
    if (cfg_.udp_audio_port != 0) {
        udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_fd_ >= 0) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(cfg_.udp_audio_port);
            if (bind(udp_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                close(udp_fd_);
                udp_fd_ = -1;
            }
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        if (udp_fd_ >= 0) close(udp_fd_);
        return false;
    }

    if (pid == 0) {
        // ---- child ----
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO); // fold stderr in too; dsd-fme logs to both across versions

        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        if (udp_fd_ >= 0) close(udp_fd_); // child doesn't need our bound socket

        auto argv_strs = build_argv();
        std::vector<char*> argv_c;
        argv_c.reserve(argv_strs.size() + 1);
        for (auto& s : argv_strs) argv_c.push_back(const_cast<char*>(s.c_str()));
        argv_c.push_back(nullptr);

        execvp(argv_c[0], argv_c.data());
        // If execvp returns, it failed.
        std::fprintf(stderr, "dsd-server: failed to exec '%s': %s\n",
                     argv_c[0], std::strerror(errno));
        _exit(127);
    }

    // ---- parent ----
    child_pid_ = pid;
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];

    // Make stdin writes non-blocking-friendly isn't required here since we
    // write from a single controlled producer thread; leave blocking.

    running_ = true;
    stdout_thread_ = std::thread(&DsdProcess::stdout_reader_loop, this);
    if (udp_fd_ >= 0) {
        udp_thread_ = std::thread(&DsdProcess::udp_reader_loop, this);
    }
    return true;
}

bool DsdProcess::write_audio(const int16_t* pcm, std::size_t n) {
    if (stdin_fd_ < 0) return false;
    std::lock_guard<std::mutex> lock(write_mutex_);
    const char* buf = reinterpret_cast<const char*>(pcm);
    std::size_t total = n * sizeof(int16_t);
    std::size_t written = 0;
    while (written < total) {
        ssize_t w = ::write(stdin_fd_, buf + written, total - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false; // e.g. EPIPE if dsd-fme exited
        }
        written += static_cast<std::size_t>(w);
    }
    return true;
}

void DsdProcess::stop() {
    if (!running_.exchange(false)) {
        // Already stopped, but make sure a partially-started instance
        // still gets its fds/threads cleaned up if start() got far enough.
    }

    if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; } // EOF -> dsd-fme should exit
    if (udp_fd_ >= 0) { shutdown(udp_fd_, SHUT_RDWR); }

    if (child_pid_ > 0) {
        int status = 0;
        // Give it a moment to exit cleanly after EOF/stdin close before
        // escalating to SIGTERM.
        for (int i = 0; i < 20; ++i) {
            pid_t r = waitpid(child_pid_, &status, WNOHANG);
            if (r == child_pid_) { child_pid_ = -1; break; }
            usleep(50 * 1000);
        }
        if (child_pid_ > 0) {
            kill(child_pid_, SIGTERM);
            waitpid(child_pid_, &status, 0);
            child_pid_ = -1;
        }
    }

    if (stdout_fd_ >= 0) { close(stdout_fd_); stdout_fd_ = -1; }
    if (udp_fd_ >= 0) { close(udp_fd_); udp_fd_ = -1; }

    if (stdout_thread_.joinable()) stdout_thread_.join();
    if (udp_thread_.joinable()) udp_thread_.join();
}

void DsdProcess::stdout_reader_loop() {
    std::string buf;
    char tmp[4096];
    while (running_.load()) {
        ssize_t n = ::read(stdout_fd_, tmp, sizeof(tmp));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break; // EOF or error: child exited or pipe closed
        }
        buf.append(tmp, static_cast<std::size_t>(n));

        std::size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && on_event_) {
                on_event_(classify_line(line));
            }
        }
    }
    // Flush any trailing partial line on exit.
    if (!buf.empty() && on_event_) {
        on_event_(classify_line(buf));
    }
}

void DsdProcess::udp_reader_loop() {
    // dsd-fme's decoded voice PCM is typically 8000 Hz, 16-bit signed,
    // mono. Verify against your build/version; adjust downstream handling
    // in session.cpp if it differs (e.g. some builds use 8000 Hz for AMBE
    // voice frames specifically, distinct from the 48000 Hz discriminator
    // input rate).
    std::vector<char> buf(8192);
    while (running_.load() && udp_fd_ >= 0) {
        ssize_t n = ::recv(udp_fd_, buf.data(), buf.size(), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        if (on_audio_) {
            on_audio_(reinterpret_cast<const int16_t*>(buf.data()),
                       static_cast<std::size_t>(n) / sizeof(int16_t));
        }
    }
}

DsdEvent DsdProcess::classify_line(const std::string& line) const {
    // Best-effort regex classification of dsd-fme's textual log output.
    // dsd-fme's log format varies by version/build flags, so treat these
    // as a starting point: run your dsd-fme interactively against a known
    // signal, capture real log lines, and tighten these patterns (or add
    // more) to match what you actually see.
    DsdEvent ev;
    ev.raw_line = line;
    ev.kind = "unknown";

    static const std::regex tg_re(R"(TG[:=]?\s*(\d+))", std::regex::icase);
    static const std::regex src_re(R"((?:SRC|RID|Source)[:=]?\s*(\d+))", std::regex::icase);
    static const std::regex slot_re(R"((?:TS|Slot)[:=]?\s*(\d))", std::regex::icase);
    static const std::regex sync_re(R"(sync|no sync|nosync)", std::regex::icase);
    static const std::regex voice_re(R"(voice|ambe)", std::regex::icase);

    std::smatch m;
    if (std::regex_search(line, m, tg_re)) ev.talkgroup = m[1].str();
    if (std::regex_search(line, m, src_re)) ev.source_id = m[1].str();
    if (std::regex_search(line, m, slot_re)) ev.slot = m[1].str();

    if (std::regex_search(line, voice_re)) ev.kind = "voice";
    else if (std::regex_search(line, sync_re)) ev.kind = "sync";
    else if (!ev.talkgroup.empty() || !ev.source_id.empty()) ev.kind = "call";

    return ev;
}

} // namespace dsdsrv
