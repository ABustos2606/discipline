// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "clock_out.hpp"
#include "speed_reader.hpp"
#include "smoother.hpp"
#include <array>

namespace discipline::dsp {
// Real-time: reads the echo line at a whole-frame delay. A delay jump
// crossfades over 1 ms; both heads keep moving at the normal rate.
class TrimReader {
public:
    bool configure(double rate) noexcept {
        fade_length_ = std::max(2U, static_cast<uint32_t>(std::lround(rate * 0.001)));
        weights_.reset(new (std::nothrow) Frame[fade_length_]);
        if (!weights_) { return false; }
        constexpr double pi = 3.141592653589793;
        for (uint32_t i = 0; i < fade_length_; ++i) {
            const float b = static_cast<float>(0.5 - 0.5 * std::cos(pi * i / (fade_length_ - 1)));
            weights_[i] = {1 - b, b};
        }
        reset(); return true;
    }
    void reset() noexcept { current_ = old_ = 0; phase_ = fade_length_; initialized_ = hold_ = false; last_ = held_ = {}; }
    Frame tick(const StereoRing& ring, uint64_t delay) noexcept {
        if (!initialized_) { current_ = delay; initialized_ = true; }
        else if (delay != current_) {
            // A jump inside a fade continues from the last mixed sample.
            hold_ = phase_ < fade_length_; held_ = last_;
            old_ = current_; current_ = delay; phase_ = 0;
        }
        Frame result = ring.read(current_);
        if (phase_ < fade_length_) {
            const Frame weights = weights_[phase_++];
            result = (hold_ ? held_ : ring.read(old_)) * weights.l + result * weights.r;
        }
        last_ = result;
        return result;
    }
private:
    std::unique_ptr<Frame[]> weights_;
    uint64_t current_ = 0, old_ = 0;
    uint32_t phase_ = 0, fade_length_ = 2;
    bool initialized_ = false, hold_ = false;
    Frame last_{}, held_{};
};

class Modifiers {
public:
    bool configure(double rate) noexcept {
        rate_ = rate;
        max_window_ = static_cast<uint64_t>(std::llround(rate * 30));
        for (auto& gain : mix_) { gain.configure(rate, 0.01); }
        for (auto* smooth : {&amount_, &speed_, &unity_, &trimming_}) { smooth->configure(rate, 0.01); }
        drop_gain_.configure(rate, 0.001, 0.001);
        stop_gain_.configure(rate, 0.005, 0.005);
        return reader_.configure(rate) && trimmer_.configure(rate);
    }
    // Real-time: modifiers act on Send. OUT's feedback stays unmodified;
    // IN/FEED receive the modified signal through the normal Return path.
    void reset(int mode, float amount) noexcept {
        for (size_t i = 0; i < mix_.size(); ++i) { mix_[i].reset(static_cast<int>(i) == mode); }
        speed_mode_ = mode == 1 ? 1 : 2;
        const auto speed = playback_rate(speed_mode_, amount);
        amount_.reset(amount); speed_.reset(static_cast<float>(speed)); unity_.reset(speed == 1);
        trimming_.reset(trim_step(amount) != 0);
        drop_gain_.reset(1); reader_.reset(); trimmer_.reset();
        stop_gain_.reset(static_cast<float>(std::min(1.0, std::fabs(speed) / 0.05)));
    }
    void clear() noexcept { reader_.reset(); trimmer_.reset(); }
    static double playback_rate(int mode, float amount) noexcept {
        if (mode == 1) {
            constexpr double steps[]{-4, -3, -2, -1.5, -1, -0.75, -0.5, 0, 0.5, 0.75, 1, 1.5, 2, 3, 4};
            return steps[static_cast<size_t>(std::lround(std::clamp(amount, 0.0f, 1.0f) * 14))];
        }
        const double value = 4.0 * amount - 2;
        return std::fabs(value - 1) < 1.0 / 128 ? 1 : value;
    }
    // Trimmer has 15 positions: noon is neutral (0); -1..-7 keep the start
    // of each echo, +1..+7 keep its end, from 3/4 down to 1/8.
    static int trim_step(float amount) noexcept {
        return static_cast<int>(std::lround(std::clamp(amount, 0.0f, 1.0f) * 14)) - 7;
    }
    static double trim_keep(int step) noexcept {
        constexpr double keep[]{1, 0.75, 2.0 / 3, 0.5, 1.0 / 3, 0.25, 1.0 / 6, 0.125};
        return keep[std::abs(step)];
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
        const int step = trim_step(amount);
        const float trimming = trimming_.tick(step != 0);
        const auto sliced = trimmer_.tick(echo, trim_delay(window, lead, step));
        const auto trimmed = trimming == 0 ? plain : plain * (1 - trimming) + sliced * trimming;
        if (timing.boundary()) { ++echo_count_; }
        const float drop = drop_gain_.tick(drop_target(timing, setting));
        if (weights[0] == 1) { return plain; }
        if ((speed_weight == 1 && unity == 1) || (weights[3] == 1 && trimming == 0) || (weights[4] == 1 && drop == 1)) { return plain; }
        return plain * (weights[0] + drop * weights[4]) + shifted * speed_weight + trimmed * weights[3];
    }
private:
    // A captured window is read for one more window, so it can be at most
    // half the 60 s echo line. Sizes above 30 s play two chunks per echo.
    EchoPhase speed_window(EchoPhase timing) const noexcept {
        if (timing.length <= max_window_) { return timing; }
        const uint64_t half = timing.length / 2;
        return timing.phase < half ? EchoPhase{timing.phase, half} : EchoPhase{timing.phase - half, timing.length - half};
    }
    // Trimmer keeps a slice of each window and repeats it to fill the window.
    // Send reads delay W - lead at window phase 0, so echo position p at phase
    // phi is at delay (W - lead) + phi - p. Delays stay below 2W.
    static uint64_t trim_delay(EchoPhase window, uint32_t lead, int step) noexcept {
        const uint64_t base = window.length > lead ? window.length - lead : 1;
        if (step == 0) { return base; }
        const auto slice = std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(window.length * trim_keep(step))));
        const uint64_t repeat = window.phase - window.phase % slice;
        if (step < 0) { return base + repeat; }
        // The end of this echo is already in the line unless the slice is
        // shorter than the host period; then the previous echo's end is used.
        const int64_t delay = static_cast<int64_t>(slice) - static_cast<int64_t>(lead) + static_cast<int64_t>(repeat);
        return static_cast<uint64_t>(delay >= 1 ? delay : delay + static_cast<int64_t>(window.length));
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
        double depth = 1, edge = std::max(1.0, std::min(rate_ * 0.0005, width * 0.25));
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
    // The last 10% of either side moves toward a 5 ms grain grid.
    float drop_target(EchoPhase timing, float amount) const noexcept {
        if (amount == 0.5f) { return 1; }
        const bool random = amount < 0.5f;
        const double depth = std::fabs(amount - 0.5) * 2;
        const double density = depth * 0.8;
        const float coarse = mask(timing, 16, density, random, true);
        const float grain = static_cast<float>(std::clamp((depth - 0.9) * 10, 0.0, 1.0));
        if (grain == 0) { return coarse; }
        const auto steps = std::max(16U, static_cast<uint32_t>(std::ceil(timing.length / (rate_ * 0.005))));
        return coarse * (1 - grain) + mask(timing, steps, density, random, false) * grain;
    }
    SpeedReader reader_;
    TrimReader trimmer_;
    std::array<OnePole, 5> mix_;
    OnePole amount_, speed_, unity_, trimming_;
    Ramp drop_gain_, stop_gain_;
    double rate_ = 48000;
    uint64_t max_window_ = 1440000;
    uint64_t echo_count_ = 0;
    int speed_mode_ = 2;
};
} // namespace discipline::dsp
