#pragma once

// The math library in one include.
//
//   scalar.hpp      PI, radians()/degrees(), clamp/lerp/approx_equal on floats
//   simd.hpp        SIMD::f32x4, the register layer (SSE2 or a scalar fallback)
//   vector2.hpp     Vector2, scalar, 8 bytes
//   vector3.hpp     Vector3, SIMD, 16 bytes with a padding lane; load()/store() for 12-byte data
//   vector4.hpp     Vector4, SIMD
//   quaternion.hpp  Quaternion, a Vector4 (x, y, z, w) with w the scalar part
//   matrix4x4.hpp   Matrix4x4, four Vector4 columns, GLSL layout, Vulkan clip space
//
// Types are global like the rest of the engine; binary helpers (dot, cross,
// lerp, min, max, approx_equal, slerp, ...) are free functions in MATH::.
// Conventions shared by everything: right-handed, +Y up, forward -Z, radians,
// column vectors so `a * b` applies `b` first.

#include "engine/math/matrix4x4.hpp"
#include "engine/math/quaternion.hpp"
#include "engine/math/scalar.hpp"
#include "engine/math/simd.hpp"
#include "engine/math/vector2.hpp"
#include "engine/math/vector3.hpp"
#include "engine/math/vector4.hpp"
