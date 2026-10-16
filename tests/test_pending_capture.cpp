// test_pending_capture.cpp
//
// Unit test for pending_capture.hpp: the deferred-decrypt capture writer and
// the directory store that backs the "Pending" panel. Covers the sidecar
// JSON round-trip, id validation, the writer's preroll flush + streaming (both
// the not-yet-wrapped and wrapped ring cases), and the store's list / remove /
// clear / size-cap eviction. Pure filesystem I/O, always runs.

#include "../src/pending_capture.hpp"

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace dsdsrv;
namespace fs = std::filesystem;

static int g_failures = 0;
static void check(bool c, const char* what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what);
    if (!c) ++g_failures;
}

static std::vector<int16_t> read_s16(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<int16_t> out(raw.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<int16_t>((static_cast<unsigned char>(raw[2 * i])) |
                                      (static_cast<unsigned char>(raw[2 * i + 1]) << 8));
    return out;
}

int main() {
    std::printf("test_pending_capture\n");

    const std::string dir = "test_pending_capture.tmp";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // ---- PendingMeta sidecar round-trip ----
    {
        PendingMeta m;
        m.id = "pend_123_7";
        m.session = 7;
        m.started_ms = 123;
        m.ended_ms = 456;
        m.mode = "1";
        m.protocol = "p25p1";
        m.center_freq_hz = 490762500;
        m.algkids = "AA:05,84:12";
        m.bytes = 96000;  // 48000 samples @ 48 kHz = 1.0 s
        m.complete = true;
        PendingMeta r = PendingMeta::from_json(m.sidecar_json());
        check(r.id == "pend_123_7", "sidecar: id round-trips");
        check(r.session == 7, "sidecar: session round-trips");
        check(r.started_ms == 123 && r.ended_ms == 456, "sidecar: timestamps round-trip");
        check(r.mode == "1" && r.protocol == "p25p1", "sidecar: mode/protocol round-trip");
        check(r.center_freq_hz == 490762500, "sidecar: center freq round-trips");
        check(r.algkids == "AA:05,84:12", "sidecar: algkids round-trip");
        check(r.complete, "sidecar: complete flag round-trips");
        check(std::abs(m.seconds() - 1.0) < 1e-6, "seconds() = bytes/2/rate");
    }

    // ---- id validation ----
    check(PendingStore::valid_id("pend_123_7"), "valid_id: accepts a normal id");
    check(!PendingStore::valid_id(""), "valid_id: rejects empty");
    check(!PendingStore::valid_id("../etc/passwd"), "valid_id: rejects path traversal");
    check(!PendingStore::valid_id("a/b"), "valid_id: rejects a slash");
    check(!PendingStore::valid_id(".hidden"), "valid_id: rejects a dot");

    // ---- writer: preroll flush (ring not wrapped) + streaming ----
    {
        PendingCapture cap;
        std::vector<int16_t> pre(1000);
        for (int i = 0; i < 1000; ++i) pre[i] = static_cast<int16_t>(i);  // 0..999 before arming
        cap.feed(pre.data(), pre.size());
        check(!cap.is_open(), "writer: not open before arm");
        const std::string path = PendingStore::s16_path(dir, "pend_a");
        check(cap.arm(path), "writer: arm() succeeds the first time");
        check(!cap.arm(path), "writer: arm() is idempotent (second call no-op)");
        std::vector<int16_t> post(500);
        for (int i = 0; i < 500; ++i) post[i] = static_cast<int16_t>(2000 + i);
        cap.feed(post.data(), post.size());  // opens the file, flushes preroll, then streams
        const std::uint64_t bytes = cap.finish();
        check(bytes == (1000 + 500) * 2, "writer: wrote preroll + streamed audio");
        auto got = read_s16(path);
        check(got.size() == 1500, "writer: file has all samples in order");
        check(got.size() == 1500 && got[0] == 0 && got[999] == 999 && got[1000] == 2000 && got[1499] == 2499,
              "writer: preroll precedes streamed, both in order");
    }

    // ---- writer: preroll ring wraps (only the last kPrerollSamples kept) ----
    {
        PendingCapture cap;
        const std::size_t extra = 10;
        std::vector<int16_t> pre(PendingCapture::kPrerollSamples + extra);
        for (std::size_t i = 0; i < pre.size(); ++i) pre[i] = static_cast<int16_t>(i & 0x7fff);
        cap.feed(pre.data(), pre.size());             // overfills the ring by `extra`
        const std::string path = PendingStore::s16_path(dir, "pend_b");
        cap.arm(path);
        int16_t one = 12345;
        cap.feed(&one, 1);                            // opens + flushes the (wrapped) ring, then streams one
        const std::uint64_t bytes = cap.finish();
        check(bytes == (PendingCapture::kPrerollSamples + 1) * 2, "writer: wrapped ring keeps exactly kPrerollSamples + stream");
        auto got = read_s16(path);
        check(got.size() == PendingCapture::kPrerollSamples + 1, "writer: wrapped file size correct");
        // Oldest retained sample is index `extra` (the first `extra` were evicted).
        check(!got.empty() && got[0] == static_cast<int16_t>(extra & 0x7fff), "writer: oldest `extra` samples evicted");
        check(got.back() == 12345, "writer: streamed sample is last");
    }

    // ---- writer: finish() with nothing fed writes no file ----
    {
        PendingCapture cap;
        check(cap.finish() == 0, "writer: finish with no audio returns 0 bytes");
    }

    // ---- store: list / remove / clear ----
    {
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        auto make = [&](const std::string& id, std::int64_t started, std::uint64_t bytes, const std::string& algk) {
            PendingMeta m; m.id = id; m.started_ms = started; m.bytes = bytes; m.algkids = algk;
            m.mode = "1"; m.protocol = "p25p1"; m.complete = true;
            PendingStore::write_sidecar(dir, m);
            std::ofstream f(PendingStore::s16_path(dir, id), std::ios::binary);
            std::vector<char> z(bytes, 0);
            f.write(z.data(), static_cast<std::streamsize>(z.size()));
        };
        make("pend_100_1", 100, 2000, "AA:05");
        make("pend_300_2", 300, 4000, "84:12");
        make("pend_200_3", 200, 1000, "");

        auto v = PendingStore::list(dir);
        check(v.size() == 3, "store: list finds all three captures");
        check(v.size() == 3 && v[0].started_ms == 100 && v[2].started_ms == 300, "store: list sorted oldest-first");
        check(v.size() == 3 && v[0].bytes == 2000, "store: list reads real .s16 size");

        const std::string j = PendingStore::list_json(dir);
        check(j.front() == '[' && j.back() == ']', "store: list_json is an array");
        check(j.find("\"pend_300_2\"") < j.find("\"pend_100_1\""), "store: list_json is newest-first");
        check(j.find("[\"AA:05\"]") != std::string::npos, "store: list_json splits algkids into an array");

        check(PendingStore::remove(dir, "pend_200_3"), "store: remove() deletes a capture");
        check(!fs::exists(PendingStore::s16_path(dir, "pend_200_3")), "store: removed .s16 is gone");
        check(!fs::exists(PendingStore::json_path(dir, "pend_200_3")), "store: removed sidecar is gone");
        check(!PendingStore::remove(dir, "pend_nope"), "store: remove() of a missing id is false");
        check(!PendingStore::remove(dir, "../x"), "store: remove() rejects a bad id");
        check(PendingStore::list(dir).size() == 2, "store: two captures remain after remove");

        auto g = PendingStore::clear(dir);
        check(g.first == 2, "store: clear() removes the rest");
        check(PendingStore::list(dir).empty(), "store: empty after clear");
    }

    // ---- store: size-cap eviction (oldest first) ----
    {
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        auto make = [&](const std::string& id, std::int64_t started, std::uint64_t bytes) {
            PendingMeta m; m.id = id; m.started_ms = started; m.bytes = bytes; m.complete = true;
            PendingStore::write_sidecar(dir, m);
            std::ofstream f(PendingStore::s16_path(dir, id), std::ios::binary);
            std::vector<char> z(bytes, 0);
            f.write(z.data(), static_cast<std::streamsize>(z.size()));
        };
        make("pend_1_a", 1, 1000);
        make("pend_2_b", 2, 1000);
        make("pend_3_c", 3, 1000);
        PendingStore::enforce(dir, 2500);  // keep <=2500 bytes: evicts the oldest (1000) -> 2000 remains
        auto v = PendingStore::list(dir);
        check(v.size() == 2, "store: enforce evicted the oldest capture");
        check(v.size() == 2 && v[0].started_ms == 2, "store: enforce kept the newest");
        PendingStore::enforce(dir, 0);  // 0 = no cap, nothing removed
        check(PendingStore::list(dir).size() == 2, "store: enforce(0) is a no-op");
    }

    fs::remove_all(dir, ec);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
