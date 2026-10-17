// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace discipline::dsp {
class OnePole {
public:
    void configure(double rate, double seconds) noexcept { coefficient_ = -std::expm1(-1.0 / (rate * seconds)); }
    // Real-time section: per-sample, independent of host slicing.
    void reset(float value) noexcept { value_ = value; }
    float tick(float target) noexcept {
        value_ += coefficient_ * (target - value_);
        if (std::fabs(target - value_) < 1.0e-9) { value_ = target; }
        return static_cast<float>(value_);
    }
    float value() const noexcept { return static_cast<float>(value_); }
private:
    double coefficient_ = 1;
    double value_ = 0;
};

class Slew {
public:
    explicit Slew(double maximum_step = 1.0) noexcept : step_(maximum_step) {}
    // Real-time section.
    void reset(double value) noexcept { value_ = value; }
    double tick(double target) noexcept {
        value_ += std::clamp(target - value_, -step_, step_);
        return value_;
    }
private:
    double value_ = 0;
    double step_;
};

class MemoryClear {
public:
    explicit MemoryClear(uint32_t fade_frames) noexcept : fade_frames_(std::max(1U, fade_frames)) {}
    // Real-time: logical invalidation is O(1); both envelopes advance per sample.
    void reset() noexcept { remaining_ = fade_in_ = 0; value_ = 1; }
    void request() noexcept {
        if (!remaining_) { start_ = value_; remaining_ = fade_frames_; fade_in_ = 0; }
    }
    template<class Ring> float tick(Ring& memory) noexcept {
        if (remaining_) {
            value_ = start_ * static_cast<float>(remaining_ - 1) / fade_frames_;
            if (!--remaining_) { memory.clear(); fade_in_ = 256; }
            return value_;
        }
        if (fade_in_) { value_ = static_cast<float>(256 - --fade_in_) / 256.0f; }
        return value_;
    }
private:
    uint32_t fade_frames_;
    uint32_t remaining_ = 0;
    uint32_t fade_in_ = 0;
    float value_ = 1, start_ = 1;
};

class Ramp {
public:
    void configure(double rate, double up, double down) noexcept {
        up_ = 1.0 / std::max(1.0, rate * up); down_ = 1.0 / std::max(1.0, rate * down);
    }
    // Real-time: exact endpoints, including unity for a held Loop.
    void reset(float value) noexcept { value_ = value; }
    float tick(float target) noexcept {
        value_ += std::clamp(static_cast<double>(target) - value_, -down_, up_);
        return static_cast<float>(value_);
    }
private:
    double value_ = 0, up_ = 1, down_ = 1;
};
} // namespace discipline::dsp
