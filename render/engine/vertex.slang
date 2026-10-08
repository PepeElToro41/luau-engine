// Vertex input locations: the VERTEX_LAYOUT convention of mesh_asset.hpp.
// Slang numbers inputs in declaration order, so every mesh attribute names
// its location explicitly; put the matching macro in front of the field.
// Declare only what the shader reads: the pipeline refuses a mesh that
// lacks an attribute the shader declares.
//
//     struct VertexInput {
//         VERTEX_POSITION  float3 position;
//         VERTEX_TEXCOORD0 float2 texcoord0;
//     };
#ifndef ENGINE_VERTEX_SLANG
#define ENGINE_VERTEX_SLANG

#define VERTEX_POSITION  [[vk::location(0)]]
#define VERTEX_NORMAL    [[vk::location(1)]]
#define VERTEX_TANGENT   [[vk::location(2)]]
#define VERTEX_COLOR     [[vk::location(3)]]
#define VERTEX_TEXCOORD0 [[vk::location(4)]]
#define VERTEX_TEXCOORD1 [[vk::location(5)]]
#define VERTEX_TEXCOORD2 [[vk::location(6)]]
#define VERTEX_TEXCOORD3 [[vk::location(7)]]
#define VERTEX_JOINTS    [[vk::location(8)]]
#define VERTEX_WEIGHTS   [[vk::location(9)]]

#endif
