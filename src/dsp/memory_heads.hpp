// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "auto_scan.hpp"
#include "stereo_ring.hpp"
#include <memory>

namespace discipline::dsp {
class MemoryHeads {
public:
    bool configure(double rate, uint64_t maximum) noexcept {
        maximum_ = maximum;
        length_ = std::max(2U, static_cast<uint32_t>(std::lround(rate * 0.06)));
        snap_length_ = std::max(2U, static_cast<uint32_t>(std::lround(rate * 0.03)));
        scrub_distance_ = std::max(1.0, rate * 0.01);
        mix_step_ = 1.0 / std::max(1.0, rate * 0.01);
        history_distance_ = std::max(1.0, rate * 0.005);
        weights_.reset(new (std::nothrow) Frame[length_]);
        if (!weights_) { return false; }
        for (uint32_t i = 0; i < length_; ++i) {
            const float incoming = static_cast<float>(0.5 - 0.5 * std::cos(3.141592653589793 * i / (length_ - 1)));
            // Complementary weights keep correlated audio at its original level.
            weights_[i] = {1 - incoming, incoming};
        }
        reset(); return true;
    }

    // Real-time: two memory pairs at most. Small moves scrub; large moves
    // scrub briefly around the destination and crossfade there in 60 ms.
    // Targets coalesce during a fade, so the latest settles within 120 ms.
    void reset() noexcept {
        current_ = old_ = {}; desired_scan_ = desired_spread_ = 0;
        amount_ = 0; size_ = 1; phase_ = fade_length_ = 0;
        snap_pending_ = false; history_ = {};
    }
    void target(double scan, float spread, uint64_t size) noexcept {
        desired_scan_ = std::clamp(scan, 0.0, static_cast<double>(maximum_));
        amount_ = spread; size_ = size;
        desired_spread_ = spread_at(desired_scan_);
    }
    void snap(double scan) noexcept {
        desired_scan_ = std::clamp(scan, 0.0, static_cast<double>(maximum_));
        desired_spread_ = spread_at(desired_scan_);
        snap_pending_ = true;
    }
    Frame tick(const StereoRing& memory, bool advance) noexcept {
        if (advance) {
            if (!fading()) {
                if (snap_pending_) {
                    start(true); snap_pending_ = false;
                } else if (std::max(std::fabs(desired_scan_ - current_.scan),
                                    std::fabs(desired_spread_ - current_.spread)) > scrub_distance_) {
                    start(false);
                } else {
                    current_.target_scan = desired_scan_; current_.target_spread = desired_spread_;
                }
            }
            advance_head(current_);
            if (fading()) { advance_head(old_); }
        }
        auto result = read_pair(memory, current_);
        if (fading()) {
            const auto old = read_pair(memory, old_);
            const Frame weights = weights_[static_cast<uint64_t>(phase_) * (length_ - 1) / (fade_length_ - 1)];
            result.full = old.full * weights.l + result.full * weights.r;
            result.history = old.history * weights.l + result.history * weights.r;
            if (advance) { ++phase_; }
        }
        history_ = result.history;
        return result.full;
    }
    Frame history() const noexcept { return history_; }
    double scan() const noexcept { return current_.scan; }
    double spread() const noexcept { return current_.spread; }
    uint64_t maximum() const noexcept { return maximum_; }
    bool reached() const noexcept {
        return !fading() && !snap_pending_ && current_.scan == desired_scan_ && current_.spread == desired_spread_;
    }
    bool scanning() const noexcept {
        return desired_scan_ > 0 || desired_spread_ > 0 || current_.scan > 0 || current_.spread > 0 ||
               (fading() && (old_.scan > 0 || old_.spread > 0));
    }
private:
    struct Head {
        double scan = 0, spread = 0, target_scan = 0, target_spread = 0, mix = 0;
    };
    struct Read { Frame full; Frame history; };
    bool fading() const noexcept { return phase_ < fade_length_; }
    double spread_at(double scan) const noexcept {
        return snap_offset(amount_ * (maximum_ - scan), size_, static_cast<uint64_t>(maximum_ - scan));
    }
    void start(bool snap) noexcept {
        if (current_.scan == desired_scan_ && current_.spread == desired_spread_) {
            current_.target_scan = desired_scan_; current_.target_spread = desired_spread_; return;
        }
        old_ = current_;
        old_.target_scan = old_.scan; old_.target_spread = old_.spread;
        if (!snap) {
            // Scrub the outgoing audio too, where it is immediately audible.
            // The travel is local (10 ms), never the whole history depth.
            old_.target_scan = std::clamp(old_.scan + std::clamp(desired_scan_ - old_.scan,
                -scrub_distance_, scrub_distance_), 0.0, maximum_ - old_.spread);
            old_.target_spread = std::clamp(old_.spread + std::clamp(desired_spread_ - old_.spread,
                -scrub_distance_, scrub_distance_), 0.0, maximum_ - old_.target_scan);
        }
        current_.target_scan = desired_scan_; current_.target_spread = desired_spread_;
        const auto lead_in = [&](double from, double to) {
            return snap ? to : to - std::clamp(to - from, -scrub_distance_, scrub_distance_);
        };
        current_.scan = lead_in(old_.scan, desired_scan_);
        current_.spread = std::clamp(lead_in(old_.spread, desired_spread_), 0.0, maximum_ - current_.scan);
        // The incoming pair fades in from silence, so a different Spread mix
        // can start at its correct gain without duplicating the old primary.
        current_.mix = desired_spread_ > 0 ? 1 : 0;
        phase_ = 0; fade_length_ = snap ? snap_length_ : length_;
    }
    void advance_head(Head& head) noexcept {
        head.scan += std::clamp(head.target_scan - head.scan, -0.5, 0.5);
        head.spread += std::clamp(head.target_spread - head.spread, -0.5, 0.5);
        head.spread = std::clamp(head.spread, 0.0, maximum_ - head.scan);
        // Only a distinct, snapped historical position enables Spread.
        const double mix_target = head.target_spread > 0 && head.spread > 0 ? 1 : 0;
        head.mix += std::clamp(mix_target - head.mix, -mix_step_, mix_step_);
    }
    static Frame read(const StereoRing& memory, double offset) noexcept {
        if (offset == 0) { return memory.read(1); }
        const double delay = offset + 1;
        return delay == std::floor(delay) ? memory.read(static_cast<uint64_t>(delay), 256) : memory.fractional(delay);
    }
    Read read_pair(const StereoRing& memory, const Head& head) const noexcept {
        constexpr float half = 0.7071067811865475f;
        const float mix = static_cast<float>(head.mix);
        const auto primary = read(memory, head.scan) * (1 - mix * (1 - half));
        const auto secondary = read(memory, head.scan + head.spread) * (mix * half);
        // Scanning monitors history only. Explicit On still monitors the full
        // mix, including the present, as requested by that control.
        const float primary_history = static_cast<float>(std::min(1.0, head.scan / history_distance_));
        const float secondary_history = static_cast<float>(std::min(1.0, (head.scan + head.spread) / history_distance_));
        return {primary + secondary, primary * primary_history + secondary * secondary_history};
    }
    std::unique_ptr<Frame[]> weights_;
    Head current_, old_;
    Frame history_{};
    uint64_t maximum_ = 0, size_ = 1;
    double desired_scan_ = 0, desired_spread_ = 0;
    double scrub_distance_ = 1, mix_step_ = 1, history_distance_ = 1;
    float amount_ = 0;
    uint32_t phase_ = 0, fade_length_ = 0, length_ = 2, snap_length_ = 2;
    bool snap_pending_ = false;
};
} // namespace discipline::dsp
