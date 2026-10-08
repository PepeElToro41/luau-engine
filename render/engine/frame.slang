// Set 0: what the Renderer writes once per frame (FrameUniforms in
// render/components.hpp). Every shader may include this; a shader's set 0
// must be a subset of it. Matrices are the engine's column-major Matrix4x4
// (the Slang backend compiles with column-major layout) and
// `mul(matrix, vector)` applies them to a column vector, exactly GLSL's
// `m * v`. Clip space is Vulkan's (see math/matrix4x4.hpp).
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

#endif
