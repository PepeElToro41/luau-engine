#pragma once

#include "engine/defines.hpp"

namespace MATH {

// Smallest power of two that is >= `value`. 0 and 1 both map to 1.
constexpr u64 next_power_of_two(u64 value) {
    if (value <= 1) {
        return 1;
    }
    value -= 1;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    value |= value >> 32;
    return value + 1;
}

constexpr bool is_power_of_two(const u64 value) {
    return value != 0 && (value & (value - 1)) == 0;
}

} // namespace MATH
