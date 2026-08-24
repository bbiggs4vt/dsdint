// test_afc.cpp
//
// Tests for the demodulators' AFC (automatic frequency control) and for
// the freq_offset sign convention it depends on.
//
// The sign convention part exists because adding AFC exposed a real
// pre-existing bug: FmDemodulator and FmDemodulatorLiquid implemented
// OPPOSITE freq_offset conventions (a channel at +2 kHz needed
// freq_offset=-2000 on one and +2000 on the other), so the same start
// message behaved differently between dsd-server and dsd-server-liquid.
// The convention is now unified as the documented one -- positive
// freq_offset means the channel sits ABOVE 0 Hz -- and the first test
// case pins it for whichever implementations this binary was built with.
//
// Core cases run on synthetic FM tones (no external deps). When built
// with DSD_AFC_TEST_HAVE_LIQUID the same template cases also run against
// FmDemodulatorLiquid. When built with DSD_AFC_TEST_HAVE_DSDCC and given
// the real DMR capture as argv[1], a final case proves the point of the
// feature: a capture mis-tuned by 3 kHz -- which decodes only partially
// without AFC (measured 88.6% on DSDcc, 0% on dsd-fme) -- decodes
// essentially in full with AFC on.

#include "fm_demod.hpp"
#if defined(DSD_AFC_TEST_HAVE_LIQUID)
#include "fm_demod_liquid.hpp"
#endif
#if defined(DSD_AFC_TEST_HAVE_DSDCC)
#include "dsdcc_decoder.hpp"
#include <set>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// FM tone generator: carrier at carrier_hz, modulated by a modest audio
// tone so the signal has realistic (nonzero) discriminator variance --
// an unmodulated carrier would also pass the AFC's variance gate, but a
// modulated one is closer to what the gate sees in practice.
std::vector<cf32> make_fm_tone(std::size_t n, double fs, double carrier_hz,
                               double tone_hz = 1000.0, double deviation_hz = 1500.0) {
    std::vector<cf32> iq(n);
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = i / fs;
        const double inst = carrier_hz + deviation_hz * std::sin(2.0 * M_PI * tone_hz * t);
        phase += 2.0 * M_PI * inst / fs;
        if (phase > M_PI) phase -= 2.0 * M_PI;
        if (phase < -M_PI) phase += 2.0 * M_PI;
        iq[i] = cf32{static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
    }
    return iq;
}

// Mean of the demod's int16 output -- proportional to residual offset;
// near zero means the NCO (plus AFC, if on) removed the carrier.
template <class DemodT>
double run_and_mean(DemodT& d, const std::vector<cf32>& iq, std::size_t block = 4096) {
    std::vector<int16_t> out;
    for (std::size_t off = 0; off < iq.size(); off += block) {
        d.process(iq.data() + off, std::min(block, iq.size() - off), out);
    }
    if (out.empty()) return 0.0;
    double m = 0;
    for (auto v : out) m += v;
    return m / static_cast<double>(out.size());
}

template <class DemodT>
void test_sign_convention(const char* name) {
    std::printf("test_sign_convention [%s]\n", name);
    // Channel at +1500 Hz, freq_offset=+1500 (the documented meaning)
    // must yield a near-zero discriminator mean; the opposite sign must
    // not. |mean| for an uncorrected 1500 Hz residual is ~5100 counts at
    // this gain, so 500 is a comfortable "near zero".
    FmDemodConfig cfg;
    cfg.input_sample_rate_hz = 48000;
    cfg.output_sample_rate_hz = 48000;
    cfg.channel_bandwidth_hz = 12500;
    cfg.disc_gain = 26000.0f;

    auto iq = make_fm_tone(96000, 48000, 1500.0);

    cfg.freq_offset_hz = 1500.0;
    DemodT good(cfg);
    const double mean_good = std::fabs(run_and_mean(good, iq));

    cfg.freq_offset_hz = -1500.0;
    DemodT bad(cfg);
    const double mean_bad = std::fabs(run_and_mean(bad, iq));

    check(mean_good < 500.0,
          "freq_offset=+1500 zeroes a channel at +1500 Hz (documented convention)");
    check(mean_bad > 2000.0,
          "freq_offset=-1500 does NOT (the conventions really are distinguishable)");
}

template <class DemodT>
void test_afc_acquisition(const char* name) {
    std::printf("test_afc_acquisition [%s]\n", name);
    // Channel at +800 Hz, freq_offset left at 0, AFC on: the correction
    // must converge to ~+800 and the residual mean to ~0.
    FmDemodConfig cfg;
    cfg.input_sample_rate_hz = 48000;
    cfg.output_sample_rate_hz = 48000;
    cfg.channel_bandwidth_hz = 12500;
    cfg.disc_gain = 26000.0f;
    cfg.afc_enabled = true;

    DemodT d(cfg);
    auto iq = make_fm_tone(4 * 48000, 48000, 800.0); // 4 s >> time constant
    run_and_mean(d, iq);

    // Converged residual, measured over a fresh second of signal.
    auto iq2 = make_fm_tone(48000, 48000, 800.0);
    const double residual_mean = std::fabs(run_and_mean(d, iq2));

    std::printf("  correction=%.1f Hz, post-lock |mean|=%.1f counts\n",
                d.afc_correction_hz(), residual_mean);
    check(std::fabs(d.afc_correction_hz() - 800.0) < 50.0,
          "AFC correction converges to the actual offset (800 +/- 50 Hz)");
    check(residual_mean < 200.0, "post-lock residual is near zero");
}

template <class DemodT>
void test_afc_clamp(const char* name) {
    std::printf("test_afc_clamp [%s]\n", name);
    // Offset far beyond the clamp: correction must stop at the limit,
    // not run away.
    FmDemodConfig cfg;
    cfg.input_sample_rate_hz = 48000;
    cfg.output_sample_rate_hz = 48000;
    cfg.channel_bandwidth_hz = 12500;
    cfg.disc_gain = 26000.0f;
    cfg.afc_enabled = true;
    cfg.afc_max_correction_hz = 1000.0;

    DemodT d(cfg);
    auto iq = make_fm_tone(4 * 48000, 48000, 3000.0);
    run_and_mean(d, iq);
    std::printf("  correction=%.1f Hz (clamp at 1000)\n", d.afc_correction_hz());
    check(d.afc_correction_hz() <= 1000.0 + 1e-6 && d.afc_correction_hz() > 900.0,
          "correction rails at the configured clamp");
}

template <class DemodT>
void test_afc_noise_gate(const char* name) {
    std::printf("test_afc_noise_gate [%s]\n", name);
    // Noise-only input: the variance gate must hold the correction at
    // (essentially) zero rather than letting it random-walk.
    FmDemodConfig cfg;
    cfg.input_sample_rate_hz = 48000;
    cfg.output_sample_rate_hz = 48000;
    cfg.channel_bandwidth_hz = 12500;
    cfg.disc_gain = 26000.0f;
    cfg.afc_enabled = true;

    DemodT d(cfg);
    std::vector<cf32> noise(4 * 48000);
    unsigned s = 12345;
    auto frand = [&s]() { s = s * 1664525u + 1013904223u; return (s >> 9) / 4194304.0f - 1.0f; };
    for (auto& v : noise) v = cf32{frand(), frand()};
    run_and_mean(d, noise);
    std::printf("  correction after 4 s of noise = %.2f Hz\n", d.afc_correction_hz());
    check(std::fabs(d.afc_correction_hz()) < 20.0,
          "variance gate holds the correction still on noise-only input");
}

template <class DemodT>
void test_afc_step_tracking(const char* name) {
    std::printf("test_afc_step_tracking [%s]\n", name);
    // Lock at +500 Hz, then step the channel to -700 Hz mid-stream: AFC
    // must re-acquire the new offset.
    FmDemodConfig cfg;
    cfg.input_sample_rate_hz = 48000;
    cfg.output_sample_rate_hz = 48000;
    cfg.channel_bandwidth_hz = 12500;
    cfg.disc_gain = 26000.0f;
    cfg.afc_enabled = true;

    DemodT d(cfg);
    auto iq1 = make_fm_tone(3 * 48000, 48000, 500.0);
    run_and_mean(d, iq1);
    const double locked1 = d.afc_correction_hz();

    auto iq2 = make_fm_tone(3 * 48000, 48000, -700.0);
    run_and_mean(d, iq2);
    const double locked2 = d.afc_correction_hz();

    std::printf("  lock 1: %.1f Hz, after step: %.1f Hz\n", locked1, locked2);
    check(std::fabs(locked1 - 500.0) < 50.0, "locks the initial +500 Hz offset");
    check(std::fabs(locked2 - (-700.0)) < 50.0, "re-locks after a step to -700 Hz");
}

template <class DemodT>
void run_all(const char* name) {
    test_sign_convention<DemodT>(name);
    test_afc_acquisition<DemodT>(name);
    test_afc_clamp<DemodT>(name);
    test_afc_noise_gate<DemodT>(name);
    test_afc_step_tracking<DemodT>(name);
}

#if defined(DSD_AFC_TEST_HAVE_DSDCC)
// The payoff case: a real DMR capture mis-tuned by 3 kHz. Without AFC
// this decodes 88.6% on DSDcc (and 0% on dsd-fme); with AFC it must
// recover to essentially full decode.
void test_afc_rescues_mistuned_dmr(const char* capture_path) {
    std::printf("test_afc_rescues_mistuned_dmr (+3 kHz error, real capture)\n");
    FILE* f = std::fopen(capture_path, "rb");
    if (!f) { std::printf("  SKIPPED: cannot open %s\n", capture_path); return; }

    FmDemodConfig dcfg;
    dcfg.input_sample_rate_hz = 48000;
    dcfg.output_sample_rate_hz = 48000;
    dcfg.channel_bandwidth_hz = 12500;
    dcfg.freq_offset_hz = 0.0; // wrong on purpose: the signal is placed at +3 kHz
    dcfg.disc_gain = 26000.0f;
    dcfg.afc_enabled = true;
    FmDemodulator demod(dcfg);

    DsdccDecoder dec;
    DsdccConfig ccfg;
    std::size_t audio = 0;
    std::set<std::string> tgs;
    dec.start(ccfg,
              [&](const DsdEvent& ev) { if (!ev.talkgroup.empty()) tgs.insert(ev.talkgroup); },
              [&](const int16_t*, std::size_t n) { audio += n; });

    const double kErrorHz = 3000.0;
    double phase = 0.0;
    std::vector<int16_t> in(4096);
    std::vector<cf32> iq(4096);
    std::vector<int16_t> pcm;
    std::size_t nread;
    while ((nread = std::fread(in.data(), sizeof(int16_t), in.size(), f)) > 0) {
        for (std::size_t i = 0; i < nread; ++i) {
            phase += in[i] / 26000.0 + 2.0 * M_PI * kErrorHz / 48000.0;
            if (phase > M_PI) phase -= 2.0 * M_PI;
            if (phase < -M_PI) phase += 2.0 * M_PI;
            iq[i] = cf32{static_cast<float>(std::cos(phase)),
                         static_cast<float>(std::sin(phase))};
        }
        pcm.clear();
        demod.process(iq.data(), nread, pcm);
        if (!pcm.empty()) dec.write_audio(pcm.data(), pcm.size());
    }
    std::fclose(f);

    std::printf("  decoded %zu samples (ref 151680), final AFC correction %.1f Hz\n",
                audio, demod.afc_correction_hz());
    // Lock happens within the first second of the 20 s capture, so only
    // a sliver of the leading audio can be lost.
    check(audio > 140000, "mis-tuned capture decodes nearly in full with AFC (>92%)");
    check(tgs.count("150607") == 1, "talkgroup recovered");
    check(std::fabs(demod.afc_correction_hz() - kErrorHz) < 100.0,
          "AFC settled on the true 3 kHz error (+/- 100 Hz)");
    dec.stop();
}
#endif

} // namespace

int main(int argc, char** argv) {
    run_all<FmDemodulator>("hand-rolled");
#if defined(DSD_AFC_TEST_HAVE_LIQUID)
    run_all<FmDemodulatorLiquid>("liquid");
#endif
#if defined(DSD_AFC_TEST_HAVE_DSDCC)
    if (argc > 1) test_afc_rescues_mistuned_dmr(argv[1]);
    else std::printf("(real-capture case skipped: no capture path argument)\n");
#else
    (void)argc; (void)argv;
#endif

    if (g_failures == 0) {
        std::printf("\nALL AFC TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
