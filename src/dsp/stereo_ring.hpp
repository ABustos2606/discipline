// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "interp.hpp"
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <sys/mman.h>

namespace discipline::dsp {
class StereoRing {
public:
    bool allocate(uint64_t frames) noexcept
    {
        if (data_ || !frames || frames > (SIZE_MAX / sizeof(Frame)) - 4) { return false; }
        data_.reset(new (std::nothrow) Frame[frames + 4]);
        if (!data_) { return false; }
        capacity_ = frames;
        bytes_ = (frames + 4) * sizeof(Frame);
        // Outside run(): touch every page and try to keep it resident.
        std::memset(static_cast<void*>(data_.get()), 0, bytes_);
        locked_ = mlock(data_.get(), bytes_) == 0;
        return true;
    }
    ~StereoRing() { if (locked_) { munlock(data_.get(), bytes_); } }
    StereoRing() = default;
    StereoRing(const StereoRing&) = delete;
    StereoRing& operator=(const StereoRing&) = delete;

    // Real-time section: no bulk resets, allocation, locks or I/O.
    void reset() noexcept { position_ = valid_from_ = 0; }
    void clear() noexcept { valid_from_ = position_; }
    uint64_t position() const noexcept { return position_; }
    uint64_t valid_from() const noexcept { return valid_from_; }
    uint64_t capacity() const noexcept { return capacity_; }
    void push(Frame f) noexcept { data_[position_++ % capacity_] = f; }
    Frame read(uint64_t delay, uint32_t boundary_fade = 0) const noexcept
    {
        if (!delay || delay > position_ || delay > capacity_) { return {}; }
        const uint64_t frame = position_ - delay;
        if (frame < valid_from_) { return {}; }
        const float gain = boundary_fade ? std::min(1.0f, static_cast<float>(frame - valid_from_) / boundary_fade) : 1.0f;
        return data_[frame % capacity_] * gain;
    }
    Frame fractional(double delay, uint32_t boundary_fade = 256) const noexcept
    {
        if (capacity_ < 4 || !std::isfinite(delay) || delay < 1 ||
            delay > static_cast<double>(capacity_ - 2) || delay > static_cast<double>(position_ - valid_from_)) { return {}; }
        const auto d = static_cast<uint64_t>(delay);
        // The sample ahead of delay 1 has not been written. Use a linear
        // segment here instead of supplying a fabricated zero to Hermite.
        if (d == 1) {
            const float t = static_cast<float>(delay - d);
            return read(1, boundary_fade) * (1 - t) + read(2, boundary_fade) * t;
        }
        return hermite(read(d - 1, boundary_fade), read(d, boundary_fade),
                       read(d + 1, boundary_fade), read(d + 2, boundary_fade), static_cast<float>(delay - d));
    }
private:
    std::unique_ptr<Frame[]> data_;
    uint64_t capacity_ = 0;
    uint64_t position_ = 0;
    uint64_t valid_from_ = 0;
    size_t bytes_ = 0;
    bool locked_ = false;
};
} // namespace discipline::dsp
