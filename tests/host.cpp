// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "host.hpp"
#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <dlfcn.h>
#include <stdexcept>

namespace test {
void require(bool condition, const std::string& message) { if (!condition) { throw std::runtime_error(message); } }
void near(float actual, float expected, float tolerance, const std::string& message) {
    require(std::isfinite(actual) && std::fabs(actual - expected) <= tolerance,
            message + ": got " + std::to_string(actual) + ", expected " + std::to_string(expected));
}
Library::Library(const char* path) : library_(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {
    if (!library_) { throw std::runtime_error(dlerror()); }
    using Get = const LV2_Descriptor* (*)(uint32_t);
    const auto get = reinterpret_cast<Get>(dlsym(library_, "lv2_descriptor"));
    require(get, "descriptor symbol"); main = get(0); ret = get(1);
    require(main && ret, "both descriptors");
}
Library::~Library() { dlclose(library_); }
LV2_URID Features::map_uri(LV2_URID_Map_Handle handle, const char* uri) {
    auto& ids = static_cast<Features*>(handle)->urids;
    const auto found = ids.find(uri);
    if (found != ids.end()) { return found->second; }
    const auto id = static_cast<LV2_URID>(ids.size() + 1);
    ids.emplace(uri, id); return id;
}
Features::Features(uint32_t length, bool nominal)
    : map{this, map_uri}, block(static_cast<int32_t>(length)),
      map_feature{LV2_URID__map, &map}, options_feature{LV2_OPTIONS__options, options.data()},
      list{&map_feature, &options_feature, nullptr}
{
    const auto type = map_uri(this, LV2_ATOM__Int);
    options[0] = {LV2_OPTIONS_INSTANCE, 0, map_uri(this, LV2_BUF_SIZE__maxBlockLength), sizeof(block), type, &block};
    if (nominal) { options[1] = {LV2_OPTIONS_INSTANCE, 0, map_uri(this, LV2_BUF_SIZE__nominalBlockLength), sizeof(block), type, &block}; }
}
Instance::Instance(const LV2_Descriptor* d, double rate, uint32_t size, bool ret, bool nominal)
    : descriptor(d), handle(nullptr), is_return(ret), block(size), features(size, nominal)
{
    handle = d->instantiate(d, rate, ".", features.list);
    require(handle, "instantiate at " + std::to_string(rate) + " Hz, block " + std::to_string(size));
    for (auto& buffer : buffers) { buffer.resize(size + 1); }
    if (ret) { controls[2] = 1; }
    else {
        control(discipline::MainPort::level) = std::sqrt(0.5f);
        control(discipline::MainPort::size) = 50;
        control(discipline::MainPort::size_beats) = 1;
        control(discipline::MainPort::bpm) = 120;
        control(discipline::MainPort::enabled) = 1;
        control(discipline::MainPort::memory_dry) = 1;
        control(discipline::MainPort::pair) = 1;
        control(discipline::MainPort::modify) = 0.5f;
        // Outside, so the Effect between Send and Return is in the loop.
        // Tests of the built-in echo set another Modifier.
        control(discipline::MainPort::modifier) = 5;
    }
    slice(0, 0); activate();
}
Instance::~Instance() { descriptor->deactivate(handle); descriptor->cleanup(handle); }
void Instance::activate() { descriptor->activate(handle); }
void Instance::slice(uint32_t offset, uint32_t frames) {
    for (uint32_t p = 0; p < (is_return ? 4U : static_cast<uint32_t>(discipline::MainPort::count)); ++p) {
        const bool audio = is_return ? p < 2 : (p < 6 || (p >= 28 && p <= 33) || p == 37);
        descriptor->connect_port(handle, p, audio ? buffers[p].data() + offset : &controls[p]);
    }
    if (measure) {
        const auto start = std::chrono::steady_clock::now();
        descriptor->run(handle, frames);
        last_time = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    } else { descriptor->run(handle, frames); }
}
Host::Host(const Library& lib, double sample_rate, uint32_t size, bool reverse, bool nominal)
    : main(std::make_unique<Instance>(lib.main, sample_rate, size, false, nominal)),
      ret(std::make_unique<Instance>(lib.ret, sample_rate, size, true, nominal)), last_send(size),
      rate(sample_rate), block(size), return_first(reverse) {}
double Host::run_slices(Instance& instance) {
    uint32_t offset = 0;
    double duration = 0;
    instance.measure = measure;
    while (offset < block) {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        const uint32_t count = random_slices ? std::min(1 + rng_ % 97, block - offset) : block;
        instance.slice(offset, count); offset += count; duration += instance.last_time;
    }
    return duration;
}
void Host::period(const Source& source, const Effect& effect) {
    for (uint32_t i = 0; i < block; ++i) {
        const auto f = source(frame + i); main->buffers[0][i] = f.l; main->buffers[1][i] = f.r;
    }
    double duration = 0;
    const auto run_return_side = [&] {
        for (uint32_t i = 0; i < block; ++i) {
            const Frame f = effect(last_send[i], frame + i);
            ret->buffers[0][i] = f.l; ret->buffers[1][i] = f.r;
        }
        if (run_return) { duration += run_slices(*ret); }
    };
    if (return_first) { run_return_side(); }
    duration += run_slices(*main);
    for (uint32_t i = 0; i < block; ++i) { last_send[i] = {main->buffers[4][i], main->buffers[5][i]}; }
    if (!return_first) { run_return_side(); }
    if (measure) { run_times.push_back(duration); }
    frame += block;
}
std::vector<Frame> Host::render(uint64_t frames, const Source& source, const Effect& effect) {
    std::vector<Frame> output; output.reserve(frames);
    while (output.size() < frames) {
        period(source, effect);
        for (uint32_t i = 0; i < block && output.size() < frames; ++i) {
            output.push_back({main->buffers[2][i], main->buffers[3][i]});
        }
    }
    return output;
}
} // namespace test
