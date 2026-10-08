#pragma pass fullscreen vertex fragment

// Copies the pass's first input to the target: the simplest post pass, and
// the template for every other one. The vertex function comes from the
// fullscreen include.

#include "engine/fullscreen.slang"
#include "engine/bindings.slang"

PASS_INPUT(0) Sampler2D input0;

[shader("fragment")]
float4 fragment(FullscreenVertex v) : SV_Target {
    return input0.Sample(v.uv);
}
