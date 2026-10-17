// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#include "discipline.hpp"

#include <cmath>
#include <new>

namespace discipline {
namespace {

class DisciplineReturn {
public:
    std::array<float*, static_cast<uint32_t>(ReturnPort::count)> ports{};
    PairClaim pair{true};
    ReturnWriter writer;
    uint64_t last_main = 0, stale = 0;
    const double rate;
    const HostConfig host;
    DisciplineReturn(double sample_rate, HostConfig config) noexcept : rate(sample_rate), host(config) {}

    // Real-time section: one bounded transport write per frame, no allocation.
    void run(uint32_t frames) noexcept
    {
        pair.update(ports[static_cast<uint32_t>(ReturnPort::pair)]);
        writer.write(pair.slot(), ports[0], ports[1], frames, host.block);
        const uint64_t main = pair.slot() ? pair.slot()->main_frames.load(std::memory_order_acquire) : 0;
        stale = main == last_main ? stale + frames : 0;
        last_main = main;
        if (auto* paired = ports[static_cast<uint32_t>(ReturnPort::paired)]) {
            *paired = pair.paired() && stale < static_cast<uint64_t>(rate * 0.5) ? 1.0f : 0.0f;
        }
    }
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const* features)
{
    if (!std::isfinite(rate) || rate < 1000 || rate > 384000) { return nullptr; }
    try { prepare_pair_registry(); return new (std::nothrow) DisciplineReturn(rate, host_config(features)); }
    catch (...) { return nullptr; }
}
void connect(LV2_Handle h, uint32_t index, void* data)
{
    auto& ports = static_cast<DisciplineReturn*>(h)->ports;
    if (index < ports.size()) { ports[index] = static_cast<float*>(data); }
}
void activate(LV2_Handle h) {
    auto* instance = static_cast<DisciplineReturn*>(h);
    instance->pair.release(); instance->writer.reset(); instance->last_main = instance->stale = 0;
}
void run(LV2_Handle h, uint32_t frames) { static_cast<DisciplineReturn*>(h)->run(frames); }
void deactivate(LV2_Handle h) { static_cast<DisciplineReturn*>(h)->pair.release(); }
void cleanup(LV2_Handle h) { delete static_cast<DisciplineReturn*>(h); }

} // namespace

const LV2_Descriptor return_descriptor{
    kReturnURI, instantiate, connect, activate, run, deactivate, cleanup, nullptr
};

} // namespace discipline
