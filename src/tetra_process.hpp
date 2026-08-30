// tetra_process.hpp
//
// Manages one osmo-tetra (sq5bpf fork) `tetra-rx` child process for the
// TETRA decode path. It is the TETRA analog of DsdProcess, and mirrors it
// deliberately: same lifecycle (start / feed / stop / running), same
// fork/exec/pipe machinery and failure detection, emitting the same
// DsdEvent so session.cpp consumes TETRA events like any other backend's.
//
// The differences from DsdProcess are the two that TETRA forces:
//
//  * Input is the demodulated BITSTREAM, not discriminator PCM. The shared
//    π/4-DQPSK front end (src/tetra_demod.*) turns IQ into bits upstream;
//    this class just relays those bits (one unpacked bit per byte) to the
//    child's stdin, which is exactly what tetra-rx reads. Keeping the demod
//    outside the backend is intentional: both TETRA backends (this osmo one
//    and a future tetra-kit one) consume the same bitstream, so the demod is
//    shared, not per-backend. write_bits() is therefore the feed method,
//    where DsdProcess has write_audio().
//
//  * Events arrive over UDP, not stdout. The fork reports decoder activity
//    as "TETMON_begin FUNC:... TETMON_end" datagrams sent to
//    TETRA_HACK_IP:TETRA_HACK_PORT (receiver id TETRA_HACK_RXID) -- env vars
//    we set on the child. We bind that UDP socket before forking and parse
//    each datagram with classify_tetmon_line() (src/tetra_tetmon.*). The
//    child's own stdout/stderr is low-level debug logging we don't parse;
//    it is sent to /dev/null by default (set inherit_child_log to keep it).
//
// Voice: TETRA traffic audio is a separate per-call UDP stream in the
// telive setup and needs the ETSI codec; it is NOT wired here yet. The
// AudioCallback exists for interface parity and to reserve the seam.
//
// STATUS: the subprocess machinery is exercised end to end by
// tests/test_tetra_process.cpp against a small fake tetra-rx stub (the same
// approach as the fake DSD server) -- fork/exec, env, stdin pipe, and the
// UDP TETMON path are all covered without needing the real binary. Decoding
// a real TETRA signal end to end still needs the actual sq5bpf tetra-rx and
// a capture. Linux-specific (fork/exec/pipe/POSIX sockets).

#pragma once

#include "dsd_backend_types.hpp"
#include "tetra_tetmon.hpp"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>
#include <mutex>

namespace dsdsrv {

// Emit filter for the TETRA backend: whether a classified event is
// forwarded to the client. Everything with a recognized kind is forwarded;
// kind:"unknown" (AFCVAL PHY diagnostics, and any TETMON func we don't map)
// is forwarded only when forward_unknown is set. Free function so it is
// unit-testable alongside the parser, mirroring dsd_fme_forward_event.
bool tetra_forward_event(const DsdEvent& ev, bool forward_unknown);

struct TetraProcessConfig {
    // The sq5bpf osmo-tetra fork's receiver. Found on PATH by default.
    std::string tetra_rx_path = "tetra-rx";

    // The input the child opens to read the bitstream. tetra-rx open()s its
    // file argument; "/dev/stdin" makes it read the stdin pipe we feed. If
    // your build treats "-" as stdin instead, set it here.
    std::string input_arg = "/dev/stdin";

    // Extra raw args appended verbatim after input_arg (rarely needed).
    std::vector<std::string> extra_args;

    // Receiver id the fork stamps into its TETMON output (TETRA_HACK_RXID);
    // useful only when several receivers share one telive. 1 is the norm.
    int rx_id = 1;

    // Forward kind:"unknown" TETMON events (AFCVAL diagnostics + unmapped
    // funcs). Default false: suppress them, exactly as the dsd-fme backend
    // suppresses its banner noise. Recognized events are always forwarded.
    bool forward_unknown = false;

    // Send the child's stdout/stderr (verbose osmo debug logging) to the
    // server's stderr instead of /dev/null. Default false to keep logs
    // clean; set true when debugging the child itself.
    bool inherit_child_log = false;
};

class TetraProcess {
public:
    using EventCallback = std::function<void(const DsdEvent&)>;
    using AudioCallback = std::function<void(const int16_t* pcm, std::size_t n)>;

    TetraProcess() = default;
    ~TetraProcess();

    TetraProcess(const TetraProcess&) = delete;
    TetraProcess& operator=(const TetraProcess&) = delete;

    // Binds the TETMON UDP socket, forks tetra-rx with the env it needs, and
    // starts the UDP reader thread. Returns false (child reaped, everything
    // undone) on fork/exec/pipe/bind failure. on_audio is accepted for
    // interface parity but not yet invoked (see header note on voice).
    bool start(const TetraProcessConfig& cfg, EventCallback on_event, AudioCallback on_audio);

    // Relays demodulated bits (one unpacked bit per byte, 0x00/0x01) to the
    // child's stdin. Safe to call from one producer thread while the reader
    // thread runs. Returns false if the pipe is closed / write failed.
    bool write_bits(const unsigned char* bits, std::size_t n);

    // Closes stdin (EOF to tetra-rx), waits for exit, joins the reader
    // thread, closes the socket. Safe to call multiple times.
    void stop();

    bool running() const { return running_.load(); }

    // The bound TETMON UDP port (host byte order), for diagnostics/tests.
    // 0 before a successful start().
    uint16_t tetmon_port() const { return tetmon_port_; }

private:
    void udp_reader_loop();
    std::vector<std::string> build_argv() const;

    TetraProcessConfig cfg_;
    EventCallback on_event_;
    AudioCallback on_audio_;

    pid_t child_pid_ = -1;
    int stdin_fd_ = -1;  // write end, our side
    int udp_fd_ = -1;    // bound TETMON receive socket
    uint16_t tetmon_port_ = 0;

    std::thread udp_thread_;
    std::atomic<bool> running_{false};
    std::mutex write_mutex_;
};

} // namespace dsdsrv
