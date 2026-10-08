// The descriptor set convention (gpu/shaders/program.hpp), as the binding
// attribute to put in front of a resource:
//
//     set 0  the frame block            engine/frame.slang declares it
//     set 1  the pass's inputs          PASS_INPUT(i): the graph's input i
//     set 2  the material               MATERIAL(b): one uniform block plus
//                                       samplers, written by Renderer::set_*
//     set 3  reserved
//
//     struct MaterialParams { float4 color; };
//     MATERIAL(0) ConstantBuffer<MaterialParams> material;
//     MATERIAL(1) Sampler2D albedo;
//     PASS_INPUT(0) Sampler2D scene_color;
#ifndef ENGINE_BINDINGS_SLANG
#define ENGINE_BINDINGS_SLANG

#define PASS_INPUT(i) [[vk::binding(i, 1)]]
#define MATERIAL(b) [[vk::binding(b, 2)]]

#endif
