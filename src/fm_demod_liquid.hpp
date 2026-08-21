// fm_demod_liquid.hpp
//
// liquid-dsp-based drop-in replacement for FmDemodulator (fm_demod.hpp).
// Same public interface (process/set_gain/set_freq_offset/config), so
// fm_demod_selector.hpp can swap it in at compile time with nothing else
// in the server changing. See fm_demod_liquid.cpp for the reasoning
// behind the specific liquid objects used. Verified against real
// liquid-dsp 1.6.0 — including on a real DMR signal, where this chain's
// output decodes sample-identically to the hand-rolled demod's (see
// test_fm_demod_liquid_real.cpp and the README's liquid section, which
// also has measured A/B performance numbers).
//
// Architecturally this differs from fm_demod.cpp's hand-rolled chain:
// instead of a separate FIR-decimate stage followed by a linear-
// interpolation resample stage, this uses ONE liquid msresamp_crcf
// (a polyphase arbitrary-rate resampler) to do channel filtering *and*
// rate conversion from the raw IQ rate straight down to
// output_sample_rate_hz, and only then runs the FM discriminator
// (freqdem) on that already-48kHz-rate complex signal.
//
// That ordering (resample-then-demod, rather than demod-at-a-higher-
// intermediate-rate-then-resample) is a valid simplification specifically
// because DMR's peak frequency deviation (~2 kHz) is well under 48kHz's
// Nyquist frequency (24kHz). Don't reuse this class as-is for wideband
// FM broadcast (~75kHz deviation) without adding back an intermediate
// higher-rate stage before the discriminator — the deviation would
// approach and potentially exceed the demod rate's Nyquist limit.

#pragma once

#include "fm_demod.hpp" // reuses cf32 and FmDemodConfig — same types, same meaning
#include <liquid/liquid.h>

#include <vector>
#include <cstdint>

namespace dsdsrv {

class FmDemodulatorLiquid {
public:
    explicit FmDemodulatorLiquid(const FmDemodConfig& cfg);
    ~FmDemodulatorLiquid();

    FmDemodulatorLiquid(const FmDemodulatorLiquid&) = delete;
    FmDemodulatorLiquid& operator=(const FmDemodulatorLiquid&) = delete;

    void process(const cf32* in, std::size_t n, std::vector<int16_t>& out);

    void set_gain(float g) { cfg_.disc_gain = g; }
    void set_freq_offset(double hz);

    const FmDemodConfig& config() const { return cfg_; }

private:
    FmDemodConfig cfg_;

    nco_crcf nco_ = nullptr;         // optional frequency shift (input rate)
    msresamp_crcf resamp_ = nullptr; // channel filter + resample, input rate -> output_sample_rate_hz
    freqdem demod_ = nullptr;        // FM discriminator, runs at output_sample_rate_hz

    // Scratch buffers reused across calls to avoid per-call allocation.
    std::vector<cf32> mixed_;
    std::vector<cf32> resampled_;
    std::vector<float> disc_out_;
};

} // namespace dsdsrv
