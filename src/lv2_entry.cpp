// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#include "discipline.hpp"

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    switch (index) {
    case 0: return &discipline::main_descriptor;
    case 1: return &discipline::return_descriptor;
    default: return nullptr;
    }
}
