#include "tetra_frontend.hpp"

namespace dsdsrv {

TetraDemodConfig TetraDemodFrontend::demod_cfg(bool coherent, int parity) const {
    TetraDemodConfig c = cfg_.demod;
    c.coherent = coherent;
    c.coherent_parity_offset = parity;
    return c;
}

TetraDemodFrontend::TetraDemodFrontend(const TetraFrontendConfig& cfg)
    : cfg_(cfg), sync_(cfg.sync) {
    if (!cfg_.coherent) {
        diff_ = std::make_unique<TetraDpqskDemod>(demod_cfg(false, 0));
    } else {
        cohA_ = std::make_unique<TetraDpqskDemod>(demod_cfg(true, 0));
        cohB_ = std::make_unique<TetraDpqskDemod>(demod_cfg(true, 1));
    }
}

std::vector<unsigned char>
TetraDemodFrontend::acquire(const std::vector<unsigned char>& a,
                            const std::vector<unsigned char>& b, bool finalizing) {
    accA_.insert(accA_.end(), a.begin(), a.end());
    accB_.insert(accB_.end(), b.begin(), b.end());

    const TetraLock la = sync_.synchronize(accA_.data(), accA_.size());
    const TetraLock lb = sync_.synchronize(accB_.data(), accB_.size());
    if (la.locked || lb.locked) {
        // Prefer the better-confirmed grid if (improbably) both locked.
        const int win = (lb.locked && (!la.locked || lb.confirmations > la.confirmations)) ? 1 : 0;
        selected_ = win;
        winner_ = std::move(win == 0 ? cohA_ : cohB_);
        std::vector<unsigned char> out = std::move(win == 0 ? accA_ : accB_);
        cohA_.reset();
        cohB_.reset();
        accA_.clear();
        accB_.clear();
        accA_.shrink_to_fit();
        accB_.shrink_to_fit();
        return out;
    }

    if (finalizing) {
        // End of stream, never locked. Best effort: emit the parity-0
        // candidate's bits (a non-TETRA signal has no right answer anyway).
        std::vector<unsigned char> out = std::move(accA_);
        accA_.clear();
        accB_.clear();
        return out;
    }

    if (accA_.size() >= cfg_.coherent_acquire_bits) {
        // Give up resolving parity: fall back to the differential path. The
        // acquisition window is dropped; a fresh differential demod re-acquires
        // timing from the next block. This is the safety net for a signal that
        // is not TETRA, or too weak for the Costas loop to lock.
        gave_up_ = true;
        cohA_.reset();
        cohB_.reset();
        accA_.clear();
        accB_.clear();
        accA_.shrink_to_fit();
        accB_.shrink_to_fit();
        diff_ = std::make_unique<TetraDpqskDemod>(demod_cfg(false, 0));
    }
    return {}; // still acquiring: emit nothing yet
}

std::vector<unsigned char>
TetraDemodFrontend::demodulate(const std::complex<float>* iq, std::size_t n) {
    if (!cfg_.coherent) return diff_->demodulate(iq, n);
    if (selected_ >= 0) return winner_->demodulate(iq, n);
    if (gave_up_) return diff_->demodulate(iq, n);
    // Acquiring: run both parities on the same IQ and look for a lock.
    const std::vector<unsigned char> a = cohA_->demodulate(iq, n);
    const std::vector<unsigned char> b = cohB_->demodulate(iq, n);
    return acquire(a, b, /*finalizing=*/false);
}

std::vector<unsigned char> TetraDemodFrontend::flush() {
    if (!cfg_.coherent) return diff_->flush();
    if (selected_ >= 0) return winner_->flush();
    if (gave_up_) return diff_->flush();
    const std::vector<unsigned char> a = cohA_->flush();
    const std::vector<unsigned char> b = cohB_->flush();
    return acquire(a, b, /*finalizing=*/true);
}

void TetraDemodFrontend::reset() {
    selected_ = -1;
    gave_up_ = false;
    winner_.reset();
    cohA_.reset();
    cohB_.reset();
    diff_.reset();
    accA_.clear();
    accB_.clear();
    if (!cfg_.coherent) {
        diff_ = std::make_unique<TetraDpqskDemod>(demod_cfg(false, 0));
    } else {
        cohA_ = std::make_unique<TetraDpqskDemod>(demod_cfg(true, 0));
        cohB_ = std::make_unique<TetraDpqskDemod>(demod_cfg(true, 1));
    }
}

double TetraDemodFrontend::last_cfo_hz() const {
    if (winner_) return winner_->last_cfo_hz();
    if (diff_) return diff_->last_cfo_hz();
    if (cohA_) return cohA_->last_cfo_hz();
    return 0.0;
}

} // namespace dsdsrv
