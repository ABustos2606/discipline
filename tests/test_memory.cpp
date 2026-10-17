// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "host.hpp"
#include "../src/dsp/memory_heads.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

using namespace discipline;
using namespace test;
namespace {
Frame silence(uint64_t) { return {}; }
Frame dc(uint64_t) { return {0.125f, -0.0625f}; }

void audit(const Library& lib) {
    const auto render_bad = [&](bool bad) {
        Host h(lib, 8000, 32); h.main->control(MainPort::routing) = 2;
        h.main->control(MainPort::repeats) = 1;
        h.render(512, silence);
        const auto start = h.frame;
        return h.render(2400, [=](uint64_t f) {
            if (f == start) { return Frame{0.125f, -0.125f}; }
            if (f == start + 517) { return Frame{bad ? NAN : 0.0f, 0.25f}; }
            if (f == start + 619) { return Frame{0.0f, bad ? INFINITY : 0.0f}; }
            return Frame{};
        });
    };
    const auto reference = render_bad(false), sanitized = render_bad(true);
    require(std::memcmp(reference.data(), sanitized.data(), reference.size() * sizeof(Frame)) == 0,
            "bad input is zeroed per channel without clearing an existing echo or its slice");

    Host h(lib); h.render(1024, silence);
    // Both ports change 37 samples into the period, with independently sliced runs.
    h.main->slice(0, 37); h.main->control(MainPort::pair) = 2; h.main->slice(37, 91);
    for (uint32_t i = 0; i < h.block; ++i) {
        h.ret->buffers[0][i] = h.main->buffers[4][i]; h.ret->buffers[1][i] = h.main->buffers[5][i];
    }
    h.ret->slice(0, 37); h.ret->controls[2] = 2; h.ret->slice(37, 91); h.frame += h.block;
    h.render(1024, silence);
    const auto pulse = h.frame + 13;
    const auto audio = h.render(512, silence, [=](Frame, uint64_t f) { return f == pulse ? Frame{0.25f, -0.125f} : Frame{}; });
    near(audio[13 + h.block].l, 0.25f, 1e-6f, "mid-period Pair change preserves one-block return delay");
    for (size_t i = 0; i < audio.size(); ++i) {
        if (i != 13 + h.block) { near(audio[i].l, 0, 1e-7f, "no shifted Pair impulse"); }
    }
    std::cout << "PASS audit: Pair changes within a block; NaN/Inf input preserves other samples and echoes\n";
}

void scan_components() {
    constexpr uint64_t maximum = 179900;
    for (uint64_t size : {50ULL, 500ULL, 60000ULL}) {
        for (int i = 0; i <= 1000; ++i) {
            const auto scan = dsp::snap_offset(i * maximum / 1000.0, size, maximum);
            const auto spread = dsp::snap_offset(maximum - scan, size, maximum - scan);
            require(scan % size == 0 && spread % size == 0 && scan + spread <= maximum,
                    "Scan/Spread snap and clamp after rounding");
        }
    }
    dsp::StereoRing ring; require(ring.allocate(180000), "memory head test allocation");
    dsp::MemoryHeads heads; require(heads.configure(1000, maximum), "memory heads configure");
    heads.target(120000, 1, 60000);
    for (int i = 0; i < 150; ++i) {
        ring.push({0.125f, -0.25f});
        const auto value = heads.tick(ring, true);
        require(heads.scan() + heads.spread() <= maximum, "Scan+Spread never wraps to newer audio");
        require(dsp::finite(value), "fractional gliding reads remain finite");
    }
    require(heads.reached() && heads.scan() == 120000 && heads.spread() == 0, "deep Scan reaches snapped position within 150 ms");
    for (int i = 150; i < 180000; ++i) { ring.push({0.125f, -0.25f}); }
    const auto before = heads.scan(); heads.target(0, 0, 60000);
    heads.tick(ring, false); require(heads.scan() == before, "paused memory heads do not move");
    heads.snap(0);
    const auto old = heads.tick(ring, true);
    require(old.l > 0, "snap starts at old history rather than cutting it");
    for (int i = 0; i < 30; ++i) { heads.tick(ring, true); }
    require(heads.scan() == 0 && !heads.scanning(), "snap returns to present in 30 ms");
    ring.clear(); heads.snap(maximum);
    for (int i = 0; i < 32; ++i) { heads.tick(ring, true); }
    near(heads.tick(ring, true).l, 0, 0, "reads beyond valid history never wrap to recorded data");

    dsp::AutoScan a, b; a.reset(); b.reset();
    unsigned jumps = 0, returns = 0; bool was_away = false;
    // Ten simulated minutes, deterministic decisions at 500 ms boundaries.
    for (int i = 0; i < 600000; ++i) {
        const bool snap = a.tick(i % 500 == 0, 0.5f, maximum, 500, true);
        require(snap == b.tick(i % 500 == 0, 0.5f, maximum, 500, true) && a.target() == b.target(), "seeded Auto Scan repeatability");
        require(a.target() <= maximum / 4 && a.target() % 500 == 0, "Auto depth curve and Size snap");
        const bool away = a.state() != dsp::AutoScan::State::present;
        if (away && !was_away) { ++jumps; }
        if (snap) { ++returns; }
        was_away = away;
    }
    require(jumps > 50 && returns > 50 && jumps - returns <= 1, "Auto jumps and returns repeatedly");
    a.reset(); require(!a.tick(true, 0, maximum, 500, true) && a.target() == 0, "Auto zero stays present");
    a.tick(true, 1, maximum, 500, true);
    for (int i = 0; i < 100; ++i) { require(!a.tick(true, 1, maximum, 500, true) && a.target() > 0, "maximum Scan stays away"); }
    require(a.tick(false, 0, maximum, 500, true), "Scan hold release requests snap-back");
    std::cout << "PASS scan_snap/clamp/auto_scan: bounded transitions, whole Size positions, silent invalid history, snap-back, seeded 10-minute decisions\n";
}

void loop(const Library& lib) {
    for (int routing : {0, 1, 2}) {
        Host h(lib, 8000, 32); h.main->control(MainPort::routing) = routing;
        h.main->control(MainPort::repeats) = 0.485f;
        h.render(4096, [](uint64_t f) { return Frame{static_cast<float>(0.6 * std::sin(f * 0.03)), static_cast<float>(0.45 * std::cos(f * 0.04))}; });
        h.main->control(MainPort::loop) = 1; h.render(800, silence);
        const auto held = h.render(4400, silence);
        double energy = 0; float peak = 0;
        for (size_t i = 400; i < held.size(); ++i) {
            require(std::memcmp(&held[i], &held[i - 400], sizeof(Frame)) == 0, "Loop holds bit-exact for ten trips in routing " + std::to_string(routing));
            energy += held[i].l * held[i].l;
            peak = std::max(peak, std::fabs(held[i].l));
        }
        require(energy > 0.001, "Loop does not freeze silence in Feed");
        require(peak > 0.5f, "bit-exact Loop test exercises limiter bypass above its threshold");
        h.main->control(MainPort::size) = 100;
        h.render(2400, silence);
        const auto resized = h.render(2400, silence);
        energy = 0;
        for (size_t i = 800; i < resized.size(); ++i) {
            require(std::memcmp(&resized[i], &resized[i - 800], sizeof(Frame)) == 0, "Size change preserves held Loop at new length");
            energy += resized[i].l * resized[i].l;
        }
        require(energy > 0.001, "Size change does not erase Loop");
        h.main->control(MainPort::repeats) = 0;
        h.main->control(MainPort::loop) = 0;
        const auto released = h.render(8000, silence);
        near(released.back().l, 0, 1e-6f, "Loop release returns feedback to Repeats");
    }
    Host h(lib, 8000, 32); h.main->control(MainPort::routing) = 2;
    h.main->control(MainPort::latch) = 1; h.render(2048, dc);
    h.main->control(MainPort::loop) = 1; h.render(1024, silence);
    h.main->control(MainPort::loop) = 0;
    const auto latched = h.render(1600, silence);
    require(std::fabs(latched.back().l) > 0.01, "Latch survives falling edge");
    h.main->control(MainPort::loop) = 1;
    const auto off = h.render(8000, silence);
    near(off.back().l, 0, 1e-7f, "second rising edge unlatches, held high does not retrigger");
    std::cout << "PASS loop/latch: ten bit-exact trips in all routings, release and rising-edge latch\n";
}

void memory_audio(const Library& lib) {
    Host h(lib, 1000, 16);
    h.main->control(MainPort::level) = 0; h.main->control(MainPort::memory_dry) = 1;
    auto out = h.render(1024, dc);
    near(out.back().l, 0.125f, 0, "Scanning at present does not double dry");
    h.main->control(MainPort::dry_kill) = 1;
    h.main->control(MainPort::scan_mode) = 1;
    h.main->control(MainPort::scan) = 0.5f;
    h.render(100000, dc); // Glide into unrecorded history before it becomes valid.
    out = h.render(100000, dc);
    near(out.back().l, 0.125f, 1e-6f, "Manual Scan exposes memory dry");
    h.main->control(MainPort::spread) = 1;
    out = h.render(100000, dc);
    near(out.back().l, 0.125f * std::sqrt(2.0f), 1e-6f, "Spread equal-power mix");
    h.main->control(MainPort::clear) = 1;
    out = h.render(512, dc);
    near(out.back().l, 0, 1e-7f, "Clear makes deep history silent");
    h.main->control(MainPort::scan) = 0;
    h.main->control(MainPort::spread) = 0;
    h.main->control(MainPort::wipe) = 1;
    out = h.render(180000, dc);
    near(out.back().l, 0.125f, 1e-6f, "Wipe forces memory dry On even at present");
    std::cout << "PASS memory_dry/wipe/spread: no dry doubling, manual history, equal-power Spread, Clear\n";

    // At 1 kHz, the real 180-second memory is cheap enough to run whole turns.
    Host c(lib, 1000, 16); c.main->control(MainPort::level) = 0;
    c.main->control(MainPort::dry_kill) = 1; c.main->control(MainPort::memory_dry) = 2;
    c.main->control(MainPort::collect) = 1;
    const auto audio = c.render(360320, [](uint64_t f) { return f == 512 ? Frame{0.25f, -0.125f} : Frame{}; });
    near(audio[180512].l, 0.175f, 1e-7f, "Collect reads old slot before overwrite");
    near(audio[360512 - 256].l, 0, 0, "Collect does not move impulse forward");
    // Render beyond the second exact trip, keeping all accesses in range.
    const auto rest = c.render(512, silence);
    const auto global_start = c.frame - 512;
    near(rest[360512 - global_start].l, 0.1225f, 1e-7f, "Collect recirculates at fixed 0.7 gain");
    std::cout << "PASS collect: two full three-minute turns, fixed recirculation gain\n";
}

void bypass(const Library& lib) {
    for (int mode = 0; mode < 3; ++mode) {
        Host h(lib, 1000, 16); h.main->control(MainPort::level) = 0;
        h.main->control(MainPort::dry_kill) = 2; h.main->control(MainPort::wipe) = 1;
        h.main->control(MainPort::collect) = mode == 1;
        h.main->control(MainPort::always) = mode == 2;
        h.main->control(MainPort::scan_mode) = 1;
        h.main->control(MainPort::scan) = 50.0f / 179950.0f;
        h.render(1024, dc);
        h.main->control(MainPort::enabled) = 0;
        auto out = h.render(1024, [](uint64_t) { return Frame{0.25f, -0.125f}; });
        near(out.back().l, 0.25f, 1e-7f, "Series dry kill passes dry while bypassed");
        h.main->control(MainPort::dry_kill) = 1;
        out = h.render(512, [](uint64_t) { return Frame{0.25f, -0.125f}; });
        near(out.back().l, 0, 0, "Always dry kill stays silent while bypassed");
        h.main->control(MainPort::enabled) = 1;
        // First sample reveals the held current memory before the next write.
        h.main->control(MainPort::scan_mode) = 1;
        h.main->control(MainPort::scan) = 50.0f / 179950.0f;
        out = h.render(96, silence);
        if (mode == 0) { near(out.back().l, 0, 1e-7f, "ordinary bypass discards memory"); }
        else { near(out[30].l, mode == 1 ? 0.125f : 0.25f, 1e-6f, "Collect pauses its clock; Always records bypass input"); }
    }
    for (bool trails : {false, true}) {
        Host h(lib, 8000, 32); h.main->control(MainPort::routing) = 2;
        h.main->control(MainPort::repeats) = 0.485f;
        h.main->control(MainPort::trails) = trails;
        h.render(4096, dc); h.main->control(MainPort::enabled) = 0;
        const auto out = h.render(800, silence);
        double energy = 0; for (size_t i = 400; i < out.size(); ++i) { energy += out[i].l * out[i].l; }
        require(trails ? energy > 0.001 : energy == 0, "Trails controls echo tail after bypass");
    }
    Host smooth(lib, 8000, 32); smooth.main->control(MainPort::level) = 0;
    smooth.main->control(MainPort::memory_dry) = 2;
    auto audio = smooth.render(1024, dc); float previous = audio.back().l;
    for (const auto& setting : {std::pair{MainPort::enabled, 0}, {MainPort::enabled, 1},
                               {MainPort::dry_kill, 1}, {MainPort::dry_kill, 2},
                               {MainPort::enabled, 0}, {MainPort::enabled, 1}, {MainPort::dry_kill, 0}}) {
        smooth.main->control(setting.first) = setting.second;
        audio = smooth.render(1024, dc);
        for (const auto f : audio) {
            require(std::fabs(f.l - previous) < 0.004f, "bypass/Dry kill output transitions have no discontinuity"); previous = f.l;
        }
    }
    std::cout << "PASS bypass/dry_kill/trails: clear, pause, Always record, dry modes and echo tails\n";
}

void feed_wipe_and_bypass_safety(const Library& lib) {
    const auto wiped = [&](float size, float external_gain) {
        Host h(lib, 1000, 16); h.main->control(MainPort::routing) = 1;
        h.main->control(MainPort::wipe) = 1; h.main->control(MainPort::dry_kill) = 1;
        h.main->control(MainPort::size) = size; h.main->control(MainPort::repeats) = 0.485f;
        h.render(512, silence); const auto start = h.frame;
        return h.render(512, [=](uint64_t f) { return f == start ? Frame{0.125f, -0.0625f} : Frame{}; },
                        [=](Frame f, uint64_t) { return f * external_gain; });
    };
    const auto a = wiped(50, 0), b = wiped(500, 8);
    near(a[16].l, 0, 0, "Wipe does not recirculate memory dry at the host period");
    near(a[32].l, 0, 0, "no second host-period repeat");
    near(b[16].l, 0, 0, "external processing cannot create memory-dry feedback");

    Host h(lib, 1000, 16); h.main->control(MainPort::routing) = 2;
    h.main->control(MainPort::always) = h.main->control(MainPort::trails) = 1;
    h.main->control(MainPort::repeats) = 1;
    h.render(512, silence); const auto start = h.frame;
    h.render(112, [=](uint64_t f) { return f == start ? Frame{0.125f, 0} : Frame{}; });
    h.main->control(MainPort::enabled) = 0;
    const auto tail = h.render(4096, dc);
    for (size_t i = 2048; i < tail.size(); ++i) { near(tail[i].l, 0.125f, 1e-5f, "Always caps bypass feedback and cuts new echo input"); }

    // Always overrides Collect, so a full memory revolution cannot revive old input.
    h.main->control(MainPort::collect) = 1;
    h.main->control(MainPort::trails) = 0;
    h.render(180032, silence);
    h.main->control(MainPort::enabled) = 1; h.main->control(MainPort::wipe) = 1;
    h.main->control(MainPort::dry_kill) = 1;
    h.main->control(MainPort::scan_mode) = 1;
    h.main->control(MainPort::scan) = 50.0f / 179950.0f;
    const auto recalled = h.render(256, silence);
    near(recalled.back().l, 0, 1e-7f, "Always disables Collect during bypass");

    Host c(lib, 1000, 16); c.main->control(MainPort::collect) = 1;
    c.main->control(MainPort::trails) = 1; c.main->control(MainPort::wipe) = 1;
    c.main->control(MainPort::dry_kill) = 1; c.render(1024, dc);
    c.main->control(MainPort::enabled) = 0; c.render(1024, dc);
    c.main->control(MainPort::enabled) = 1;
    c.main->control(MainPort::scan_mode) = 1; c.main->control(MainPort::scan) = 50.0f / 179950.0f;
    const auto continued = c.render(96, silence);
    near(continued[30].l, 0, 1e-7f, "Collect with Trails advances without recording live bypass input");
    std::cout << "PASS feed_wipe/bypass_safety: no memory-dry comb, Always feedback cap/Collect override, Trails memory clock\n";
}

void slice_cv(const Library& lib) {
    {
        Host h(lib, 1000, 16); h.main->control(MainPort::size) = 60000;
        h.main->control(MainPort::level) = 0; h.main->control(MainPort::dry_kill) = 1;
        h.render(180016, dc); h.main->control(MainPort::scan_hold) = 1;
        const auto audio = h.render(16, dc);
        require(audio.back().l > 0.0001f, "Scan hold starts immediately instead of waiting a 60-second Size boundary");
    }
    {
        Host h(lib, 1000, 16); h.main->control(MainPort::level) = 0;
        h.main->control(MainPort::dry_kill) = 1; h.main->control(MainPort::memory_dry) = 1;
        h.main->control(MainPort::scan_mode) = 1;
        h.render(180016, dc);
        for (auto& value : h.main->buffers[31]) { value = 1; }
        auto audio = h.render(1024, dc);
        require(audio.back().l > 0.05f, "Spread CV turns on historical memory dry and its second head");
        for (auto& value : h.main->buffers[31]) { value = -1; }
        h.render(4096, dc);
        for (auto& value : h.main->buffers[32]) { value = 1; }
        audio = h.render(1024, dc);
        require(audio.back().l > 0.05f, "Scan CV engages manual memory playback");
        for (auto& value : h.main->buffers[32]) { value = -1; }
        audio = h.render(16000, dc);
        near(audio.back().l, 0, 1e-7f, "negative Scan/Spread CV clamps at present and mutes Scanning dry");
        h.main->control(MainPort::scan_mode) = 0; h.main->control(MainPort::scan_hold) = 1;
        audio = h.render(1024, dc);
        require(audio.back().l > 0.05f, "Scan hold overrides negative CV and starts Auto at maximum");
        h.main->control(MainPort::scan_hold) = 0;
        audio = h.render(2048, dc);
        near(audio.back().l, 0, 1e-7f, "Scan hold release snaps to present and restores Scanning dry");
    }
    const auto run = [&](bool sliced) {
        Host h(lib, 1000, 16); h.random_slices = sliced;
        h.main->control(MainPort::memory_dry) = 1;
        h.main->control(MainPort::scan_mode) = 1;
        h.main->control(MainPort::collect) = 1;
        std::vector<Frame> out;
        for (int p = 0; p < 800; ++p) {
            for (uint32_t i = 0; i < h.block; ++i) {
                h.main->buffers[31][i] = p > 200 && p < 400 ? 1 : -1;
                h.main->buffers[32][i] = p > 100 && p < 300 ? 1 : -1;
            }
            h.main->control(MainPort::enabled) = p < 400 || p > 600;
            h.main->control(MainPort::trails) = p > 500;
            h.main->control(MainPort::loop) = p > 650 && p < 720;
            const auto part = h.render(h.block, dc); out.insert(out.end(), part.begin(), part.end());
        }
        return out;
    };
    const auto a = run(false), b = run(true);
    require(std::memcmp(a.data(), b.data(), a.size() * sizeof(Frame)) == 0, "memory controls and CV are independent of host slices");
    std::cout << "PASS memory_cv/slices: Scan/Spread CV endpoints and bypass/Loop changes are slice independent\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: test_memory discipline.so"); Library lib(argv[1]);
        audit(lib); scan_components(); loop(lib); memory_audio(lib); bypass(lib); feed_wipe_and_bypass_safety(lib); slice_cv(lib);
    } catch (const std::exception& e) { std::cerr << "FAIL memory: " << e.what() << '\n'; return 1; }
}
