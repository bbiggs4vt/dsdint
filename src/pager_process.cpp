#include "pager_process.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dsdsrv {

std::string default_multimon_path() {
    const char* env = std::getenv("MULTIMON_NG");
    return (env && *env) ? std::string(env) : std::string("multimon-ng");
}

namespace {
// execvp-style lookup: a name containing '/' is used as-is, otherwise each
// $PATH entry is tried. Returns "" if nothing executable is found.
std::string find_executable(const std::string& name) {
    if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0 ? name : std::string();
    const char* path = std::getenv("PATH");
    const std::string p = path ? path : "/usr/bin:/bin";
    std::size_t start = 0;
    while (start <= p.size()) {
        std::size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        std::string dir = p.substr(start, end - start);
        if (dir.empty()) dir = ".";
        const std::string cand = dir + "/" + name;
        if (access(cand.c_str(), X_OK) == 0) return cand;
        start = end + 1;
    }
    return {};
}
} // namespace

bool MultimonConfig::default_line_buffered() {
    static const bool v = [] {
        const char* off = std::getenv("PAGER_NO_STDBUF");
        if (off && *off && std::strcmp(off, "0") != 0) return false;
        if (!find_executable("stdbuf").empty()) return true;
        std::fprintf(stderr, "dsd-server: WARNING: 'stdbuf' not found; FLEX pages may be delayed "
                             "until multimon-ng's output buffer fills\n");
        return false;
    }();
    return v;
}

std::vector<std::string> build_multimon_argv(const MultimonConfig& cfg) {
    std::vector<std::string> argv;
    if (cfg.line_buffered) {
        argv.push_back("stdbuf");
        argv.push_back("-oL");
    }
    argv.push_back(cfg.path.empty() ? default_multimon_path() : cfg.path);
    argv.push_back("-q");       // no banner
    argv.push_back("--json");   // one JSON object per decoded line
    argv.push_back("-c");       // start from an empty demod set...
    for (const auto& d : cfg.demods) {
        argv.push_back("-a");   // ...and add exactly the requested ones
        argv.push_back(d);
    }
    if (!cfg.pocsag_mode.empty() && cfg.pocsag_mode != "auto") {
        argv.push_back("-f");
        argv.push_back(cfg.pocsag_mode);
    }
    argv.push_back("-t");
    argv.push_back("raw");
    argv.push_back("-");        // read PCM from stdin
    return argv;
}

namespace {
// Child side, between fork and exec: close every inherited fd above stderr
// except `keep` (the exec-status pipe, which CLOEXEC closes on success).
// O_CLOEXEC on our own pipes isn't enough -- Asio's sockets are NOT
// close-on-exec, so without this every decoder child holds a copy of every
// WebSocket socket open at the time it was forked, and a session's socket
// isn't really closed (no FIN reaches the client) until every such child
// exits. Only async-signal-safe calls here.
void close_fds_except(int keep) {
#if defined(SYS_close_range)
    if (keep > 3) syscall(SYS_close_range, 3u, static_cast<unsigned>(keep - 1), 0u);
    if (syscall(SYS_close_range, static_cast<unsigned>(keep + 1), ~0u, 0u) == 0) return;
#endif
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 65536;
    for (int fd = 3; fd < max_fd; ++fd)
        if (fd != keep) close(fd);
}
} // namespace

bool MultimonProcess::start(const MultimonConfig& cfg, LineCallback on_line, ExitCallback on_exit) {
    std::signal(SIGPIPE, SIG_IGN);
    // Resolve the decoder up front: under `stdbuf` a missing multimon-ng would
    // otherwise exec stdbuf successfully and only fail inside it, invisible to
    // the exec-status pipe below.
    const std::string decoder = cfg.path.empty() ? default_multimon_path() : cfg.path;
    if (find_executable(decoder).empty()) {
        std::fprintf(stderr, "dsd-server: decoder '%s' not found or not executable\n", decoder.c_str());
        return false;
    }
    on_line_ = std::move(on_line);
    on_exit_ = std::move(on_exit);
    stopping_ = false;

    int in_pipe[2], out_pipe[2], exec_pipe[2];
    if (pipe2(in_pipe, O_CLOEXEC) != 0) return false;
    if (pipe2(out_pipe, O_CLOEXEC) != 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        return false;
    }
    if (pipe2(exec_pipe, O_CLOEXEC) != 0) {
        close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
        return false;
    }

    // Build argv before fork: allocation after fork() in a multithreaded
    // process is not async-signal-safe.
    const auto argv_strs = build_multimon_argv(cfg);
    std::vector<char*> argv_c;
    for (const auto& s : argv_strs) argv_c.push_back(const_cast<char*>(s.c_str()));
    argv_c.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
        close(exec_pipe[0]); close(exec_pipe[1]);
        return false;
    }
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close_fds_except(exec_pipe[1]);
        execvp(argv_c[0], argv_c.data());
        int err = errno;
        ssize_t unused = ::write(exec_pipe[1], &err, sizeof(err));
        (void)unused;
        _exit(127);
    }

    close(exec_pipe[1]);
    int exec_errno = 0;
    ssize_t n;
    do { n = ::read(exec_pipe[0], &exec_errno, sizeof(exec_errno)); } while (n < 0 && errno == EINTR);
    close(exec_pipe[0]);
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (n > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        close(in_pipe[1]); close(out_pipe[0]);
        std::fprintf(stderr, "dsd-server: cannot exec '%s': %s\n", argv_c[0], std::strerror(exec_errno));
        return false;
    }

    child_pid_ = pid;
    stdin_fd_ = in_pipe[1];
    stdout_fd_ = out_pipe[0];
    running_ = true;
    reader_ = std::thread(&MultimonProcess::reader_loop, this);
    return true;
}

bool MultimonProcess::write_audio(const int16_t* pcm, std::size_t n) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (stdin_fd_ < 0) return false;
    const char* buf = reinterpret_cast<const char*>(pcm);
    std::size_t total = n * sizeof(int16_t), written = 0;
    while (written < total) {
        ssize_t w = ::write(stdin_fd_, buf + written, total - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false; // EPIPE: decoder exited
        }
        written += static_cast<std::size_t>(w);
    }
    return true;
}

void MultimonProcess::stop() {
    stopping_ = true;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; } // EOF -> flush + exit
    }
    if (child_pid_ > 0) {
        int status = 0;
        bool reaped = false;
        for (int i = 0; i < 40 && !reaped; ++i) { // up to 2 s to drain and exit
            if (waitpid(child_pid_, &status, WNOHANG) == child_pid_) reaped = true;
            else usleep(50 * 1000);
        }
        if (!reaped) {
            kill(child_pid_, SIGTERM);
            waitpid(child_pid_, &status, 0);
        }
        child_pid_ = -1;
    }
    // Join before closing: the reader ends on EOF (the child is gone and, with
    // O_CLOEXEC, no other process holds the write end), and closing first
    // would free the fd number for reuse under a still-running reader.
    if (reader_.joinable()) reader_.join();
    if (stdout_fd_ >= 0) { close(stdout_fd_); stdout_fd_ = -1; }
    running_ = false;
}

void MultimonProcess::reader_loop() {
    std::string buf;
    char tmp[4096];
    for (;;) {
        ssize_t n = ::read(stdout_fd_, tmp, sizeof(tmp));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        buf.append(tmp, static_cast<std::size_t>(n));
        std::size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && on_line_) on_line_(line);
        }
    }
    if (!buf.empty() && on_line_) on_line_(buf);
    running_ = false;
    if (!stopping_.load() && on_exit_) on_exit_();
}

} // namespace dsdsrv
