#include "tetrapol_frame_sync.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace dsdsrv {

namespace {

// The 7-bit differentially-encoded frame-sync word as it appears in the raw
// bitstream, verbatim from tetrapol-kit (lib/phys_ch.c, frame_dsync).
const uint8_t kFrameDsync[7] = { 1, 0, 1, 0, 0, 1, 1 };
constexpr int kSyncLen = 7;

// Hamming distance of the template (optionally inverted) against the stream at
// `pos`, short-circuiting once it exceeds `limit`.
int hamming(const uint8_t* stream, bool invert, int limit) {
    int e = 0;
    for (int i = 0; i < kSyncLen; ++i) {
        const uint8_t t = invert ? (kFrameDsync[i] ^ 1u) : kFrameDsync[i];
        if (stream[i] != t) {
            if (++e > limit) return e;
        }
    }
    return e;
}

} // namespace

std::vector<TetrapolFrameMatch>
TetrapolFrameSync::find_all(const uint8_t* bits, std::size_t n) const {
    std::vector<TetrapolFrameMatch> out;
    if (n < static_cast<std::size_t>(kSyncLen)) return out;
    const std::size_t last = n - kSyncLen;
    for (std::size_t i = 0; i <= last; ++i) {
        const int en = hamming(bits + i, /*invert=*/false, cfg_.max_errors);
        const int ei = hamming(bits + i, /*invert=*/true, cfg_.max_errors);
        // Take the better-fitting polarity; ties resolve to the canonical one.
        if (en <= cfg_.max_errors && en <= ei) {
            out.push_back({i, en, false});
        } else if (ei <= cfg_.max_errors) {
            out.push_back({i, ei, true});
        }
    }
    return out;
}

TetrapolLock
TetrapolFrameSync::find_lock(const std::vector<TetrapolFrameMatch>& matches) const {
    // Group matched frame indices by (phase, polarity); a match at bit `pos`
    // sits in frame k = pos/160 at phase pos%160.
    std::map<std::pair<long, bool>, std::vector<long>> groups;
    for (const TetrapolFrameMatch& m : matches) {
        const long phase = static_cast<long>(m.pos % kTetrapolFrameBits);
        const long k = static_cast<long>(m.pos / kTetrapolFrameBits);
        groups[{phase, m.inverted}].push_back(k);
    }

    TetrapolLock lock;
    for (auto& kv : groups) {
        std::vector<long>& ks = kv.second;
        std::sort(ks.begin(), ks.end());
        // Longest run of consecutive frame indices (each 160 bits after the
        // previous). Real TETRAPOL carries the sync in every frame -> a long
        // unbroken run; scattered random matches barely chain.
        int best_run = 0, run = 0;
        long prev = 0;
        for (std::size_t i = 0; i < ks.size(); ++i) {
            if (i > 0 && ks[i] == ks[i - 1]) continue; // dedupe (shouldn't happen)
            if (i > 0 && ks[i] == prev + 1) ++run;
            else run = 1;
            prev = ks[i];
            if (run > best_run) best_run = run;
        }
        if (best_run > lock.confirmations) {
            lock.confirmations = best_run;
            lock.phase = kv.first.first;
            lock.inverted = kv.first.second;
        }
    }
    lock.locked = lock.confirmations >= cfg_.min_confirmations;
    return lock;
}

} // namespace dsdsrv
