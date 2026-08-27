#include "tetra_burst_sync.hpp"

#include <climits>

namespace dsdsrv {

namespace {

// Training-sequence bit patterns, verbatim from osmo-tetra
// (src/phy/tetra_burst.c), which takes them from ETSI EN 300 392-2.
const uint8_t n_bits[22] = { 1,1, 0,1, 0,0, 0,0, 1,1, 1,0, 1,0, 0,1, 1,1, 0,1, 0,0 };            // normal 1
const uint8_t p_bits[22] = { 0,1, 1,1, 1,0, 1,0, 0,1, 0,0, 0,0, 1,1, 0,1, 1,1, 1,0 };            // normal 2
const uint8_t y_bits[38] = { 1,1, 0,0, 0,0, 0,1, 1,0, 0,1, 1,1, 0,0, 1,1, 1,0, 1,0, 0,1,
                             1,1, 0,0, 0,0, 0,1, 1,0, 0,1, 1,1 };                                  // sync
const uint8_t x_bits[30] = { 1,0, 0,1, 1,1, 0,1, 0,0, 0,0, 1,1, 1,0, 1,0, 0,1,
                             1,1, 0,1, 0,0, 0,0, 1,1 };                                            // extended

// Within-burst bit offset of each training sequence's first bit (osmo-tetra
// sync logic): the burst's first bit is at (match position − offset).
constexpr int kOffsetSync = 214;
constexpr int kOffsetNorm = 244;

struct SeqDef {
    const uint8_t* bits;
    int len;
    TetraTrain type;
    int offset;   // within-burst offset, or -1 if unknown (excluded from grid)
    int max_err;
};

// Hamming distance between a template and the stream at `pos`, short-circuit
// once it exceeds `limit` (so a mismatch is cheap).
int hamming(const uint8_t* stream, const uint8_t* templ, int len, int limit) {
    int e = 0;
    for (int i = 0; i < len; ++i) {
        if (stream[i] != templ[i]) {
            if (++e > limit) return e;
        }
    }
    return e;
}

} // namespace

std::vector<TrainMatch> TetraBurstSync::find_all(const uint8_t* bits, std::size_t n) const {
    const SeqDef defs[] = {
        { y_bits, 38, TetraTrain::Sync,  kOffsetSync, cfg_.max_errors_sync },
        { n_bits, 22, TetraTrain::Norm1, kOffsetNorm, cfg_.max_errors_norm },
        { p_bits, 22, TetraTrain::Norm2, kOffsetNorm, cfg_.max_errors_norm },
        { x_bits, 30, TetraTrain::Ext,   -1,          cfg_.max_errors_ext  },
    };

    std::vector<TrainMatch> out;
    for (std::size_t i = 0; i < n; ++i) {
        // Best (fewest-error) sequence that matches starting at bit i.
        const SeqDef* best = nullptr;
        int best_err = INT_MAX;
        for (const SeqDef& d : defs) {
            if (i + static_cast<std::size_t>(d.len) > n) continue;
            int e = hamming(bits + i, d.bits, d.len, d.max_err);
            if (e <= d.max_err && e < best_err) {
                best_err = e;
                best = &d;
            }
        }
        if (!best) continue;

        TrainMatch m;
        m.pos = i;
        m.type = best->type;
        m.errors = best_err;
        if (best->offset >= 0) {
            m.burst_start = static_cast<long>(i) - best->offset;
            m.has_burst_start = true;
        } else {
            m.burst_start = LONG_MIN;
            m.has_burst_start = false;
        }
        out.push_back(m);
    }
    return out;
}

TetraLock TetraBurstSync::find_lock(const std::vector<TrainMatch>& matches) const {
    // Vote on the 510-bit grid phase (burst_start mod 510). A real downlink
    // grid produces one dominant phase across many bursts; scattered false
    // matches in random data do not concentrate on any single phase.
    int votes[kTetraBitsPerTs] = {0};
    for (const TrainMatch& m : matches) {
        if (!m.has_burst_start) continue;
        long ph = m.burst_start % kTetraBitsPerTs;
        if (ph < 0) ph += kTetraBitsPerTs;
        votes[ph]++;
    }
    TetraLock lock;
    for (int ph = 0; ph < kTetraBitsPerTs; ++ph) {
        if (votes[ph] > lock.confirmations) {
            lock.confirmations = votes[ph];
            lock.phase = ph;
        }
    }
    lock.locked = lock.confirmations >= cfg_.min_confirmations;
    return lock;
}

} // namespace dsdsrv
