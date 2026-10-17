// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include "dsp/frame.hpp"

namespace discipline {

// One pair per host process: a Return joins the Discipline that has picked
// Outside. A second Discipline on Outside waits until the first lets go.
inline constexpr uint32_t kPairCount = 1;
inline constexpr uint32_t kReturnRing = 8192;
static_assert(std::atomic<void*>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

struct PairSlot {
    std::atomic<void*> owner_main{nullptr};
    std::atomic<void*> owner_return{nullptr};
    std::atomic<uint32_t> epoch{0};
    std::atomic<uint64_t> main_frames{0};
    std::atomic<uint64_t> written{0};
    std::atomic<uint32_t> written_epoch{0};
    std::atomic<uint32_t> block_length{128};
    struct Cell {
        std::atomic<uint64_t> stamp{0};
        std::atomic<uint64_t> stereo{0};
    };
    std::array<Cell, kReturnRing> ring{};
};

// Defined once in discipline.so, shared by both descriptors.
extern std::array<PairSlot, kPairCount> pair_slots;
void prepare_pair_registry();
bool read_return(const PairSlot& slot, uint32_t epoch, uint64_t frame, dsp::Frame& sample) noexcept;

class ReturnWriter {
public:
    void reset() noexcept { slot_ = nullptr; epoch_ = 0; frame_ = clock_ = 0; }
    void write(PairSlot* slot, const float* left, const float* right, uint32_t frames, uint32_t fallback_block = 128) noexcept;
private:
    PairSlot* slot_ = nullptr;
    uint32_t epoch_ = 0;
    uint64_t frame_ = 0;
    uint64_t clock_ = 0;
};

class PairClaim {
public:
    explicit PairClaim(bool is_return) noexcept : is_return_(is_return) {}
    ~PairClaim() { release(); }
    PairClaim(const PairClaim&) = delete;
    PairClaim& operator=(const PairClaim&) = delete;

    // Claims the shared slot while wanted, releases it otherwise.
    void update(bool wanted, uint32_t block_length = 128) noexcept;
    void release() noexcept;
    bool paired() const noexcept;
    PairSlot* slot() const noexcept { return slot_; }

private:
    bool is_return_;
    PairSlot* slot_ = nullptr;
};

} // namespace discipline
