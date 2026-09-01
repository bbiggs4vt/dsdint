// tetrapol_process.cpp -- see tetrapol_process.hpp for the model and rationale.
// The fork/exec/pipe/failure-detection machinery mirrors dsd_process.cpp and
// tetra_process.cpp (read dsd_process.cpp's comments for why each step is the
// way it is: SIGPIPE handling, O_CLOEXEC pipes for concurrent sessions, the
// exec-status pipe, and the join-before-close ordering in stop()). The feed is
// TetraProcess's write_bits (bits, not PCM); the event source is DsdProcess's
// stdout reader (text, not UDP), but run through the stateful TetrapolParser
// because tetrapol_dump prints a multi-line tree per message.

#include "tetrapol_process.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <string>

namespace dsdsrv {

TetrapolProcess::~TetrapolProcess() { stop(); }

std::vector<std::string> TetrapolProcess::build_argv() const {
    // tetrapol_dump reads its bitstream from stdin by default (no "-i", or
    // "-i -"); we feed the stdin pipe, so no input arg is needed.
    std::vector<std::string> argv;
    argv.push_back(cfg_.tetrapol_dump_path);
    for (const auto& a : cfg_.extra_args) argv.push_back(a);
    return argv;
}

bool TetrapolProcess::start(const TetrapolProcessConfig& cfg, EventCallback on_event) {
    std::signal(SIGPIPE, SIG_IGN); // see dsd_process.cpp: a dead child must not kill us

    cfg_ = cfg;
    on_event_ = std::move(on_event);
    parser_.reset();

    int stdin_pipe[2];  // [0]=read (child), [1]=write (us)
    int stdout_pipe[2]; // [0]=read (us), [1]=write (child)
    if (pipe2(stdin_pipe, O_CLOEXEC) != 0) return false;
    if (pipe2(stdout_pipe, O_CLOEXEC) != 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        return false;
    }

    int exec_pipe[2]; // child reports exec failure here; CLOEXEC -> EOF on success
    if (pipe2(exec_pipe, O_CLOEXEC) != 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(exec_pipe[0]); close(exec_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // ---- child ----
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        if (cfg_.inherit_child_log) {
            // Keep the child's stderr on our stderr for debugging.
        } else {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }
        }
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(exec_pipe[0]); // keep exec_pipe[1]; execvp closes it via CLOEXEC

        auto argv_strs = build_argv();
        std::vector<char*> argv_c;
        argv_c.reserve(argv_strs.size() + 1);
        for (auto& s : argv_strs) argv_c.push_back(const_cast<char*>(s.c_str()));
        argv_c.push_back(nullptr);

        execvp(argv_c[0], argv_c.data());
        int err = errno;
        std::fprintf(stderr, "dsd-server: failed to exec '%s': %s\n",
                     argv_c[0], std::strerror(err));
        ssize_t unused = ::write(exec_pipe[1], &err, sizeof(err));
        (void)unused;
        _exit(127);
    }

    // ---- parent ----
    close(exec_pipe[1]);
    int exec_errno = 0;
    ssize_t n = ::read(exec_pipe[0], &exec_errno, sizeof(exec_errno));
    close(exec_pipe[0]);
    if (n > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        std::fprintf(stderr, "dsd-server: cannot start '%s': %s\n",
                     cfg_.tetrapol_dump_path.c_str(), std::strerror(exec_errno));
        return false;
    }

    child_pid_ = pid;
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];

    running_ = true;
    stdout_thread_ = std::thread(&TetrapolProcess::stdout_reader_loop, this);
    return true;
}

bool TetrapolProcess::write_bits(const unsigned char* bits, std::size_t n) {
    if (stdin_fd_ < 0) return false;
    std::lock_guard<std::mutex> lock(write_mutex_);
    std::size_t written = 0;
    while (written < n) {
        ssize_t w = ::write(stdin_fd_, bits + written, n - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false; // EPIPE if tetrapol_dump exited
        }
        written += static_cast<std::size_t>(w);
    }
    return true;
}

void TetrapolProcess::stdout_reader_loop() {
    auto emit = [&](const DsdEvent& ev) {
        if (on_event_ && tetrapol_forward_event(ev, cfg_.forward_unknown))
            on_event_(ev);
    };

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
            parser_.feed_line(line, emit);
        }
    }
    // Feed any trailing partial line, then flush the message in progress
    // (tetrapol_dump only emits a message once the NEXT header arrives, so the
    // last decoded message would otherwise be lost at EOF).
    if (!buf.empty()) parser_.feed_line(buf, emit);
    parser_.flush(emit);
}

void TetrapolProcess::stop() {
    running_.exchange(false);

    if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; } // EOF -> tetrapol_dump exits

    if (child_pid_ > 0) {
        int status = 0;
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

    // Join before closing the stdout fd (see dsd_process.cpp stop() for why
    // this order is load-bearing under concurrent sessions). What ends the
    // reader is EOF on the child's exit (the O_CLOEXEC pipes guarantee this
    // process holds the only other reference, so EOF is prompt).
    if (stdout_thread_.joinable()) stdout_thread_.join();
    if (stdout_fd_ >= 0) { close(stdout_fd_); stdout_fd_ = -1; }
}

} // namespace dsdsrv
