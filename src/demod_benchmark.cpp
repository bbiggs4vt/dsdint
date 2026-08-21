// demod_benchmark.cpp
//
// Head-to-head performance comparison of FmDemodulator (fm_demod.cpp)
// vs FmDemodulatorLiquid (fm_demod_liquid.cpp), run in the same process
// against identical synthetic input so the numbers are directly
// comparable. No Boost, no dsd-fme, no network -- just the DSP core,
// same spirit as test_fm_demod.cpp/test_fm_demod_liquid.cpp but focused
// on timing instead of correctness.
//
// Two phases:
//
//   Phase 1 -- single-threaded throughput, with and without a frequency
//   offset active. This directly measures the NCO cost difference
//   discussed in-chat (the hand-rolled version calls std::cos/std::sin
//   per sample at full input rate when a frequency offset is set; the
//   liquid version's nco_crcf avoids that). If that discussion was
//   right, the gap between the two implementations should be much
//   smaller with freq_offset=0 than with it set to something nonzero.
//
//   Phase 2 -- concurrency scaling: run N independent demod instances
//   (one per thread, private data each, no shared state) simultaneously
//   and measure aggregate throughput as N increases. Sweeps a fixed
//   {1,2,4,8,16} thread list rather than deriving it from
//   hardware_concurrency() -- this is specifically to cover the
//   16-sessions-on-8-cores scenario discussed earlier, including going
//   past whatever core count this machine actually has, not just up to
//   it. hardware_concurrency() is printed for reference so you can see
//   where you cross from "fits on real cores" into oversubscription.
//
// IMPORTANT SCOPE NOTE, consistent with the rest of this project: this
// only measures the demod stage. Per the earlier discussion, the DSD
// decode itself (dsd-fme or DSDcc) is plausibly the larger cost at high
// session counts, and this benchmark says nothing about that -- it's
// answering "does liquid-dsp help the piece it can help", not "will N
// sessions fit on this machine" (that needs dsd_process.cpp/
// dsdcc_decoder.cpp in the loop too, which this deliberately excludes to
// keep the comparison isolated to the one variable being changed).

#include "fm_demod.hpp"
#ifdef DSD_HAVE_LIQUID_BENCHMARK
#include "fm_demod_liquid.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace dsdsrv;

namespace {

// Same construction as test_fm_demod.cpp/test_fm_demod_liquid.cpp --
// content doesn't matter for a timing benchmark (unlike the correctness
// tests, we're not checking the output values here), it just needs to
// be realistic-shaped input so neither implementation gets an unfair
// shortcut from e.g. a constant signal.
std::vector<cf32> make_test_signal(std::size_t n_samples, double fs, double tone_hz, double deviation_hz) {
    std::vector<cf32> iq(n_samples);
    double phase = 0.0;
    for (std::size_t i = 0; i < n_samples; ++i) {
        const double t = static_cast<double>(i) / fs;
        const double inst_freq = deviation_hz * std::sin(2.0 * M_PI * tone_hz * t);
        phase += 2.0 * M_PI * inst_freq / fs;
        iq[i] = cf32{static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
    }
    return iq;
}

struct BenchResult {
    double min_ms = 0.0;
    double median_ms = 0.0;
    double samples_per_sec = 0.0;
    double realtime_factor = 0.0; // (signal duration represented) / (wall time taken)
};

// Runs `trials` full passes of `signal` through a fresh DemodT instance
// each time (fresh instance per trial so filter/resampler history state
// doesn't carry between trials), in block_size chunks matching a
// realistic WebSocket frame size. Reports min and median wall time
// across trials -- min because it's the least polluted by scheduler
// jitter/other-process noise, median as a more typical-case number.
template <class DemodT>
BenchResult benchmark_single_thread(const FmDemodConfig& cfg, const std::vector<cf32>& signal,
                                     std::size_t block_size, int trials) {
    std::vector<double> times_ms;
    times_ms.reserve(static_cast<std::size_t>(trials));

    for (int t = 0; t < trials; ++t) {
        DemodT demod(cfg);
        std::vector<int16_t> out;
        out.reserve(signal.size()); // rough upper bound, avoids reallocation noise in the timed region

        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t off = 0; off < signal.size(); off += block_size) {
            const std::size_t n = std::min(block_size, signal.size() - off);
            demod.process(signal.data() + off, n, out);
        }
        auto t1 = std::chrono::steady_clock::now();

        times_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    std::sort(times_ms.begin(), times_ms.end());
    BenchResult r;
    r.min_ms = times_ms.front();
    r.median_ms = times_ms[times_ms.size() / 2];
    const double signal_duration_s = static_cast<double>(signal.size()) / cfg.input_sample_rate_hz;
    r.samples_per_sec = static_cast<double>(signal.size()) / (r.median_ms / 1000.0);
    r.realtime_factor = signal_duration_s / (r.median_ms / 1000.0);
    return r;
}

// Runs n_threads independent demod instances concurrently, each with
// its own private signal buffer (generated up front, outside the timed
// region) and its own private DemodT instance -- deliberately no shared
// state between threads, mirroring N independent WebSocket sessions
// rather than testing any kind of shared-buffer contention.
template <class DemodT>
double benchmark_concurrent(const FmDemodConfig& cfg, std::size_t n_samples_per_thread,
                             std::size_t block_size, int n_threads) {
    std::vector<std::vector<cf32>> signals(static_cast<std::size_t>(n_threads));
    for (int i = 0; i < n_threads; ++i) {
        signals[static_cast<std::size_t>(i)] =
            make_test_signal(n_samples_per_thread, cfg.input_sample_rate_hz, 1000.0, 2000.0);
    }

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(n_threads));

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n_threads; ++i) {
        threads.emplace_back([&, i] {
            DemodT demod(cfg);
            std::vector<int16_t> out;
            const auto& sig = signals[static_cast<std::size_t>(i)];
            out.reserve(sig.size());
            for (std::size_t off = 0; off < sig.size(); off += block_size) {
                const std::size_t n = std::min(block_size, sig.size() - off);
                demod.process(sig.data() + off, n, out);
            }
        });
    }
    for (auto& th : threads) th.join();
    auto t1 = std::chrono::steady_clock::now();

    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double signal_duration_s = static_cast<double>(n_samples_per_thread) / cfg.input_sample_rate_hz;
    // Total "channel-seconds" of audio processed, divided by wall time
    // actually taken -- e.g. a result of 12.0 means this machine can
    // sustain the equivalent of 12 real-time channels' worth of demod
    // work per wall-clock second, at this thread count.
    return (signal_duration_s * n_threads) / (ms / 1000.0);
}

void print_row(const std::string& label, const BenchResult& r) {
    std::printf("  %-28s min=%8.2fms  median=%8.2fms  %12.1f samples/s  %8.1fx realtime\n",
                label.c_str(), r.min_ms, r.median_ms, r.samples_per_sec, r.realtime_factor);
}

} // namespace

int main(int argc, char** argv) {
    const unsigned hw_threads = std::thread::hardware_concurrency();
    std::printf("std::thread::hardware_concurrency() reports %u\n", hw_threads);
#ifdef DSD_HAVE_LIQUID_BENCHMARK
    std::printf("Built WITH liquid-dsp support -- comparing both implementations.\n\n");
#else
    std::printf("Built WITHOUT liquid-dsp support -- only the hand-rolled implementation "
                "will be benchmarked. Rebuild via the same CMake target that builds "
                "dsd-server-liquid (needs libliquid found) to get the comparison.\n\n");
#endif

    const double input_rate = (argc > 1) ? std::atof(argv[1]) : 2'000'000.0;
    std::printf("Input sample rate: %.0f Hz (pass a different rate as argv[1] to override)\n\n",
                input_rate);

    // --- Phase 1: single-threaded throughput, offset on vs off ---
    std::printf("=== Phase 1: single-threaded throughput ===\n");
    const std::size_t block_size = 4096; // matches a realistic WebSocket IQ frame size
    const double signal_seconds = 1.0;
    const std::size_t n_samples = static_cast<std::size_t>(input_rate * signal_seconds);
    const int trials = 7;

    auto signal = make_test_signal(n_samples, input_rate, 1000.0, 2000.0);

    for (double freq_offset : {0.0, 1500.0}) {
        std::printf("\n-- freq_offset = %.0f Hz --\n", freq_offset);

        FmDemodConfig cfg;
        cfg.input_sample_rate_hz = input_rate;
        cfg.output_sample_rate_hz = 48'000.0;
        cfg.channel_bandwidth_hz = 12'500.0;
        cfg.freq_offset_hz = freq_offset;

        auto orig = benchmark_single_thread<FmDemodulator>(cfg, signal, block_size, trials);
        print_row("hand-rolled (fm_demod.cpp)", orig);

#ifdef DSD_HAVE_LIQUID_BENCHMARK
        auto liq = benchmark_single_thread<FmDemodulatorLiquid>(cfg, signal, block_size, trials);
        print_row("liquid-dsp", liq);

        const double speedup = orig.median_ms / liq.median_ms;
        std::printf("  -> liquid-dsp is %.2fx %s at this configuration\n",
                    speedup >= 1.0 ? speedup : 1.0 / speedup,
                    speedup >= 1.0 ? "faster" : "slower");
#endif
    }

    // --- Phase 2: concurrency scaling ---
    std::printf("\n=== Phase 2: concurrency scaling (freq_offset = 0) ===\n");
    std::printf("Fixed thread-count sweep {1,2,4,8,16} regardless of this machine's actual\n");
    std::printf("core count (%u), to cover the oversubscribed case directly rather than\n", hw_threads);
    std::printf("stopping at hardware_concurrency().\n\n");

    FmDemodConfig cfg2;
    cfg2.input_sample_rate_hz = input_rate;
    cfg2.output_sample_rate_hz = 48'000.0;
    cfg2.channel_bandwidth_hz = 12'500.0;
    cfg2.freq_offset_hz = 0.0;

    const std::size_t per_thread_samples = static_cast<std::size_t>(input_rate * 0.25); // 0.25s/thread, keeps total runtime bounded

    std::printf("%-6s %-28s %-28s\n", "N", "hand-rolled (channel-x-realtime)", "liquid-dsp (channel-x-realtime)");
    for (int n_threads : {1, 2, 4, 8, 16}) {
        double orig_factor = benchmark_concurrent<FmDemodulator>(cfg2, per_thread_samples, block_size, n_threads);
#ifdef DSD_HAVE_LIQUID_BENCHMARK
        double liq_factor = benchmark_concurrent<FmDemodulatorLiquid>(cfg2, per_thread_samples, block_size, n_threads);
        std::printf("%-6d %-28.1f %-28.1f\n", n_threads, orig_factor, liq_factor);
#else
        std::printf("%-6d %-28.1f %-28s\n", n_threads, orig_factor, "(liquid not built)");
#endif
    }

    std::printf("\nReminder: this measures the demod stage only. The DSD decode itself\n"
                "(dsd-fme/DSDcc) is a separate, likely larger cost per session at high\n"
                "concurrency -- see the README's discussion of the 16-sessions-on-8-cores\n"
                "question. This benchmark tells you whether liquid-dsp helps the piece it\n"
                "can help, not whether N total sessions fit on this machine.\n");

    return 0;
}
