// pending_decode.hpp
//
// Stage 2 of deferred decryption: replay a stored capture (pending_capture.hpp)
// through a fresh dsd-fme with the keyring now loaded, and ingest the decrypted
// calls back into the live association model.
//
// The capture is the 48 kHz discriminator audio that fed dsd-fme live without a
// key. The key only changes how dsd-fme decodes that same audio, so feeding the
// stored audio to a new dsd-fme that HAS the key produces the cleartext voice
// and the call metadata exactly as a live keyed session would have. We ingest
// that under a fresh synthetic stream id, anchored to the capture's original
// start time (so the recovered calls land at roughly the right time of day,
// not "now"), with keyed=true so the audio store keeps the now-decoded voice.

#pragma once

#include "dsd_process.hpp"
#include "assoc_model.hpp"
#include "pending_capture.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace dsdsrv {

class PendingDecoder {
public:
    struct Result {
        bool ok = false;
        std::string message;
        std::uint64_t bytes = 0;     // discriminator audio fed to dsd-fme
        std::size_t recovered = 0;   // net new calls in the model afterwards
    };

    // Blocking. Replays `s16_path` through a fresh dsd-fme started with
    // `key_args` (the dsd-fme key flags, including any `-K <path>`), ingesting
    // events + decoded audio into `model`. The caller owns `udp_port` (a bound
    // loopback port for dsd-fme's audio out) and any temp `-K` file named in
    // `key_args` -- both are cleaned up by the caller after this returns.
    static Result decode(AssocModel& model, const PendingMeta& meta,
                         const std::string& s16_path,
                         const std::vector<std::string>& key_args,
                         const std::string& dsd_fme_path, std::uint16_t udp_port) {
        Result r;
        std::ifstream f(s16_path, std::ios::binary);
        if (!f) { r.message = "capture audio not found: " + s16_path; return r; }

        static std::atomic<std::uint64_t> seq{0};
        const std::uint64_t sid = (1ull << 62) | (++seq);   // never collides with a live session id
        const std::string family = assoc_family(meta.protocol);
        const std::int64_t freq = AssocModel::channel_hz(static_cast<double>(meta.center_freq_hz));
        const std::size_t before = model.call_count();

        // A decode clock advanced by the audio fed so far, so the recovered
        // calls carry their approximate ORIGINAL time (the capture's start plus
        // how far into it we are) instead of the wall-clock decode time. Read on
        // the dsd-fme reader threads (the callbacks), written on this thread.
        std::atomic<std::int64_t> clock{meta.started_ms};

        model.begin_stream(sid, meta.protocol, meta.started_ms, family, freq);

        DsdProcessConfig cfg;
        cfg.dsd_fme_path = dsd_fme_path;
        cfg.mode_flag = meta.mode.empty() ? std::string("a") : meta.mode;
        cfg.input_sample_rate_hz = PendingCapture::kRate;
        cfg.udp_audio_port = udp_port;
        cfg.decode_short_data = true;
        cfg.extra_args = key_args;
        cfg.on_suppressed = [&model, sid, &clock](const DsdEvent& ev) {
            model.ingest(sid, ev, clock.load(std::memory_order_relaxed));
        };
        cfg.on_slot_audio = [&model, sid, &clock](int slot, const int16_t* pcm, std::size_t n) {
            model.audio(sid, slot, pcm, n, /*keyed=*/true, clock.load(std::memory_order_relaxed));
        };

        DsdProcess proc;
        DsdProcess::EventCallback on_event = [&model, sid, &clock](const DsdEvent& ev) {
            model.ingest(sid, ev, clock.load(std::memory_order_relaxed));
        };
        if (!proc.start(cfg, on_event, /*on_audio=*/{})) {
            model.end_stream(sid, clock.load(std::memory_order_relaxed));
            r.message = "could not start dsd-fme for decode";
            return r;
        }

        // Feed in ~0.5 s chunks, advancing the clock by input position. Pipe
        // backpressure paces this to dsd-fme's consumption, so the clock stays
        // within a pipe buffer of what the decoder is actually emitting.
        std::vector<int16_t> buf(PendingCapture::kRate / 2);
        std::uint64_t total_samples = 0;
        while (f) {
            f.read(reinterpret_cast<char*>(buf.data()),
                   static_cast<std::streamsize>(buf.size() * sizeof(int16_t)));
            const std::size_t ns = static_cast<std::size_t>(f.gcount()) / sizeof(int16_t);
            if (!ns) break;
            proc.write_audio(buf.data(), ns);
            total_samples += ns;
            clock.store(meta.started_ms +
                            static_cast<std::int64_t>(total_samples * 1000ull / PendingCapture::kRate),
                        std::memory_order_relaxed);
        }
        r.bytes = total_samples * sizeof(int16_t);

        proc.drain_stop();   // EOF -> dsd-fme emits the rest and exits; the reader drains every event (no tail dropped)
        model.end_stream(sid, clock.load(std::memory_order_relaxed));

        const std::size_t after = model.call_count();
        r.recovered = after > before ? after - before : 0;
        r.ok = true;
        r.message = "decoded " + std::to_string(r.bytes / 2 / PendingCapture::kRate) + "s, recovered " +
                    std::to_string(r.recovered) + " call" + (r.recovered == 1 ? "" : "s");
        return r;
    }
};

} // namespace dsdsrv
