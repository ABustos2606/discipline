// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "frame.hpp"

namespace discipline::dsp {
// Real-time: four-point cubic Hermite interpolation, y1 to y2.
inline float hermite(float y0, float y1, float y2, float y3, float t) noexcept
{
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}
inline Frame hermite(Frame a, Frame b, Frame c, Frame d, float t) noexcept
{
    return {hermite(a.l, b.l, c.l, d.l, t), hermite(a.r, b.r, c.r, d.r, t)};
}
} // namespace discipline::dsp
