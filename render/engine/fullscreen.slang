// The vertex stage of a fullscreen pass: one triangle covering the target
// from the vertex index, no vertex buffer. `uv` runs 0..1 across the
// target, top-left origin (Vulkan clip space). Include it from a shader
// whose fragment function does the work:
//
//     #pragma pass fullscreen vertex fragment
//     #include "engine/frame.slang"
//     #include "engine/fullscreen.slang"
//     #include "engine/bindings.slang"
//     PASS_INPUT(0) Texture2D input0;              // the pass's first input
//     [shader("fragment")]
//     float4 fragment(FullscreenVertex v) : SV_Target { return input0.Sample(sampler_linear_clamp, v.uv); }
#ifndef ENGINE_FULLSCREEN_SLANG
#define ENGINE_FULLSCREEN_SLANG

struct FullscreenVertex {
    float4 clip_position : SV_Position;
    float2 uv;
};

[shader("vertex")]
FullscreenVertex vertex(uint index : SV_VertexID) {
    // Vertices 0, 1, 2 at (-1,-1), (-1,3), (3,-1): counter-clockwise in
    // Vulkan's y-down framebuffer space, so the default back-face culling
    // keeps it.
    FullscreenVertex o;
    o.uv = float2(index & 2, (index << 1) & 2);
    o.clip_position = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

#endif
