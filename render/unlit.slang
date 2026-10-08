#pragma pass forward vertex fragment
#pragma pass shadow vertex

// Flat color, no lighting: the simplest material. Set 2 is the material
// interface; its block members are what Renderer::set_* address by name.
// The shadow pass compiles only `vertex`: a depth-only pipeline.

#include "engine/frame.slang"
#include "engine/object.slang"
#include "engine/vertex.slang"
#include "engine/bindings.slang"

struct MaterialParams {
    float4 color;
};
MATERIAL(0) ConstantBuffer<MaterialParams> material;

struct VertexInput {
    VERTEX_POSITION float3 position;
};

struct VertexOutput {
    float4 clip_position : SV_Position;
};

[shader("vertex")]
VertexOutput vertex(VertexInput v) {
    VertexOutput o;
    o.clip_position = mul(frame.view_projection, mul(object.model, float4(v.position, 1.0)));
    return o;
}

[shader("fragment")]
float4 fragment(VertexOutput v) : SV_Target {
    return material.color;
}
