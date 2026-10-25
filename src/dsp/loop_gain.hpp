// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "frame.hpp"

namespace discipline::dsp {
class LoopGain {
public:
    void configure(double rate) noexcept {
        attack_ = -std::expm1(-1.0 / (rate * 0.001));
        release_ = -std::expm1(-1.0 / (rate * 0.3));
    }
    // Real-time: unity below threshold, linked stereo envelope, safety ceiling.
    void reset() noexcept { gain_ = 1; }
    Frame process(Frame input) noexcept {
        input = ceiling(input);
        const float peak = std::max(std::fabs(input.l), std::fabs(input.r));
        const double target = peak > 0.5f ? 0.5 / peak : 1.0;
        gain_ += (target < gain_ ? attack_ : release_) * (target - gain_);
        if (target == 1.0 && 1.0 - gain_ < 1.0e-9) { gain_ = 1.0; }
        return flush(ceiling(input * static_cast<float>(gain_)));
    }
private:
    double gain_ = 1, attack_ = 1, release_ = 1;
};
} // namespace discipline::dsp
