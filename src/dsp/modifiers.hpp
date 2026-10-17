// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "clock_out.hpp"
#include "speed_reader.hpp"
#include "smoother.hpp"
#include <array>

namespace discipline::dsp {
class Modifiers {
public:
    bool configure(double rate) noexcept {
        rate_ = rate;
        max_window_ = static_cast<uint64_t>(std::llround(rate * 30));
        for (auto& gain : mix_) { gain.configure(rate, 0.01); }
        for (auto* smooth : {&amount_, &speed_, &unity_}) { smooth->configure(rate, 0.01); }
        trim_gain_.configure(rate, 0.001, 0.001); drop_gain_.configure(rate, 0.001, 0.001);
        stop_gain_.configure(rate, 0.005, 0.005);
        return reader_.configure(rate);
    }
    // Real-time: modifiers act on Send. OUT's feedback stays unmodified;
    // IN/FEED receive the modified signal through the normal Return path.
    void reset(int mode, float amount) noexcept {
        for (size_t i = 0; i < mix_.size(); ++i) { mix_[i].reset(static_cast<int>(i) == mode); }
        speed_mode_ = mode == 1 ? 1 : 2;
        const auto speed = playback_rate(speed_mode_, amount);
        amount_.reset(amount); speed_.reset(static_cast<float>(speed)); unity_.reset(speed == 1);
        trim_gain_.reset(1); drop_gain_.reset(1); reader_.reset();
        stop_gain_.reset(static_cast<float>(std::min(1.0, std::fabs(speed) / 0.05)));
    }
    void clear() noexcept { reader_.reset(); }
    static double playback_rate(int mode, float amount) noexcept {
        if (mode == 1) {
            constexpr double steps[]{-4, -3, -2, -1.5, -1, -0.75, -0.5, 0, 0.5, 0.75, 1, 1.5, 2, 3, 4};
            return steps[static_cast<size_t>(std::lround(std::clamp(amount, 0.0f, 1.0f) * 14))];
        }
        const double value = 4.0 * amount - 2;
        return std::fabs(value - 1) < 1.0 / 128 ? 1 : value;
    }
    Frame process(const StereoRing& echo, Frame plain, EchoPhase timing, uint32_t lead, int mode, float amount) noexcept {
        if (mode == 1 || mode == 2) { speed_mode_ = mode; }
        const float setting = amount_.tick(amount);
        const double target_speed = playback_rate(speed_mode_, amount);
        const double speed = speed_.tick(static_cast<float>(target_speed));
        const float unity = unity_.tick(target_speed == 1);
        std::array<float, 5> weights{};
        for (size_t i = 0; i < weights.size(); ++i) { weights[i] = mix_[i].tick(static_cast<int>(i) == mode); }
        const float speed_weight = weights[1] + weights[2];
        const auto window = speed_window(timing);
        const auto pitched = reader_.tick(echo, window.length, window.phase, lead, speed, speed_weight > 0 && unity < 1);
        const float stop_gain = stop_gain_.tick(static_cast<float>(std::min(1.0, std::fabs(speed) / 0.05)));
        const auto shifted = unity == 1 ? plain : plain * unity + pitched * ((1 - unity) * stop_gain);
        const float trim = trim_gain_.tick(trim_target(timing, setting));
        const float drop = drop_gain_.tick(drop_target(timing, setting));
        if (weights[0] == 1) { return plain; }
        if ((speed_weight == 1 && unity == 1) || (weights[3] == 1 && trim == 1) || (weights[4] == 1 && drop == 1)) { return plain; }
        return plain * weights[0] + shifted * speed_weight + plain * (trim * weights[3] + drop * weights[4]);
    }
private:
    // A captured window is read for one more window, so it can be at most
    // half the 60 s echo line. Sizes above 30 s play two chunks per echo.
    EchoPhase speed_window(EchoPhase timing) const noexcept {
        if (timing.length <= max_window_) { return timing; }
        const uint64_t half = timing.length / 2;
        return timing.phase < half ? EchoPhase{timing.phase, half} : EchoPhase{timing.phase - half, timing.length - half};
    }
    static double smoothstep(double x) noexcept { x = std::clamp(x, 0.0, 1.0); return x * x * (3 - 2 * x); }
    float trim_target(EchoPhase timing, float amount) const noexcept {
        const double cut = 2.0 * amount - 1;
        if (cut == 0) { return 1; }
        const double start = cut < 0 ? -cut * timing.length : 0;
        const double end = cut > 0 ? (1 - cut) * timing.length : static_cast<double>(timing.length);
        const double width = end - start;
        if (width <= 0) { return 0; }
        const double edge = std::max(1.0, std::min(rate_ * 0.001, width * 0.25));
        const double gate = smoothstep((timing.phase - start) / edge) * smoothstep((end - timing.phase) / edge);
        const double strength = std::min(1.0, std::fabs(cut) * timing.length / std::max(1.0, rate_ * 0.001));
        return static_cast<float>(1 - strength + gate * strength);
    }
    static uint32_t hash(uint32_t value) noexcept {
        value ^= 0xB7E15162; value ^= value >> 16; value *= 0x7FEB352D;
        value ^= value >> 15; value *= 0x846CA68B; return value ^ (value >> 16);
    }
    float mask(EchoPhase timing, uint32_t steps, float density) const noexcept {
        const double position = static_cast<double>(timing.phase) * steps / timing.length;
        const auto index = std::min(static_cast<uint32_t>(position), steps - 1);
        const auto kept = [&](uint32_t step) { return hash(step) * (1.0 / 4294967296.0) >= density; };
        if (!kept(index)) { return 0; }
        const double width = static_cast<double>(timing.length) / steps;
        const double within = (position - index) * width;
        const double edge = std::max(1.0, std::min(rate_ * 0.0005, width * 0.25));
        const double in = kept((index + steps - 1) % steps) ? 1 : smoothstep(within / edge);
        const double out = kept((index + 1) % steps) ? 1 : smoothstep((width - within) / edge);
        return static_cast<float>(in * out);
    }
    float drop_target(EchoPhase timing, float amount) const noexcept {
        if (amount == 0) { return 1; }
        const float density = amount * 0.8f;
        const float coarse = mask(timing, 16, density);
        const float grain = std::clamp((amount - 0.9f) * 10, 0.0f, 1.0f);
        if (grain == 0) { return coarse; }
        const auto steps = std::max(16U, static_cast<uint32_t>(std::ceil(timing.length / (rate_ * 0.005))));
        return coarse * (1 - grain) + mask(timing, steps, density) * grain;
    }
    SpeedReader reader_;
    std::array<OnePole, 5> mix_;
    OnePole amount_, speed_, unity_;
    Ramp trim_gain_, drop_gain_, stop_gain_;
    double rate_ = 48000;
    uint64_t max_window_ = 1440000;
    int speed_mode_ = 2;
};
} // namespace discipline::dsp
