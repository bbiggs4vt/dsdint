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
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace dsdsrv;

namespace {

// Same construction as test_fm_demod.cpp/test_fm_demod_liquid.cpp --
// content doesn't matter for a timing benchmark (unlike the correctness
// tests, we're not checking the output values here), it just needs to
// be realistic-shaped input so neither implementation gets an unfair
// shortcut from e.g. a constant signal.
std::vector<cf32> make_test_signal(std::size_t n_samples, double fs, double tone_hz, double deviation_hz,
                                   double carrier_offset_hz = 0.0) {
    std::vector<cf32> iq(n_samples);
    double phase = 0.0;
    for (std::size_t i = 0; i < n_samples; ++i) {
        const double t = static_cast<double>(i) / fs;
        const double inst_freq = carrier_offset_hz + deviation_hz * std::sin(2.0 * M_PI * tone_hz * t);
        phase += 2.0 * M_PI * inst_freq / fs;
        if (phase > M_PI) phase -= 2.0 * M_PI;
        if (phase < -M_PI) phase += 2.0 * M_PI;
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

        // Untimed warmup pass: primes caches and -- important for the
        // AFC rows -- lets the AFC lock before the clock starts, so the
        // timed region measures steady-state tracking cost rather than
        // a mix of pre-lock (NCO off) and post-lock (NCO maybe on).
        for (std::size_t off = 0; off < signal.size(); off += block_size) {
            demod.process(signal.data() + off, std::min(block_size, signal.size() - off), out);
        }
        out.clear();

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

    // Construct the demod instances OUTSIDE the timed region, matching
    // both Phase 1 and the server's real lifecycle (a session builds its
    // demod once at "start", then it runs for the whole session). This
    // matters a lot for the comparison's fairness: FmDemodulatorLiquid's
    // constructor designs a multi-stage polyphase resampler
    // (msresamp_crcf_create), which at a 2 MHz -> 48 kHz ratio costs on
    // the order of this benchmark's entire 0.25 s-of-signal workload --
    // the original version constructed inside the timed threads, which
    // made liquid's Phase 2 numbers ~5x worse than its own Phase 1
    // throughput for the same configuration (construction cost measured
    // as if it were throughput).
    std::vector<std::unique_ptr<DemodT>> demods;
    std::vector<std::vector<int16_t>> outs(static_cast<std::size_t>(n_threads));
    for (int i = 0; i < n_threads; ++i) {
        demods.push_back(std::make_unique<DemodT>(cfg));
        outs[static_cast<std::size_t>(i)].reserve(n_samples_per_thread);
    }

    // Threads spawn before the clock starts and spin on a start flag, so
    // thread-creation overhead isn't measured either and all workers
    // begin at once.
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(n_threads));
    for (int i = 0; i < n_threads; ++i) {
        threads.emplace_back([&, i] {
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            DemodT& demod = *demods[static_cast<std::size_t>(i)];
            std::vector<int16_t>& out = outs[static_cast<std::size_t>(i)];
            const auto& sig = signals[static_cast<std::size_t>(i)];
            for (std::size_t off = 0; off < sig.size(); off += block_size) {
                const std::size_t n = std::min(block_size, sig.size() - off);
                demod.process(sig.data() + off, n, out);
            }
        });
    }

    auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
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

    // --- Phase 1b: AFC cost ---
    // Two AFC scenarios against the afc-off baseline above:
    //   centered -- the signal needs no correction; thanks to the NCO
    //     deadband (see fm_demod.cpp's kNcoDeadbandHz) the correction
    //     settles inside the deadband and the NCO stays OFF, so this
    //     should track the freq_offset=0 row. (Before the deadband,
    //     AFC's near-zero-but-nonzero correction kept the NCO
    //     permanently on: 2.4x the cost for nothing.)
    //   locked at +2 kHz -- AFC has real work; the NCO is on, so this
    //     should track the freq_offset=1500 row: the AFC loop's own
    //     bookkeeping (a mean/variance pass over the decimated output)
    //     is measured to be free, the cost is all NCO.
    std::printf("\n=== Phase 1b: AFC cost (hand-rolled; afc:true vs the rows above) ===\n");
    {
        FmDemodConfig acfg;
        acfg.input_sample_rate_hz = input_rate;
        acfg.output_sample_rate_hz = 48'000.0;
        acfg.channel_bandwidth_hz = 12'500.0;
        acfg.freq_offset_hz = 0.0;
        acfg.afc_enabled = true;

        auto centered = benchmark_single_thread<FmDemodulator>(acfg, signal, block_size, trials);
        print_row("afc on, signal centered (NCO idle)", centered);

        auto offset_signal = make_test_signal(n_samples, input_rate, 1000.0, 2000.0, 2000.0);
        auto locked = benchmark_single_thread<FmDemodulator>(acfg, offset_signal, block_size, trials);
        print_row("afc on, locked at +2 kHz (NCO on)", locked);
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
