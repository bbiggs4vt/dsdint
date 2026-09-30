// blue_writer.hpp
//
// Streams interleaved little-endian float32 I/Q to an attached-header MIDAS
// BLUE file (type 1000, format "CF"), the same layout the repo's tools read
// and write (tools/midas_ws_client.py, tools/make_test_bluefile.py). The IQ
// the client sends over the WebSocket is already CF data (I0,Q0,I1,Q1,… as
// little-endian float32), so it is written verbatim -- no conversion -- and
// only the 512-byte header's data_size is patched on close.
//
// Byte order: the header's "EEEI" rep is little-endian IEEE, matching the
// x86/ARM64 hosts this server runs on, so the numeric header fields are
// written as native memcpy. (A big-endian host would need byte-swapping;
// the server isn't built for one.)

#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

namespace dsdsrv {

class BlueFileWriter {
public:
    ~BlueFileWriter() { close(); }

    // Open `path` and write the placeholder header. sample_rate_hz sets
    // xdelta (1/fs). max_bytes caps the data section (0 = unlimited): once the
    // cap is reached writing stops (sample-aligned) and truncated() is set.
    bool open(const std::string& path, double sample_rate_hz, std::uint64_t max_bytes = 0) {
        f_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!f_) return false;
        path_ = path;
        max_bytes_ = max_bytes;
        bytes_ = 0;
        truncated_ = false;

        char hdr[512];
        std::memset(hdr, 0, sizeof hdr);
        std::memcpy(hdr + 0, "BLUE", 4);
        std::memcpy(hdr + 4, "EEEI", 4);                       // header rep (LE IEEE)
        std::memcpy(hdr + 8, "EEEI", 4);                       // data rep
        put_i32(hdr + 12, 0);                                  // attached header
        put_f64(hdr + 32, 512.0);                              // data_start
        put_f64(hdr + 40, 0.0);                                // data_size (patched on close)
        put_i32(hdr + 48, 1000);                               // type 1000 (1-D)
        std::memcpy(hdr + 52, "CF", 2);                        // complex float32
        put_f64(hdr + 256, 0.0);                               // xstart
        put_f64(hdr + 264, sample_rate_hz > 0.0 ? 1.0 / sample_rate_hz : 1.0); // xdelta
        f_.write(hdr, sizeof hdr);
        return static_cast<bool>(f_);
    }

    bool is_open() const { return const_cast<std::ofstream&>(f_).is_open(); }
    bool truncated() const { return truncated_; }
    std::uint64_t bytes() const { return bytes_; }
    const std::string& path() const { return path_; }

    // Append raw IQ bytes (interleaved LE float32). No-op once the cap is hit.
    void write(const void* data, std::size_t n) {
        if (!f_.is_open() || truncated_) return;
        if (max_bytes_ > 0 && bytes_ + n > max_bytes_) {
            std::uint64_t room = (bytes_ < max_bytes_) ? (max_bytes_ - bytes_) : 0;
            room -= room % 8;                                  // keep whole complex samples (8 B)
            if (room) {
                f_.write(static_cast<const char*>(data), static_cast<std::streamsize>(room));
                bytes_ += room;
            }
            truncated_ = true;
            return;
        }
        f_.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
        bytes_ += n;
    }

    // Patch data_size and close. Safe to call more than once.
    void close() {
        if (!f_.is_open()) return;
        f_.flush();
        f_.seekp(40, std::ios::beg);
        char b[8];
        put_f64(b, static_cast<double>(bytes_));
        f_.write(b, 8);
        f_.close();
    }

private:
    static void put_i32(char* p, std::int32_t v) { std::memcpy(p, &v, sizeof v); }
    static void put_f64(char* p, double v)        { std::memcpy(p, &v, sizeof v); }

    std::ofstream f_;
    std::string path_;
    std::uint64_t bytes_ = 0;
    std::uint64_t max_bytes_ = 0;
    bool truncated_ = false;
};

} // namespace dsdsrv
