// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alejandro Bustos
#pragma once

#include <cstdint>

namespace discipline {

inline constexpr char kMainURI[] = "https://github.com/ABustos2606/discipline#discipline";
inline constexpr char kReturnURI[] = "https://github.com/ABustos2606/discipline#return";

// Permanent port ABI. The kind comments are consumed by tests/ttl_check.py.
enum class MainPort : uint32_t {
    in_l = 0,        // audio_in
    in_r = 1,        // audio_in
    out_l = 2,       // audio_out
    out_r = 3,       // audio_out
    send_l = 4,      // audio_out
    send_r = 5,      // audio_out
    level = 6,       // control_in
    repeats = 7,     // control_in
    size = 8,        // control_in
    size_beats = 9,  // control_in
    sync = 10,       // control_in
    bpm = 11,        // control_in
    spread = 12,     // control_in
    scan = 13,       // control_in
    scan_mode = 14,  // control_in
    routing = 15,    // control_in
    collect = 16,    // control_in
    loop = 17,       // control_in
    latch = 18,      // control_in
    scan_hold = 19,  // control_in
    clear = 20,      // control_in
    memory_dry = 21, // control_in
    wipe = 22,       // control_in
    dry_kill = 23,   // control_in
    always = 24,     // control_in
    trails = 25,     // control_in
    pair = 26,       // control_in
    enabled = 27,    // control_in
    level_cv = 28,   // cv_in
    repeats_cv = 29, // cv_in
    size_cv = 30,    // cv_in
    spread_cv = 31,  // cv_in
    scan_cv = 32,    // cv_in
    clock = 33,      // cv_out
    paired = 34,     // control_out
    modifier = 35,   // control_in
    modify = 36,     // control_in
    modify_cv = 37,  // cv_in
    count = 38
};

enum class ReturnPort : uint32_t {
    in_l = 0,   // audio_in
    in_r = 1,   // audio_in
    pair = 2,   // control_in
    paired = 3, // control_out
    count = 4
};

} // namespace discipline
