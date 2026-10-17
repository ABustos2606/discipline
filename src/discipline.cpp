// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "discipline.hpp"
#include <cmath>
#include <new>

namespace discipline {
namespace {
LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const* features)
{
    const auto config = host_config(features);
    if (!config.has_map || !std::isfinite(rate) || rate < 1000 || rate > 384000 ||
        config.block > kReturnRing / 2 || config.block >= std::llround(rate * 0.05)) { return nullptr; }
    try {
        prepare_pair_registry();
        auto* instance = new (std::nothrow) Discipline(rate, config);
        if (instance && !instance->initialize()) { delete instance; instance = nullptr; }
        return instance;
    } catch (...) { return nullptr; } // No exception may cross the LV2 C ABI.
}
void connect(LV2_Handle h, uint32_t index, void* data) { static_cast<Discipline*>(h)->connect(index, data); }
void activate(LV2_Handle h) { static_cast<Discipline*>(h)->activate(); }
void run(LV2_Handle h, uint32_t frames) { static_cast<Discipline*>(h)->run(frames); }
void deactivate(LV2_Handle h) { static_cast<Discipline*>(h)->deactivate(); }
void cleanup(LV2_Handle h) { delete static_cast<Discipline*>(h); }
} // namespace

Discipline::Discipline(double rate, HostConfig config) noexcept
    : rate_(rate), host_(config), clear_(static_cast<uint32_t>(std::lround(rate * 0.02)))
{
    for (auto* gain : {&level_, &repeats_, &in_mix_, &out_mix_, &feed_mix_}) { gain->configure(rate, 0.01); }
    echo_gain_.configure(rate);
    feed_gain_.configure(rate);
}

bool Discipline::initialize() noexcept
{
    return memory_.allocate(static_cast<uint64_t>(std::llround(rate_ * 180))) &&
           echo_.allocate(static_cast<uint64_t>(std::ceil(rate_ * 60.05)) + host_.maximum) &&
           tap_.configure(static_cast<uint32_t>(std::lround(rate_ * 0.03)));
}

void Discipline::activate() noexcept
{
    pair_.release();
    memory_.reset(); echo_.reset(); clear_.reset();
    echo_gain_.reset(); feed_gain_.reset(); return_fade_.reset(0);
    last_slot_ = nullptr; epoch_ = 0;
    transport_ = fresh_from_ = stale_frames_ = 0;
    last_return_ = {};
    control_phase_ = 0;
    first_ = true; clear_high_ = false; fault_ = false;
}

void Discipline::connect(uint32_t index, void* data) noexcept
{
    if (index < ports_.size()) { ports_[index] = static_cast<float*>(data); }
}

// Real-time section: fixed work per sample; no allocation, locks, bulk buffer
// clearing, file/console I/O or host feature calls. Slot memory is static.
float Discipline::control(MainPort index, float fallback, float minimum, float maximum) const noexcept
{
    const float* data = port(index);
    return data && std::isfinite(*data) ? std::clamp(*data, minimum, maximum) : fallback;
}
float Discipline::cv(MainPort index, uint32_t sample) const noexcept
{
    const float* data = port(index);
    return data && std::isfinite(data[sample]) ? std::clamp(data[sample], -1.0f, 1.0f) : 0;
}
uint64_t Discipline::size_frames(float milliseconds, float beats, float bpm, bool sync, float modulation) const noexcept
{
    const double seconds = std::clamp(sync ? beats * 60.0 / bpm : milliseconds * 0.001, 0.05, 60.0);
    const double scale = std::log(1200.0);
    const double normalized = std::clamp(std::log(seconds / 0.05) / scale + modulation, 0.0, 1.0);
    return static_cast<uint64_t>(std::llround(0.05 * std::exp(normalized * scale) * rate_));
}

void Discipline::run(uint32_t frames) noexcept
{
    pair_.update(port(MainPort::pair), host_.block);
    PairSlot* slot = pair_.slot();
    const uint32_t epoch = slot ? slot->epoch.load(std::memory_order_acquire) : 0;
    if (slot != last_slot_ || epoch != epoch_) {
        if (slot != last_slot_ && slot) {
            const uint64_t previous = slot->main_frames.load(std::memory_order_acquire);
            transport_ = ((previous + host_.block - 1) / host_.block) * host_.block;
        }
        last_slot_ = slot; epoch_ = epoch;
        fresh_from_ = transport_; stale_frames_ = 0;
    }

    const float level = control(MainPort::level, 0.5f);
    const float repeats = control(MainPort::repeats, 0.4f);
    const float milliseconds = control(MainPort::size, 500, 50, 60000);
    const float beats = control(MainPort::size_beats, 1, 0.125f, 128);
    const float bpm = control(MainPort::bpm, 120, 30, 300);
    const bool sync = control(MainPort::sync, 0) >= 0.5f;
    const int routing = static_cast<int>(control(MainPort::routing, 0, 0, 2) + 0.5f);
    const bool clear_high = control(MainPort::clear, 0) >= 0.5f;
    if (clear_high && !clear_high_) { clear_.request(); }
    clear_high_ = clear_high;
    bool slice_fault = false;

    for (uint32_t i = 0; i < frames; ++i, ++transport_) {
        const float l = std::clamp(level + cv(MainPort::level_cv, i), 0.0f, 1.0f);
        const float r = std::clamp(repeats + cv(MainPort::repeats_cv, i), 0.0f, 1.0f);
        if (first_) {
            level_.reset(2 * l * l); repeats_.reset(std::min(1.0f, r / 0.97f));
            in_mix_.reset(routing == 0); feed_mix_.reset(routing == 1); out_mix_.reset(routing == 2);
            tap_.reset(size_frames(milliseconds, beats, bpm, sync, cv(MainPort::size_cv, i)) - host_.block);
            first_ = false;
        }
        const float wet_gain = level_.tick(2 * l * l);
        const float repeat_gain = repeats_.tick(std::min(1.0f, r / 0.97f));
        const float in_mix = in_mix_.tick(routing == 0);
        const float feed_mix = feed_mix_.tick(routing == 1);
        const float out_mix = out_mix_.tick(routing == 2);
        if (!control_phase_) { tap_.target(size_frames(milliseconds, beats, bpm, sync, cv(MainPort::size_cv, i)) - host_.block); }
        control_phase_ = (control_phase_ + 1) & 31;

        dsp::Frame ret{};
        const bool available = slot && pair_.paired() && transport_ >= host_.block &&
            transport_ - host_.block >= fresh_from_ && read_return(*slot, epoch_, transport_ - host_.block, ret);
        if (available) {
            stale_frames_ = 0;
            if (!dsp::finite(ret)) { ret = {}; slice_fault = true; }
            last_return_ = dsp::ceiling(ret);
        } else { ++stale_frames_; }
        ret = last_return_ * static_cast<float>(return_fade_.tick(available ? 1.0 : 0.0));

        dsp::Frame dry{port(MainPort::in_l) ? port(MainPort::in_l)[i] : 0,
                       port(MainPort::in_r) ? port(MainPort::in_r)[i] : 0};
        if (!dsp::finite(dry)) { dry = {}; slice_fault = true; }
        const float memory_gain = clear_.tick(memory_);
        memory_.push(dry + feed_gain_.process(ret * (repeat_gain * feed_mix)));
        const auto reads = tap_.read_pair(echo_, host_.block);
        dsp::Frame send = reads.send;
        dsp::Frame echo_input = memory_.read(1) * memory_gain +
            echo_gain_.process((ret * in_mix + reads.feedback * out_mix) * repeat_gain);
        dsp::Frame output{wet_gain == 0 || ret.l == 0 ? dry.l : dry.l + ret.l * wet_gain,
                          wet_gain == 0 || ret.r == 0 ? dry.r : dry.r + ret.r * wet_gain};
        if (!dsp::finite(send) || !dsp::finite(echo_input) || !dsp::finite(output)) {
            send = echo_input = output = {}; slice_fault = true;
        }
        echo_.push(echo_input);
        if (auto* out = port(MainPort::out_l)) { out[i] = output.l; }
        if (auto* out = port(MainPort::out_r)) { out[i] = output.r; }
        if (auto* out = port(MainPort::send_l)) { out[i] = send.l; }
        if (auto* out = port(MainPort::send_r)) { out[i] = send.r; }
        if (auto* out = port(MainPort::clock)) { out[i] = 0; }
    }
    if (slot) { slot->main_frames.store(transport_, std::memory_order_release); }
    if (auto* paired = port(MainPort::paired)) {
        *paired = pair_.paired() && stale_frames_ < static_cast<uint64_t>(rate_ * 0.5) ? 1.0f : 0.0f;
    }
    if (slice_fault) {
        fault_ = true; echo_.clear(); echo_gain_.reset(); feed_gain_.reset();
        for (auto index : {MainPort::out_l, MainPort::out_r, MainPort::send_l, MainPort::send_r}) {
            if (auto* out = port(index)) { for (uint32_t i = 0; i < frames; ++i) { out[i] = 0; } }
        }
    }
}

const LV2_Descriptor main_descriptor{kMainURI, instantiate, connect, activate, run, deactivate, cleanup, nullptr};
} // namespace discipline
