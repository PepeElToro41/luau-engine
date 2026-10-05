#pragma once

#include "engine/defines.hpp"

#include <cstdio>

// Small text helpers shared by the panels.
namespace UI {

// Writes `bytes` as "1.2 MB" style text into `out`.
inline void format_size(const u64 bytes, char* out, const usz out_size) {
    static constexpr const char* UNITS[] = {"B", "KB", "MB", "GB", "TB"};
    f64 value = static_cast<f64>(bytes);
    usz unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(UNITS) / sizeof(UNITS[0])) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        snprintf(out, out_size, "%llu %s", static_cast<unsigned long long>(bytes), UNITS[unit]);
    } else {
        snprintf(out, out_size, "%.1f %s", value, UNITS[unit]);
    }
}

} // namespace UI
