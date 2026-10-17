// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include <algorithm>
#include <cmath>

namespace discipline::dsp {
// Real-time primitives: arithmetic only, with no allocation or host calls.
struct Frame {
    float l;
    float r;
    Frame operator+(Frame b) const noexcept { return {l + b.l, r + b.r}; }
    Frame operator*(float gain) const noexcept { return {l * gain, r * gain}; }
};
inline bool finite(Frame f) noexcept { return std::isfinite(f.l) && std::isfinite(f.r); }
inline float flush(float x) noexcept { return std::fabs(x) < 1.0e-15f ? 0.0f : x; }
inline Frame flush(Frame f) noexcept { return {flush(f.l), flush(f.r)}; }
inline Frame ceiling(Frame f) noexcept {
    return {std::clamp(f.l, -4.0f, 4.0f), std::clamp(f.r, -4.0f, 4.0f)};
}
} // namespace discipline::dsp
