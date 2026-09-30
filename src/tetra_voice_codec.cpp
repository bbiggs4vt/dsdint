// tetra_voice_codec.cpp
//
// The concrete TetraVoiceDecoder, built on the tetra-kit ACELP audio decoder.
// This translation unit is compiled ONLY in a DSD_WITH_TETRA_CODEC build
// (opt-in; see TETRA_VOICE.md and the Dockerfile's TETRA_CODEC arg), which
// puts tetra-kit's recorder/audio sources on the include/link path. It is the
// single file that touches the GPLv3/ETSI-derived codec, keeping it out of
// every other file -- a default build never compiles this and gets no codec.
//
// A build that links this is GPLv3 by virtue of the codec; that is why it is
// off by default and fetched only at the user's request in their own image.
//
// Reentrancy: the decoder keeps all state inside the cdecoder/sdecoder objects
// (no file-scope globals), so one instance per stream is safe for the server's
// concurrent sessions.

#include "tetra_voice.hpp"

#include <audio_decoder.h>   // tetra-kit recorder/audio: audio_decoder + namespaced ETSI codec

#include <memory>
#include <vector>

namespace dsdsrv {
namespace {

class TetraKitVoiceDecoder : public TetraVoiceDecoder {
public:
    TetraKitVoiceDecoder() { dec_.init(); }

    // One extracted TETRA speech frame (690 int16, a 60 ms channel frame =
    // two speech frames) -> 480 int16 of 8 kHz mono PCM appended to `pcm`.
    bool decode_frame(const std::vector<int16_t>& frame,
                      std::vector<int16_t>& pcm,
                      bool frame_stealing) override {
        if (frame.size() < 690) return false;   // malformed / short frame
        int16_t out[480];
        const int rc = dec_.process_frame(
            frame.data(), out, static_cast<int16_t>(frame_stealing ? 1 : 0));
        if (rc != 1) return false;              // codec rejected the frame
        pcm.insert(pcm.end(), out, out + 480);
        return true;
    }

    // Re-prime the synthesis filter for a new call (init() resets the
    // sdecoder state and the first-pass flag; it does not reallocate).
    void reset() override { dec_.init(); }

private:
    audio_decoder dec_;   // owns cdec/sdec; non-copyable use only (held by value, never copied)
};

} // namespace

std::unique_ptr<TetraVoiceDecoder> make_tetra_voice_decoder() {
    return std::make_unique<TetraKitVoiceDecoder>();
}

} // namespace dsdsrv
