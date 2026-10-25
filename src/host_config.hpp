// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace discipline {
struct HostConfig {
    uint32_t block = 128;
    uint32_t maximum = 128;
    bool has_map = false;
};

// Instantiate only. Return can use these options when a map is offered, but
// otherwise gets the main's block length through its claimed slot.
inline HostConfig host_config(const LV2_Feature* const* features) noexcept
{
    HostConfig config;
    const LV2_URID_Map* map = nullptr;
    const LV2_Options_Option* options = nullptr;
    for (auto f = features; f && *f; ++f) {
        if (!(*f)->URI) { continue; }
        if (std::strcmp((*f)->URI, LV2_URID__map) == 0) { map = static_cast<const LV2_URID_Map*>((*f)->data); }
        if (std::strcmp((*f)->URI, LV2_OPTIONS__options) == 0) { options = static_cast<const LV2_Options_Option*>((*f)->data); }
    }
    if (!map || !map->map) { return config; }
    config.has_map = true;
    if (!options) { return config; }
    const auto nominal_key = map->map(map->handle, LV2_BUF_SIZE__nominalBlockLength);
    const auto maximum_key = map->map(map->handle, LV2_BUF_SIZE__maxBlockLength);
    const auto int_type = map->map(map->handle, LV2_ATOM__Int);
    uint32_t nominal = 0, maximum = 0;
    for (auto* o = options; o->key; ++o) {
        if (o->context != LV2_OPTIONS_INSTANCE || o->subject || o->type != int_type ||
            o->size != sizeof(int32_t) || !o->value) { continue; }
        int32_t value;
        std::memcpy(&value, o->value, sizeof(value));
        if (value <= 0 || value > (1 << 20)) { continue; }
        if (o->key == nominal_key) { nominal = static_cast<uint32_t>(value); }
        if (o->key == maximum_key) { maximum = static_cast<uint32_t>(value); }
    }
    config.block = nominal ? nominal : (maximum ? maximum : 128);
    config.maximum = std::max(config.block, maximum);
    return config;
}
} // namespace discipline
