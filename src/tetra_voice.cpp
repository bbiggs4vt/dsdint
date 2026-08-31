// tetra_voice.cpp -- see tetra_voice.hpp.

#include "tetra_voice.hpp"
#include "tetra_kit_json.hpp"

#include <zlib.h>
#include <cstring>

namespace dsdsrv {

namespace {

// Reverse base64 table: value 0..63 for a base64 char, -1 otherwise.
int b64val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

std::vector<uint8_t> base64_decode(const std::string& in) {
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4 + 3);
    int acc = 0, bits = 0;
    for (unsigned char c : in) {
        if (c == '=' ) break;
        const int v = b64val(c);
        if (v < 0) continue; // skip whitespace/newlines and stray chars
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

bool tetrakit_extract_speech_frame(const std::string& json_line,
                                   std::vector<int16_t>& out) {
    out.clear();

    // Only traffic-channel reports carry speech. Reuse the JSON parser so the
    // "frame" string is pulled out with the same tolerance as everything else.
    const TetraKitReport rep = parse_tetrakit_json(json_line);
    if (!rep.valid) return false;
    auto sit = rep.fields.find("service");
    auto fit = rep.fields.find("frame");
    if (fit == rep.fields.end() || fit->second.empty()) return false;
    const bool is_uplane = (sit != rep.fields.end() && sit->second == "UPLANE");
    auto pit = rep.fields.find("pdu");
    const bool is_tch = (pit != rep.fields.end() && pit->second.rfind("TCH", 0) == 0);
    if (!is_uplane && !is_tch) return false;

    const std::vector<uint8_t> comp = base64_decode(fit->second);
    if (comp.empty()) return false;

    // zlib-inflate. uzsize tells us the expected size when present; otherwise
    // grow a scratch buffer. TETRA speech frames are 1380 bytes (690 int16).
    std::size_t expected = 0;
    auto uit = rep.fields.find("uzsize");
    if (uit != rep.fields.end()) expected = static_cast<std::size_t>(std::strtoul(uit->second.c_str(), nullptr, 10));
    std::vector<uint8_t> raw(expected ? expected : 4096);

    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = const_cast<Bytef*>(comp.data());
    zs.avail_in = static_cast<uInt>(comp.size());
    std::size_t total = 0;
    int rc = Z_OK;
    do {
        if (total == raw.size()) raw.resize(raw.size() * 2);
        zs.next_out = raw.data() + total;
        zs.avail_out = static_cast<uInt>(raw.size() - total);
        rc = inflate(&zs, Z_NO_FLUSH);
        total = raw.size() - zs.avail_out;
        if (rc == Z_STREAM_END) break;
    } while (rc == Z_OK);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && rc != Z_OK) return false;
    raw.resize(total);
    if (raw.size() < 2) return false;

    // Reinterpret as host-endian int16 (the producer is little-endian x86, as
    // are our targets). An odd trailing byte, if any, is dropped.
    const std::size_t n = raw.size() / 2;
    out.resize(n);
    std::memcpy(out.data(), raw.data(), n * 2);
    return true;
}

#if !defined(DSD_WITH_TETRA_CODEC)
// No codec compiled in: no audio. The server null-checks and skips.
std::unique_ptr<TetraVoiceDecoder> make_tetra_voice_decoder() { return nullptr; }
#endif
// A DSD_WITH_TETRA_CODEC build provides make_tetra_voice_decoder() and a
// concrete TetraVoiceDecoder in a separate, codec-linked translation unit
// (see TETRA_VOICE.md) so the GPLv3/ETSI codec never enters this file.

} // namespace dsdsrv
