// tetra_voice.hpp
//
// TETRA voice: extracting encoded speech from tetra-kit's reports, and the
// seam for turning it into PCM. See TETRA_VOICE.md for the full design.
//
// tetra-kit carries speech inside its UPLANE/TCH_S JSON reports as a
// zlib-compressed, base64-encoded payload in the "frame" field (with "zsize"
// = compressed and "uzsize" = uncompressed sizes). Decompressed it is 690
// int16 values -- exactly one 30/60 ms speech frame in the form tetra-kit's
// codec consumes (its audio_decoder::process_frame takes 690 int16 in and
// yields 480 int16 PCM samples). This header provides that extraction (pure,
// dependency-light: an in-tree base64 decoder + zlib) plus the codec-agnostic
// TetraVoiceDecoder seam.
//
// Validated: the real off-air capture's frames extract to 1380 bytes each
// (== uzsize) and, run through the TETRA speech codec, produce ~2.7 s of
// intelligible 8 kHz voice.
//
// CODEC. The ACELP synthesis codec itself is NOT here: the ETSI reference is
// patent-encumbered and tetra-kit's implementation is GPLv3, so neither is
// vendored into this repo. make_tetra_voice_decoder() returns a working
// decoder only in a build that supplies one (DSD_WITH_TETRA_CODEC + the codec
// objects); otherwise it returns nullptr and the server extracts frames but
// emits no audio. The extraction below is the stable, license-clean half.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace dsdsrv {

// Decode standard base64 (ignoring whitespace; '=' padding). Returns the
// bytes; returns empty on a malformed input.
std::vector<uint8_t> base64_decode(const std::string& in);

// If `json_line` is a tetra-kit UPLANE/TCH_S report carrying a "frame"
// payload, base64-decode + zlib-inflate it into `out` (host-endian int16
// speech samples, normally 690) and return true. Returns false for any other
// line, or if the payload is missing/'malformed. Does not allocate on the
// hot path beyond `out`.
bool tetrakit_extract_speech_frame(const std::string& json_line,
                                   std::vector<int16_t>& out);

// The codec seam. An implementation turns one extracted speech frame
// (690 int16) into 8 kHz mono PCM (480 int16 appended to `pcm`). Stateful:
// the ACELP synthesis filter carries across frames, so one instance per
// stream. frame_stealing is 1 when a control burst stole this speech frame.
class TetraVoiceDecoder {
public:
    virtual ~TetraVoiceDecoder() = default;
    virtual bool decode_frame(const std::vector<int16_t>& frame,
                              std::vector<int16_t>& pcm,
                              bool frame_stealing = false) = 0;
    virtual void reset() = 0;
};

// Returns a decoder when the build supplies a codec (DSD_WITH_TETRA_CODEC),
// else nullptr. Callers must null-check and simply skip audio when null.
std::unique_ptr<TetraVoiceDecoder> make_tetra_voice_decoder();

} // namespace dsdsrv
