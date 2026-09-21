// test_matched_filter.cpp
//
// Unit tests for the opt-in RRC symbol matched filter in the FM path
// (design_rrc_fir + FmDemodulator's matched-filter stage). Pure DSP with
// synthetic inputs -- no capture files, no external deps -- so it always
// runs. It proves the filter is a CORRECT root-raised-cosine (symmetric,
// unity DC gain, strongly low-pass, and ISI-free when cascaded with
// itself) and that enabling it in FmDemodulator preserves signal level
// while producing finite output. It does NOT (and can't, here) measure
// the sensitivity/BER gain -- that needs real off-air captures A/B'd
// through the decoder.

#include "../src/fm_demod.hpp"

#include <cstdio>
#include <cmath>
#include <vector>
#include <complex>
#include <random>

using namespace dsdsrv;

static int g_failures = 0;
static void check(bool c, const char* what) {
    std::printf("  %s: %s\n", c ? "OK" : "FAIL", what);
    if (!c) ++g_failures;
}

// |H(f)| of a real FIR at normalized frequency f_norm = f/fs (cycles/sample).
static double fir_mag(const std::vector<float>& h, double f_norm) {
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < h.size(); ++n) {
        const double ph = -2.0 * M_PI * f_norm * static_cast<double>(n);
        re += h[n] * std::cos(ph);
        im += h[n] * std::sin(ph);
    }
    return std::sqrt(re * re + im * im);
}

int main() {
    std::printf("test_matched_filter\n");

    const double fs = 96000.0, rs = 4800.0, beta = 0.2;
    const int sps = static_cast<int>(std::lround(fs / rs)); // 20
    std::vector<float> h = design_rrc_fir(fs, rs, beta, 8 * sps);

    // ---- filter shape ----
    check(h.size() % 2 == 1, "RRC length is odd (symmetric Type-I FIR)");

    bool symmetric = true;
    for (std::size_t i = 0; i < h.size() / 2; ++i)
        if (std::fabs(h[i] - h[h.size() - 1 - i]) > 1e-6f) symmetric = false;
    check(symmetric, "RRC taps are symmetric about the center");

    double sum = 0.0;
    for (float c : h) sum += c;
    check(std::fabs(sum - 1.0) < 1e-4, "RRC has unity DC gain (taps sum to 1)");

    const std::size_t mid = h.size() / 2;
    bool peak_centered = true;
    for (float c : h) if (c > h[mid] + 1e-6f) peak_centered = false;
    check(peak_centered, "RRC peak is at the center tap");

    // ---- low-pass response with the textbook RRC landmarks: flat through
    // the passband edge (1-beta)*rs/2, exactly 1/sqrt(2) at rs/2, and a null
    // at the stopband edge (1+beta)*rs/2 -- so it passes the symbol band and
    // cuts the high-frequency discriminator noise above it ----
    check(std::fabs(fir_mag(h, 0.0) - 1.0) < 1e-4, "|H(0)| = 1 (DC preserved)");
    const double mag_pass = fir_mag(h, (1.0 - beta) * rs / 2.0 / fs); // 1920 Hz
    check(mag_pass > 0.9, "passband (below (1-beta)*Rs/2) is flat");
    const double mag_half = fir_mag(h, rs / 2.0 / fs);                // 2400 Hz
    check(std::fabs(mag_half - M_SQRT1_2) < 0.05, "|H(Rs/2)| ~ 1/sqrt(2) (RRC -3 dB)");
    const double mag_null = fir_mag(h, (1.0 + beta) * rs / 2.0 / fs); // 2880 Hz
    // The ideal RRC is 0 here, but truncating to a finite length leaves
    // Gibbs ripple exactly at this infinite-slope edge, so allow for it.
    check(mag_null < 0.15, "|H((1+beta)*Rs/2)| well down (RRC stopband edge)");
    const double mag_stop = fir_mag(h, 1.5 * rs / fs);              // 3600 Hz, past the edge
    check(mag_stop < 0.02, "deep stopband just past the symbol band (HF noise killed)");
    const double mag_2sym = fir_mag(h, 2.0 * rs / fs);             // 9600 Hz, far out
    check(mag_2sym < 0.02, "energy well outside the symbol band is rejected");

    // ---- ISI-free: RRC cascaded with itself is a raised cosine, which is
    // a Nyquist pulse -> zero at nonzero integer symbol offsets ----
    {
        const std::size_t n = h.size();
        std::vector<double> rc(2 * n - 1, 0.0);
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < n; ++j)
                rc[i + j] += static_cast<double>(h[i]) * h[j];
        const std::size_t c = n - 1; // center of the cascade
        const double peak = rc[c];
        check(peak > 0.0, "cascade (raised cosine) has a positive peak");
        const double at1 = std::fabs(rc[c + sps]) / peak;
        const double at2 = std::fabs(rc[c + 2 * sps]) / peak;
        check(at1 < 0.02, "raised cosine ~0 at +/-1 symbol (ISI-free)");
        check(at2 < 0.02, "raised cosine ~0 at +/-2 symbols (ISI-free)");
    }

    // ---- FmDemodulator with the matched filter enabled ----
    // A pure complex tone at a fixed offset demodulates to a CONSTANT
    // instantaneous frequency (a DC level after discrimination). A unity-DC
    // matched filter must preserve that level, and the output must be
    // finite -- this catches gross wiring/normalization bugs.
    {
        FmDemodConfig cfg;
        cfg.input_sample_rate_hz = 96000.0;
        cfg.output_sample_rate_hz = 48000.0;
        cfg.channel_bandwidth_hz = 12500.0;
        cfg.disc_gain = 20000.0f;
        cfg.matched_filter_enabled = true;

        const double f_off = 3000.0; // Hz, within the passband
        const std::size_t N = 20000;
        std::vector<cf32> iq(N);
        double ph = 0.0;
        const double dph = 2.0 * M_PI * f_off / cfg.input_sample_rate_hz;
        for (std::size_t i = 0; i < N; ++i) {
            iq[i] = cf32(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
            ph += dph;
        }

        FmDemodulator demod(cfg);
        std::vector<int16_t> out;
        demod.process(iq.data(), N, out);

        check(!out.empty(), "matched-filter demod produces output");
        // Steady-state level: average the back half (past the filter transient).
        long long acc = 0; std::size_t cnt = 0;
        for (std::size_t i = out.size() / 2; i < out.size(); ++i) { acc += out[i]; ++cnt; }
        const double mean = cnt ? static_cast<double>(acc) / cnt : 0.0;
        // Expected discriminator level: 2*pi*f/fs_dec * gain, fs_dec = 96k/decim.
        const double fs_dec = demod.decimated_rate_hz();
        const double expected = (2.0 * M_PI * f_off / fs_dec) * cfg.disc_gain;
        check(std::fabs(mean - expected) < std::fabs(expected) * 0.05 + 5.0,
              "steady tone level preserved through the matched filter (unity DC)");
    }

    // ---- quantitative: noise reduction on a constant tone + complex AWGN ----
    // A constant-frequency tone demodulates to a DC level (passed at unity
    // gain by both filters, matched-filter or not), so with AWGN added the
    // output = DC + noise. Measuring the output variance MF-off vs MF-on
    // isolates the noise: the matched filter narrows the post-detection
    // noise bandwidth to the symbol band and should cut it substantially
    // while leaving the DC level unchanged. Deterministic (fixed seed).
    //
    // NB: this measures the noise-bandwidth reduction in the *raw
    // discriminator output* -- an upper bound on the benefit. The real
    // decode gain is smaller, because the downstream decoder already
    // filters some of this out-of-band noise itself; that end-to-end dB
    // still needs a real-capture A/B and is NOT claimed here.
    {
        const double fs_iq = 192000.0, f_off = 2000.0;
        const std::size_t Nn = 160000;
        auto run = [&](bool mf, double& out_mean, double& out_var) {
            std::mt19937 rng(4242);
            std::normal_distribution<double> gg(0.0, 1.0);
            const double snr_db = 12.0;
            const double sigma = std::sqrt((1.0 / std::pow(10.0, snr_db / 10.0)) / 2.0);
            std::vector<cf32> iq(Nn);
            double ph = 0.0;
            for (std::size_t i = 0; i < Nn; ++i) {
                ph += 2.0 * M_PI * f_off / fs_iq;
                iq[i] = cf32(static_cast<float>(std::cos(ph) + sigma * gg(rng)),
                             static_cast<float>(std::sin(ph) + sigma * gg(rng)));
            }
            FmDemodConfig cfg;
            cfg.input_sample_rate_hz = fs_iq;
            cfg.output_sample_rate_hz = 48000.0;
            cfg.channel_bandwidth_hz = 12500.0;
            cfg.disc_gain = 4000.0f;
            cfg.matched_filter_enabled = mf;
            FmDemodulator demod(cfg);
            std::vector<int16_t> out;
            demod.process(iq.data(), Nn, out);
            const std::size_t s = out.size() / 10; // drop the filter transient
            double m = 0; std::size_t c = 0;
            for (std::size_t i = s; i < out.size(); ++i) { m += out[i]; ++c; }
            m /= (c ? c : 1);
            double v = 0;
            for (std::size_t i = s; i < out.size(); ++i) { double e = out[i] - m; v += e * e; }
            v /= (c ? c : 1);
            out_mean = m; out_var = v;
        };
        double mean_off, var_off, mean_on, var_on;
        run(false, mean_off, var_off);
        run(true,  mean_on,  var_on);
        const double reduction_db = 10.0 * std::log10(var_off / var_on);
        std::printf("    [measured] DC level off=%.1f on=%.1f; noise reduction = %.1f dB\n",
                    mean_off, mean_on, reduction_db);
        check(std::fabs(mean_on - mean_off) < std::fabs(mean_off) * 0.02 + 1.0,
              "AWGN: DC signal level unchanged by the matched filter");
        check(var_on < var_off * 0.5,
              "AWGN: matched filter cuts post-detection noise variance (>3 dB)");
    }

    if (g_failures == 0) {
        std::printf("\nALL MATCHED-FILTER TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
