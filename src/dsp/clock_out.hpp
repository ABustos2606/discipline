// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace discipline::dsp {
struct EchoPhase {
    uint64_t phase, length;
    bool boundary() const noexcept { return phase == 0; }
    EchoPhase ahead(uint32_t frames, uint64_t next_length) const noexcept {
        const auto next = phase + frames;
        return next < length ? EchoPhase{next, length} : EchoPhase{next - length, next_length};
    }
};
class ClockOut {
public:
    void configure(double rate) noexcept { width_ = std::max(1U, static_cast<uint32_t>(std::lround(rate * 0.001))); }
    // Real-time. A Size change takes effect at a wrap, never halfway through
    // a pulse/period. Activation starts with an edge at sample zero.
    void reset() noexcept { phase_ = 0; length_ = 1; }
    EchoPhase tick(uint64_t current_size) noexcept {
        if (!phase_) { length_ = std::max<uint64_t>(1, current_size); }
        const EchoPhase result{phase_, length_};
        if (++phase_ >= length_) { phase_ = 0; }
        return result;
    }
    float value(EchoPhase phase) const noexcept { return phase.phase < width_ ? 1.0f : 0.0f; }
private:
    uint64_t phase_ = 0, length_ = 1;
    uint32_t width_ = 1;
};
} // namespace discipline::dsp
