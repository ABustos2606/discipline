// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "rng.hpp"
#include <algorithm>
#include <cmath>

namespace discipline::dsp {
inline uint64_t snap_offset(double value, uint64_t size, uint64_t maximum) noexcept {
    const auto steps = static_cast<uint64_t>(std::llround(std::max(0.0, value) / size));
    return std::min(steps, maximum / size) * size;
}

class AutoScan {
public:
    enum class State { present, jump, away };
    explicit AutoScan(uint32_t seed = 0x6A09E667) noexcept { rng_.reset(seed); }
    // Reset playback state without restarting the random stream.
    void reset() noexcept { reset_position(); }
    // Real-time. Returns true only when a snap-back crossfade is requested.
    bool tick(bool boundary, float amount, uint64_t maximum, uint64_t size, bool reached) noexcept {
        if (amount <= 0 && state_ != State::present) { reset_position(); return true; }
        if (target_) {
            const auto limit = static_cast<uint64_t>(static_cast<double>(amount) * amount * maximum);
            if (limit < size) { reset_position(); return true; }
            target_ = std::max(size, snap_offset(target_, size, limit));
        }
        if (state_ == State::jump && reached) { state_ = State::away; hold_ = 1 + rng_.next() % 4; }
        if (!boundary) { return false; }
        if (state_ == State::away) {
            if (amount < 1 && !--hold_) { reset_position(); return true; }
        } else if (state_ == State::present && rng_.unit() < amount) {
            const auto depths = static_cast<uint64_t>(amount * amount * maximum) / size;
            if (depths) { target_ = (1 + rng_.next() % depths) * size; state_ = State::jump; }
        }
        return false;
    }
    uint64_t target() const noexcept { return target_; }
    State state() const noexcept { return state_; }
private:
    void reset_position() noexcept { target_ = 0; state_ = State::present; hold_ = 0; }
    Rng rng_;
    State state_ = State::present;
    uint64_t target_ = 0;
    uint32_t hold_ = 0;
};
} // namespace discipline::dsp
