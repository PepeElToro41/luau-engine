#pragma pass fullscreen vertex fragment

// Copies the pass's first input to the target: the simplest post pass, and
// the template for every other one. The vertex function comes from the
// fullscreen include; the input is sampled with the frame's clamped
// sampler.

#include "engine/frame.slang"
#include "engine/fullscreen.slang"
#include "engine/bindings.slang"

PASS_INPUT(0) Texture2D input0;

[shader("fragment")]
float4 fragment(FullscreenVertex v) : SV_Target {
    return input0.Sample(sampler_linear_clamp, v.uv);
}
