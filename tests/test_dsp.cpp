// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "host.hpp"
#include "../src/dsp/stereo_ring.hpp"
#include "../src/dsp/smoother.hpp"
#include "../src/dsp/xfade_tap.hpp"
#include "../src/dsp/loop_gain.hpp"
#include "../src/host_config.hpp"
#include "../src/pair_registry.hpp"
#include <atomic>
#include <iostream>
#include <thread>

using namespace discipline;
using test::require;
using test::near;

int main()
{
    try {
        dsp::StereoRing ring;
        require(ring.allocate(1024), "ring allocation");
        near(ring.read(1).l, 0, 0, "startup silence");
        for (int i = 0; i < 2048; ++i) { ring.push({static_cast<float>(i), -static_cast<float>(i)}); }
        near(ring.read(1).l, 2047, 0, "ring newest");
        near(ring.read(1024).l, 1024, 0, "ring oldest after wrap");
        near(ring.read(1025).l, 0, 0, "ring never reads overwritten history");
        near(ring.fractional(3.5, 0).l, 2044.5f, 0, "fractional ramp");
        near(ring.fractional(1.5, 0).l, 2046.5f, 0, "interpolation at present does not invent an unwritten sample");
        near(dsp::hermite(0, 1, 2, 3, 0.5f), 1.5f, 0, "Hermite linear interpolation");
        ring.clear(); near(ring.read(1).l, 0, 0, "logical clear");
        ring.push({0.25f, -0.5f}); near(ring.read(1).l, 0.25f, 0, "fresh input is uncoloured");
        near(ring.fractional(1.1, 0).l, 0, 0, "fractional reads cannot bleed across Clear");
        near(ring.read(1, 256).l, 0, 0, "history boundary starts silent");
        std::cout << "PASS stereo_ring/interp: wrap, fractional reads, validity, boundary fade\n";

        dsp::OnePole a, b; a.configure(48000, 0.01); b.configure(48000, 0.01);
        for (int i = 0; i < 1000; ++i) { a.tick(1); }
        for (int slice : {37, 91, 256, 616}) { for (int i = 0; i < slice; ++i) { b.tick(1); } }
        near(a.value(), b.value(), 0, "smoothing slice independence");
        require(a.value() > 0.8f && a.value() < 1, "one-pole response");
        for (int i = 0; i < 15000; ++i) { a.tick(1); }
        near(a.value(), 1, 0, "smoother reaches exact unity after a parameter change");
        dsp::Slew slew(1); near(static_cast<float>(slew.tick(100)), 1, 0, "slew limit up");
        near(static_cast<float>(slew.tick(-100)), 0, 0, "slew limit down");
        dsp::MemoryClear clear(20); clear.request();
        const auto before = ring.position();
        for (int i = 0; i < 20; ++i) { clear.tick(ring); }
        require(ring.position() == before && ring.valid_from() == before, "clear advances no audio counter");
        near(ring.read(1).l, 0, 0, "clear invalidates prior contents");
        for (int i = 0; i < 256; ++i) { clear.tick(ring); }
        near(clear.tick(ring), 1, 0, "clear fade recovers");
        std::cout << "PASS smoother/clear: per-sample gain, slew, constant-time clear\n";

        struct FakeRing { dsp::Frame read(uint64_t delay) const { return {static_cast<float>(delay), 0}; } } fake;
        dsp::CrossfadeTap tap; require(tap.configure(30), "fade allocation"); tap.reset(10); tap.target(20);
        near(tap.read(fake).l, 10, 0, "fade starts at old tap");
        for (int i = 1; i < 30; ++i) { tap.target(50 + i); tap.read(fake); }
        require(!tap.fading() && tap.current() == 20, "target does not interrupt a fade");
        tap.read(fake); require(tap.fading(), "latest target starts next fade");
        for (int i = 1; i < 30; ++i) { tap.read(fake); }
        require(tap.current() == 79, "latest target retained");
        std::cout << "PASS xfade_tap: fixed read delays, 30 ms cadence, pending target\n";

        dsp::LoopGain limiter; limiter.configure(48000);
        near(limiter.process({0.25f, -0.25f}).l, 0.25f, 0, "feedback unity below threshold");
        for (int i = 0; i < 4800; ++i) {
            const auto output = limiter.process({20, -20});
            require(std::fabs(output.l) <= 4 && output.l == -output.r, "linked safety ceiling");
        }
        require(limiter.process({20, -20}).l < 0.501f, "limiter attack");
        for (int i = 0; i < 48000 * 8; ++i) { limiter.process({0.25f, -0.25f}); }
        near(limiter.process({0.25f, -0.25f}).l, 0.25f, 0, "limiter release returns to exact unity");
        limiter.reset(); near(limiter.process({1e-20f, 0}).l, 0, 0, "feedback denormal flush");
        std::cout << "PASS loop_gain: unity region, stereo linking, bounded feedback\n";

        test::Features features(64);
        int32_t maximum = 256;
        features.options[0].value = &maximum;
        auto config = host_config(features.list);
        require(config.block == 64 && config.maximum == 256, "nominal precedes maximum");
        features.options[1].size = 1;
        config = host_config(features.list);
        require(config.block == 256, "malformed nominal uses maximum");
        features.options[0].value = nullptr;
        require(host_config(features.list).block == 128, "missing block options use 128");
        require(!host_config(nullptr).has_map, "map absence");
        std::cout << "PASS host options: typed values, precedence, validation and fallback\n";

        prepare_pair_registry();
        PairClaim main(false), ret(true); float id = 16;
        main.update(&id, 64); ret.update(&id);
        auto& slot = *main.slot(); slot.main_frames.store(64);
        ReturnWriter writer;
        std::atomic<bool> ready{false}, done{false};
        std::thread producer([&] {
            ready.store(true);
            float left[32], right[32];
            for (int j = 0; j < 20000; ++j) {
                for (int i = 0; i < 32; ++i) { left[i] = static_cast<float>(j * 32 + i + 1); right[i] = -left[i]; }
                writer.write(&slot, left, right, 32);
            }
            done.store(true);
        });
        while (!ready.load()) { std::this_thread::yield(); }
        uint64_t reads = 0; bool coherent = true;
        while (!done.load()) {
            const auto end = slot.written.load();
            dsp::Frame sample{};
            if (end && read_return(slot, slot.epoch.load(), end - 1, sample)) {
                coherent = coherent && sample.l == -sample.r && sample.l > 0;
                ++reads;
            }
        }
        producer.join();
        require(coherent && reads > 0, "concurrent atomic transport snapshots");
        std::cout << "PASS transport: concurrent overwrite gives coherent stereo or silence\n";
    } catch (const std::exception& e) { std::cerr << "FAIL dsp: " << e.what() << '\n'; return 1; }
}
