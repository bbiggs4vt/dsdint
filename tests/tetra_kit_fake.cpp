// tetra_kit_fake.cpp
//
// Stand-in for tetra-kit's `decoder`, used by test_tetra_kit_process.cpp to
// exercise TetraKitProcess's fork/exec + dual-UDP machinery without the real
// decoder. It parses the -r / -t port arguments exactly as tetra-kit does,
// binds -r to receive the bitstream, sends two JSON reports (a call and a
// non-call) to -t, then waits for one bit datagram (or a short timeout) so
// the parent's write_bits is observed, and exits when signalled.

#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    int rport = 0, tport = 0;
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "-r") == 0) rport = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "-t") == 0) tport = std::atoi(argv[i + 1]);
    }
    if (rport == 0 || tport == 0) { std::fprintf(stderr, "fake decoder: missing -r/-t\n"); return 2; }

    // Bind -r to receive the bitstream (as the real decoder does).
    int rfd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in ra{};
    ra.sin_family = AF_INET;
    ra.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ra.sin_port = htons(static_cast<uint16_t>(rport));
    if (bind(rfd, reinterpret_cast<sockaddr*>(&ra), sizeof(ra)) != 0) {
        std::perror("fake decoder: bind -r");
        return 3;
    }

    // Send JSON reports to -t.
    int tfd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in ta{};
    ta.sin_family = AF_INET;
    ta.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ta.sin_port = htons(static_cast<uint16_t>(tport));
    auto send = [&](const char* m) {
        sendto(tfd, m, std::strlen(m), 0, reinterpret_cast<sockaddr*>(&ta), sizeof(ta));
    };
    send("{\"service\":\"CMCE\",\"pdu\":\"D-SETUP\",\"ssi\":1234567,\"usage marker\":3}");
    send("{\"service\":\"MM\",\"pdu\":\"D-LOCATION-UPDATE-ACCEPT\",\"ssi\":7}");

    // Wait for a bit datagram (proves write_bits reached us), with a timeout
    // so we don't hang if the parent sends nothing.
    timeval tv{2, 0};
    setsockopt(rfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[8192];
    (void)::recv(rfd, buf, sizeof(buf), 0);

    // Idle until the parent SIGTERMs us (default disposition terminates).
    for (;;) pause();
    return 0;
}
