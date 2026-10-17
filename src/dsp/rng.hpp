// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include <cstdint>

namespace discipline::dsp {
class Rng {
public:
    void reset(uint32_t seed = 0x6A09E667) noexcept { state_ = seed ? seed : 1; }
    // Real-time: bounded, deterministic, instance-local randomness.
    uint32_t next() noexcept {
        state_ ^= state_ << 13; state_ ^= state_ >> 17; state_ ^= state_ << 5;
        return state_;
    }
    double unit() noexcept { return next() * (1.0 / 4294967296.0); }
private:
    uint32_t state_ = 0x6A09E667;
};
} // namespace discipline::dsp
