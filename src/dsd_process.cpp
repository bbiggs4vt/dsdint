#include "dsd_process.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cctype>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <regex>
#include <sstream>
#include <iostream>

namespace dsdsrv {

namespace {
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
                on_event_(classify_line(line));
            }
        }
    }
    // Flush any trailing partial line on exit.
    std::string tail = strip_ansi(buf);
    if (!tail.empty() && on_event_) {
        on_event_(classify_line(tail));
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

namespace {

// Strip leading zeros from a decimal string ("04" -> "4", "0" -> "0").
std::string strip_leading_zeros(const std::string& s) {
    std::size_t nz = s.find_first_not_of('0');
    return (nz == std::string::npos) ? "0" : s.substr(nz);
}

// Uppercase a hex string in place ("bee0a" -> "BEE0A").
std::string upper_hex(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Tidy a D-STAR/YSF callsign or text field: collapse internal whitespace
// runs to a single space, trim the ends, drop non-printable bytes, and
// treat an all-'*' value (YSF's "unaddressed / group CQ" destination) as
// empty. Mirrors the DSDcc backend's tidy_cs so both backends present
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
    static const std::regex slot_re(R"((?:TS|Slot)[:=]?\s*(\d))", std::regex::icase);
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
    // P25 (dsd-fme formats: dsd_frame.c "NAC: %03X;" / "NAC/CC: %03llX;",
    // p25p1_hdu.c/ldu2.c "ALG ID: 0x%02X KEY ID: 0x%04X", plus the
    // "ALG: 0x.. KEY ID: 0x.." error form). NAC is the P25 network access
    // code (its color-code/RAN analog); ALG/KEY are the encryption ids.
    static const std::regex nac_re(R"(\bNAC(?:/CC)?:\s*([0-9A-Fa-f]+))", std::regex::icase);
    static const std::regex algid_re(R"(\bALG(?:\s*ID)?:\s*0x([0-9A-Fa-f]+))", std::regex::icase);
    static const std::regex keyid_re(R"(\bKEY(?:\s*ID)?:\s*0x([0-9A-Fa-f]+))", std::regex::icase);
    // P25 trunking system identity, in dsd-fme's two forms: colon
    // ("RFSS: 001; Site: 097;") and bracketed ("RFSS[001] SITE [091]
    // SYSID [715]", "WACN [BEE0A]") -- both verified against a real P25
    // control-channel capture.
    // The ":" or "[" delimiter is REQUIRED (a bare "RFSS" is captured
    // without it -- e.g. "Valid RFSS Connection" grabbed the 'C' of
    // "Connection", a valid hex digit).
    static const std::regex rfss_re(R"(\bRFSS\s*(?::\s*|\[\s*)([0-9A-Fa-f]+))", std::regex::icase);
    static const std::regex sysid_re(R"(\bSYS\s*ID\s*(?::\s*|\[\s*)([0-9A-Fa-f]+))", std::regex::icase);
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
    if (std::regex_search(line, m, src_re)) ev.source_id = strip_leading_zeros(m[1].str());
    if (std::regex_search(line, m, slot_bracket_re)) ev.slot = m[1].str();
    else if (std::regex_search(line, m, slot_re)) ev.slot = m[1].str();
    if (std::regex_search(line, m, cc_re)) {
        // dsd-fme zero-pads ("Color Code=04"); normalize to match the
        // DSDcc backend's bare decimal so clients see one format.
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

    // Assemble trunking/site detail into extra as key=value tokens.
    std::vector<std::string> tokens;
    if (std::regex_search(line, m, site_re))   tokens.push_back("site_code=" + m[1].str());
    if (std::regex_search(line, m, sys_re))     tokens.push_back("system_code=" + m[1].str());
    if (std::regex_search(line, m, loc_re))     tokens.push_back("location_id=" + m[1].str());
    if (std::regex_search(line, m, cat_re))     tokens.push_back("category=" + m[1].str());
    // DMR trunking (dsd-fme only; DSDcc doesn't decode CSBK payloads).
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
    if (std::regex_search(line, m, algid_re))   tokens.push_back("alg_id=" + m[1].str());
    if (std::regex_search(line, m, keyid_re))   tokens.push_back("key_id=" + m[1].str());
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
    const bool is_dstar = std::regex_search(line, dstar_ctx_re);
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
            // mapped to the same tokens the DSDcc backend emits.
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

    if (cs_call) ev.kind = "call"; // callsign call info takes precedence
    else if (std::regex_search(line, voice_re)) ev.kind = "voice";
    else if (std::regex_search(line, sync_re)) ev.kind = "sync";
    else if (!ev.talkgroup.empty() || !ev.source_id.empty()) ev.kind = "call";

    return ev;
}

} // namespace dsdsrv
