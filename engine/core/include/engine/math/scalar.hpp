#pragma once

#include "engine/defines.hpp"

#include <cmath>

// Scalar constants and helpers shared by the vector, quaternion and matrix
// types. Angles are always radians.

namespace MATH {

constexpr f32 PI = 3.14159265358979323846f;
constexpr f32 TAU = 2.0f * PI;
constexpr f32 HALF_PI = 0.5f * PI;
// Default tolerance for approx_equal on unit-scale values.
constexpr f32 EPSILON = 1e-5f;

constexpr f32 radians(const f32 degrees) { return degrees * (PI / 180.0f); }
constexpr f32 degrees(const f32 radians) { return radians * (180.0f / PI); }

template <typename T>
constexpr T min(const T a, const T b) { return a < b ? a : b; }
template <typename T>
constexpr T max(const T a, const T b) { return a > b ? a : b; }
template <typename T>
constexpr T clamp(const T value, const T low, const T high) { return value < low ? low : (value > high ? high : value); }

constexpr f32 lerp(const f32 a, const f32 b, const f32 t) { return a + (b - a) * t; }
constexpr f32 saturate(const f32 value) { return clamp(value, 0.0f, 1.0f); }

inline bool approx_equal(const f32 a, const f32 b, const f32 epsilon = EPSILON) {
    return std::fabs(a - b) <= epsilon;
}

} // namespace MATH
