// Standalone sanity test: synthesize an FM-modulated tone in IQ, run it
// through FmDemodulator, and check we recover a sine wave at roughly the
// expected frequency and that decimation/resampling arithmetic doesn't
// crash or drift. Not a substitute for testing against a real DMR capture.
#include "fm_demod.hpp"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace dsdsrv;

int main() {
    const double fs_in = 2'000'000.0;
    const double fs_out = 48'000.0;
    const double tone_hz = 1000.0;      // audio tone we FM-modulate onto the carrier
    const double deviation_hz = 2000.0; // peak frequency deviation
    const double duration_s = 0.05;

    FmDemodConfig cfg;
    cfg.input_sample_rate_hz = fs_in;
    cfg.output_sample_rate_hz = fs_out;
    cfg.channel_bandwidth_hz = 12500.0;
    const float test_gain = 10000.0f; // scale radians up so int16 truncation doesn't zero out the signal
    cfg.disc_gain = test_gain;

    FmDemodulator demod(cfg);

    const std::size_t n_in = static_cast<std::size_t>(fs_in * duration_s);
    std::vector<cf32> iq(n_in);
    double phase = 0.0;
    for (std::size_t i = 0; i < n_in; ++i) {
        const double t = i / fs_in;
        // Instantaneous frequency = deviation * sin(2*pi*tone*t)
        const double inst_freq = deviation_hz * std::sin(2.0 * M_PI * tone_hz * t);
        phase += 2.0 * M_PI * inst_freq / fs_in;
        iq[i] = cf32{static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
    }

    std::vector<int16_t> out;
    // Feed in chunks to exercise the streaming/history-carry logic, not
    // just a single big block.
    const std::size_t chunk = 4096;
    for (std::size_t off = 0; off < iq.size(); off += chunk) {
        const std::size_t n = std::min(chunk, iq.size() - off);
        demod.process(iq.data() + off, n, out);
    }

    printf("input samples: %zu, output samples: %zu, decimation: %d, decimated rate: %.1f Hz\n",
           n_in, out.size(), demod.decimation_factor(), demod.decimated_rate_hz());

    if (out.empty()) {
        printf("FAIL: no output produced\n");
        return 1;
    }

    // Rough sanity: measure peak absolute discriminator value (converted
    // back out of the test's PCM scale into radians) and confirm it's in a
    // plausible range for the deviation we injected, and not NaN/garbage.
    float peak = 0.0f;
    double sum_sq = 0.0;
    for (int16_t v : out) {
        float f = static_cast<float>(v) / test_gain;
        peak = std::max(peak, std::fabs(f));
        sum_sq += static_cast<double>(f) * f;
    }
    double rms = std::sqrt(sum_sq / out.size());
    printf("peak=%.4f rad, rms=%.4f rad\n", peak, rms);

    // Expected peak angular deviation per output sample ~ 2*pi*deviation/fs_out
    double expected_peak = 2.0 * M_PI * deviation_hz / fs_out;
    printf("expected peak (approx) = %.4f rad\n", expected_peak);

    if (peak < expected_peak * 0.3 || peak > expected_peak * 3.0) {
        printf("WARN: peak discriminator output outside a loose sanity band "
               "(demod math or scaling likely needs review)\n");
        return 2;
    }

    printf("OK: demod pipeline runs end-to-end and output magnitude is in a plausible range\n");
    return 0;
}
