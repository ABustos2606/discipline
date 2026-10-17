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
        // Instantiate only: independent streams, without randomness or locks in run().
        static std::atomic<uint32_t> seeds{0x6A09E667};
        auto* instance = new (std::nothrow) Discipline(rate, config,
            seeds.fetch_add(0x9E3779B9, std::memory_order_relaxed));
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

Discipline::Discipline(double rate, HostConfig config, uint32_t seed) noexcept
    : rate_(rate), host_(config), clear_(static_cast<uint32_t>(std::lround(rate * 0.02))), auto_scan_(seed)
{
    for (auto* gain : {&level_, &repeats_, &in_mix_, &out_mix_, &feed_mix_}) { gain->configure(rate, 0.01); }
    memory_dry_.configure(rate, 0.05);
    collect_mix_.configure(rate, 0.01);
    for (auto* ramp : {&effect_, &wet_, &dry_, &echo_input_, &wipe_}) { ramp->configure(rate, 0.01, 0.01); }
    loop_.configure(rate, 0.01, 0.05);
    echo_gain_.configure(rate);
    feed_gain_.configure(rate);
    collect_gain_.configure(rate);
    clock_.configure(rate);
}

bool Discipline::initialize() noexcept
{
    const auto memory_frames = static_cast<uint64_t>(std::llround(rate_ * 180));
    const auto margin = host_.maximum + 4 + static_cast<uint64_t>(std::lround(rate_ * 0.03));
    if (margin >= memory_frames) { return false; }
    return memory_.allocate(memory_frames) &&
           echo_.allocate(static_cast<uint64_t>(std::ceil(rate_ * 60.05)) + host_.maximum) &&
           tap_.configure(static_cast<uint32_t>(std::lround(rate_ * 0.03))) &&
           heads_.configure(rate_, memory_frames - margin) && modifiers_.configure(rate_);
}

void Discipline::activate() noexcept
{
    pair_.release();
    memory_.reset(); echo_.reset(); clear_.reset(); heads_.reset(); auto_scan_.reset();
    clock_.reset(); modifiers_.clear();
    echo_gain_.reset(); feed_gain_.reset(); collect_gain_.reset(); return_fade_.reset(0);
    last_slot_ = nullptr; epoch_ = 0;
    transport_ = fresh_from_ = stale_frames_ = 0;
    last_return_ = {};
    control_phase_ = 0;
    first_ = true; clear_high_ = false; fault_ = false;
    loop_high_ = loop_latched_ = hold_high_ = manual_ = false;
    memory_mode_ = -1; size_ = 1;
    scan_amount_ = spread_amount_ = 0;
    snap_pending_ = echo_discard_ = echo_silent_ = false; was_enabled_ = true;
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
            const uint64_t phase = transport_ % host_.block;
            transport_ = ((previous + host_.block - 1) / host_.block) * host_.block + phase;
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
    const int modifier = static_cast<int>(control(MainPort::modifier, 0, 0, 4) + 0.5f);
    const float modify = control(MainPort::modify, 0.5f);
    const int routing = static_cast<int>(control(MainPort::routing, 0, 0, 2) + 0.5f);
    const bool enabled = control(MainPort::enabled, 1) >= 0.5f;
    const bool collect = control(MainPort::collect, 0) >= 0.5f;
    const bool always = control(MainPort::always, 0) >= 0.5f;
    const bool trails = control(MainPort::trails, 0) >= 0.5f;
    const bool wipe = control(MainPort::wipe, 0) >= 0.5f;
    const int dry_kill = static_cast<int>(control(MainPort::dry_kill, 0, 0, 2) + 0.5f);
    const int memory_dry = static_cast<int>(control(MainPort::memory_dry, 1, 0, 2) + 0.5f);
    const bool loop_high = control(MainPort::loop, 0) >= 0.5f;
    const bool latch = control(MainPort::latch, 0) >= 0.5f;
    if (loop_high && !loop_high_) { loop_latched_ = !loop_latched_; }
    loop_high_ = loop_high;
    if (!latch) { loop_latched_ = loop_high; }
    if (!enabled && was_enabled_) { loop_latched_ = false; }
    const bool looping = enabled && (latch ? loop_latched_ : loop_high);
    if (enabled) { echo_silent_ = false; }
    if (!enabled && !trails && !echo_silent_) { echo_discard_ = true; }
    was_enabled_ = enabled;
    const bool hold = control(MainPort::scan_hold, 0) >= 0.5f;
    const bool hold_pressed = hold && !hold_high_;
    const bool manual = control(MainPort::scan_mode, 0) >= 0.5f && !hold;
    if (!first_ && ((hold_high_ && !hold) || manual != manual_)) {
        auto_scan_.reset();
        if (!hold_pressed) { snap_pending_ = true; }
    }
    hold_high_ = hold; manual_ = manual;
    // Mode 0 clears/stops; 1 pauses; 2 overdubs silently; 3 records live.
    const int memory_mode = enabled || always ? 3 : collect ? (trails ? 2 : 1) : 0;
    if (memory_mode == 0 && memory_mode_ != 0 && memory_mode_ != -1) { clear_.request(); }
    memory_mode_ = memory_mode;
    const bool moving = memory_mode >= 2;
    const bool collecting = collect && (enabled || !always);
    const bool clear_high = control(MainPort::clear, 0) >= 0.5f;
    if (clear_high && !clear_high_) { clear_.request(); }
    clear_high_ = clear_high;
    bool slice_fault = false;

    for (uint32_t i = 0; i < frames; ++i, ++transport_) {
        const float l = std::clamp(level + cv(MainPort::level_cv, i), 0.0f, 1.0f);
        const float r = std::clamp(repeats + cv(MainPort::repeats_cv, i), 0.0f, 1.0f);
        // Per sample: Modify drives speed and rhythm gates, which smooth per sample.
        const float amount = std::clamp(modify + cv(MainPort::modify_cv, i), 0.0f, 1.0f);
        if (first_) {
            level_.reset(2 * l * l); repeats_.reset(std::min(1.0f, r / 0.97f));
            in_mix_.reset(routing == 0); feed_mix_.reset(routing == 1); out_mix_.reset(routing == 2);
            size_ = size_frames(milliseconds, beats, bpm, sync, cv(MainPort::size_cv, i));
            tap_.reset(size_ - host_.block);
            modifiers_.reset(modifier, amount);
            effect_.reset(enabled); wet_.reset(enabled || trails); wipe_.reset(!wipe);
            dry_.reset(dry_kill == 0 || (dry_kill == 2 && !enabled));
            echo_input_.reset(enabled && !looping); loop_.reset(looping);
            memory_dry_.reset(wipe || memory_dry == 2);
            collect_mix_.reset(collecting);
            first_ = false;
        }
        const float wet_gain = level_.tick(2 * l * l);
        const float repeat_gain = repeats_.tick(std::min(!enabled && always ? 0.7f : 1.0f, r / 0.97f));
        const float in_mix = in_mix_.tick(routing == 0);
        const float feed_mix = feed_mix_.tick(routing == 1);
        const float out_mix = out_mix_.tick(routing == 2);
        if (!control_phase_ || snap_pending_ || (hold_pressed && i == 0)) {
            size_ = size_frames(milliseconds, beats, bpm, sync, cv(MainPort::size_cv, i));
            tap_.target(size_ - host_.block);
            scan_amount_ = hold ? 1 : std::clamp(control(MainPort::scan, 0) + cv(MainPort::scan_cv, i), 0.0f, 1.0f);
            spread_amount_ = std::clamp(control(MainPort::spread, 0) + cv(MainPort::spread_cv, i), 0.0f, 1.0f);
        }
        control_phase_ = (control_phase_ + 1) & 31;
        const uint64_t current_size = tap_.current() + host_.block;
        const auto timing = clock_.tick(current_size);
        const bool boundary = timing.boundary();
        const bool scan_boundary = boundary || (hold && auto_scan_.state() == dsp::AutoScan::State::present);
        if (moving && !manual && auto_scan_.tick(scan_boundary, scan_amount_, heads_.maximum(), size_, heads_.reached())) {
            heads_.snap(0);
        }
        const auto scan_target = manual ? dsp::snap_offset(scan_amount_ * heads_.maximum(), size_, heads_.maximum()) : auto_scan_.target();
        heads_.target(scan_target, spread_amount_, size_);
        if (snap_pending_) { heads_.snap(scan_target); snap_pending_ = false; }
        const float effect_gain = effect_.tick(enabled);
        const float wet_enabled = wet_.tick(!echo_discard_ && (enabled || trails));
        const float audible_wet = wet_enabled * wipe_.tick(!wipe);
        const float dry_gain = dry_.tick(dry_kill == 0 || (dry_kill == 2 && !enabled));
        const float input_gain = echo_input_.tick(enabled && !looping && !echo_discard_);
        const float loop_gain = loop_.tick(looping);
        const float collect_mix = collect_mix_.tick(collecting);

        dsp::Frame ret{};
        const bool available = slot && pair_.paired() && transport_ >= host_.block &&
            transport_ - host_.block >= fresh_from_ && read_return(*slot, epoch_, transport_ - host_.block, ret);
        if (available) {
            stale_frames_ = 0;
            if (!dsp::finite(ret)) { ret = {}; slice_fault = true; }
            last_return_ = dsp::ceiling(ret);
        } else { ++stale_frames_; }
        ret = last_return_ * static_cast<float>(return_fade_.tick(available ? 1.0 : 0.0));
        if (echo_discard_ && wet_enabled == 0) {
            echo_.clear(); modifiers_.clear(); echo_gain_.reset(); last_return_ = {}; ret = {};
            return_fade_.reset(0); fresh_from_ = transport_;
            echo_discard_ = false; echo_silent_ = !enabled;
        }

        dsp::Frame dry{port(MainPort::in_l) ? port(MainPort::in_l)[i] : 0,
                       port(MainPort::in_r) ? port(MainPort::in_r)[i] : 0};
        // A bad live sample is local to that channel; it must not erase a loop.
        if (!std::isfinite(dry.l)) { dry.l = 0; }
        if (!std::isfinite(dry.r)) { dry.r = 0; }
        const auto valid_from = memory_.valid_from();
        const float memory_gain = clear_.tick(memory_);
        if (memory_.valid_from() != valid_from) { collect_gain_.reset(); }
        if (moving) {
            // Feed recirculates only Return. Level and memory monitoring cannot
            // create extra loop gain or a feedback loop at the host period.
            const auto feed = ret * (repeat_gain * feed_mix);
            const auto old = memory_.read(memory_.capacity(), 256); // Before overwrite, faded at validity boundary.
            const auto overdub = collect_gain_.process(old * (0.7f * collect_mix * memory_gain));
            memory_.push((memory_mode == 3 ? dry : dsp::Frame{}) + overdub + feed_gain_.process(feed));
        }
        const auto mem = heads_.tick(memory_, moving) * memory_gain;
        const float mdry_gain = memory_dry_.tick(wipe || memory_dry == 2 || (memory_dry == 1 && heads_.scanning()));
        const auto monitored = !wipe && memory_dry == 1 ? heads_.history() * memory_gain : mem;
        const auto memory_output = monitored * (mdry_gain * effect_gain);
        const auto reads = tap_.read_pair(echo_, host_.block);
        dsp::Frame send = modifiers_.process(echo_, reads.send, timing.ahead(host_.block, current_size), host_.block, modifier, amount);
        const auto normal_feedback = echo_gain_.process((ret * in_mix + reads.feedback * out_mix) * repeat_gain);
        // Feed has no ordinary echo feedback, but Loop must still hold its echo.
        const auto held_feedback = dsp::ceiling(ret * in_mix + reads.feedback * (1 - in_mix));
        dsp::Frame echo_input = loop_gain == 1 ? held_feedback :
            mem * input_gain + normal_feedback * (1 - loop_gain) + held_feedback * loop_gain;
        const auto wet_output = ret * (wet_gain * audible_wet) + memory_output;
        // Keep untouched dry samples bit-exact, including signed zero.
        dsp::Frame output{wet_output.l == 0 ? (dry_gain == 1 ? dry.l : dry.l * dry_gain) : dry.l * dry_gain + wet_output.l,
                          wet_output.r == 0 ? (dry_gain == 1 ? dry.r : dry.r * dry_gain) : dry.r * dry_gain + wet_output.r};
        if (!dsp::finite(send) || !dsp::finite(echo_input) || !dsp::finite(output)) {
            send = echo_input = output = {}; slice_fault = true;
        }
        echo_.push(echo_input);
        if (auto* out = port(MainPort::out_l)) { out[i] = output.l; }
        if (auto* out = port(MainPort::out_r)) { out[i] = output.r; }
        if (auto* out = port(MainPort::send_l)) { out[i] = send.l; }
        if (auto* out = port(MainPort::send_r)) { out[i] = send.r; }
        if (auto* out = port(MainPort::clock)) { out[i] = clock_.value(timing); }
    }
    if (slot) { slot->main_frames.store(transport_, std::memory_order_release); }
    if (auto* paired = port(MainPort::paired)) {
        *paired = pair_.paired() && stale_frames_ < static_cast<uint64_t>(rate_ * 0.5) ? 1.0f : 0.0f;
    }
    if (slice_fault) {
        fault_ = true; echo_.clear(); modifiers_.clear(); echo_gain_.reset(); feed_gain_.reset();
        for (auto index : {MainPort::out_l, MainPort::out_r, MainPort::send_l, MainPort::send_r}) {
            if (auto* out = port(index)) { for (uint32_t i = 0; i < frames; ++i) { out[i] = 0; } }
        }
    }
}

const LV2_Descriptor main_descriptor{kMainURI, instantiate, connect, activate, run, deactivate, cleanup, nullptr};
} // namespace discipline
