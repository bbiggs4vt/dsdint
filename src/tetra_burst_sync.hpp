// tetra_burst_sync.hpp
//
// TETRA burst / frame synchronisation: the step after the π/4-DQPSK
// demodulator (tetra_demod.hpp). The demod produces a continuous 36 kbit/s
// bitstream; this finds the known TETRA training sequences in it to lock
// the burst grid, so downstream layers know where each 510-bit timeslot
// begins and which fields sit where.
//
// TETRA downlink is TDMA: a timeslot is TETRA_SYM_PER_TS = 255 symbols =
// 510 bits (TETRA_BITS_PER_TS). Each burst carries a training sequence at a
// fixed offset within the slot, and on a continuously-transmitting downlink
// the bursts repeat every 510 bits. So synchronisation is: correlate the
// training sequences against the bitstream, then confirm that the implied
// burst starts fall on a single, consistent 510-bit grid.
//
// Constants (training-sequence bit patterns, within-burst offsets, and the
// slot length) are taken verbatim from Osmocom's osmo-tetra
// (src/phy/tetra_burst.c and its sync logic) and ETSI EN 300 392-2:
//   - synchronisation training sequence (y, 38 bits) at burst offset 214,
//   - normal training sequence 1 (n, 22 bits) and 2 (p, 22 bits) at 244,
//   - extended training sequence (x, 30 bits) — detected, but its
//     within-burst offset isn't used for the grid here.
// osmo-tetra matches the sequences exactly (memcmp); this generalises that
// to a small Hamming-distance tolerance so a slightly noisy real bitstream
// still locks, and leans on grid consistency (a real burst grid repeats;
// spurious matches in random data don't) to reject false positives.
//
// Verification: tests/test_tetra_burst_sync.cpp builds synthetic bursts
// with the real training sequences at the real offsets and checks they are
// found at the right positions with the right types, that the grid locks
// (including under injected bit errors and a stream-wide offset), that pure
// random data does NOT lock, and end-to-end that a burst stream pushed
// through the modulator + demod still locks. Confirming this against a real
// off-air TETRA capture remains a later step.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsdsrv {

// Bits per TDMA timeslot / burst (255 symbols × 2). The burst grid repeats
// at this interval on a continuous downlink.
inline constexpr int kTetraBitsPerTs = 510;

enum class TetraTrain { Norm1, Norm2, Sync, Ext };

// One training-sequence detection in the bitstream.
struct TrainMatch {
    std::size_t pos = 0;     // bit index where the training sequence starts
    TetraTrain type = TetraTrain::Norm1;
    int errors = 0;          // Hamming distance at this position
    // Implied burst start = pos - within-burst offset, i.e. the bit index of
    // the enclosing timeslot's first bit. LONG_MIN when the offset is
    // unknown (Ext), so it is excluded from grid locking.
    long burst_start = 0;
    bool has_burst_start = false;
};

struct TetraSyncConfig {
    // Per-sequence Hamming-distance tolerance. Defaults are a few percent of
    // each sequence length — enough to ride a low residual BER, low enough
    // that random-data false matches stay rare and are filtered by the grid.
    int max_errors_sync = 4; // of 38
    int max_errors_norm = 2; // of 22
    int max_errors_ext = 3;  // of 30
    // Grid lock is declared once this many detections agree on one 510-bit
    // phase (burst_start mod 510).
    int min_confirmations = 3;
};

struct TetraLock {
    bool locked = false;
    long phase = 0;        // burst_start mod 510 of the locked grid
    int confirmations = 0; // detections agreeing on that phase
};

class TetraBurstSync {
public:
    explicit TetraBurstSync(const TetraSyncConfig& cfg = TetraSyncConfig{}) : cfg_(cfg) {}

    // Every training-sequence match in the bitstream (0/1 bytes), in order.
    std::vector<TrainMatch> find_all(const uint8_t* bits, std::size_t n) const;

    // Reduce a set of matches to the dominant 510-bit burst grid: the phase
    // (burst_start mod 510) shared by the most detections. Locked once that
    // count reaches min_confirmations.
    TetraLock find_lock(const std::vector<TrainMatch>& matches) const;

    // Convenience: find_all + find_lock over one buffer.
    TetraLock synchronize(const uint8_t* bits, std::size_t n) const {
        return find_lock(find_all(bits, n));
    }

    const TetraSyncConfig& config() const { return cfg_; }

private:
    TetraSyncConfig cfg_;
};

} // namespace dsdsrv
