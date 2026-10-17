// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "pair_registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace discipline {

std::array<PairSlot, kPairCount> pair_slots{};

void prepare_pair_registry()
{
    static std::once_flag prepared;
    std::call_once(prepared, [] {
        // Instantiate only: fault the static transport pages in before run().
        for (auto& slot : pair_slots) {
            for (auto& cell : slot.ring) {
                cell.stamp.store(0, std::memory_order_relaxed);
                cell.stereo.store(0, std::memory_order_relaxed);
            }
        }
    });
}

namespace {
// Real-time bit-preserving transport helpers.
uint64_t stamp(uint32_t epoch, uint64_t frame) noexcept
{
    return (static_cast<uint64_t>(epoch) << 32) | (frame & UINT32_MAX);
}
uint64_t pack(dsp::Frame sample) noexcept
{
    static_assert(sizeof(sample) == sizeof(uint64_t));
    uint64_t bits;
    std::memcpy(&bits, &sample, sizeof(bits));
    return bits;
}
dsp::Frame unpack(uint64_t bits) noexcept
{
    dsp::Frame sample;
    std::memcpy(&sample, &bits, sizeof(bits));
    return sample;
}
} // namespace

// Real-time section. All cell operations are sequentially consistent: marking
// a cell invalid BEFORE replacing its atomic stereo payload, then checking its
// tag twice, gives a bounded coherent snapshot even during overwrite. No retry
// loop, naked float races, or pointer into another instance's allocation.
bool read_return(const PairSlot& slot, uint32_t epoch, uint64_t frame, dsp::Frame& sample) noexcept
{
    if (slot.written_epoch.load(std::memory_order_acquire) != epoch) { return false; }
    const uint64_t end = slot.written.load(std::memory_order_acquire);
    if (frame >= end || end - frame > kReturnRing) { return false; }
    const auto& cell = slot.ring[frame % kReturnRing];
    const uint64_t expected = stamp(epoch, frame);
    if (cell.stamp.load() != expected) { return false; }
    const uint64_t bits = cell.stereo.load();
    if (cell.stamp.load() != expected) { return false; }
    sample = unpack(bits);
    return true;
}

void ReturnWriter::write(PairSlot* slot, const float* left, const float* right, uint32_t frames, uint32_t fallback_block) noexcept
{
    if (!slot) { reset(); return; }
    if (!frames) { return; } // Zero-frame host calls must not establish timing.
    uint32_t epoch = slot->epoch.load(std::memory_order_acquire);
    const uint64_t main = slot->main_frames.load(std::memory_order_acquire);
    const uint32_t block = slot->owner_main.load(std::memory_order_acquire) ?
        slot->block_length.load(std::memory_order_acquire) : fallback_block;
    const bool gap = slot == slot_ && epoch == epoch_ &&
        (main > frame_ + block || frame_ > main + kReturnRing / 2);
    if (gap) { epoch = slot->epoch.fetch_add(1, std::memory_order_acq_rel) + 1; }
    if (slot != slot_ || epoch != epoch_) {
        slot_ = slot;
        epoch_ = epoch;
        // main_frames is an exclusive end. This also works if main's first
        // run was just a slice of a period, rather than its full block.
        frame_ = ((main ? main - 1 : 0) / block) * block;
        slot->written_epoch.store(0, std::memory_order_release);
    }
    for (uint32_t i = 0; i < frames; ++i, ++frame_) {
        auto& cell = slot->ring[frame_ % kReturnRing];
        cell.stamp.store(0);
        cell.stereo.store(pack({left ? left[i] : 0, right ? right[i] : 0}));
        cell.stamp.store(stamp(epoch_, frame_));
    }
    slot->written.store(frame_, std::memory_order_release);
    slot->written_epoch.store(epoch_, std::memory_order_release);
}

// Real-time section: bounded lock-free claims only; no allocation, locks or I/O.
void PairClaim::update(const float* control, uint32_t block_length) noexcept
{
    const float value = control && std::isfinite(*control) ? *control : 1.0f;
    const auto id = static_cast<uint32_t>(std::clamp(value, 1.0f, 16.0f) + 0.5f);
    auto* desired = &pair_slots[id - 1];
    if (slot_ == desired) {
        return;
    }
    release();
    auto& owner = is_return_ ? desired->owner_return : desired->owner_main;
    void* expected = nullptr;
    if (owner.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) {
        slot_ = desired;
        if (!is_return_) { slot_->block_length.store(block_length, std::memory_order_release); }
        slot_->epoch.fetch_add(1, std::memory_order_release);
    }
    // A duplicate retries next run, allowing recovery when the owner goes away.
}

void PairClaim::release() noexcept
{
    if (slot_) {
        auto& owner = is_return_ ? slot_->owner_return : slot_->owner_main;
        void* expected = this;
        if (owner.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
            slot_->epoch.fetch_add(1, std::memory_order_release);
        }
        slot_ = nullptr;
    }
}

bool PairClaim::paired() const noexcept
{
    if (!slot_) {
        return false;
    }
    const auto& other = is_return_ ? slot_->owner_main : slot_->owner_return;
    return other.load(std::memory_order_acquire) != nullptr;
}

} // namespace discipline
