// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "host.hpp"
#include "../src/dsp/memory_heads.hpp"
#include <iostream>

using namespace discipline;
using namespace test;
namespace {
Frame silence(uint64_t) { return {}; }

void scan_transitions() {
    dsp::StereoRing ring; require(ring.allocate(180000), "audit memory allocation");
    for (int i = 0; i < 181000; ++i) { ring.push({0.125f, -0.0625f}); }
    dsp::MemoryHeads heads; require(heads.configure(1000, 179900), "audit heads configure");
    for (double target : {179900.0, 0.0, 10000.0}) {
        heads.target(target, 0, 50);
        for (int i = 0; i < 150; ++i) {
            ring.push({0.125f, -0.0625f});
            near(heads.tick(ring, true).l, 0.125f, 2e-8f, "Scan crossfade does not boost correlated audio");
        }
        require(heads.reached() && heads.scan() == target, "full-depth Scan and return complete in 150 ms");
    }
    heads.target(150000, 0, 50);
    for (int i = 0; i < 20; ++i) { heads.tick(ring, true); }
    heads.target(50000, 1, 50);
    const auto paused = heads.tick(ring, false);
    for (int i = 0; i < 200; ++i) {
        near(heads.tick(ring, false).l, paused.l, 0, "pause freezes scrub and crossfade");
    }
    for (int i = 0; i < 150; ++i) { heads.tick(ring, true); }
    require(heads.reached() && heads.scan() == 50000 && heads.scan() + heads.spread() <= heads.maximum(),
            "coalesced Scan and Spread target arrives within 150 ms");

    dsp::AutoScan scan(17); heads.reset();
    for (int i = 0; i < 20 && !scan.target(); ++i) { scan.tick(true, 0.5f, heads.maximum(), 50, true); }
    require(scan.target() > 0, "Auto picked a depth"); heads.target(scan.target(), 0, 50);
    for (int i = 0; i < 150; ++i) {
        heads.tick(ring, true); scan.tick(false, 0.5f, heads.maximum(), 50, heads.reached());
    }
    require(scan.state() == dsp::AutoScan::State::away, "Auto hold starts after milliseconds rather than minutes");
    dsp::StereoRing tone; require(tone.allocate(1024), "scrub tone allocation");
    for (int i = 0; i <= 1100; ++i) { tone.push({static_cast<float>(0.1 * std::sin(i * 0.06283185307179586)), 0}); }
    dsp::MemoryHeads scrub; require(scrub.configure(1000, 900), "scrub configure");
    scrub.target(500, 0, 50);
    near(scrub.tick(tone, true).l, tone.fractional(1.5).l, 1e-8f, "short scrub affects outgoing audio immediately");
    std::cout << "PASS audit_scan_time: full depth, return, retargeting, paused fades, constant gain and Auto arrival\n";
}

void feed(const Library& lib) {
    for (float level : {0.25f, 0.5f, 0.70710678f, 1.0f}) {
        Host h(lib); h.main->control(MainPort::routing) = 1;
        h.main->control(MainPort::level) = level; h.main->control(MainPort::repeats) = 1;
        h.render(1024, silence); const auto start = h.frame;
        const auto audio = h.render(2400 * 4, [=](uint64_t f) { return f == start ? Frame{0.125f, 0} : Frame{}; });
        for (size_t i = 1; i <= 3; ++i) {
            near(audio[i * 2400].l, 0.125f * 2 * level * level, 1e-7f, "Feed unity feedback is independent of Level");
        }
    }
    for (bool wipe : {false, true}) {
        Host h(lib); h.main->control(MainPort::routing) = 1;
        h.main->control(MainPort::memory_dry) = 2; h.main->control(MainPort::wipe) = wipe;
        h.main->control(MainPort::dry_kill) = 1; h.main->control(MainPort::repeats) = 1;
        h.render(1024, silence); const auto start = h.frame;
        const auto audio = h.render(2400 * 6, [=](uint64_t f) { return f == start ? Frame{0.25f, 0} : Frame{}; });
        for (size_t i = 1; i < audio.size(); ++i) {
            const float expected = i % 2400 == 0 ? (wipe ? 0.25f : 0.5f) : 0;
            near(audio[i].l, expected, 1e-7f, "Feed has no block-period comb or growing memory-dry loop");
        }
    }
    Host h(lib, 1000, 16); h.main->control(MainPort::routing) = 1;
    h.main->control(MainPort::wipe) = 1; h.main->control(MainPort::dry_kill) = 1;
    h.main->control(MainPort::scan_mode) = 1; h.main->control(MainPort::scan) = 100.0f / 179950.0f;
    h.main->control(MainPort::repeats) = 1;
    h.render(512, silence); const auto start = h.frame;
    const auto audio = h.render(1024, [=](uint64_t f) { return f == start ? Frame{0.25f, 0} : Frame{}; });
    for (size_t i = 0; i < audio.size(); ++i) {
        near(audio[i].l, i >= 100 && (i - 100) % 150 == 0 ? 0.25f : 0, 1e-7f,
             "deep Feed repeats stay on the Size grid without one-block drift");
    }
    std::cout << "PASS audit_feed: Level-independent unity repeats, no host-period comb, bounded gain, Size-grid history\n";
}

void manual_return_and_spread(const Library& lib) {
    Host h(lib, 1000, 16); h.main->control(MainPort::level) = 0;
    h.main->control(MainPort::memory_dry) = 2; h.main->control(MainPort::dry_kill) = 1;
    h.main->control(MainPort::scan_mode) = 1;
    h.main->control(MainPort::scan) = 10000.0f / 179950.0f;
    const auto ramp = [](uint64_t f) { return Frame{static_cast<float>(f) * 1e-6f, 0}; };
    h.render(200000, ramp); h.main->control(MainPort::scan_hold) = 1;
    h.render(2000, ramp); h.main->control(MainPort::scan_hold) = 0;
    const auto from = h.frame; const auto recalled = h.render(128, ramp);
    near(recalled.back().l, ramp(from + 127 - 10000).l, 1e-7f, "Scan hold returns to Manual's 10-second setting within 100 ms");

    Host s(lib, 1000, 16); s.main->control(MainPort::level) = 0;
    s.main->control(MainPort::size) = 500; s.main->control(MainPort::spread) = 0.001f;
    const auto dc = [](uint64_t) { return Frame{0.1f, -0.1f}; };
    const auto tiny = s.render(2048, dc);
    near(tiny.back().l, 0.1f, 0, "Spread snapped to zero never enables mixing or Scanning memory dry");

    Host p(lib, 1000, 16); p.main->control(MainPort::level) = 0;
    p.main->control(MainPort::spread) = 50.0f / 179950.0f;
    p.render(1024, silence); const auto start = p.frame;
    const auto audio = p.render(512, [=](uint64_t f) { return f == start ? Frame{0.1f, 0} : Frame{}; });
    near(audio[0].l, 0.1f, 1e-7f, "Spread Scanning does not add a second copy of live dry");
    near(audio[50].l, 0.1f * 0.70710678f, 1e-7f, "Spread Scanning still plays the historical second tap");
    std::cout << "PASS audit_manual/spread: correct hold return, snapped-zero mix and historical-only Scanning\n";
}

void bypass_and_collect(const Library& lib) {
    for (uint32_t off_frames : {32U, 128U}) {
        Host h(lib, 8000, 32); h.main->control(MainPort::routing) = 2;
        h.main->control(MainPort::repeats) = 1;
        h.render(512, silence); const auto start = h.frame;
        h.render(1024, [=](uint64_t f) { return f == start ? Frame{0.125f, 0} : Frame{}; });
        h.main->control(MainPort::enabled) = 0; h.render(off_frames, silence);
        h.main->control(MainPort::enabled) = 1; const auto audio = h.render(2400, silence);
        for (size_t i = 200; i < audio.size(); ++i) { near(audio[i].l, 0, 1e-7f, "short bypass does not resurrect old echoes"); }
    }
    {
        Host h(lib, 8000, 32); h.main->control(MainPort::routing) = 2;
        h.main->control(MainPort::repeats) = 1;
        h.render(1024, silence);
        h.main->control(MainPort::enabled) = 0; h.render(32, silence);
        h.main->control(MainPort::enabled) = 1;
        h.render(64, [](uint64_t) { return Frame{0.125f, 0}; });
        // The previous pending fade completed during that short re-enable.
        // A second bypass must discard newly recorded audio as well.
        h.main->control(MainPort::enabled) = 0; h.render(32, silence);
        h.main->control(MainPort::enabled) = 1; const auto audio = h.render(2400, silence);
        for (size_t i = 200; i < audio.size(); ++i) { near(audio[i].l, 0, 1e-7f, "repeated quick bypass cannot retain newly recorded echoes"); }
    }
    Host l(lib, 8000, 32); l.main->control(MainPort::routing) = 2;
    l.main->control(MainPort::repeats) = 0.485f; l.main->control(MainPort::trails) = 1;
    l.render(4096, [](uint64_t) { return Frame{0.125f, 0}; });
    l.main->control(MainPort::loop) = 1; l.render(1024, silence);
    l.main->control(MainPort::enabled) = 0; const auto tail = l.render(16000, silence);
    near(tail.back().l, 0, 1e-7f, "Trails releases held Loop back to ordinary decaying repeats");

    Host c(lib, 1000, 16); c.main->control(MainPort::level) = 0;
    c.main->control(MainPort::dry_kill) = 1; c.main->control(MainPort::memory_dry) = 2;
    c.main->control(MainPort::collect) = 1;
    c.render(180000, [](uint64_t f) { return f < 512 ? Frame{0.125f, 0} : Frame{}; });
    const auto first = c.render(512, silence);
    for (size_t i = 0; i <= 256; ++i) {
        near(first[i].l, 0.0875f * static_cast<float>(i) / 256, 1e-7f, "Collect fades the startup validity boundary");
    }
    c.main->control(MainPort::clear) = 1; const auto start = c.frame;
    const auto after_clear = c.render(180400, [=](uint64_t f) { return f < start + 512 ? Frame{0.125f, 0} : Frame{}; });
    for (size_t i = 0; i <= 256; ++i) {
        near(after_clear[180019 + i].l, 0.0875f * static_cast<float>(i) / 256, 1e-7f, "Collect fades the post-Clear validity boundary");
    }
    std::cout << "PASS audit_bypass/collect: short bypass discards echoes, Trails releases Loop, Collect boundaries fade\n";
}

void rng_and_interpolation(const Library& lib) {
    dsp::AutoScan a(123), b(123), other(456);
    uint64_t first = 0; bool continued = false, distinct = false;
    for (int i = 0; i < 12; ++i) {
        a.reset(); b.reset(); other.reset();
        a.tick(true, 1, 179900, 50, true); b.tick(true, 1, 179900, 50, true); other.tick(true, 1, 179900, 50, true);
        require(a.target() == b.target(), "explicit seeds remain reproducible");
        if (i == 0) { first = a.target(); }
        else { continued |= a.target() != first; }
        distinct |= a.target() != other.target();
    }
    require(continued && distinct, "mode/hold resets preserve randomness and independent seeds differ");
    const auto excursions = [&] {
        Host h(lib, 1000, 16); h.main->control(MainPort::level) = 0;
        h.main->control(MainPort::memory_dry) = 2; h.main->control(MainPort::dry_kill) = 1;
        const auto ramp = [](uint64_t f) { return Frame{static_cast<float>(f) * 1e-6f, 0}; };
        h.render(180032, ramp); std::vector<float> positions;
        for (int i = 0; i < 8; ++i) {
            h.main->control(MainPort::scan_hold) = 1;
            positions.push_back(h.render(256, ramp).back().l);
            h.main->control(MainPort::scan_hold) = 0; h.render(128, ramp);
        }
        return positions;
    };
    const auto one = excursions(), two = excursions();
    require(one != two, "separate plugin instances do not scan in lockstep");
    dsp::StereoRing ring; require(ring.allocate(1024), "interpolation ring");
    for (int i = 0; i < 1024; ++i) { ring.push({0.125f, -0.25f}); }
    for (double offset : {0.001, 0.25, 0.5, 0.75, 0.999}) {
        const auto sample = ring.fractional(1 + offset, 0);
        near(sample.l, 0.125f, 1e-8f, "leaving the present does not overshoot a constant signal");
        near(sample.r, -0.25f, 1e-8f, "both channels interpolate the present correctly");
    }
    std::cout << "PASS audit_rng/interpolation: continuing independent random streams and clean present boundary\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: test_m2_audit discipline.so"); Library lib(argv[1]);
        scan_transitions(); feed(lib); manual_return_and_spread(lib); bypass_and_collect(lib); rng_and_interpolation(lib);
    } catch (const std::exception& e) { std::cerr << "FAIL Milestone 2 audit: " << e.what() << '\n'; return 1; }
}
