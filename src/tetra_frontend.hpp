// tetra_frontend.hpp
//
// Thin coordinator that sits between the π/4-DQPSK modem (tetra_demod.*) and
// the TETRA lower-MAC, presenting the same demodulate()/flush()/reset() +
// bitstream interface as the bare demod so it drops into the same places
// (session.cpp's worker, tetra_bit_source). It exists to make the *coherent*
// (Costas) detection path usable end to end, because coherent detection has a
// 1-bit ambiguity the bare modem can't resolve on its own:
//
//   π/4-DQPSK alternates between two QPSK constellations. Coherent detection
//   must know which recovered symbol is "even" to collapse them onto one grid.
//   That parity is not observable from the constellation (both hypotheses land
//   on a valid QPSK grid) -- but the WRONG parity decodes to garbage while the
//   RIGHT one decodes to real TETRA bits. So burst-grid lock (tetra_burst_sync)
//   is the oracle: run both parities, keep the one whose bitstream locks.
//
// Modes:
//   * Differential (default): a single differential demod, exactly today's
//     behavior and cost. The robust, phase-blind path; no parity to resolve.
//   * Coherent: during acquisition, run BOTH coherent parities in parallel and
//     watch for a burst-grid lock; the moment one locks, keep that demod
//     (steady state = one demod) and emit its bits. If neither locks within a
//     bounded window (a non-TETRA or too-weak signal), fall back to the
//     differential path -- so "keep the old non-Costas path" holds two ways:
//     it is the default mode, and it is the coherent mode's safety net.
//
// The two paths produce the SAME TETRA bits (differential vs. coherent detect
// the same symbols); coherent just trades a carrier loop for ~1.5-1.7 dB of
// low-BER margin above a ~1.3 dB Eb/N0 crossover (see tetra_demod.hpp and
// tests/test_tetra_demod.cpp). Coherent mode buffers during acquisition and
// emits the winning parity's bits once it locks, so it never interleaves the
// two candidates' output.

#pragma once

#include "tetra_demod.hpp"
#include "tetra_burst_sync.hpp"

#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

namespace dsdsrv {

struct TetraFrontendConfig {
    TetraDemodConfig demod;            // modem params (sps, CFO, RRC, timing)
    TetraSyncConfig sync;              // burst-sync tolerances used to resolve parity
    bool coherent = false;             // false = differential (default), true = coherent w/ auto parity
    // Coherent acquisition budget: if no burst-grid lock is found within this
    // many demodulated bits, give up resolving parity and fall back to the
    // differential path. ~12 timeslots; generous for a real downlink, bounded
    // for a non-TETRA input.
    std::size_t coherent_acquire_bits = 510 * 12;
};

class TetraDemodFrontend {
public:
    explicit TetraDemodFrontend(const TetraFrontendConfig& cfg = TetraFrontendConfig{});

    // Same contract as TetraDpqskDemod::demodulate: feed IQ, get hard bits
    // (2 per symbol, streaming). In coherent mode, returns no bits while still
    // acquiring parity, then the winning candidate's accumulated bits on lock.
    std::vector<unsigned char> demodulate(const std::complex<float>* iq, std::size_t n);

    // Drain end-of-stream (both candidates if still acquiring, finalizing the
    // parity decision best-effort).
    std::vector<unsigned char> flush();

    // Back to initial state for a fresh, unrelated signal.
    void reset();

    // Diagnostics.
    bool coherent() const { return cfg_.coherent; }
    bool coherent_locked() const { return selected_ >= 0; }   // a parity has been resolved
    int  resolved_parity() const { return selected_; }        // 0/1, or -1 if unresolved
    bool fell_back() const { return gave_up_; }               // coherent gave up -> differential
    double last_cfo_hz() const;

private:
    // Accumulate the two candidates' new bits, test for a lock, and return
    // whatever should be emitted this step (nothing while acquiring; the
    // winner's buffer on lock; best-effort at finalize).
    std::vector<unsigned char> acquire(const std::vector<unsigned char>& a,
                                       const std::vector<unsigned char>& b, bool finalizing);
    TetraDemodConfig demod_cfg(bool coherent, int parity) const;

    TetraFrontendConfig cfg_;
    TetraBurstSync sync_;

    // Differential mode, and the coherent mode's fallback after give-up.
    std::unique_ptr<TetraDpqskDemod> diff_;
    // Coherent candidates during acquisition (parity 0 / 1); the winner is
    // promoted to winner_ and the pair is released once one locks.
    std::unique_ptr<TetraDpqskDemod> cohA_, cohB_;
    std::unique_ptr<TetraDpqskDemod> winner_;
    std::vector<unsigned char> accA_, accB_; // accumulated candidate bits (for lock testing)

    int selected_ = -1;    // -1 = acquiring / not-applicable; 0/1 = locked parity
    bool gave_up_ = false; // coherent acquisition failed -> using diff_
};

} // namespace dsdsrv
