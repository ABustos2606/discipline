// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "stereo_ring.hpp"
#include <memory>

namespace discipline::dsp {
class SpeedReader {
public:
    bool configure(double rate) noexcept {
        fade_length_ = std::max(2U, static_cast<uint32_t>(std::lround(rate * 0.001)));
        weights_.reset(new (std::nothrow) Frame[fade_length_]);
        kernels_.reset(new (std::nothrow) float[bands * phases * taps]);
        if (!weights_ || !kernels_) { return false; }
        constexpr double pi = 3.141592653589793;
        for (uint32_t i = 0; i < fade_length_; ++i) {
            const float b = static_cast<float>(0.5 - 0.5 * std::cos(pi * i / (fade_length_ - 1)));
            weights_[i] = {1 - b, b};
        }
        // Instantiate only: windowed-sinc antialias filters for playback above
        // 1x. Adjacent fractional phases interpolate; each kernel has unity DC.
        for (uint32_t band = 0; band < bands; ++band) {
            const double cutoff = 0.45 / (1 + 0.25 * band);
            for (uint32_t phase = 0; phase < phases; ++phase) {
                double sum = 0;
                auto* kernel = kernels_.get() + (band * phases + phase) * taps;
                for (uint32_t j = 0; j < taps; ++j) {
                    const double x = static_cast<double>(j) - 15 - static_cast<double>(phase) / (phases - 1);
                    const double window = 0.42 - 0.5 * std::cos(2 * pi * j / (taps - 1)) +
                                          0.08 * std::cos(4 * pi * j / (taps - 1));
                    kernel[j] = static_cast<float>(window * (std::fabs(x) < 1e-12 ? 2 * cutoff : std::sin(2 * pi * cutoff * x) / (pi * x)));
                    sum += kernel[j];
                }
                for (uint32_t j = 0; j < taps; ++j) { kernel[j] = static_cast<float>(kernel[j] / sum); }
            }
        }
        reset(); return true;
    }
    // Real-time: references complete, fixed Size windows; never copies audio.
    // The echo ring holds two windows plus the fade margin; Modifiers caps
    // a window at half the ring.
    void reset() noexcept {
        current_ = old_ = {}; initialized_ = old_hold_ = false;
        last_ = held_ = {}; phase_ = fade_length_;
    }
    Frame tick(const StereoRing& ring, uint64_t size, uint64_t phase, uint32_t lead, double speed, bool audible) noexcept {
        const bool boundary = phase == 0;
        if (boundary || !initialized_ || size != current_.size) {
            // A fast wrap can still be fading when Size restarts. Preserve
            // that mixed output without adding a third read head.
            old_hold_ = initialized_ && phase_ < fade_length_;
            held_ = last_;
            old_ = current_;
            // Send leads the audible boundary by one host period. The forward
            // offset matches the ordinary tap at +1x; reverse starts at the
            // last captured sample. Neither path can read unwritten audio.
            // A Size fade can commit inside the one-period Send lead. Rebuild
            // that window at its existing boundary rather than waiting an
            // entire new Size to correct a length predicted before the commit.
            const uint64_t elapsed = initialized_ && !boundary ? phase : 0;
            double position = (speed < 0 ? static_cast<double>(size - 1) : static_cast<double>(lead % size)) + elapsed * speed;
            position = std::fmod(position, static_cast<double>(size));
            if (position < 0) { position += size; }
            current_ = {ring.position() - std::min(ring.position(), elapsed), size, position};
            phase_ = initialized_ ? 0 : fade_length_; initialized_ = true;
        }
        Frame result{};
        if (audible) {
            result = read(ring, current_, speed);
            if (phase_ < fade_length_) {
                const Frame weights = weights_[phase_];
                result = (old_hold_ ? held_ : read(ring, old_, speed)) * weights.l + result * weights.r;
            }
        }
        if (phase_ < fade_length_) {
            if (!old_hold_) { old_.position = std::clamp(old_.position + speed, 0.0, static_cast<double>(old_.size - 1)); }
            ++phase_;
        }
        const double next = current_.position + speed;
        if (next >= current_.size || next < 0) {
            old_hold_ = phase_ < fade_length_;
            held_ = result;
            if (phase_ >= fade_length_) {
                old_ = current_;
                old_.position = std::clamp(next, 0.0, static_cast<double>(old_.size - 1));
            }
            phase_ = 0;
            current_.position = next < 0 ? next + current_.size : next - current_.size;
        } else { current_.position = next; }
        last_ = result;
        return result;
    }
private:
    struct Head { uint64_t end = 0, size = 1; double position = 0; };
    static Frame sample(const StereoRing& ring, const Head& head, int64_t index) noexcept {
        const double absolute = static_cast<double>(head.end) - head.size +
            std::clamp<int64_t>(index, 0, static_cast<int64_t>(head.size - 1));
        if (absolute < 0 || absolute >= static_cast<double>(ring.position())) { return {}; }
        return ring.read(ring.position() - static_cast<uint64_t>(absolute));
    }
    Frame read(const StereoRing& ring, const Head& head, double speed) const noexcept {
        const auto index = static_cast<int64_t>(head.position);
        const float fraction = static_cast<float>(head.position - index);
        if (std::fabs(speed) <= 1) {
            return hermite(sample(ring, head, index - 1), sample(ring, head, index),
                           sample(ring, head, index + 1), sample(ring, head, index + 2), fraction);
        }
        const auto band = static_cast<uint32_t>(std::clamp(std::ceil((std::fabs(speed) - 1) * 4), 0.0, 12.0));
        const float phase = fraction * (phases - 1);
        const auto p = std::min(static_cast<uint32_t>(phase), phases - 2);
        const float blend = phase - p;
        const float* a = kernels_.get() + (band * phases + p) * taps;
        const float* b = a + taps;
        Frame result{};
        for (uint32_t j = 0; j < taps; ++j) {
            result = result + sample(ring, head, index + j - 15) * (a[j] + blend * (b[j] - a[j]));
        }
        return result;
    }
    static constexpr uint32_t bands = 13, phases = 129, taps = 32;
    std::unique_ptr<Frame[]> weights_;
    std::unique_ptr<float[]> kernels_;
    Head current_, old_;
    Frame last_{}, held_{};
    uint32_t phase_ = 0, fade_length_ = 2;
    bool initialized_ = false, old_hold_ = false;
};
} // namespace discipline::dsp
