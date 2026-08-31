// tetra_fake_rx.cpp
//
// A tiny stand-in for the sq5bpf osmo-tetra `tetra-rx`, used by
// test_tetra_process.cpp to exercise TetraProcess's subprocess + UDP
// machinery without the real decoder (the same idea as the fake DSD
// server). On start it reads the env TetraProcess sets -- TETRA_HACK_IP,
// TETRA_HACK_PORT, TETRA_HACK_RXID -- sends two TETMON datagrams to that
// UDP address (one call-control message, one AFC diagnostic), then drains
// stdin until EOF so the parent controls its lifetime exactly as it would
// the real child (stop() closes stdin -> EOF -> we exit).

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main() {
    const char* ip = std::getenv("TETRA_HACK_IP");
    const char* ps = std::getenv("TETRA_HACK_PORT");
    if (!ip) ip = "127.0.0.1";
    if (!ps) { std::fprintf(stderr, "fake tetra-rx: no TETRA_HACK_PORT\n"); return 2; }

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 3;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(std::atoi(ps)));
    inet_pton(AF_INET, ip, &a.sin_addr);

    auto send = [&](const char* m) {
        sendto(s, m, std::strlen(m), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    };
    // A call-control message (should be forwarded) and an AFC diagnostic
    // (kind "unknown", suppressed unless forward_unknown is set).
    // Real osmo TETMON funcs: DSETUPDEC is a call (forwarded); AFCVAL is a
    // diagnostic (kind "unknown", suppressed unless forward_unknown).
    send("TETMON_begin FUNC:DSETUPDEC IDX:3 SSI:1234567 SSI2:222 CID:5 NID:9 RX:1 TETMON_end");
    send("TETMON_begin FUNC:AFCVAL AFC:-2 RX:1 TETMON_end");

    // Drain stdin until EOF (parent closed its write end in stop()).
    char buf[4096];
    while (::read(STDIN_FILENO, buf, sizeof(buf)) > 0) { /* discard bits */ }
    return 0;
}
