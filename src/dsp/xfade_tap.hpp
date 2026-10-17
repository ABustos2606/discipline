// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "frame.hpp"
#include <cstdint>
#include <memory>
#include <new>

namespace discipline::dsp {
class CrossfadeTap {
public:
    struct Read { Frame send; Frame feedback; };
    bool configure(uint32_t frames) noexcept {
        length_ = std::max(2U, frames);
        weights_.reset(new (std::nothrow) Frame[length_]);
        if (!weights_) { return false; }
        for (uint32_t i = 0; i < length_; ++i) {
            const double phase = 1.5707963267948966 * i / (length_ - 1);
            weights_[i] = {static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
        }
        return true;
    }
    // Real-time section: fixed taps; a target waits for the current fade.
    void reset(uint64_t delay) noexcept { current_ = incoming_ = target_ = delay; phase_ = length_; }
    void target(uint64_t delay) noexcept { target_ = delay; }
    uint64_t current() const noexcept { return current_; }
    bool fading() const noexcept { return phase_ < length_; }
    template<class Ring> Read read_pair(const Ring& ring, uint32_t return_delay) noexcept {
        const uint64_t difference = current_ > target_ ? current_ - target_ : target_ - current_;
        if (!fading() && difference > 1) { incoming_ = target_; phase_ = 0; }
        if (!fading()) { return {ring.read(current_), ring.read(current_ + return_delay)}; }
        const Frame weights = weights_[phase_++];
        const Read result{ring.read(current_) * weights.l + ring.read(incoming_) * weights.r,
                          ring.read(current_ + return_delay) * weights.l + ring.read(incoming_ + return_delay) * weights.r};
        if (!fading()) { current_ = incoming_; }
        return result;
    }
    template<class Ring> Frame read(const Ring& ring) noexcept { return read_pair(ring, 0).send; }
private:
    std::unique_ptr<Frame[]> weights_;
    uint64_t current_ = 1, incoming_ = 1, target_ = 1;
    uint32_t phase_ = 0, length_ = 2;
};
} // namespace discipline::dsp
