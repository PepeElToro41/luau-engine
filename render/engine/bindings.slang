// The descriptor set convention (shaders/program.hpp), as the binding
// attribute to put in front of a resource:
//
//     set 0  the frame block and the sampler table   engine/frame.slang declares them
//     set 1  the pass's inputs                        PASS_INPUT(i): the graph's input i, a Texture2D
//     set 2  the material                             MATERIAL(b): one uniform block, textures and
//                                                     samplers, written by MATERIAL::set_*
//     set 3  reserved
//
// Textures and samplers are separate (no Sampler2D): sample with one of the
// frame's samplers, or declare a SamplerState of your own in the material.
//
//     struct MaterialParams { float4 color; };
//     MATERIAL(0) ConstantBuffer<MaterialParams> material;
//     MATERIAL(1) Texture2D albedo;                       // albedo.Sample(sampler_linear, uv)
//     PASS_INPUT(0) Texture2D scene_color;                // scene_color.Sample(sampler_linear_clamp, uv)
#ifndef ENGINE_BINDINGS_SLANG
#define ENGINE_BINDINGS_SLANG

#define PASS_INPUT(i) [[vk::binding(i, 1)]]
#define MATERIAL(b) [[vk::binding(b, 2)]]

#endif
