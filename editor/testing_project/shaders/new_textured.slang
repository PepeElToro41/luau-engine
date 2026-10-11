#pragma pass forward vertex fragment
#pragma pass shadow vertex

// Albedo texture times a tint. `tint` starts at zero like every material
// value, so set it (MATERIAL::set_vec4) or the mesh draws black. The
// texture is sampled with the frame's trilinear repeat sampler.

#include "engine/frame.slang"
#include "engine/object.slang"
#include "engine/vertex.slang"
#include "engine/bindings.slang"

struct MaterialParams {
    float4 tint;
};

MATERIAL(0) ConstantBuffer<MaterialParams> material;
MATERIAL(1) Texture2D albedo;

struct VertexInput {
    VERTEX_POSITION  float3 position;
    VERTEX_TEXCOORD0 float2 texcoord0;
};

struct VertexOutput {
    float4 clip_position : SV_Position;
    float2 uv;
};

[shader("vertex")]
VertexOutput vertex(VertexInput v) {
    VertexOutput o;
    o.clip_position = mul(frame.view_projection, mul(object.model, float4(v.position, 1.0)));
    o.uv = v.texcoord0;
    return o;
}

[shader("fragment")]
float4 fragment(VertexOutput v) : SV_Target {
    return albedo.Sample(sampler_linear, v.uv) * material.tint;
}
