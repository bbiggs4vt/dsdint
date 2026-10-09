#include "dsd_process.hpp"
#include "child_fds.hpp"
#include "audio_quality.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <locale>
#include <regex>
#include <sstream>
#include <iostream>

namespace dsdsrv {

namespace {
// libstdc++ fills std::ctype<char>::narrow()'s per-char cache lazily, and
// std::regex's \b handling narrows through it -- so the first \b match on two
// dsd-fme reader threads at once races on that fill (benign, both write the
// same byte, but ThreadSanitizer reports it). Fill the cache once during
// static initialization, before any thread exists.
[[maybe_unused]] const bool g_ctype_narrow_warm = [] {
    const auto& ct = std::use_facet<std::ctype<char>>(std::locale());
    for (int c = 1; c < 256; ++c) ct.narrow(static_cast<char>(c), '\0');
    return true;
}();

// Removes ANSI escape sequences (CSI "\x1b[...<letter>" -- real dsd-fme
// colorizes its log with these) plus stray carriage returns, so parsing
// and the raw_line clients receive see clean text.
std::string strip_ansi(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '\x1b' && i + 1 < in.size() && in[i + 1] == '[') {
            i += 2;
            while (i < in.size() && !std::isalpha(static_cast<unsigned char>(in[i]))) ++i;
            continue; // also swallows the terminating letter
        }
        if (c == '\r') continue;
        out.push_back(c);
    }
    return out;
}
} // namespace

DsdProcess::~DsdProcess() { stop(); }

// ---------------------------------------------------------------------
// DSD-FME VERIFICATION NOTES
//
// This backend was originally written without access to dsd-fme; its
// command line and event parsing were educated guesses flagged as such.
// It has since been verified against real dsd-fme (lwvmobile/dsd-fme,
// commit 198f0ea) built from source and fed a real DMR discriminator
// capture (DSDcc's samples/dmr_it_8.dis). What that established:
//
//   - "-i -" (stdin input) works even though `dsd-fme -h` doesn't list
//     it: openAudioInDevice() in dsd_audio.c opens stdin via libsndfile
//     as raw S16LE mono at the wav rate (default 48000). Guessed right.
//   - "-f" + "d" was WRONG for DMR: dsd-fme's -f takes a letter where
//     'd' means D-STAR. DMR is 's' (TDMA BS/MS simplex; 'a' = auto).
//   - "-U host:port" was WRONG for audio: real dsd-fme's -U is the
//     RIGCTL TCP port (sscanf %d would have read "127" out of our
//     "127.0.0.1:..." and enabled rigctl). Decoded audio out is
//     "-o udp:host:port" -- raw headerless PCM, 8 kHz, and for the
//     DMR stereo mode ('s') it is STEREO interleaved (slot1 left,
//     slot2 right; 640-byte packets = 20 ms).
//   - With no "-o" at all, dsd-fme defaults to PulseAudio and EXITS at
//     startup when no Pulse daemon exists ("Connection refused") --
//     fatal in a server environment, hence the explicit "-o null" when
//     audio relay is off.
//   - dsd-fme logs exclusively to STDERR (stdout stays empty), so
//     start() folding the child's stderr into our stdout pipe is what
//     makes event reading work at all.
//   - Real event lines look like:
//       "20:37:20 Sync: +DMR   slot1  [SLOT2] | Color Code=04 | VC6"
//       " SLOT 2 TGT=19535 SRC=2222223 Group Call"
//     with ANSI color sequences embedded (stripped in
//     stdout_reader_loop before parsing). "TGT=" is why the talkgroup
//     regex accepts TGT as well as TG, and the bracketed "[SLOT2]" is
//     why classify_line prefers the bracketed slot marker (the bracket
//     marks the slot the current burst belongs to; a bare "slot1"
//     appears in every sync line regardless of which slot is active).
// ---------------------------------------------------------------------

std::vector<std::string> DsdProcess::build_argv() const {
    // Verified command line (see notes above):
    //   dsd-fme -i - -f s -o udp:127.0.0.1:PORT [extra_args...]
    //   dsd-fme -i - -f s -o null               [extra_args...]
    std::vector<std::string> argv;
    argv.push_back(cfg_.dsd_fme_path);
    argv.push_back("-i");
    argv.push_back("-");            // stdin: raw S16LE mono 48 kHz discriminator audio
    argv.push_back("-f");
    argv.push_back(cfg_.mode_flag); // "s" = DMR (see DsdProcessConfig)
    argv.push_back("-o");
    if (cfg_.udp_audio_port != 0) {
        argv.push_back("udp:127.0.0.1:" + std::to_string(cfg_.udp_audio_port));
    } else {
        argv.push_back("null"); // never let it default to Pulse -- see notes above
    }
    // -Z ("Log MBE/PDU Payloads to console") makes dsd-fme print the
    // reassembled data-PDU bytes -- the only way it emits a DMR short-data /
    // SMS body. The stdout reader turns a text-carrying PDU into a `message`
    // (see DmrPduTextCarry); the rest of -Z's output classifies as suppressed
    // "unknown", so this adds no client-visible noise.
    if (cfg_.decode_short_data) argv.push_back("-Z");
    for (const auto& a : cfg_.extra_args) argv.push_back(a);
    return argv;
}

bool DsdProcess::start(const DsdProcessConfig& cfg, EventCallback on_event, AudioCallback on_audio) {
    // Writing to a pipe whose reader died raises SIGPIPE, whose default
    // action TERMINATES THE PROCESS -- so without this, one crashed
    // dsd-fme child would take down the whole server, every session
    // included, the moment its session's next write_audio() ran. With
    // SIGPIPE ignored the write fails with EPIPE instead and
    // write_audio() reports it as the ordinary false return the callers
    // already handle. Process-wide and idempotent; nothing in this
    // server wants SIGPIPE's default (Asio sockets suppress it on their
    // own). Found the hard way: under QEMU user-mode emulation without
    // binfmt the child exec fails instantly, and every subprocess test
    // died of SIGPIPE -- the same fate a dsd-fme crash would inflict in
    // production.
    std::signal(SIGPIPE, SIG_IGN);

    cfg_ = cfg;
    on_event_ = std::move(on_event);
    on_audio_ = std::move(on_audio);

    int stdin_pipe[2];  // [0]=read (child), [1]=write (us)
    int stdout_pipe[2]; // [0]=read (us), [1]=write (child)
    // O_CLOEXEC matters here, and specifically because there is one
    // DsdProcess per Session and Sessions start concurrently: a plain
    // pipe() is inherited by EVERY subsequently-forked child, so
    // session B's dsd-fme would hold duplicates of session A's pipe
    // ends. Then closing A's stdin write end in stop() no longer
    // delivers EOF to A's child (B still holds the write end), and --
    // worse -- A's stdout pipe never hits EOF either, leaving A's
    // stdout_reader_loop blocked in read() and stop() deadlocked in
    // join(), taking A's strand thread with it. The concurrency test's
    // start/stop churn case reproduced exactly that hang. pipe2() is
    // atomic, so there's no window for a concurrent fork to slip
    // through between pipe() and a separate fcntl(FD_CLOEXEC). The
    // child's own copies are fine: dup2() onto the stdio fd numbers
    // clears the close-on-exec flag for the duplicates.
    if (pipe2(stdin_pipe, O_CLOEXEC) != 0) return false;
    if (pipe2(stdout_pipe, O_CLOEXEC) != 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        return false;
    }

    // Exec-status pipe: the classic trick for making fork/exec failure
    // visible to the caller. Both ends are O_CLOEXEC and the child
    // never dup2s the write end, so a SUCCESSFUL execvp closes it and
    // the parent's read() returns 0 (EOF). If execvp fails, the child
    // writes errno into the pipe before _exit, and the parent's read()
    // returns that instead. Without this, start() reported success for
    // a nonexistent dsd-fme (fork worked; the exec failure happened
    // where the parent couldn't see it), the client got "started" plus
    // a cryptic unknown-kind event, and the real error frame ("failed
    // to start DSD backend") was never sent.
    int exec_pipe[2];
    if (pipe2(exec_pipe, O_CLOEXEC) != 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return false;
    }

    // Optional UDP socket for decoded voice audio, bound before fork so we
    // know it's ready by the time the child starts sending to it.
    udp_fd_ = -1;
    if (cfg_.udp_audio_port != 0) {
        // SOCK_CLOEXEC for the same reason as the pipes above.
        udp_fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
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
        close(exec_pipe[0]); close(exec_pipe[1]);
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
        close(exec_pipe[0]); // keep exec_pipe[1]; execvp closes it via CLOEXEC
        if (udp_fd_ >= 0) close(udp_fd_); // child doesn't need our bound socket
        close_inherited_fds(exec_pipe[1]); // incl. other sessions' WebSocket sockets

        auto argv_strs = build_argv();
        std::vector<char*> argv_c;
        argv_c.reserve(argv_strs.size() + 1);
        for (auto& s : argv_strs) argv_c.push_back(const_cast<char*>(s.c_str()));
        argv_c.push_back(nullptr);

        execvp(argv_c[0], argv_c.data());
        // If execvp returns, it failed: report errno to the parent
        // through the exec pipe (and to any log reader via stderr).
        int err = errno;
        std::fprintf(stderr, "dsd-server: failed to exec '%s': %s\n",
                     argv_c[0], std::strerror(err));
        ssize_t unused = ::write(exec_pipe[1], &err, sizeof(err));
        (void)unused;
        _exit(127);
    }

    // ---- parent ----
    close(exec_pipe[1]);
    // Blocks only until the child either execs (CLOEXEC closes the pipe
    // -> EOF) or reports failure -- microseconds either way.
    int exec_errno = 0;
    ssize_t n = ::read(exec_pipe[0], &exec_errno, sizeof(exec_errno));
    close(exec_pipe[0]);
    if (n > 0) {
        // exec failed; the child has already _exit(127)ed. Reap it and
        // undo everything so the Session's start handler reports the
        // failure to the client instead of a phantom "started".
        int status = 0;
        waitpid(pid, &status, 0);
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        if (udp_fd_ >= 0) { close(udp_fd_); udp_fd_ = -1; }
        std::fprintf(stderr, "dsd-server: cannot start '%s': %s\n",
                     cfg_.dsd_fme_path.c_str(), std::strerror(exec_errno));
        return false;
    }

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
    // shutdown() (not close()!) is what wakes udp_reader_loop out of a
    // blocked recv(); the fd itself stays open until after the join
    // below.
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

    // Join the reader threads BEFORE closing their fds -- this order is
    // load-bearing, and the concurrency test's TSan build flagged the
    // old order (close first, join after) on both fds. Closing an fd a
    // thread is blocked reading does not wake that thread on Linux, so
    // close-then-join never sped anything up; what actually ends the
    // readers is EOF on the child's exit (stdout -- and the O_CLOEXEC
    // pipes in start() guarantee this process holds the only other
    // reference, so EOF is prompt) and the shutdown() above (UDP).
    // Worse, close-first frees the fd number for reuse while the reader
    // may not have entered read() yet, at which point the reader is
    // reading someone else's fd -- with concurrent sessions opening
    // sockets constantly, "someone else" is another session's pipe.
    if (stdout_thread_.joinable()) stdout_thread_.join();
    if (udp_thread_.joinable()) udp_thread_.join();

    if (stdout_fd_ >= 0) { close(stdout_fd_); stdout_fd_ = -1; }
    if (udp_fd_ >= 0) { close(udp_fd_); udp_fd_ = -1; }
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
            std::string line = strip_ansi(buf.substr(0, pos));
            buf.erase(0, pos + 1);
            // Skip blank lines too: real dsd-fme's log is full of them
            // (and of lines that are pure ANSI color churn, which
            // strip_ansi reduces to empty).
            if (!line.empty() && on_event_) {
                // A completed short-data PDU (its hex dump ends at this line)
                // becomes a `message`, emitted before this line's own event.
                if (cfg_.decode_short_data) {
                    if (auto mev = pdu_text_carry_.feed(line)) {
                        slot_carry_.apply(*mev);
                        on_event_(*mev);
                    }
                }
                DsdEvent ev = classify_line(line);
                // (an ARS PDU printed as text: binary, not a message -- the
                // decoded ARS message comes from its hex dump)
                if (cfg_.decode_short_data && ev.kind == "message" && pdu_text_carry_.binary_text(line)) {
                    ev.kind = "unknown";
                    ev.message.clear();
                }
                slot_carry_.apply(ev);   // stamp the burst's slot onto unmarked lines
                publish_active_slot(ev);
                if (dsd_fme_forward_event(ev, cfg_.forward_unknown)) on_event_(ev);
                else if (cfg_.on_suppressed) cfg_.on_suppressed(ev);
            }
        }
    }
    // Flush any trailing partial line on exit.
    std::string tail = strip_ansi(buf);
    if (!tail.empty() && on_event_) {
        if (cfg_.decode_short_data) {
            if (auto mev = pdu_text_carry_.feed(tail)) { slot_carry_.apply(*mev); on_event_(*mev); }
        }
        DsdEvent ev = classify_line(tail);
        slot_carry_.apply(ev);
        publish_active_slot(ev);
        if (dsd_fme_forward_event(ev, cfg_.forward_unknown)) on_event_(ev);
        else if (cfg_.on_suppressed) cfg_.on_suppressed(ev);
    }
    // End of stream: emit any text PDU still being accumulated.
    if (cfg_.decode_short_data && on_event_) {
        if (auto mev = pdu_text_carry_.flush()) { slot_carry_.apply(*mev); on_event_(*mev); }
    }
}

int voice_slot_of(const DsdEvent& ev) {
    if (ev.slot != "1" && ev.slot != "2") return 0;
    bool voice = ev.kind == "voice";
    if (!voice && ev.kind == "sync") {
        std::string up = ev.raw_line;
        for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        for (const char* v : {"| VC", "| VLC", "VOICE", "LDU", "HDU", "4V", "2V"})
            if (up.find(v) != std::string::npos) { voice = true; break; }
    }
    return voice ? ev.slot[0] - '0' : 0;
}

// Track which TDMA slot is carrying voice, for the audio's slot (the mono
// mix's channel, and the network explorer's per-call audio). Only voice
// moves it (voice_slot_of); every other line leaves the last value in place,
// so it stays put through the other slot's bursts and between a call's.
void DsdProcess::publish_active_slot(const DsdEvent& ev) {
    if (!cfg_.mono_follow_slot && !cfg_.on_slot_audio) return;
    if (const int s = voice_slot_of(ev)) active_slot_.store(s, std::memory_order_relaxed);
}

std::size_t stereo_to_mono_for_slot(const int16_t* pcm, std::size_t nsamp,
                                    int slot, std::vector<int16_t>& out) {
    // Interleaved L,R pairs -> one mono sample per pair. A stray trailing
    // sample (odd count -- shouldn't happen for stereo) is dropped rather
    // than misaligning the L/R phase.
    const std::size_t npairs = nsamp / 2;
    out.resize(npairs);
    for (std::size_t i = 0; i < npairs; ++i) {
        const int16_t l = pcm[2 * i];
        const int16_t r = pcm[2 * i + 1];
        if (slot == 1)      out[i] = l;
        else if (slot == 2) out[i] = r;
        else                out[i] = static_cast<int16_t>((static_cast<int>(l) + r) / 2);
    }
    return npairs;
}

void route_stereo_slots(const int16_t* pcm, std::size_t nsamp, int active_slot, std::vector<int16_t>& scratch,
                        const std::function<void(int, const int16_t*, std::size_t)>& out) {
    const std::size_t npairs = nsamp / 2;
    bool same = true;
    for (std::size_t i = 0; same && i < npairs; ++i) same = pcm[2 * i] == pcm[2 * i + 1];
    if (same) {
        stereo_to_mono_for_slot(pcm, nsamp, 1, scratch);
        out(active_slot == 1 || active_slot == 2 ? active_slot : 0, scratch.data(), scratch.size());
        return;
    }
    stereo_to_mono_for_slot(pcm, nsamp, 1, scratch);
    out(1, scratch.data(), scratch.size());
    stereo_to_mono_for_slot(pcm, nsamp, 2, scratch);
    out(2, scratch.data(), scratch.size());
}

void DsdProcess::udp_reader_loop() {
    // dsd-fme's decoded voice PCM here is 8000 Hz, 16-bit signed, and (in
    // the DMR "-f s" mode this backend uses) STEREO interleaved -- TDMA
    // slot 1 on the left channel, slot 2 on the right (verified against
    // real dsd-fme; see DsdProcessConfig::udp_audio_port).
    //
    // (The single-channel modes -- P25p1, NXDN, dPMR, D-STAR, YSF, EDACS --
    // send MONO packets; see DsdProcessConfig::stereo_audio.)
    //
    // With mono_follow_slot (the default) we collapse that to one mono
    // stream for on_audio: pick the channel for
    // whichever slot is currently active (published by the stdout reader),
    // or an (L+R) downmix while the active slot isn't known yet. Otherwise
    // the raw stereo interleave is relayed unchanged.
    std::vector<char> buf(8192);
    std::vector<int16_t> mono; // reused scratch for the deinterleaved output
    while (running_.load() && udp_fd_ >= 0) {
        ssize_t n = ::recv(udp_fd_, buf.data(), buf.size(), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        const int16_t* pcm = reinterpret_cast<const int16_t*>(buf.data());
        std::size_t nsamp = static_cast<std::size_t>(n) / sizeof(int16_t);
        const bool stereo = cfg_.stereo_audio();
        if (cfg_.on_slot_audio) {
            if (stereo) {
                route_stereo_slots(pcm, nsamp, active_slot_.load(std::memory_order_relaxed), mono, cfg_.on_slot_audio);
            } else {
                cfg_.on_slot_audio(0, pcm, nsamp);
            }
        }
        if (!on_audio_) continue;
        // The single-channel modes already send mono: relay it as is
        // (splitting it as stereo would keep every other sample).
        if (!cfg_.mono_follow_slot || !stereo) {
            on_audio_(pcm, nsamp);
            continue;
        }
        std::size_t npairs = stereo_to_mono_for_slot(
            pcm, nsamp, active_slot_.load(std::memory_order_relaxed), mono);
        on_audio_(mono.data(), npairs);
    }
}

namespace {

// Strip leading zeros from a decimal string ("04" -> "4", "0" -> "0").
std::string strip_leading_zeros(const std::string& s) {
    std::size_t nz = s.find_first_not_of('0');
    return (nz == std::string::npos) ? "0" : s.substr(nz);
}

// Lowercase a short token ("ARS" -> "ars").
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Uppercase a hex string in place ("bee0a" -> "BEE0A").
std::string upper_hex(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Tidy a D-STAR/YSF callsign or text field: collapse internal whitespace
// runs to a single space, trim the ends, drop non-printable bytes, and
// treat an all-'*' value (YSF's "unaddressed / group CQ" destination) as
// empty. Presents
// callsigns the same way (e.g. "F1ZIL  B" -> "F1ZIL B").
std::string tidy_callsign(const std::string& in) {
    std::string out;
    bool pending_space = false;
    bool all_star = true;
    for (char c : in) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc > 0x7e) c = ' ';
        if (c == ' ') {
            if (!out.empty()) pending_space = true;
        } else {
            if (pending_space) { out.push_back(' '); pending_space = false; }
            if (c != '*') all_star = false;
            out.push_back(c);
        }
    }
    if (out.empty() || all_star) return std::string();
    return out;
}

} // namespace

std::string decode_moto_ars(const std::string& hex_upper) {
    std::vector<unsigned char> b;
    for (std::size_t i = 0; i + 1 < hex_upper.size(); i += 2) {
        const int v = std::stoi(hex_upper.substr(i, 2), nullptr, 16);
        b.push_back(static_cast<unsigned char>(v));
    }
    // MNIS header: 1F 10 02 <dir> 33 <seq hi> <seq lo> (0x33 = ARS), then the PDU.
    if (b.size() < 10 || b[0] != 0x1F || b[4] != 0x33) return std::string();
    const std::size_t p = 7;
    const std::size_t len = (static_cast<std::size_t>(b[p]) << 8) | b[p + 1];
    if (len < 1 || p + 2 + len > b.size()) return std::string();
    const unsigned char hdr = b[p + 2];
    const int type = hdr & 0x0F;
    std::size_t i = p + 3 + ((hdr & 0x80) ? 1 : 0);         // skip the header extension byte
    const std::size_t end = p + 2 + len;
    // A length-prefixed ASCII string at i (advances i); false if malformed.
    auto str = [&](std::string& out) {
        if (i >= end) return false;
        const std::size_t n = b[i++];
        if (i + n > end) return false;
        out.clear();
        for (std::size_t k = 0; k < n; ++k) {
            const unsigned char c = b[i + k];
            if (c < 0x20 || c > 0x7E) return false;
            out += static_cast<char>(c);
        }
        i += n;
        return true;
    };
    switch (type) {
        case 0x0: {                                          // device registration (radio -> gateway)
            std::string dev, user;
            if (!str(dev) || dev.empty()) return std::string();
            std::string out = "ARS registration \xC2\xB7 radio " + dev;
            if (str(user) && !user.empty()) out += " \xC2\xB7 user " + user;
            return out;
        }
        case 0xF: return "ARS registration ACK";             // gateway -> radio
        case 0x1: return "ARS de-registration";
        case 0x4: return "ARS query";
        default:  return "ARS (type " + std::to_string(type) + ")";
    }
}

bool dsd_fme_forward_event(const DsdEvent& ev, bool forward_unknown) {
    return forward_unknown || ev.kind != "unknown";
}

std::string decode_dmr_pdu_text(const std::string& hex_upper) {
    // Hex string -> bytes. Any stray non-hex nibble (shouldn't happen: the
    // caller only accumulates validated hex) aborts cleanly.
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    if (hex_upper.size() < 2) return std::string();
    std::vector<unsigned char> b;
    b.reserve(hex_upper.size() / 2);
    for (std::size_t i = 0; i + 1 < hex_upper.size(); i += 2) {
        int hi = nib(hex_upper[i]), lo = nib(hex_upper[i + 1]);
        if (hi < 0 || lo < 0) return std::string();
        b.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    auto printable = [](unsigned char c) { return c >= 0x20 && c <= 0x7E; };

    // Longest UTF-16LE run: consecutive (printable, 0x00) code units. This is
    // how Motorola/Hytera TMS carries text; the null interleave makes a chance
    // match in binary header/CRC bytes very unlikely, so a run >= 2 is trusted.
    std::string best16, cur16;
    for (std::size_t i = 0; i + 1 < b.size(); i += 2) {
        if (printable(b[i]) && b[i + 1] == 0x00) cur16.push_back(static_cast<char>(b[i]));
        else { if (cur16.size() > best16.size()) best16 = cur16; cur16.clear(); }
    }
    if (cur16.size() > best16.size()) best16 = cur16;

    // Longest plain printable-ASCII run (ASCII / UTF-8 short data). Needs a
    // higher bar (>= 4) so a few incidental printable header bytes don't read
    // as a "message".
    std::string best8, cur8;
    for (unsigned char c : b) {
        if (printable(c)) cur8.push_back(static_cast<char>(c));
        else { if (cur8.size() > best8.size()) best8 = cur8; cur8.clear(); }
    }
    if (cur8.size() > best8.size()) best8 = cur8;

    const bool ok16 = best16.size() >= 2;
    const bool ok8 = best8.size() >= 4;
    if (ok16 && best16.size() >= best8.size()) return best16; // prefer UTF-16 text
    if (ok8) return best8;
    if (ok16) return best16;
    return std::string();
}

DsdEvent classify_dsd_fme_line(const std::string& line) {
    // Best-effort regex classification of dsd-fme's textual log output.
    // dsd-fme's log format varies by version/build flags, so treat these
    // as a starting point: run your dsd-fme interactively against a known
    // signal, capture real log lines, and tighten these patterns (or add
    // more) to match what you actually see.
    DsdEvent ev;
    ev.raw_line = line;
    ev.kind = "unknown";

    // Formats verified against real dsd-fme output (see the DSD-FME
    // VERIFICATION NOTES above build_argv), DMR:
    //   " SLOT 2 TGT=19535 SRC=2222223 Group Call"
    //   "20:37:20 Sync: +DMR   slot1  [SLOT2] | Color Code=04 | VC6"
    // and NXDN/IDAS (verified against a real off-air NXDN48 capture):
    //   "Sync: NXDN48  RTCH Voice  RAN 02 PF X/4"
    //   " Session Call - ... - Src=958 - Dst/TG=2043 - Prefix Ch: 3"
    //   "Site ID Message - Area: 0; Site Type: 8 Narrow; Site Code: 1 Open Access;"
    //   "Adjacent Information - Cat: Global - Sys Code: 8 - Site Code 2"
    //   "Service Information - Location ID [008002] SVC [01A8] RST [000000]"
    // TGT? because real dsd-fme writes "TGT=", not "TG=" (the old
    // TG-only regex silently never matched a real talkgroup); it also
    // matches NXDN's "Dst/TG=2043" and "TGT: 2043". The bracketed-slot
    // regex is tried first because DMR sync lines name BOTH slots
    // ("slot1  [SLOT2]") and the brackets mark the one the current burst
    // belongs to; matching a bare "slot" first would always report 1.
    static const std::regex tg_re(R"(TGT?[:=]?\s*(\d+))", std::regex::icase);
    static const std::regex src_re(R"((?:SRC|RID|Source)[:=]?\s*(\d+))", std::regex::icase);
    static const std::regex slot_bracket_re(R"(\[slot\s*(\d)\])", std::regex::icase);
    // \b before TS/Slot so it can't match the "TS" tail of P25 status tokens
    // (NETSTS, ADJSTS, RFSSSTS, UNITS, ...) followed by a channel number, which
    // would otherwise set a bogus slot on a P25 sync (P25 Phase 1 has no slots;
    // Phase 2 only 0/1). "TSBK" etc. never matched (no digit follows).
    static const std::regex slot_re(R"(\b(?:TS|Slot)[:=]?\s*(\d))", std::regex::icase);
    // "Colour/Color Code" (DMR) or "Channel Code" (dPMR) -- both are the
    // per-channel colour code, so both land in color_code.
    static const std::regex cc_re(R"((?:Colou?r|Channel)\s*Code[:=]?\s*(\d+))", std::regex::icase);
    // dsd-fme marks failed FEC/CRC checks inline in the affected line
    // (post-ANSI-strip): "CSBK (CRC ERR)", "CACH/Burst FEC ERR",
    // "SLOT 2 FLCO FEC ERR", "CACH/EMB ERR". Lift that into the
    // structured crc_error flag so clients can discount the same
    // event's other fields -- notably color_code: the one wrong
    // Color Code the reference capture produces sits on a line marked
    // "(FEC ERR)", so filtering on this flag removes it.
    static const std::regex err_re(R"((CRC|FEC|EMB)\s*ERR)", std::regex::icase);
    static const std::regex sync_re(R"(sync|no sync|nosync)", std::regex::icase);
    static const std::regex voice_re(R"(voice|ambe)", std::regex::icase);
    // NXDN-specific. RAN is the NXDN analog of DMR's color code (repeater
    // access number); it gets its own field. The trunking identity fields
    // are routed into `extra` as "; "-joined key=value tokens rather than
    // separate columns because, e.g., "Site Code" means the home site on
    // a Site ID line but an adjacent site on an Adjacent Information line
    // -- the accompanying `raw` disambiguates.
    static const std::regex ran_re(R"(\bRAN\s+(\d+))", std::regex::icase);
    static const std::regex site_re(R"(Site\s*Code:?\s*(\d+))", std::regex::icase);
    static const std::regex sys_re(R"(Sys(?:tem)?\s*Code:?\s*(\d+))", std::regex::icase);
    static const std::regex loc_re(R"(Location\s*ID\s*\[?\s*([0-9A-Fa-f]+)\s*\]?)", std::regex::icase);
    // Require a word boundary AND the colon: without them "Cat" matches
    // inside "Lo(cat)ion", pulling a bogus category out of "Location ID".
    static const std::regex cat_re(R"(\bCat(?:egory)?\s*:\s*([A-Za-z]+))", std::regex::icase);
    // DMR trunking (Con+/Cap+/Tier III) and LC fields. These formats come
    // from lwvmobile/dsd-fme's own printf strings (dmr_csbk.c, dmr_flco.c,
    // dsd_alias.c) -- the project has no trunking capture to exercise them
    // live, so they are pinned against those exact source formats in
    // tests/test_dsd_fme_parse.cpp rather than against a decode.
    static const std::regex netid_re(R"(Net\s*ID:\s*(\d+))", std::regex::icase);
    // Combined site regex: DMR "Site ID: N[.M]", P25 "Site: N" and the
    // bracketed "SITE [N]". The ":" or "[" delimiter is REQUIRED -- without
    // it, "Site Code" (NXDN, its own token) and prose like "Site active"
    // would false-match.
    static const std::regex siteid_re(R"(\bSite(?:\s*ID)?\s*(?::\s*|\[\s*)(\d+(?:\.\d+)?))", std::regex::icase);
    static const std::regex rest_re(R"(Rest\s*LSN:\s*(\d+))", std::regex::icase);
    static const std::regex lcn_re(R"(\bLP?CN:\s*(\d+))", std::regex::icase);
    // Emergency as a flag, but NOT the "Emergency: <timer>" / "Emergency =
    // <n>" value forms (a timer table and dPMR field), hence the negative
    // lookahead.
    static const std::regex emerg_re(R"(\bEmergency\b(?!\s*[:=]))", std::regex::icase);
    // dPMR prints the emergency bit as a value ("Emergency = 1"); the flag
    // form above deliberately skips "Emergency =", so match the set bit
    // explicitly here (and NOT "Emergency = 0").
    static const std::regex emerg_val_re(R"(\bEmergency\s*=\s*1\b)", std::regex::icase);
    // Talker alias text runs to end of line after "Alias: "; the colon
    // keeps it off "Alias CRC Error" / "Talker Alias LC Header" lines.
    static const std::regex alias_re(R"(\bAlias:\s*(\S.*?)\s*$)", std::regex::icase);
    // DMR short data / SMS text. dsd-fme renders decoded short-data and UDT
    // message bodies with an encoding-tagged label and the characters run to
    // end of line -- its own fprintf strings (dmr_pdu.c "UTF8 Text: ",
    // dmr_block.c "ISO7 Text: " / "ISO8 Text: " / "UTF16 Text: "). We capture
    // the whole tail as free text (it may embed spaces, ';', '='), which is
    // exactly why the message gets its own field rather than an `extra` token:
    // an SMS body would corrupt the "; "-joined key=value extra string.
    // Non-printable/padding bytes are rendered by dsd-fme as '_' (nulls) or
    // '-'/spaces, so trailing runs of those are trimmed below.
    static const std::regex sms_re(
        R"(\b(?:UTF-?8|UTF-?16|ISO\s?7|ISO\s?8)\s*Text:\s*(.*\S)?\s*$)", std::regex::icase);
    // P25 (dsd-fme formats: dsd_frame.c "NAC: %03X;" / "NAC/CC: %03llX;",
    // p25p1_hdu.c/ldu2.c "ALG ID: 0x%02X KEY ID: 0x%04X", plus the
    // "ALG: 0x.. KEY ID: 0x.." error form). NAC is the P25 network access
    // code (its color-code/RAN analog); ALG/KEY are the encryption ids --
    // DMR prints them without "0x" (dmr_pi.c "DMR PI H- ALG ID: %02X; KEY ID:
    // %02X;", dmr_flco.c "Slot %d Alg: %02X; KEY ID: %02X;"), NXDN as its
    // cipher name and a decimal key id (nxdn_element.c "DES - Key ID 3 - ").
    // Never "Key: <hex>": that is dsd-fme printing a loaded key's value.
    static const std::regex nac_re(R"(\bNAC(?:/CC)?:\s*([0-9A-Fa-f]+))", std::regex::icase);
    static const std::regex algid_re(R"(\bALG(?:\s*ID)?:\s*(?:0x)?([0-9A-Fa-f]{1,2})\b)", std::regex::icase);
    static const std::regex keyid_re(R"(\b(?:KEY\s*ID|KID):\s*(?:0x)?([0-9A-Fa-f]{1,4})\b)", std::regex::icase);
    static const std::regex nxdn_cipher_re(R"(\b(Scrambler|DES|AES)\s*-\s*Key ID (\d{1,3})\b)");
    // P25 trunking system identity, in dsd-fme's two forms: colon
    // ("RFSS: 001; Site: 097;") and bracketed ("RFSS[001] SITE [091]
    // SYSID [715]", "WACN [BEE0A]") -- both verified against a real P25
    // control-channel capture.
    // The ":" or "[" delimiter is REQUIRED (a bare "RFSS" is captured
    // without it -- e.g. "Valid RFSS Connection" grabbed the 'C' of
    // "Connection", a valid hex digit).
    static const std::regex rfss_re(R"(\bRFSS\s*(?::\s*|\[\s*)([0-9A-Fa-f]+))", std::regex::icase);
    // (also the sync line's "WACN: 580A0; SYS: 006;" -- a colon right after
    // SYS; NXDN's "Sys Code:" doesn't match)
    static const std::regex sysid_re(R"(\bSYS\s*(?:ID\s*)?(?::\s*|\[\s*)([0-9A-Fa-f]+))", std::regex::icase);
    static const std::regex wacn_re(R"(\bWACN\s*(?::\s*|\[\s*)([0-9A-Fa-f]+))", std::regex::icase);

    // D-STAR / YSF (callsign-based amateur protocols). dsd-fme reprints
    // the protocol's sync marker ("-DSTAR VOICE", "+YSF") on every frame
    // line at its default verbosity, so these callsign labels share a
    // line with the marker -- which is what makes it safe to key the
    // whole block on the marker's presence (see below) and thereby keep
    // these labels from ever firing on DMR/P25 numeric-id lines. The
    // values are fixed-width, space-padded callsigns (which may contain an
    // internal space, e.g. "F1ZIL  B" = callsign + module), so each field
    // is captured non-greedily up to the NEXT known label (or end of
    // line) and then tidied. Formats are dsd-fme's own fprintf strings
    // (src/dstar.c, src/ysf.c): D-STAR " RPT 2: %s RPT 1: %s DST: %s SRC:
    // %s"; YSF "DST: %s SRC: %s", "U/L: %s D/L: %s", "DST RID: %s SRC RID:
    // %s".
    static const std::regex dstar_ctx_re(R"(DSTAR)", std::regex::icase);
    // The D-STAR header's own shape, for builds that print it on the voice
    // frame's line (payload logging: " AMBE ... RPT 2: DIRECT RPT 1: DIRECT
    // DST: CQCQCQ SRC: ...") rather than after the "Sync: -DSTAR" marker.
    static const std::regex dstar_hdr_re(R"(\bRPT\s*2:.*\bRPT\s*1:.*\bDST:)", std::regex::icase);
    static const std::regex ysf_ctx_re(R"(\bYSF\b)", std::regex::icase);
    static const std::regex cs_src_re(
        R"(\bSRC:\s*(.*?)\s*(?:DST:|U/?L:|D/?L:|RM\d|DATA\b|REPEATER\b|INTERRUPTED\b|CONTROL\b|URGENT\b|$))",
        std::regex::icase);
    static const std::regex cs_dst_re(
        R"(\bDST:\s*(.*?)\s*(?:SRC:|U/?L:|D/?L:|RPT|DATA\b|REPEATER\b|$))",
        std::regex::icase);
    static const std::regex cs_rpt2_re(R"(RPT\s*2:\s*(.*?)\s*(?:RPT\s*1:|DST:|SRC:|$))", std::regex::icase);
    static const std::regex cs_rpt1_re(R"(RPT\s*1:\s*(.*?)\s*(?:DST:|SRC:|$))", std::regex::icase);
    static const std::regex dstar_text_re(R"(\bTEXT:\s*(\S.*?)\s*$)", std::regex::icase);
    static const std::regex ysf_ul_re(R"(\bU/?L:\s*(.*?)\s*(?:D/?L:|RM\d|$))", std::regex::icase);
    static const std::regex ysf_dl_re(R"(\bD/?L:\s*(.*?)\s*(?:RM\d|$))", std::regex::icase);
    static const std::regex ysf_dstrid_re(R"(\bDST\s*RID:\s*(\S+))", std::regex::icase);
    static const std::regex ysf_srcrid_re(R"(\bSRC\s*RID:\s*(\S+))", std::regex::icase);

    std::smatch m;
    // strip_leading_zeros normalizes P25's zero-padded "%08d" IDs (and is
    // a no-op on DMR/NXDN's unpadded ones).
    if (std::regex_search(line, m, tg_re)) ev.talkgroup = strip_leading_zeros(m[1].str());
    // DMR CSBK / data-header lines name the destination "Target: N" rather than
    // TG/TGT ("Preamble CSBK - Group Data - Source: 123 - Target: 1", "Slot 1
    // Data Header - Group - ... Source: 123 Target: 1"). Same meaning as TGT
    // (the dst id: a talkgroup, or a radio for an individual call), so use it
    // when no TG/TGT was found. \b keeps it off "unit_target"-style tokens.
    static const std::regex target_re(R"(\bTarget:?\s*(\d+))", std::regex::icase);
    if (ev.talkgroup.empty() && std::regex_search(line, m, target_re))
        ev.talkgroup = strip_leading_zeros(m[1].str());
    // P25 link control (LCW) names the talkgroup as "Group N": "LCW Encrypted
    // Circuit Priority 4 Group Voice Channel User - Group 1 Source 6746067".
    static const std::regex lcw_group_re(R"(Voice Channel User\b.*?\bGroup\s+(\d+))", std::regex::icase);
    if (ev.talkgroup.empty() && std::regex_search(line, m, lcw_group_re))
        ev.talkgroup = strip_leading_zeros(m[1].str());
    if (std::regex_search(line, m, src_re)) ev.source_id = strip_leading_zeros(m[1].str());
    // A Capacity Plus channel status or a Connect Plus grant names a slot
    // ("TS: 1") of the channel it reports on, not the burst it came in.
    static const std::regex other_ts_re(R"(Capacity Plus Channel Status|\bLCN\b)", std::regex::icase);
    if (std::regex_search(line, m, slot_bracket_re)) ev.slot = m[1].str();
    else if (std::regex_search(line, m, slot_re) && !std::regex_search(line, other_ts_re)) ev.slot = m[1].str();
    if (std::regex_search(line, m, cc_re)) {
        // dsd-fme zero-pads ("Color Code=04"); normalize to bare decimal.
        ev.color_code = strip_leading_zeros(m[1].str());
    }
    if (std::regex_search(line, m, ran_re)) ev.ran = strip_leading_zeros(m[1].str());
    if (std::regex_search(line, m, nac_re)) {
        ev.nac = m[1].str();
        for (char& c : ev.nac) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (std::regex_search(line, emerg_re)) ev.emergency = "1";
    else if (std::regex_search(line, emerg_val_re)) ev.emergency = "1";
    if (std::regex_search(line, m, alias_re)) ev.alias = m[1].str();
    if (std::regex_search(line, err_re)) ev.crc_error = "1";
    // A short-data "Text:" body. When the frame failed CRC, dsd-fme still
    // prints its (garbage) decoded bytes and appends a "(CRC ERR)"/"(FEC ERR)"
    // marker -- which err_re just set crc_error from. That body is untrustworthy
    // and its non-printable bytes come out as '_'/'-' placeholders, so don't
    // surface it as a message; the event still carries crc_error=1. Only a
    // CRC-clean body becomes a message.
    if (ev.crc_error != "1" && std::regex_search(line, m, sms_re) && m[1].matched) {
        std::string msg = m[1].str();
        // Defensive: strip a trailing decoder status marker if one ever slips
        // through in a form err_re didn't catch, so it never lands in the body.
        static const std::regex trailing_status_re(
            R"(\s*\(?\s*(?:CRC|FEC|EMB)\s*ERR\s*\)?\s*$)", std::regex::icase);
        msg = std::regex_replace(msg, trailing_status_re, "");
        // Trim leading whitespace and trailing padding: dsd-fme substitutes
        // '_' for null bytes and '-'/space for other non-printables, so a
        // short body in a fixed-width block is tail-padded with those.
        std::size_t b = msg.find_first_not_of(" \t");
        std::size_t e = msg.find_last_not_of(" \t_-");
        if (b != std::string::npos && e != std::string::npos && e >= b)
            ev.message = msg.substr(b, e - b + 1);
    }

    // Assemble trunking/site detail into extra as key=value tokens.
    std::vector<std::string> tokens;
    if (std::regex_search(line, m, site_re))   tokens.push_back("site_code=" + m[1].str());
    if (std::regex_search(line, m, sys_re))     tokens.push_back("system_code=" + m[1].str());
    if (std::regex_search(line, m, loc_re))     tokens.push_back("location_id=" + m[1].str());
    if (std::regex_search(line, m, cat_re))     tokens.push_back("category=" + m[1].str());
    // DMR trunking.
    if (line.find("Connect Plus") != std::string::npos)  tokens.push_back("network_type=con+");
    else if (line.find("Capacity Plus") != std::string::npos) tokens.push_back("network_type=cap+");
    if (std::regex_search(line, m, netid_re))   tokens.push_back("network_id=" + m[1].str());
    if (std::regex_search(line, m, siteid_re))  tokens.push_back("site_id=" + strip_leading_zeros(m[1].str()));
    if (std::regex_search(line, m, rest_re))    tokens.push_back("rest_channel=" + m[1].str());
    if (std::regex_search(line, m, lcn_re))     tokens.push_back("lcn=" + m[1].str());
    // P25 trunking system identity (rfss/system id/wacn; wacn+sysid hex).
    if (std::regex_search(line, m, rfss_re))    tokens.push_back("rfss=" + strip_leading_zeros(m[1].str()));
    if (std::regex_search(line, m, sysid_re))   tokens.push_back("system_id=" + upper_hex(m[1].str()));
    if (std::regex_search(line, m, wacn_re))    tokens.push_back("wacn=" + upper_hex(m[1].str()));
    // P25 encryption identifiers (bare hex; alg 0x80=clear, 0xAA=ADP, etc.;
    // key 0x0000=unencrypted). crc_error flags the FEC-ERR variants.
    // (Not a P25 supergroup's "SG: n; KEY: kkkk; ALG: aa;": that describes
    // the regroup, not this call.) NXDN: alg_id is the cipher type (1
    // scrambler, 2 DES, 3 AES), key_id the key id in hex.
    if (line.find("SG:") == std::string::npos) {
        if (std::regex_search(line, m, algid_re))   tokens.push_back("alg_id=" + m[1].str());
        if (std::regex_search(line, m, keyid_re))   tokens.push_back("key_id=" + m[1].str());
    }
    if (std::regex_search(line, m, nxdn_cipher_re)) {
        const std::string c = m[1].str();
        char kid[8];
        std::snprintf(kid, sizeof kid, "%02X", std::stoi(m[2].str()) & 0xFF);
        tokens.push_back(std::string("alg_id=") + (c == "Scrambler" ? "1" : c == "DES" ? "2" : "3"));
        tokens.push_back(std::string("key_id=") + kid);
    }
    // What a data call carries (svc=): its announcement (preamble CSBK), an
    // acknowledgement (Response Packet), a data packet, and -- from the
    // Motorola MNIS header after it -- which service (ARS registration, LRRP
    // location, ...; or the type number dsd-fme doesn't name).
    {
        static const std::regex mnis_re(R"(\bMNIS\s+([A-Za-z]{2,8})\s*;)");
        static const std::regex mnis_type_re(R"(MNIS\s+Type:\s*([0-9A-Fa-f]+))", std::regex::icase);
        const bool hdr = line.find("Data Header") != std::string::npos;
        if (line.find("Preamble CSBK") != std::string::npos && line.find("Data") != std::string::npos)
            tokens.push_back("svc=preamble");
        else if (hdr && line.find("Response Packet") != std::string::npos) tokens.push_back("svc=ack");
        else if (hdr && line.find("Delivery") != std::string::npos) tokens.push_back("svc=data");
        else if (std::regex_search(line, m, mnis_type_re)) tokens.push_back("svc=mnis:" + upper_hex(m[1].str()));
        else if (std::regex_search(line, m, mnis_re) && m[1].str() != "Type") tokens.push_back("svc=" + lower(m[1].str()));
        else if (line.find("LRRP") != std::string::npos) tokens.push_back("svc=lrrp");
    }
    // A position report (P25 LCW GPS, DMR LRRP / GPS): "gps=lat,lon" in
    // signed decimal degrees, from dsd-fme's "(39.03494, -76.98460)".
    if (line.find("Lat") != std::string::npos && line.find("Lon") != std::string::npos) {
        static const std::regex pair_re(R"(\(\s*(-?\d{1,2}\.\d+)\s*,\s*(-?\d{1,3}\.\d+)\s*\))");
        static const std::regex latlon_re(R"(Lat\w*\s*:?\s*(-?\d{1,2}\.\d+)[^NSns\d-]{0,4}([NSns])?.*?Lon\w*\s*:?\s*(-?\d{1,3}\.\d+)[^EWew\d-]{0,4}([EWew])?)");
        double lat = 0, lon = 0;
        bool ok = false;
        if (std::regex_search(line, m, pair_re)) {
            lat = std::stod(m[1].str()); lon = std::stod(m[2].str()); ok = true;
        } else if (std::regex_search(line, m, latlon_re)) {
            lat = std::stod(m[1].str()); lon = std::stod(m[3].str()); ok = true;
            if (m[2].matched && (m[2].str() == "S" || m[2].str() == "s") && lat > 0) lat = -lat;
            if (m[4].matched && (m[4].str() == "W" || m[4].str() == "w") && lon > 0) lon = -lon;
        }
        if (ok && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 && (lat != 0 || lon != 0)) {
            char b[48];
            std::snprintf(b, sizeof b, "gps=%.5f,%.5f", lat, lon);
            tokens.push_back(b);
        }
    }
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i) ev.extra += "; ";
        ev.extra += tokens[i];
    }

    // D-STAR / YSF callsign extraction, keyed on the line carrying the
    // protocol's sync marker (see the regex block above). Kept separate
    // from the numeric-id path: the callsign labels reuse "SRC:"/"DST:",
    // so a stray numeric match from the digit regexes above is cleared
    // first and only the callsign/RID logic below repopulates these
    // fields.
    bool cs_call = false;
    const bool is_dstar = std::regex_search(line, dstar_ctx_re) || std::regex_search(line, dstar_hdr_re);
    const bool is_ysf   = std::regex_search(line, ysf_ctx_re);
    if (is_dstar || is_ysf) {
        std::smatch cm;
        std::string src, dst, rpt1, rpt2, ul, dl, text, srcrid, dstrid;
        if (std::regex_search(line, cm, cs_src_re)) src = tidy_callsign(cm[1].str());
        if (std::regex_search(line, cm, cs_dst_re)) dst = tidy_callsign(cm[1].str());
        if (is_dstar) {
            if (std::regex_search(line, cm, cs_rpt2_re))    rpt2 = tidy_callsign(cm[1].str());
            if (std::regex_search(line, cm, cs_rpt1_re))    rpt1 = tidy_callsign(cm[1].str());
            if (std::regex_search(line, cm, dstar_text_re)) text = tidy_callsign(cm[1].str());
        }
        std::vector<std::string> cs_extra;
        if (is_ysf) {
            if (std::regex_search(line, cm, ysf_ul_re))     ul = tidy_callsign(cm[1].str());
            if (std::regex_search(line, cm, ysf_dl_re))     dl = tidy_callsign(cm[1].str());
            if (std::regex_search(line, cm, ysf_srcrid_re)) srcrid = tidy_callsign(cm[1].str());
            if (std::regex_search(line, cm, ysf_dstrid_re)) dstrid = tidy_callsign(cm[1].str());
            // FICH call mode / data type, from dsd-fme's textual markers,
            // mapped to stable tokens.
            if (line.find("Group/CQ") != std::string::npos)     cs_extra.push_back("call_mode=group_cq");
            else if (line.find("RID Mode") != std::string::npos) cs_extra.push_back("call_mode=radio_id");
            else if (line.find("Private") != std::string::npos)  cs_extra.push_back("call_mode=individual");
            if (line.find("V/D1") != std::string::npos)      cs_extra.push_back("data_type=vd1");
            else if (line.find("V/D2") != std::string::npos) cs_extra.push_back("data_type=vd2");
            else if (line.find("VWFR") != std::string::npos) cs_extra.push_back("data_type=voice_full");
        }
        if (is_dstar) {
            if (!rpt1.empty()) cs_extra.push_back("rpt1=" + rpt1);
            if (!rpt2.empty()) cs_extra.push_back("rpt2=" + rpt2);
            if (!text.empty()) cs_extra.push_back("radio_text=" + text);
        }
        if (!ul.empty())     cs_extra.push_back("uplink=" + ul);
        if (!dl.empty())     cs_extra.push_back("downlink=" + dl);
        if (!srcrid.empty()) cs_extra.push_back("src_rid=" + srcrid);
        if (!dstrid.empty()) cs_extra.push_back("dst_rid=" + dstrid);

        cs_call = !src.empty() || !dst.empty() || !rpt1.empty() || !rpt2.empty()
                  || !ul.empty() || !dl.empty() || !text.empty()
                  || !srcrid.empty() || !dstrid.empty();
        if (cs_call) {
            ev.source_id = src; // callsign, not a numeric id
            ev.talkgroup = dst;
            for (const auto& t : cs_extra) {
                if (!ev.extra.empty()) ev.extra += "; ";
                ev.extra += t;
            }
        }
    }

    // EDACS / ProVoice (dsd-fme -fh/-fH/-fe/-fE/-fp): trunking control + its
    // ProVoice digital voice. dsd-fme prints identifiers in "Label [N]"
    // brackets (some ProVoice lines in "Label: N" colon form) -- a different
    // shape from the DMR/P25/NXDN "TGT=/SRC=" the regexes above key on, so the
    // generic patterns don't populate these and a dedicated, context-gated
    // block does. Pinned to dsd-fme's edacs*/provoice.c fprintf formats (e.g.
    // "Group [%05d] Source [%08d] LCN [%02d]", "Digital Group Call",
    // "AFS [%03d]"); tested against those in tests/test_dsd_fme_parse.cpp, not
    // yet validated on a live EDACS/ProVoice signal (no capture in the tree).
    bool edacs_call = false;
    {
        static const std::regex ed_ctx_re(
            R"(\bLCN\s*[\[:]|\bAFS\b|\bCall(?:er|ee)\b|(?:Digital|Analog|Data)\s+(?:Group|System|I-|Individual))",
            std::regex::icase);
        if (std::regex_search(line, ed_ctx_re)) {
            static const std::regex ed_src_re(R"(\b(?:Source|Caller)\D{0,4}(\d+))", std::regex::icase);
            static const std::regex ed_grp_re(R"(\b(?:Group|Target|Callee)\D{0,4}(\d+))", std::regex::icase);
            static const std::regex ed_lcn_re(R"(\bLCN\D{0,4}(\d+))", std::regex::icase);
            static const std::regex ed_afs_re(R"(\bAFS\D{0,4}(\d+))", std::regex::icase);
            static const std::regex ed_lid_re(R"(\bLID\D{0,4}(\d+))", std::regex::icase);
            static const std::regex ed_sys_re(R"(\bSystem\s*ID\s*\[?\s*([0-9A-Fa-f]+))", std::regex::icase);
            std::smatch em;
            if (std::regex_search(line, em, ed_src_re)) ev.source_id = strip_leading_zeros(em[1].str());
            if (std::regex_search(line, em, ed_grp_re)) ev.talkgroup = strip_leading_zeros(em[1].str());
            std::vector<std::string> ed_extra;
            if (std::regex_search(line, em, ed_lcn_re)) ed_extra.push_back("lcn=" + strip_leading_zeros(em[1].str()));
            if (std::regex_search(line, em, ed_afs_re)) ed_extra.push_back("afs=" + strip_leading_zeros(em[1].str()));
            if (std::regex_search(line, em, ed_lid_re)) ed_extra.push_back("lid=" + strip_leading_zeros(em[1].str()));
            if (std::regex_search(line, em, ed_sys_re)) ed_extra.push_back("system_id=" + upper_hex(em[1].str()));
            for (const auto& t : ed_extra) {
                if (!ev.extra.empty()) ev.extra += "; ";
                ev.extra += t;
            }
            // A recognized call-grant line, or one that yielded an id, is a
            // call event (so "System All-Call"-type lines with no ids aren't
            // dropped as unknown).
            edacs_call = !ev.source_id.empty() || !ev.talkgroup.empty() ||
                         line.find("Call") != std::string::npos;
        }
    }

    if (cs_call) ev.kind = "call"; // callsign call info takes precedence
    else if (std::regex_search(line, voice_re)) ev.kind = "voice";
    else if (std::regex_search(line, sync_re)) ev.kind = "sync";
    else if (edacs_call) ev.kind = "call"; // EDACS/ProVoice trunking call
    else if (!ev.message.empty()) ev.kind = "message"; // a decoded SMS/short-data body
    else if (!ev.talkgroup.empty() || !ev.source_id.empty()) ev.kind = "call";

    // AMBE voice-frame detail for the quality analyzer, from dsd-fme's "-Z"
    // payload log: " AMBE F801A99F8CE080 err = [0] [0] ". The codeword's b0
    // pitch index classifies the frame (speech/silence/erasure/tone); the two
    // err numbers are the frame's FEC error counts. AMBE+2 only (DMR/NXDN/
    // P25p2); P25p1's IMBE prints a different marker and is left alone.
    static const std::regex ambe_re(R"(\bAMBE\s+([0-9A-Fa-f]+)\s+err\s*=\s*\[(\d+)\]\s*\[(\d+)\])",
                                    std::regex::icase);
    if (std::regex_search(line, m, ambe_re)) {
        ev.voice_b0 = VoiceQuality::b0_of(m[1].str());
        ev.voice_err = std::atoi(m[2].str().c_str()) + std::atoi(m[3].str().c_str());
    }

    return ev;
}

} // namespace dsdsrv
