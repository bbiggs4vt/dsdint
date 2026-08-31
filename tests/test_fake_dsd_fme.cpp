// test_fake_dsd_fme.cpp
//
// Stand-in for dsd-fme, used only by test_session.cpp. Mirrors the shape
// of the throwaway Python script used earlier in this project's
// development to test dsd_process.cpp in isolation, but written in C++
// so the test suite doesn't depend on Python being installed on
// whatever machine runs `make test` / `ctest`.
//
// Behavior: echoes its argv as the first stdout line (so a test could
// verify DsdProcess::build_argv() if it wanted to), optionally sends a
// small fake PCM packet to a UDP port given via "-o udp:host:port" (the
// real dsd-fme's audio-output syntax, which build_argv now emits), and
// prints a fake event line to stdout every time it reads a chunk from
// stdin --
// enough to exercise DsdProcess's stdin-write / stdout-read / UDP-read
// plumbing without needing a real DSD binary or a real DMR signal.
//
// Built as its own executable (see CMakeLists.txt's test_session target)
// and located via PATH at test runtime under the literal name "dsd-fme"
// (DsdProcessConfig::dsd_fme_path's default), the same way the real
// dsd-fme would be.

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::string args;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) args += ' ';
        args += argv[i];
    }
    std::printf("ARGS:%s\n", args.c_str());
    std::fflush(stdout);

    // Parse the audio output the way real dsd-fme does: "-o" followed by
    // "udp:host:port" (or "null" for no audio, which we honor by simply
    // not sending any).
    int udp_port = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-o" && i + 1 < argc) {
            std::string dev = argv[i + 1];
            if (dev.rfind("udp:", 0) == 0) {
                auto last_colon = dev.rfind(':');
                if (last_colon != std::string::npos && last_colon > 3) {
                    udp_port = std::atoi(dev.c_str() + last_colon + 1);
                }
            }
        }
    }

    if (udp_port > 0) {
        int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(static_cast<uint16_t>(udp_port));
            int16_t fake_pcm[4] = {100, -100, 200, -200};
            ::sendto(sock, fake_pcm, sizeof(fake_pcm), 0,
                     reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            ::close(sock);
        }
    }

    std::size_t total_bytes = 0;
    bool sent_sms = false;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        total_bytes += static_cast<std::size_t>(n);
        std::printf("TG=12345 SRC=6789 TS=1 voice sync total=%zu\n", total_bytes);
        // Emit one DMR short-data / SMS line (real dsd-fme UDT format) so the
        // session test can verify the message field reaches the client.
        if (!sent_sms) {
            std::printf("Slot 1 - SRC: 6789; TGT: 12345; UDT ISO7 Text: HELLO WORLD\n");
            sent_sms = true;
        }
        std::fflush(stdout);
    }

    std::printf("EOF received, total_bytes=%zu\n", total_bytes);
    std::fflush(stdout);
    return 0;
}
