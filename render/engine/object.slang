// Per-object data, pushed before every draw by the DRAW_SCENE pass.
#ifndef ENGINE_OBJECT_SLANG
#define ENGINE_OBJECT_SLANG

struct ObjectConstants {
    float4x4 model;
};
[[vk::push_constant]] ConstantBuffer<ObjectConstants> object;

#endif
