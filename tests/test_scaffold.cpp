// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "../src/ports.hpp"

#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace discipline;
using DescriptorFunction = const LV2_Descriptor* (*)(uint32_t);

namespace {
void check(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

LV2_URID map_uri(LV2_URID_Map_Handle, const char*) { return 1; }
LV2_URID_Map map{nullptr, map_uri};
LV2_Feature map_feature{LV2_URID__map, &map};
const LV2_Feature* features[]{&map_feature, nullptr};

struct Instance {
    const LV2_Descriptor* descriptor;
    LV2_Handle handle;
    std::array<std::array<float, 1026>, 35> buffers{};
    uint32_t count;

    explicit Instance(const LV2_Descriptor* d, bool is_return = false)
        : descriptor(d), handle(d->instantiate(d, 8000, ".", is_return ? nullptr : features)),
          count(is_return ? 4 : 35)
    {
        check(handle, "instantiate");
        for (uint32_t p = 0; p < count; ++p) { d->connect_port(handle, p, buffers[p].data()); }
        buffers[is_return ? 2 : 26][0] = 1.0f;
        if (!is_return) { buffers[8][0] = 60000.0f; }
        if (d->activate) { d->activate(handle); }
    }
    ~Instance() { descriptor->cleanup(handle); }
    Instance(const Instance&) = delete;
    void run(uint32_t frames = 0) { descriptor->run(handle, frames); }
    void pair(float id) { buffers[count == 4 ? 2 : 26][0] = id; }
    bool paired() const { return buffers[count == 4 ? 3 : 34][0] == 1.0f; }
};

void test_audio(const LV2_Descriptor* main)
{
    Instance instance(main);
    uint32_t rng = 0x12345678;
    for (const auto frames : {0U, 1U, 37U, 91U, 128U, 256U, 1024U}) {
        for (uint32_t p : {2U, 3U, 4U, 5U, 33U}) { instance.buffers[p].fill(99.0f); }
        for (uint32_t i = 0; i < frames; ++i) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            instance.buffers[0][i] = static_cast<float>(rng % 1000) / 1000.0f;
            instance.buffers[1][i] = -instance.buffers[0][i];
        }
        uint32_t offset = 0;
        do {
            const uint32_t slice = frames ? std::min(1 + rng % 43, frames - offset) : 0;
            for (uint32_t p : {0U, 1U, 2U, 3U, 4U, 5U, 33U}) {
                main->connect_port(instance.handle, p, instance.buffers[p].data() + offset);
            }
            instance.run(slice);
            offset += slice;
        } while (offset < frames);
        for (uint32_t i = 0; i < frames; ++i) {
            check(instance.buffers[0][i] == instance.buffers[2][i], "left passthrough");
            check(instance.buffers[1][i] == instance.buffers[3][i], "right passthrough");
            check(instance.buffers[4][i] == 0 && instance.buffers[5][i] == 0 && instance.buffers[33][i] == 0, "Send/clock silence");
        }
        for (uint32_t p : {2U, 3U, 4U, 5U, 33U}) {
            check(instance.buffers[p][frames] == 99.0f, "write past slice boundary");
        }
    }
    main->connect_port(instance.handle, 0, instance.buffers[0].data());
    main->connect_port(instance.handle, 1, instance.buffers[1].data());
    main->connect_port(instance.handle, 2, instance.buffers[0].data());
    main->connect_port(instance.handle, 3, instance.buffers[1].data());
    for (uint32_t p : {4U, 5U, 33U}) {
        main->connect_port(instance.handle, p, instance.buffers[p].data());
    }
    const auto left = instance.buffers[0];
    const auto right = instance.buffers[1];
    instance.run(128);
    check(instance.buffers[0] == left && instance.buffers[1] == right, "in-place passthrough");
    // Unconnected ports and unknown indices must not crash, even for a zero-frame run.
    for (uint32_t p = 0; p < 35; ++p) { main->connect_port(instance.handle, p, nullptr); }
    main->connect_port(instance.handle, 999, nullptr);
    instance.run(128);
    instance.run(0);
    std::cout << "PASS audio: random slices, zero frames, silence, bounds, in-place\n";
}

void test_pairs(const LV2_Descriptor* main, const LV2_Descriptor* ret)
{
    for (bool return_first : {false, true}) {
        Instance m(main), r(ret, true), duplicate_m(main), duplicate_r(ret, true);
        if (return_first) { r.run(); m.run(); } else { m.run(); r.run(); }
        m.run(); r.run();
        check(m.paired() && r.paired(), "pair creation order");
        duplicate_m.run(); duplicate_r.run();
        check(!duplicate_m.paired() && !duplicate_r.paired(), "duplicate ownership");
        m.pair(16); m.run(); r.run();
        check(!m.paired() && !r.paired(), "Pair change releases old slot");
        r.pair(16); r.run(); m.run();
        check(m.paired() && r.paired(), "Pair 16 joins");
        duplicate_m.run(); duplicate_r.run(); duplicate_m.run();
        check(duplicate_m.paired() && duplicate_r.paired(), "duplicate retries released slot");
        main->deactivate(m.handle); r.run();
        check(!r.paired(), "deactivate releases");
        main->activate(m.handle); m.run(); r.run();
        check(m.paired() && r.paired(), "reactivation");
        ret->deactivate(r.handle); m.run();
        check(!m.paired(), "Return deactivate releases");
    }
    {
        Instance r(ret, true);
        auto m = std::make_unique<Instance>(main);
        m->run(); r.run(); check(r.paired(), "paired before cleanup");
        m.reset(); r.run(); check(!r.paired(), "main cleanup releases");
    }
    {
        Instance m(main);
        auto r = std::make_unique<Instance>(ret, true);
        r->run(); m.run(); check(m.paired(), "paired before Return cleanup");
        r.reset(); m.run(); check(!m.paired(), "Return cleanup releases");
    }
    for (float value : {-100.0f, 100.0f, 1.49f, 1.5f, std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity()}) {
        Instance m(main), r(ret, true);
        m.pair(value); r.pair(value); m.run(); r.run(); m.run();
        check(m.paired() && r.paired(), "Pair finite/clamped/rounded");
    }
    std::cout << "PASS pair: both orders, collisions, changes, cleanup, reactivation, invalid values\n";
}

void test_concurrent_cleanup(const LV2_Descriptor* main, const LV2_Descriptor* ret)
{
    Instance r(ret, true);
    std::atomic<bool> running{true};
    std::thread worker([&] { while (running.load()) { r.run(37); } });
    for (int i = 0; i < 64; ++i) {
        Instance m(main);
        m.run(91);
    }
    running.store(false);
    worker.join();
    r.run(); check(!r.paired(), "concurrent cleanup leaves no owner");
    std::cout << "PASS pair: Return runs while main instances are created and deleted\n";
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) { std::cerr << "usage: test_scaffold discipline.so\n"; return 1; }
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::cerr << "FAIL dlopen: " << dlerror() << '\n'; return 1; }
    int result = 0;
    try {
        const auto descriptor = reinterpret_cast<DescriptorFunction>(dlsym(library, "lv2_descriptor"));
        check(descriptor, "lv2_descriptor export");
        const auto* main = descriptor(0);
        const auto* ret = descriptor(1);
        check(main && ret && std::strcmp(main->URI, kMainURI) == 0 && std::strcmp(ret->URI, kReturnURI) == 0, "descriptor URIs/order");
        check(!descriptor(2) && !descriptor(UINT32_MAX), "descriptor terminator");
        check(!main->instantiate(main, 48000, ".", nullptr), "required urid:map");
        const LV2_Feature* empty_features[]{nullptr};
        check(!main->instantiate(main, 48000, ".", empty_features), "empty required features");
        LV2_URID_Map broken_map{nullptr, nullptr};
        LV2_Feature broken_feature{LV2_URID__map, &broken_map};
        const LV2_Feature* broken_features[]{&broken_feature, nullptr};
        check(!main->instantiate(main, 48000, ".", broken_features), "invalid map");
        for (double rate : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            check(!main->instantiate(main, rate, ".", features), "invalid main rate");
            check(!ret->instantiate(ret, rate, ".", nullptr), "invalid Return rate");
        }
        for (double rate : {44100.0, 48000.0, 96000.0}) {
            for (auto* d : {main, ret}) {
                auto h = d->instantiate(d, rate, ".", features);
                check(h, "supported sample rate"); d->cleanup(h);
            }
        }
        std::cout << "PASS descriptors: shared library, features, sample rates\n";
        test_audio(main);
        test_pairs(main, ret);
        test_concurrent_cleanup(main, ret);
    } catch (const std::exception& e) {
        std::cerr << "FAIL scaffold: " << e.what() << '\n'; result = 1;
    }
    dlclose(library);
    return result;
}
