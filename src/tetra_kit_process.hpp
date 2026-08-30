// tetra_kit_process.hpp
//
// Manages one tetra-kit `decoder` child process: the second TETRA backend,
// selected at build time via TETRA_USE_TETRAKIT_BACKEND (see
// tetra_backend_selector.hpp). It presents the exact same surface as the osmo
// TetraProcess -- start / write_bits / stop / running, emitting the shared
// DsdEvent -- so session.cpp drives either without change; only the transport
// differs.
//
// tetra-kit's decoder is UDP on both sides (verified from its -h):
//   -r <port>  receive the demodulated bitstream (unpacked, one bit per byte
//              without -P -- exactly what src/tetra_demod.* produces)
//   -t <port>  send JSON reports (one object per line) for each decoded PDU
//
// So, unlike the osmo backend (bits on stdin, events on UDP), this backend:
//   * binds a loopback socket for the JSON reports and passes its port as -t;
//   * picks a free loopback port for the bitstream, passes it as -r, and
//     sendto()s the bits there (the decoder binds -r to receive them);
//   * has NO stdin pipe. The decoder is stopped with SIGTERM (there is no
//     stdin EOF to trigger a clean exit over a UDP input).
// JSON reports are parsed with classify_tetrakit_json (src/tetra_kit_json.*).
//
// Voice: tetra-kit carries speech as zlib+base64 inside the same JSON; it is
// not decoded to PCM here yet (that needs the ETSI codec). The AudioCallback
// exists for interface parity.
//
// STATUS: the subprocess + dual-UDP machinery is exercised end to end by
// tests/test_tetra_kit_process.cpp against a fake decoder stub, like the osmo
// backend. A real decode still needs the actual tetra-kit `decoder` on PATH
// and a capture. Linux-specific (fork/exec/POSIX sockets).

#pragma once

#include "dsd_backend_types.hpp"
#include "tetra_kit_json.hpp"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>
#include <mutex>

namespace dsdsrv {

// Emit filter: forward everything with a recognized kind; kind:"unknown"
// (non-call/voice PDUs) only when forward_unknown is set. Free function,
// unit-testable, mirroring the osmo backend's tetra_forward_event.
bool tetrakit_forward_event(const DsdEvent& ev, bool forward_unknown);

struct TetraKitProcessConfig {
    std::string decoder_path = "decoder"; // tetra-kit's decoder, on PATH
    std::vector<std::string> extra_args;  // appended verbatim after -r/-t
    bool forward_unknown = false;         // forward kind:"unknown" reports too
    bool inherit_child_log = false;       // send child stdout/stderr to ours vs /dev/null
};

class TetraKitProcess {
public:
    using EventCallback = std::function<void(const DsdEvent&)>;
    using AudioCallback = std::function<void(const int16_t* pcm, std::size_t n)>;

    TetraKitProcess() = default;
    ~TetraKitProcess();

    TetraKitProcess(const TetraKitProcess&) = delete;
    TetraKitProcess& operator=(const TetraKitProcess&) = delete;

    // Binds the JSON socket, picks the bitstream port, forks the decoder with
    // -r/-t, and starts the JSON reader thread. Returns false (child reaped,
    // everything undone) on failure. on_audio is accepted for parity, not yet
    // invoked.
    bool start(const TetraKitProcessConfig& cfg, EventCallback on_event, AudioCallback on_audio);

    // Send demodulated bits (one unpacked bit per byte) to the decoder over
    // UDP, chunked into datagrams. Safe from one producer thread. Returns
    // false if the socket is gone.
    bool write_bits(const unsigned char* bits, std::size_t n);

    // SIGTERM the decoder, join the reader thread, close sockets. Idempotent.
    void stop();

    bool running() const { return running_.load(); }

    uint16_t json_port() const { return json_port_; }        // -t (we receive)
    uint16_t bitstream_port() const { return bitstream_port_; } // -r (we send)

private:
    void json_reader_loop();
    std::vector<std::string> build_argv() const;

    TetraKitProcessConfig cfg_;
    EventCallback on_event_;
    AudioCallback on_audio_;

    pid_t child_pid_ = -1;
    int json_fd_ = -1;      // bound; receives JSON reports (-t)
    int bits_fd_ = -1;      // sends the bitstream to the decoder (-r)
    uint16_t json_port_ = 0;
    uint16_t bitstream_port_ = 0;

    std::thread json_thread_;
    std::atomic<bool> running_{false};
    std::mutex write_mutex_;
};

} // namespace dsdsrv
