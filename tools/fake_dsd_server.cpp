// fake_dsd_server.cpp
//
// Standalone runner for the FakeDsdServer in fake_dsd_server.hpp, so a
// test suite that isn't C++ (or prefers a subprocess to an in-process
// fixture) can spawn it and point a WebSocket client at it. C++ callers
// should prefer including the header directly and driving the server
// in-process — that gives direct access to the recorded control messages
// for assertions (see the header's usage note and test_fake_dsd_server).
//
// Behavior mirrors the real server's timing: on "start" it does the
// minimum — just the "started" reply, no events yet. Output only begins
// once the client streams IQ: the FIRST binary IQ frame triggers a sync
// event and a DMR call event (TG 150607, SRC 2222223, slot 2); every
// SUBSEQUENT IQ frame yields one 20 ms voice-audio frame. Every control
// frame the client sends is echoed to stdout as one line.
//
// Build:  g++ -std=c++17 -O2 -pthread tools/fake_dsd_server.cpp -o fake_dsd_server
// Run:    ./fake_dsd_server --port 8765 [--udp-port 46000]
//
//   --port <n>      listen port (default 8765; 0 = ephemeral, printed on start)
//   --udp-port <n>  value reported in "started".udp_audio_port (default 0 =
//                   mimic the DSDcc backend; non-zero mimics dsd-fme)

#include "fake_dsd_server.hpp"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>

using namespace fakedsd;

int main(int argc, char** argv) {
    FakeDsdServer::Options opts;
    opts.port = 8765;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) opts.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--udp-port" && i + 1 < argc) opts.started_udp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--help" || a == "-h") {
            std::printf("usage: %s [--port N] [--udp-port N]\n", argv[0]);
            return 0;
        }
    }

    std::signal(SIGPIPE, SIG_IGN);

    FakeDsdServer srv(opts);
    // "start" does the minimum: just the automatic "started" reply (below).
    // Echo control frames so a driving test can see them.
    srv.on_control = [](const ControlMessage& msg, FakeDsdServer&) {
        std::printf("control: %s\n", msg.raw_json.c_str());
        std::fflush(stdout);
    };
    // Output tracks IQ, like the real server: first IQ push -> acquire the
    // signal and report the call; every push after that -> a voice frame.
    srv.on_iq = [](std::size_t frame_index, std::size_t, FakeDsdServer& s) {
        if (frame_index == 0) {
            s.send_event(Event{}.k("sync").sl("2").cc("4")
                             .rw("(fake) sync acquired"));
            s.send_event(Event{}.k("call").tg("150607").src("2222223").sl("2")
                             .rw("(fake) SLOT 2 TGT=150607 SRC=2222223 Group Call"));
        } else {
            s.send_audio(std::vector<int16_t>(160, 0)); // one 20 ms 8 kHz frame
        }
    };

    uint16_t port = srv.start();
    std::printf("fake_dsd_server listening on ws://127.0.0.1:%u  (udp_audio_port=%u)\n",
                port, opts.started_udp_port);
    std::fflush(stdout);

    // Run until killed.
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
    return 0;
}
