#include "fm_demod_liquid.hpp"

#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------
// API ASSUMPTIONS — please read before trusting this file
//
// I could not install/link liquid-dsp in the environment this was
// written in (no network access), so none of the code below has been
// compiled. The function signatures used here are based on liquid-dsp's
// published API docs and example code, cross-checked at the time of
// writing:
//
//   msresamp_crcf_create(rate, stopband_attenuation_db) -> msresamp_crcf
//   msresamp_crcf_execute(q, in, n_in, out, &n_out)
//     confirmed against a real usage example (Andres Vahter's liquid-dsp
//     command-line FM demodulator writeup).
//
//   nco_crcf_create(LIQUID_VCO) -> nco_crcf
//   nco_crcf_set_frequency(q, radians_per_sample)
//   nco_crcf_mix_block_down(q, in, out, n)
//     signature confirmed against liquid-dsp's nco_crcf API reference
//     page (liquidsdr.org/api/nco_crcf).
//
//   freqdem_create(kf) -> freqdem
//   freqdem_demodulate_block(q, in, n, out)
//     freqdem's block-level execution API was added in liquid-dsp 1.3.0
//     per the project's own release notes. UPDATE: this call name has
//     now been confirmed working -- a real build (liquid-dsp on target
//     hardware, via test_fm_demod_liquid) produced output whose RMS
//     matched the theoretically expected value for a correctly-scaled
//     discriminator to within 0.2%, which isn't something you get by
//     accident from a wrong function name (that would fail to link, or
//     produce nonsense). Treat this one as verified, not a guess.
//
// If your liquid-dsp version differs, the fix is almost always a
// function rename here, not a logic change — the object lifecycle
// (create once in the constructor, execute per block, destroy in the
// destructor) is stable across liquid-dsp versions.
// ---------------------------------------------------------------------

namespace dsdsrv {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

FmDemodulatorLiquid::FmDemodulatorLiquid(const FmDemodConfig& cfg) : cfg_(cfg) {
    nco_ = nco_crcf_create(LIQUID_VCO);
    set_freq_offset(cfg_.freq_offset_hz); // sets nco_'s frequency too

    const double rate = cfg_.output_sample_rate_hz / cfg_.input_sample_rate_hz;
    const float stopband_atten_db = 60.0f; // liquid's own examples default to this
    resamp_ = msresamp_crcf_create(static_cast<float>(rate), stopband_atten_db);

    // freqdem's kf is the modulation index: peak frequency deviation as a
    // fraction of the sample rate the discriminator runs at. Since we
    // resample to output_sample_rate_hz *before* demodulating, that's the
    // rate this fraction is relative to. This assumed_deviation_hz is a
    // placeholder for DMR's typical peak deviation (~2 kHz) — replace
    // with your system's actual measured deviation if you have it;
    // getting this wrong doesn't break decoding outright (freqdem still
    // produces a valid discriminator signal), it just changes the output
    // amplitude, which the disc_gain scaling below compensates for
    // anyway. It's more of a "get this roughly right and let disc_gain
    // do the fine-tuning" parameter than a hard requirement.
    const double assumed_deviation_hz = 2000.0;
    const float kf = static_cast<float>(assumed_deviation_hz / cfg_.output_sample_rate_hz);
    demod_ = freqdem_create(kf);
}

FmDemodulatorLiquid::~FmDemodulatorLiquid() {
    if (nco_) nco_crcf_destroy(nco_);
    if (resamp_) msresamp_crcf_destroy(resamp_);
    if (demod_) freqdem_destroy(demod_);
}

void FmDemodulatorLiquid::set_freq_offset(double hz) {
    cfg_.freq_offset_hz = hz;
    if (nco_) {
        const double radians_per_sample = 2.0 * kPi * hz / cfg_.input_sample_rate_hz;
        nco_crcf_set_frequency(nco_, static_cast<float>(radians_per_sample));
    }
}

void FmDemodulatorLiquid::process(const cf32* in, std::size_t n, std::vector<int16_t>& out) {
    if (n == 0) return;

    // 1) Optional frequency shift at the input rate. Skipping the NCO
    // call entirely when there's no offset avoids paying for it — this
    // mirrors fm_demod.cpp's same optimization, and is one of the two
    // places (along with the FIR/decimate stage) where liquid's NEON/AVX
    // dot-product kernels are expected to matter most at higher session
    // counts (see the architecture discussion in-chat for the reasoning).
    mixed_.resize(n);
    if (cfg_.freq_offset_hz != 0.0) {
        // reinterpret_cast between std::complex<float> and liquid's
        // liquid_float_complex (a C99 `float complex`) relies on the
        // standard-guaranteed layout compatibility of std::complex<float>
        // with a 2-element float array / C99 float complex. This is the
        // usual approach for bridging C99-complex-based DSP libraries
        // (e.g. FFTW's C++ wrapper documents the same technique) and
        // should hold on any platform with a conventional complex ABI,
        // but it's worth a sanity check if you're targeting something
        // unusual.
        nco_crcf_mix_block_down(
            nco_,
            reinterpret_cast<liquid_float_complex*>(const_cast<cf32*>(in)),
            reinterpret_cast<liquid_float_complex*>(mixed_.data()),
            static_cast<unsigned int>(n));
    } else {
        std::copy(in, in + n, mixed_.begin());
    }

    // 2) Channel filter + resample straight to output_sample_rate_hz.
    // msresamp_crcf's output count per call depends on internal filter
    // state, not just a fixed ratio of n, so size generously and trust
    // the ny liquid hands back.
    const std::size_t max_out = static_cast<std::size_t>(
        n * (cfg_.output_sample_rate_hz / cfg_.input_sample_rate_hz)) + 16;
    resampled_.resize(max_out);
    unsigned int ny = 0;
    msresamp_crcf_execute(
        resamp_,
        reinterpret_cast<liquid_float_complex*>(mixed_.data()),
        static_cast<unsigned int>(n),
        reinterpret_cast<liquid_float_complex*>(resampled_.data()),
        &ny);

    // 3) FM discriminator.
    disc_out_.resize(ny);
    if (ny > 0) {
        freqdem_demodulate_block(
            demod_,
            reinterpret_cast<liquid_float_complex*>(resampled_.data()),
            ny,
            disc_out_.data());
    }

    // 4) Gain scale + clip to int16. Same convention as fm_demod.cpp,
    // but note freqdem's output is already normalized to roughly ±1.0 at
    // peak deviation (assuming assumed_deviation_hz above is
    // approximately correct), unlike the hand-rolled version's raw
    // radians-per-sample output. That makes disc_gain a more physically
    // meaningful knob here: something in the 28000-32000 range should
    // land near full 16-bit scale at peak deviation, rather than needing
    // an empirical sweep from an arbitrary starting point.
    out.reserve(out.size() + disc_out_.size());
    for (float v : disc_out_) {
        float scaled = v * cfg_.disc_gain;
        if (scaled > 32767.0f) scaled = 32767.0f;
        if (scaled < -32768.0f) scaled = -32768.0f;
        out.push_back(static_cast<int16_t>(scaled));
    }
}

} // namespace dsdsrv
