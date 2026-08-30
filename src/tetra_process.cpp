// tetra_process.cpp -- see tetra_process.hpp for the model and rationale.
// The fork/exec/pipe/failure-detection machinery mirrors dsd_process.cpp
// (read its comments for why each step is the way it is: SIGPIPE handling,
// O_CLOEXEC pipes for concurrent sessions, the exec-status pipe, and the
// join-before-close ordering in stop()).

#include "tetra_process.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dsdsrv {

bool tetra_forward_event(const DsdEvent& ev, bool forward_unknown) {
    return forward_unknown || ev.kind != "unknown";
}

TetraProcess::~TetraProcess() { stop(); }

std::vector<std::string> TetraProcess::build_argv() const {
    std::vector<std::string> argv;
    argv.push_back(cfg_.tetra_rx_path);
    argv.push_back(cfg_.input_arg);
    for (const auto& a : cfg_.extra_args) argv.push_back(a);
    return argv;
}

bool TetraProcess::start(const TetraProcessConfig& cfg, EventCallback on_event, AudioCallback on_audio) {
    std::signal(SIGPIPE, SIG_IGN); // see dsd_process.cpp: a dead child must not kill us

    cfg_ = cfg;
    on_event_ = std::move(on_event);
    on_audio_ = std::move(on_audio);

    int stdin_pipe[2]; // [0]=read (child), [1]=write (us)
    if (pipe2(stdin_pipe, O_CLOEXEC) != 0) return false;

    int exec_pipe[2]; // child reports exec failure here; CLOEXEC -> EOF on success
    if (pipe2(exec_pipe, O_CLOEXEC) != 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        return false;
    }

    // TETMON receive socket: bind an ephemeral loopback port before forking
    // and learn which port we got, so we can hand it to the child via env.
    udp_fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (udp_fd_ < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(exec_pipe[0]); close(exec_pipe[1]);
        return false;
    }
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral
        if (bind(udp_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(udp_fd_); udp_fd_ = -1;
            close(stdin_pipe[0]); close(stdin_pipe[1]);
            close(exec_pipe[0]); close(exec_pipe[1]);
            return false;
        }
        sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        if (getsockname(udp_fd_, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
            close(udp_fd_); udp_fd_ = -1;
            close(stdin_pipe[0]); close(stdin_pipe[1]);
            close(exec_pipe[0]); close(exec_pipe[1]);
            return false;
        }
        tetmon_port_ = ntohs(bound.sin_port);
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(exec_pipe[0]); close(exec_pipe[1]);
        close(udp_fd_); udp_fd_ = -1;
        return false;
    }

    if (pid == 0) {
        // ---- child ----
        dup2(stdin_pipe[0], STDIN_FILENO);
        if (!cfg_.inherit_child_log) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }
        }
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(exec_pipe[0]); // keep exec_pipe[1]; execvp closes it via CLOEXEC
        close(udp_fd_);      // child sends via its own socket, not our bound one

        // The env the sq5bpf fork reads to decide where to send TETMON.
        setenv("TETRA_HACK_IP", "127.0.0.1", 1);
        char portbuf[16];
        std::snprintf(portbuf, sizeof(portbuf), "%u", static_cast<unsigned>(tetmon_port_));
        setenv("TETRA_HACK_PORT", portbuf, 1);
        char rxbuf[16];
        std::snprintf(rxbuf, sizeof(rxbuf), "%d", cfg_.rx_id);
        setenv("TETRA_HACK_RXID", rxbuf, 1);

        auto argv_strs = build_argv();
        std::vector<char*> argv_c;
        argv_c.reserve(argv_strs.size() + 1);
        for (auto& s : argv_strs) argv_c.push_back(const_cast<char*>(s.c_str()));
        argv_c.push_back(nullptr);

        execvp(argv_c[0], argv_c.data());
        int err = errno;
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
        close(udp_fd_); udp_fd_ = -1;
        tetmon_port_ = 0;
        std::fprintf(stderr, "dsd-server: cannot start '%s': %s\n",
                     cfg_.tetra_rx_path.c_str(), std::strerror(exec_errno));
        return false;
    }

    child_pid_ = pid;
    close(stdin_pipe[0]);
    stdin_fd_ = stdin_pipe[1];

    running_ = true;
    udp_thread_ = std::thread(&TetraProcess::udp_reader_loop, this);
    return true;
}

bool TetraProcess::write_bits(const unsigned char* bits, std::size_t n) {
    if (stdin_fd_ < 0) return false;
    std::lock_guard<std::mutex> lock(write_mutex_);
    std::size_t written = 0;
    while (written < n) {
        ssize_t w = ::write(stdin_fd_, bits + written, n - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false; // EPIPE if tetra-rx exited
        }
        written += static_cast<std::size_t>(w);
    }
    return true;
}

void TetraProcess::udp_reader_loop() {
    std::vector<char> buf(8192);
    while (running_.load() && udp_fd_ >= 0) {
        ssize_t n = ::recv(udp_fd_, buf.data(), buf.size(), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        // A datagram is normally one TETMON message; split on newlines to
        // be robust to a build that packs several, and skip blank lines.
        const std::string data(buf.data(), static_cast<std::size_t>(n));
        std::size_t pos = 0;
        while (pos < data.size()) {
            std::size_t nl = data.find('\n', pos);
            std::string line = data.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? data.size() : nl + 1;
            if (line.empty()) continue;
            DsdEvent ev = classify_tetmon_line(line);
            if (on_event_ && tetra_forward_event(ev, cfg_.forward_unknown))
                on_event_(ev);
        }
    }
}

void TetraProcess::stop() {
    running_.exchange(false);

    if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; } // EOF -> tetra-rx exits
    if (udp_fd_ >= 0) { shutdown(udp_fd_, SHUT_RDWR); }        // wake recv() (see dsd_process.cpp)

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

    // Join before closing the socket fd (see dsd_process.cpp stop() for why
    // this order is load-bearing under concurrent sessions).
    if (udp_thread_.joinable()) udp_thread_.join();
    if (udp_fd_ >= 0) { close(udp_fd_); udp_fd_ = -1; }
    tetmon_port_ = 0;
}

} // namespace dsdsrv
