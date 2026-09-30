// test_blue_writer.cpp
//
// Unit test for BlueFileWriter (src/blue_writer.hpp): writes a small
// attached-header MIDAS BLUE (type 1000, CF) file and checks the header
// fields, the patched data_size, the IQ round-trips verbatim, and the size
// cap truncates on a sample boundary. Pure filesystem I/O, always runs.

#include "../src/blue_writer.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace dsdsrv;

static int g_failures = 0;
static void check(bool c, const char* what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what);
    if (!c) ++g_failures;
}

static std::vector<char> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static double f64(const char* p) { double v; std::memcpy(&v, p, 8); return v; }
static std::int32_t i32(const char* p) { std::int32_t v; std::memcpy(&v, p, 4); return v; }

int main() {
    std::printf("test_blue_writer\n");

    const std::string path = "test_blue_writer.tmp.blue";
    std::remove(path.c_str());

    // Four complex float samples (8 floats).
    float iq[8] = {1.0f, -1.0f, 0.5f, 0.25f, -0.5f, 0.0f, 2.0f, -2.0f};

    {
        BlueFileWriter w;
        check(w.open(path, 32000.0), "open() succeeds");
        w.write(iq, sizeof iq);
        check(w.bytes() == sizeof iq, "bytes() counts the written payload");
        check(!w.truncated(), "not truncated under no cap");
        w.close();
    }

    auto buf = slurp(path);
    check(buf.size() == 512 + sizeof iq, "file is header + payload");
    check(std::memcmp(buf.data() + 0, "BLUE", 4) == 0, "magic 'BLUE'");
    check(std::memcmp(buf.data() + 4, "EEEI", 4) == 0, "header rep 'EEEI'");
    check(i32(buf.data() + 48) == 1000, "type 1000");
    check(std::memcmp(buf.data() + 52, "CF", 2) == 0, "format 'CF' (complex float)");
    check(f64(buf.data() + 32) == 512.0, "data_start = 512");
    check(f64(buf.data() + 40) == double(sizeof iq), "data_size patched to payload size");
    check(f64(buf.data() + 264) == 1.0 / 32000.0, "xdelta = 1/fs");
    // Payload round-trips verbatim.
    check(std::memcmp(buf.data() + 512, iq, sizeof iq) == 0, "IQ payload written verbatim");

    // ---- size cap truncates on a sample boundary ----
    {
        const std::string cpath = "test_blue_writer_cap.tmp.blue";
        std::remove(cpath.c_str());
        BlueFileWriter w;
        // Cap at 20 bytes -> only 16 bytes (2 complex samples) fit.
        w.open(cpath, 8000.0, /*max_bytes=*/20);
        w.write(iq, sizeof iq);         // 32 bytes offered
        check(w.truncated(), "cap: truncated flag set");
        check(w.bytes() == 16, "cap: stops at a sample boundary (16 B, not 20)");
        w.write(iq, sizeof iq);         // further writes are no-ops
        check(w.bytes() == 16, "cap: further writes are ignored");
        w.close();
        auto cb = slurp(cpath);
        check(cb.size() == 512 + 16, "cap: file has the capped payload");
        check(f64(cb.data() + 40) == 16.0, "cap: data_size reflects the capped bytes");
        std::remove(cpath.c_str());
    }

    std::remove(path.c_str());

    if (g_failures == 0) {
        std::printf("\nALL BLUE WRITER TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
