// Set 0: what the renderer provides every frame. Binding 0 is the frame
// block (FrameUniforms in render/components.hpp); bindings 1..4 are the
// engine's sampler table, so most shaders never declare a sampler of their
// own. Every shader may include this; a shader's set 0 must be a subset of
// it. Matrices are the engine's column-major Matrix4x4 (the Slang backend
// compiles with column-major layout) and `mul(matrix, vector)` applies them
// to a column vector, exactly GLSL's `m * v`. Clip space is Vulkan's (see
// math/matrix4x4.hpp).
#ifndef ENGINE_FRAME_SLANG
#define ENGINE_FRAME_SLANG

struct FrameUniforms {
    float4x4 view;
    float4x4 projection;
    float4x4 view_projection;
    float4 camera_position; // xyz world position, w unused
    float4 time;            // x seconds since start, y last frame's dt, zw unused
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameUniforms> frame;

// Trilinear, repeat.
[[vk::binding(1, 0)]] SamplerState sampler_linear;
// Trilinear, clamp to edge: pass inputs and anything screen-sized.
[[vk::binding(2, 0)]] SamplerState sampler_linear_clamp;
// Point sampling, repeat.
[[vk::binding(3, 0)]] SamplerState sampler_nearest;
// Point sampling, clamp to edge.
[[vk::binding(4, 0)]] SamplerState sampler_nearest_clamp;

#endif
