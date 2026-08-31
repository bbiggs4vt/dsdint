// tetra_kit_process.cpp -- see tetra_kit_process.hpp. The fork/exec/failure
// machinery mirrors dsd_process.cpp / tetra_process.cpp (SIGPIPE ignore,
// O_CLOEXEC, the exec-status pipe, join-before-close in stop()); the only
// structural difference is dual UDP (bits out, JSON in) instead of stdin.

#include "tetra_kit_process.hpp"

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

bool tetrakit_forward_event(const DsdEvent& ev, bool forward_unknown) {
    return forward_unknown || ev.kind != "unknown";
}

TetraKitProcess::~TetraKitProcess() { stop(); }

namespace {
// Bind a loopback UDP socket to an ephemeral port and return {fd, port}.
// fd = -1 on failure. O_CLOEXEC set. If keep is false the socket is closed
// and only the port number is returned (used to reserve a free port for the
// child to bind) -- a tiny TOCTOU window on loopback, acceptable here.
struct BoundPort { int fd; uint16_t port; };
BoundPort bind_ephemeral(bool keep) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return {-1, 0};
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { close(fd); return {-1, 0}; }
    sockaddr_in got{};
    socklen_t gl = sizeof(got);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&got), &gl) != 0) { close(fd); return {-1, 0}; }
    const uint16_t port = ntohs(got.sin_port);
    if (!keep) { close(fd); return {-1, port}; }
    return {fd, port};
}
} // namespace

std::vector<std::string> TetraKitProcess::build_argv() const {
    std::vector<std::string> argv;
    argv.push_back(cfg_.decoder_path);
    argv.push_back("-r"); argv.push_back(std::to_string(bitstream_port_));
    argv.push_back("-t"); argv.push_back(std::to_string(json_port_));
    for (const auto& a : cfg_.extra_args) argv.push_back(a);
    return argv;
}

bool TetraKitProcess::start(const TetraKitProcessConfig& cfg, EventCallback on_event, AudioCallback on_audio) {
    std::signal(SIGPIPE, SIG_IGN);

    cfg_ = cfg;
    on_event_ = std::move(on_event);
    on_audio_ = std::move(on_audio);

    // JSON receive socket (we hold it bound; the decoder sends here via -t).
    BoundPort js = bind_ephemeral(/*keep=*/true);
    if (js.fd < 0) return false;
    json_fd_ = js.fd;
    json_port_ = js.port;

    // Reserve a free port for the bitstream; the decoder binds it via -r.
    BoundPort bs = bind_ephemeral(/*keep=*/false);
    if (bs.port == 0) { close(json_fd_); json_fd_ = -1; json_port_ = 0; return false; }
    bitstream_port_ = bs.port;

    // Our sending socket for the bitstream.
    bits_fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (bits_fd_ < 0) { close(json_fd_); json_fd_ = -1; json_port_ = 0; return false; }

    int exec_pipe[2];
    if (pipe2(exec_pipe, O_CLOEXEC) != 0) {
        close(json_fd_); json_fd_ = -1; close(bits_fd_); bits_fd_ = -1;
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(json_fd_); json_fd_ = -1; close(bits_fd_); bits_fd_ = -1;
        close(exec_pipe[0]); close(exec_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // ---- child ----
        if (!cfg_.inherit_child_log) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }
        }
        close(json_fd_);  // child uses its own sockets, not ours
        close(bits_fd_);
        close(exec_pipe[0]);

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
        close(json_fd_); json_fd_ = -1; json_port_ = 0;
        close(bits_fd_); bits_fd_ = -1; bitstream_port_ = 0;
        std::fprintf(stderr, "dsd-server: cannot start '%s': %s\n",
                     cfg_.decoder_path.c_str(), std::strerror(exec_errno));
        return false;
    }

    child_pid_ = pid;
    // Optional speech decoder (null unless a codec build supplies one).
    voice_ = make_tetra_voice_decoder();
    running_ = true;
    json_thread_ = std::thread(&TetraKitProcess::json_reader_loop, this);
    return true;
}

bool TetraKitProcess::write_bits(const unsigned char* bits, std::size_t n) {
    if (bits_fd_ < 0) return false;
    std::lock_guard<std::mutex> lock(write_mutex_);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(bitstream_port_);
    // Chunk into datagrams no larger than the decoder's UDP read buffer.
    // tetra-kit reads a fixed 1024 bytes per datagram and DISCARDS anything
    // beyond that (UDP truncation), so oversized datagrams silently drop most
    // of the bitstream and desync the decode -- found running a real capture
    // end to end through a protocol":"tetrakit" session. See bits_datagram_bytes.
    const std::size_t kChunk = cfg_.bits_datagram_bytes ? cfg_.bits_datagram_bytes : 1024;
    std::size_t off = 0;
    while (off < n) {
        const std::size_t m = std::min(kChunk, n - off);
        ssize_t w = ::sendto(bits_fd_, bits + off, m, 0,
                             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<std::size_t>(w);
    }
    return true;
}

void TetraKitProcess::json_reader_loop() {
    std::vector<char> buf(16384);
    while (running_.load() && json_fd_ >= 0) {
        ssize_t n = ::recv(json_fd_, buf.data(), buf.size(), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        const std::string data(buf.data(), static_cast<std::size_t>(n));
        std::size_t pos = 0;
        while (pos < data.size()) {
            std::size_t nl = data.find('\n', pos);
            std::string line = data.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? data.size() : nl + 1;
            if (line.empty()) continue;
            DsdEvent ev = classify_tetrakit_json(line);
            if (on_event_ && tetrakit_forward_event(ev, cfg_.forward_unknown))
                on_event_(ev);

            // Voice: on a codec build, pull the speech frame out of a traffic
            // report and synthesize PCM for the client. Skipped entirely when
            // no codec is present (voice_ is null), so there is no extraction
            // cost in an events-only build.
            if (voice_ && on_audio_) {
                std::vector<int16_t> frame;
                if (tetrakit_extract_speech_frame(line, frame)) {
                    std::vector<int16_t> pcm;
                    if (voice_->decode_frame(frame, pcm) && !pcm.empty())
                        on_audio_(pcm.data(), pcm.size());
                }
            }
        }
    }
}

void TetraKitProcess::stop() {
    running_.exchange(false);

    // No stdin EOF over a UDP input: signal the decoder to exit.
    if (child_pid_ > 0) kill(child_pid_, SIGTERM);
    if (json_fd_ >= 0) shutdown(json_fd_, SHUT_RDWR); // wake recv()

    if (child_pid_ > 0) {
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            pid_t r = waitpid(child_pid_, &status, WNOHANG);
            if (r == child_pid_) { child_pid_ = -1; break; }
            usleep(50 * 1000);
        }
        if (child_pid_ > 0) {
            kill(child_pid_, SIGKILL);
            waitpid(child_pid_, &status, 0);
            child_pid_ = -1;
        }
    }

    if (json_thread_.joinable()) json_thread_.join();
    if (json_fd_ >= 0) { close(json_fd_); json_fd_ = -1; }
    if (bits_fd_ >= 0) { close(bits_fd_); bits_fd_ = -1; }
    json_port_ = 0;
    bitstream_port_ = 0;
    voice_.reset(); // reader joined; safe to drop the decoder
}

} // namespace dsdsrv
