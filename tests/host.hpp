// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "../src/ports.hpp"
#include "../src/dsp/frame.hpp"
#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace test {
using discipline::dsp::Frame;
void require(bool condition, const std::string& message);
void near(float actual, float expected, float tolerance, const std::string& message);

class Library {
public:
    explicit Library(const char* path);
    ~Library();
    const LV2_Descriptor* main = nullptr;
    const LV2_Descriptor* ret = nullptr;
private:
    void* library_;
};

class Features {
public:
    explicit Features(uint32_t block, bool nominal = true);
    static LV2_URID map_uri(LV2_URID_Map_Handle handle, const char* uri);
    std::map<std::string, LV2_URID> urids;
    LV2_URID_Map map;
    int32_t block;
    std::array<LV2_Options_Option, 3> options{};
    LV2_Feature map_feature, options_feature;
    const LV2_Feature* list[3];
};

class Instance {
public:
    Instance(const LV2_Descriptor* descriptor, double rate, uint32_t block, bool is_return = false, bool nominal = true);
    ~Instance();
    void slice(uint32_t offset, uint32_t frames);
    void activate();
    float& control(discipline::MainPort port) { return controls[static_cast<uint32_t>(port)]; }
    const LV2_Descriptor* descriptor;
    LV2_Handle handle;
    bool is_return;
    uint32_t block;
    Features features;
    std::array<float, static_cast<uint32_t>(discipline::MainPort::count)> controls{};
    std::array<std::vector<float>, static_cast<uint32_t>(discipline::MainPort::count)> buffers;
    bool measure = false;
    double last_time = 0;
};

class Host {
public:
    Host(const Library& library, double rate = 48000, uint32_t block = 128, bool return_first = false, bool nominal = true);
    using Source = std::function<Frame(uint64_t)>;
    using Effect = std::function<Frame(Frame, uint64_t)>;
    // Returns full output including dry; last_send exposes the current period.
    std::vector<Frame> render(uint64_t frames, const Source& source,
                              const Effect& effect = [](Frame f, uint64_t) { return f; });
    void period(const Source& source, const Effect& effect);
    std::unique_ptr<Instance> main, ret;
    std::vector<Frame> last_send;
    double rate;
    uint32_t block;
    uint64_t frame = 0;
    bool return_first, random_slices = true, run_return = true;
    std::vector<double> run_times;
    bool measure = false;
private:
    double run_slices(Instance& instance);
    uint32_t rng_ = 0x6A09E667;
};
} // namespace test
