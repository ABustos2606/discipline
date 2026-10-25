// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "clock_out.hpp"
#include "speed_reader.hpp"
#include "smoother.hpp"
#include <array>

namespace discipline::dsp {
// Modifier 5 sends the echo out through Send and back through the Return,
// in place of the built-in modifiers.
inline constexpr int kOutside = 5;

class Modifiers {
public:
    bool configure(double rate) noexcept {
        rate_ = rate;
        max_window_ = static_cast<uint64_t>(std::llround(rate * 30));
        for (auto& gain : mix_) { gain.configure(rate, 0.01); }
        for (auto* smooth : {&amount_, &speed_, &unity_}) { smooth->configure(rate, 0.01); }
        drop_gain_.configure(rate, 0.001, 0.001);
        trim_gain_.configure(rate, 0.003, 0.003);
        stop_gain_.configure(rate, 0.005, 0.005);
        return reader_.configure(rate);
    }
    // Real-time: modifiers process the echo at its full Size, inside the
    // loop. IN and FEED repeat the result; OUT repeats the plain echo.
    void reset(int mode, float amount) noexcept {
        for (size_t i = 0; i < mix_.size(); ++i) { mix_[i].reset(static_cast<int>(i) == mode); }
        speed_mode_ = mode == 1 ? 1 : 2;
        const auto speed = playback_rate(speed_mode_, amount);
        amount_.reset(amount); speed_.reset(static_cast<float>(speed)); unity_.reset(speed == 1);
        drop_gain_.reset(1); trim_gain_.reset(1); reader_.reset();
        stop_gain_.reset(static_cast<float>(std::min(1.0, std::fabs(speed) / 0.05)));
    }
    void clear() noexcept { reader_.reset(); }
    static double playback_rate(int mode, float amount) noexcept {
        if (mode == 1) {
            constexpr double steps[]{-4, -3, -2, -1.5, -1, -0.75, -0.5, 0, 0.5, 0.75, 1, 1.5, 2, 3, 4};
            return steps[static_cast<size_t>(std::lround(std::clamp(amount, 0.0f, 1.0f) * 14))];
        }
        // Smooth: stopped in a small band at noon, then pitch moves evenly in
        // octaves: 1/4x just off noon, 1x at 3/4 of each side, 2x at the end.
        const double x = 2.0 * std::clamp(amount, 0.0f, 1.0f) - 1;
        constexpr double stop = 0.04, unity = (0.5 - stop) / (1 - stop);
        if (std::fabs(x) < stop) { return 0; }
        const double u = (std::fabs(x) - stop) / (1 - stop);
        const double magnitude = u <= unity ? std::exp2(-2 + 2 * u / unity) : std::exp2((u - unity) / (1 - unity));
        const double value = std::copysign(magnitude, x);
        return std::fabs(value - 1) < 1.0 / 128 ? 1 : value;
    }
    // Trimmer has 15 positions: noon is neutral (0); -1..-7 keep the start
    // of each echo, +1..+7 keep its end, from 3/4 down to 1/8. The rest of
    // the echo is silent, so each echo gets shorter.
    static int trim_step(float amount) noexcept {
        return static_cast<int>(std::lround(std::clamp(amount, 0.0f, 1.0f) * 14)) - 7;
    }
    static double trim_keep(int step) noexcept {
        constexpr double keep[]{1, 0.75, 2.0 / 3, 0.5, 1.0 / 3, 0.25, 1.0 / 6, 0.125};
        return keep[std::abs(step)];
    }
    Frame process(const StereoRing& echo, Frame plain, EchoPhase timing, int mode, float amount) noexcept {
        if (mode == 1 || mode == 2) { speed_mode_ = mode; }
        const float setting = amount_.tick(amount);
        const double target_speed = playback_rate(speed_mode_, amount);
        const double speed = speed_.tick(static_cast<float>(target_speed));
        const float unity = unity_.tick(target_speed == 1);
        std::array<float, 5> weights{};
        for (size_t i = 0; i < weights.size(); ++i) { weights[i] = mix_[i].tick(static_cast<int>(i) == mode); }
        const float speed_weight = weights[1] + weights[2];
        const auto window = speed_window(timing);
        const auto pitched = reader_.tick(echo, window.length, window.phase, speed, speed_weight > 0 && unity < 1);
        const float stop_gain = stop_gain_.tick(static_cast<float>(std::min(1.0, std::fabs(speed) / 0.05)));
        const auto shifted = unity == 1 ? plain : plain * unity + pitched * ((1 - unity) * stop_gain);
        const float trim = trim_gain_.tick(trim_target(timing, trim_step(amount)));
        if (timing.boundary()) { ++echo_count_; }
        const float drop = drop_gain_.tick(drop_target(timing, setting));
        if (weights[0] == 1) { return plain; }
        if ((speed_weight == 1 && unity == 1) || (weights[3] == 1 && trim == 1) || (weights[4] == 1 && drop == 1)) { return plain; }
        return plain * (weights[0] + drop * weights[4] + trim * weights[3]) + shifted * speed_weight;
    }
private:
    // A captured window is read for one more window, so it can be at most
    // half the 60 s echo line. Sizes above 30 s play two chunks per echo.
    EchoPhase speed_window(EchoPhase timing) const noexcept {
        if (timing.length <= max_window_) { return timing; }
        const uint64_t half = timing.length / 2;
        return timing.phase < half ? EchoPhase{timing.phase, half} : EchoPhase{timing.phase - half, timing.length - half};
    }
    // Trimmer opens a gate over the kept part of each echo; the gain ramp
    // gives each cut a 3 ms edge.
    static float trim_target(EchoPhase timing, int step) noexcept {
        if (step == 0) { return 1; }
        const auto kept = std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(timing.length * trim_keep(step))));
        return step < 0 ? timing.phase < kept : timing.phase >= timing.length - kept;
    }
    static double smoothstep(double x) noexcept { x = std::clamp(x, 0.0, 1.0); return x * x * (3 - 2 * x); }
    static uint32_t hash(uint32_t value) noexcept {
        value ^= 0xB7E15162; value ^= value >> 16; value *= 0x7FEB352D;
        value ^= value >> 15; value *= 0x846CA68B; return value ^ (value >> 16);
    }
    static double unit(uint32_t value) noexcept { return value * (1.0 / 4294967296.0); }
    struct Drop { bool dropped; double depth, edge; };
    // Pattern drops repeat every echo; random drops get a new draw per echo.
    // Shaped drops vary: some shallow and smooth, others deep and abrupt.
    Drop drop_at(uint64_t echo, int64_t step, uint32_t steps, double density, double width, bool random, bool shaped) const noexcept {
        if (step < 0) { --echo; step += steps; }
        if (step >= static_cast<int64_t>(steps)) { ++echo; step -= steps; }
        const uint32_t salt = random ? hash(static_cast<uint32_t>(echo) * 0x9E3779B9U + 0x632BE5ABU) : 0;
        const uint32_t key = hash(static_cast<uint32_t>(step) ^ salt);
        if (unit(key) >= density) { return {false, 0, 1}; }
        // Grains get soft edges, so the granular end of the knob sounds like
        // grains rather than clicks.
        double depth = 1, edge = std::max(1.0, width * 0.15);
        if (shaped) {
            const double shape = unit(hash(key + 0x9E3779B9U)), softness = unit(hash(key + 0x7F4A7C15U));
            depth = shape > 0.6 ? 1 : 0.35 + shape;
            edge = std::max(1.0, std::min(width * 0.25, rate_ * (0.0005 + 0.0195 * softness)));
        }
        return {true, depth, edge};
    }
    float mask(EchoPhase timing, uint32_t steps, double density, bool random, bool shaped) const noexcept {
        const double position = static_cast<double>(timing.phase) * steps / timing.length;
        const auto index = static_cast<int64_t>(std::min(static_cast<uint32_t>(position), steps - 1));
        const double width = static_cast<double>(timing.length) / steps;
        const auto here = drop_at(echo_count_, index, steps, density, width, random, shaped);
        if (!here.dropped) { return 1; }
        const auto before = drop_at(echo_count_, index - 1, steps, density, width, random, shaped);
        const auto after = drop_at(echo_count_, index + 1, steps, density, width, random, shaped);
        const double within = (position - index) * width;
        const double in = smoothstep(within / here.edge), out = smoothstep((width - within) / here.edge);
        // Neighbouring drops join without returning to full level between them.
        const double depth = before.dropped ? before.depth + (here.depth - before.depth) * in : here.depth;
        const double envelope = (before.dropped ? 1 : in) * (after.dropped ? 1 : out);
        return static_cast<float>(1 - depth * envelope);
    }
    // Dropper: noon is neutral; left drops at random, right repeats a pattern.
    // The last quarter of either side moves toward a 15 ms grain grid, with
    // half of the grains kept.
    float drop_target(EchoPhase timing, float amount) const noexcept {
        if (amount == 0.5f) { return 1; }
        const bool random = amount < 0.5f;
        const double depth = std::fabs(amount - 0.5) * 2;
        const double density = depth * 0.8;
        const float coarse = mask(timing, 16, density, random, true);
        const float grain = static_cast<float>(std::clamp((depth - 0.75) * 4, 0.0, 1.0));
        if (grain == 0) { return coarse; }
        const auto steps = std::max(16U, static_cast<uint32_t>(std::ceil(timing.length / (rate_ * 0.015))));
        return coarse * (1 - grain) + mask(timing, steps, std::min(density, 0.5), random, false) * grain;
    }
    SpeedReader reader_;
    std::array<OnePole, 5> mix_;
    OnePole amount_, speed_, unity_;
    Ramp drop_gain_, trim_gain_, stop_gain_;
    double rate_ = 48000;
    uint64_t max_window_ = 1440000;
    uint64_t echo_count_ = 0;
    int speed_mode_ = 2;
};
} // namespace discipline::dsp
