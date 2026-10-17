// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once

#include "pair_registry.hpp"
#include "ports.hpp"
#include "host_config.hpp"
#include "dsp/stereo_ring.hpp"
#include "dsp/smoother.hpp"
#include "dsp/xfade_tap.hpp"
#include "dsp/loop_gain.hpp"
#include "dsp/memory_heads.hpp"

#include <lv2/core/lv2.h>
#include <array>

namespace discipline {

class Discipline {
public:
    Discipline(double rate, HostConfig config, uint32_t seed) noexcept;
    bool initialize() noexcept;
    void connect(uint32_t port, void* data) noexcept;
    void activate() noexcept;
    void run(uint32_t frames) noexcept;
    void deactivate() noexcept { pair_.release(); }

private:
    float* port(MainPort index) const noexcept { return ports_[static_cast<uint32_t>(index)]; }
    float control(MainPort index, float fallback, float minimum = 0, float maximum = 1) const noexcept;
    float cv(MainPort index, uint32_t sample) const noexcept;
    uint64_t size_frames(float milliseconds, float beats, float bpm, bool sync, float modulation) const noexcept;
    std::array<float*, static_cast<uint32_t>(MainPort::count)> ports_{};
    PairClaim pair_{false};
    const double rate_;
    const HostConfig host_;
    dsp::StereoRing memory_, echo_;
    dsp::MemoryClear clear_;
    dsp::CrossfadeTap tap_;
    dsp::MemoryHeads heads_;
    dsp::AutoScan auto_scan_;
    dsp::OnePole level_, repeats_, in_mix_, out_mix_, feed_mix_;
    dsp::OnePole memory_dry_, collect_mix_;
    dsp::Ramp effect_, wet_, dry_, echo_input_, loop_, wipe_;
    dsp::Slew return_fade_{1.0 / 64.0};
    dsp::LoopGain echo_gain_, feed_gain_, collect_gain_;
    PairSlot* last_slot_ = nullptr;
    uint32_t epoch_ = 0;
    uint64_t transport_ = 0, fresh_from_ = 0, stale_frames_ = 0;
    uint32_t control_phase_ = 0;
    uint64_t size_ = 1, size_phase_ = 0, clock_size_ = 1;
    float scan_amount_ = 0, spread_amount_ = 0;
    dsp::Frame last_return_{};
    bool first_ = true, clear_high_ = false, fault_ = false;
    bool loop_high_ = false, loop_latched_ = false, hold_high_ = false;
    bool manual_ = false;
    bool snap_pending_ = false, was_enabled_ = true, echo_discard_ = false, echo_silent_ = false;
    int memory_mode_ = -1;
};

extern const LV2_Descriptor main_descriptor;
extern const LV2_Descriptor return_descriptor;

} // namespace discipline
