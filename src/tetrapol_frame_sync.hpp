// tetrapol_frame_sync.hpp
//
// TETRAPOL frame synchronisation: the step after the GMSK demodulator
// (tetrapol_demod.hpp). The demod produces a continuous 8 kbit/s bitstream;
// this finds the known TETRAPOL frame-sync word in it to lock the 160-bit
// frame grid, so downstream (tetrapol_dump) knows where each 20 ms frame
// begins. It is the TETRAPOL analog of tetra_burst_sync.
//
// TETRAPOL frame layout (from tetrapol-kit lib/phys_ch.c and the TETRAPOL PAS):
//   - a frame is FRAME_LEN = 160 bits (8-bit header + 152-bit data), 20 ms at
//     8000 bit/s, and repeats continuously on a channel;
//   - the header carries a 7-bit *differentially-encoded* frame-sync word,
//     frame_dsync = {1,0,1,0,0,1,1}, verbatim from tetrapol-kit;
//   - sync is deliberately detected on the RAW bitstream (before differential
//     decode / descramble) — the differentially-encoded pattern is a fixed
//     bit sequence there, which "simplifies search" (tetrapol-kit's words).
//     tetrapol_dump does the differential decode + descramble after sync.
//
// Two things this handles that the TETRA sync doesn't:
//   - Polarity. TETRAPOL is differentially encoded (so the *decoded* data is
//     invariant to a global bit inversion), but the raw-stream sync word is
//     not: an inverted raw stream carries ~frame_dsync. Our GMSK demod's bit
//     polarity is a 1-bit unknown (its `invert`), so we search both the
//     pattern and its inverse and report which locked.
//   - False matches. A 7-bit word with a 1-error tolerance matches ~6% of
//     random positions, so phase-voting alone (as TETRA's long sequences
//     allow) would false-lock. Instead we require a *run of consecutive
//     frames* 160 bits apart — real TETRAPOL puts the sync in every frame, so
//     it forms a long unbroken run, while random matches almost never chain.
//
// Verification: tests/test_tetrapol_frame_sync.cpp plants the sync word on a
// 160-bit grid, checks it is found and the grid locks (including under
// injected bit errors, an inverted stream, and a stream-wide offset), that
// random data does NOT lock, and end-to-end that a framed bitstream pushed
// through the GMSK modulator + demodulator still locks.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsdsrv {

// Bits per TETRAPOL frame (8-bit header + 152-bit data). The frame grid
// repeats at this interval on a continuous channel.
inline constexpr int kTetrapolFrameBits = 160;

// One frame-sync match in the bitstream.
struct TetrapolFrameMatch {
    std::size_t pos = 0;    // bit index where the 7-bit sync word starts
    int errors = 0;         // Hamming distance at this position
    bool inverted = false;  // matched the inverse pattern (raw stream inverted)
};

struct TetrapolSyncConfig {
    // Per-word Hamming tolerance. 1 matches tetrapol-kit's MAX_FRAME_SYNC_ERR.
    int max_errors = 1;
    // Grid lock needs a run of this many consecutive frames (160 bits apart)
    // all carrying the sync. A weak 7-bit word demands the adjacency test
    // rather than independent voting; 5 rejects random data comfortably while
    // a real downlink (sync in every frame) blows past it.
    int min_confirmations = 5;
};

struct TetrapolLock {
    bool locked = false;
    long phase = 0;        // sync position mod 160 of the locked grid
    int confirmations = 0; // length of the consecutive-frame run at that phase
    bool inverted = false; // the locked grid's polarity
};

class TetrapolFrameSync {
public:
    explicit TetrapolFrameSync(const TetrapolSyncConfig& cfg = TetrapolSyncConfig{}) : cfg_(cfg) {}

    // Every frame-sync match in the bitstream (0/1 bytes), in order. Both the
    // canonical pattern and its inverse are searched (polarity is recorded).
    std::vector<TetrapolFrameMatch> find_all(const uint8_t* bits, std::size_t n) const;

    // Reduce matches to the dominant 160-bit frame grid: the (phase, polarity)
    // with the longest run of consecutive frames. Locked once that run reaches
    // min_confirmations.
    TetrapolLock find_lock(const std::vector<TetrapolFrameMatch>& matches) const;

    // Convenience: find_all + find_lock over one buffer.
    TetrapolLock synchronize(const uint8_t* bits, std::size_t n) const {
        return find_lock(find_all(bits, n));
    }

    const TetrapolSyncConfig& config() const { return cfg_; }

private:
    TetrapolSyncConfig cfg_;
};

} // namespace dsdsrv
