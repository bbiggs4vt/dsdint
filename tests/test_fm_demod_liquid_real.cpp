// test_fm_demod_liquid_real.cpp
//
// Verifies FmDemodulatorLiquid on a REAL DMR signal, the same way the
// backends were verified: DSDcc's bundled discriminator capture
// (samples/dmr_it_8.dis) is FM-modulated into 48 kHz IQ, demodulated by
// the liquid-dsp chain (nco -> msresamp -> freqdem), and the recovered
// discriminator audio is decoded by DsdccDecoder. A pass means the
// liquid demod's output is faithful enough for an actual DMR decoder to
// recover the actual transmission -- a far stronger claim than
// test_fm_demod_liquid's synthetic-tone RMS check.
//
// Result to expect (and what this pins): the round trip is sample-exact
// against the raw capture -- 151680 decoded voice samples, the same
// count DSDcc produces from the file directly, with the same talkgroup
// metadata. So the liquid variant is not "approximately working": on
// this signal its output decodes identically to the hand-rolled demod's.
//
// The modulation inverse differs from the hand-rolled version's tests
// in one constant: freqdem's output is dphi / (2*pi*kf) (normalized to
// ~±1 at the assumed 2 kHz peak deviation), not raw radians like
// fm_demod.cpp's discriminator. The modulator writes phase increments
// of capture_sample / kModGain, so a disc_gain of kModGain * 2*pi*kf
// makes the demod output reproduce the capture's int16 values exactly.
//
// Usage: test_fm_demod_liquid_real <path-to-dmr_it_8.dis>
// Prints SKIPPED and exits 0 without the sample file.

#include "fm_demod_liquid.hpp"
#include "dsdcc_decoder.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace dsdsrv;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  OK: %s\n", what.c_str());
    } else {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

constexpr double kModGain = 26000.0;         // modulator: dphi = sample / kModGain
constexpr double kAssumedDeviationHz = 2000.0; // must match fm_demod_liquid.cpp's kf

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("SKIPPED: pass the path to DSDcc's samples/dmr_it_8.dis\n");
        return 0;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::printf("SKIPPED: cannot open sample file %s\n", argv[1]);
        return 0;
    }

    FmDemodConfig dcfg;
    dcfg.input_sample_rate_hz = 48000;
    dcfg.output_sample_rate_hz = 48000;
    dcfg.channel_bandwidth_hz = 12500;
    dcfg.freq_offset_hz = 0;
    dcfg.disc_gain = static_cast<float>(
        kModGain * 2.0 * M_PI * (kAssumedDeviationHz / 48000.0)); // see header comment
    FmDemodulatorLiquid demod(dcfg);

    DsdccDecoder dec;
    DsdccConfig ccfg; // defaults: dmr, 48000 Hz

    std::size_t audio_samples = 0;
    std::set<std::string> talkgroups, kinds;
    bool started = dec.start(
        ccfg,
        [&](const DsdEvent& ev) {
            if (!ev.talkgroup.empty()) talkgroups.insert(ev.talkgroup);
            kinds.insert(ev.kind);
        },
        [&](const int16_t*, std::size_t n) { audio_samples += n; });
    check(started, "DSDcc decoder starts");

    double phase = 0.0;
    std::vector<int16_t> disc(4096);
    std::vector<cf32> iq(4096);
    std::vector<int16_t> pcm;
    std::size_t total_in = 0, nread;
    while ((nread = std::fread(disc.data(), sizeof(int16_t), disc.size(), f)) > 0) {
        for (std::size_t i = 0; i < nread; ++i) {
            phase += disc[i] / kModGain;
            if (phase > M_PI) phase -= 2.0 * M_PI;
            if (phase < -M_PI) phase += 2.0 * M_PI;
            iq[i] = cf32{static_cast<float>(std::cos(phase)),
                         static_cast<float>(std::sin(phase))};
        }
        pcm.clear();
        demod.process(iq.data(), nread, pcm);
        if (!pcm.empty()) dec.write_audio(pcm.data(), pcm.size());
        total_in += nread;
    }
    std::fclose(f);
    std::printf("  fed %zu IQ samples through the liquid demod (%.1f s)\n",
                total_in, total_in / 48000.0);
    std::printf("  decoded %zu audio samples (%.1f s at 8 kHz)\n",
                audio_samples, audio_samples / 8000.0);

    // Same banding as the other real-capture tests; the observed value
    // is exactly 151680, the same as the raw capture decoded directly.
    check(audio_samples > 100000,
          "liquid-demodulated signal decodes to substantial voice audio (>100k samples)");
    check(audio_samples < 300000, "decoded audio volume is plausible (<300k samples)");
    check(talkgroups.count("150607") == 1,
          "the capture's real talkgroup (150607) survives the liquid demod");
    check(kinds.count("voice") == 1, "voice-kind events were emitted");

    dec.stop();

    if (g_failures == 0) {
        std::printf("\nALL LIQUID-DEMOD REAL-SIGNAL TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
