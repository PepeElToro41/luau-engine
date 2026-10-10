#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/geometry/primitives.hpp"
#include "engine/gpu/gpu_types.hpp"
#include "engine/math/math.hpp"
#include "engine/shaders/preprocessing.hpp"
#include "engine/shaders/program.hpp"

// The ECS components the renderer reads and the ones it manages. All are
// plain data the World copies by value, so none holds a container: what a
// Shader or Material owns is released by the removed hooks
// RENDER_COMPONENTS::register_all installs.
//
// Scene side, set by the app or scripts:
//
//     Transform     where an entity is (world = local for now)
//     Camera        with a Transform, a view the frame can be drawn from
//     RenderCamera  tag: the Camera the frame is drawn from. The app puts it
//                   on exactly one camera (the editor on its own fly camera,
//                   the standalone on the scene's); without it the renderer
//                   falls back to the first Camera it finds
//     MeshRenderer  a mesh asset drawn with a material entity
//     PrimitiveRenderer
//                   a built-in shape (cube, sphere, cylinder) drawn with a
//                   material entity; the shape is unit-sized, the
//                   Transform's scale gives it its size
//
// Renderer side, created through SHADER_LIBRARY::load / MATERIAL::create:
//
//     Shader        a loaded shader file: compiled passes, reflection and
//                   the bind layouts its passes use
//     Material      a shader entity plus the values of its set-2 interface,
//                   CPU data only: pushed to the GPU every frame it is drawn;
//                   loaded from a .material file (MATERIAL::load) or built
//                   in code (MATERIAL::create)

struct World;
struct Renderer;

struct Transform {
    Vector3 position;
    Quaternion rotation;
    Vector3 scale = Vector3::one();

    Matrix4x4 matrix() const { return Matrix4x4::trs(this->position, this->rotation, this->scale); }
};

struct Camera {
    // Vertical field of view, radians.
    f32 fov_y = MATH::radians(60.0f);
    f32 near_plane = 0.1f;
    f32 far_plane = 1000.0f;
};

// Marks the Camera entity the renderer draws the frame from (see above).
struct RenderCamera {};

struct MeshRenderer {
    AssetGuid mesh;
    // A Material entity; 0 draws nothing. One material covers every submesh
    // for now.
    EntityId material = 0;
};

// Draws one of the engine's primitive shapes (engine/geometry/primitives.hpp)
// the way a MeshRenderer draws an asset: same passes, same materials, same
// draw list. The geometry is generated and uploaded once per shape and
// shared by every entity using it, so a scene of a thousand cubes holds
// one vertex buffer. The shape is also what physics will read later to
// build a collider from the Transform's scale alone, with no triangles.
struct PrimitiveRenderer {
    PrimitiveShape shape = PRIMITIVE_CUBE;
    // A Material entity; 0 draws nothing.
    EntityId material = 0;
};

struct Shader {
    char name[64] = {};
    ShaderProgram program;
    // Bumped by SHADER_LIBRARY::reload; materials compare it to theirs to
    // know their parameter block needs rebuilding, pipelines are keyed by it.
    u32 generation = 0;
    // Set 2 of the material interface; invalid when the shader has none.
    GpuBindLayout material_layout;
    // Set 1 (pass inputs) per pass, same order as program.passes; invalid
    // for passes that read no input.
    GpuBindLayout input_layouts[SHADER_PREPROCESSING::MAX_PASSES] = {};
};

static constexpr u32 MATERIAL_MAX_TEXTURES = 8;
static constexpr u32 MATERIAL_MAX_SAMPLERS = 4;

struct MaterialTextureSlot {
    // Binding in set 2.
    u32 binding = 0;
    // Null draws the renderer's white texture.
    AssetGuid texture;
};

struct MaterialSamplerSlot {
    u32 binding = 0;
    GpuSamplerDesc sampler;
};

struct Material {
    EntityId shader = 0;
    u32 shader_generation = 0;
    // The .material asset this was loaded from (MATERIAL::load); null for
    // a material built in code. Renderer::materials maps it back.
    AssetGuid asset;
    // CPU copy of the material block (std140, laid out by the shader's
    // reflection). Heap memory, freed by the removed hook.
    u8* params = nullptr;
    u32 param_size = 0;
    MaterialTextureSlot textures[MATERIAL_MAX_TEXTURES] = {};
    u32 texture_count = 0;
    MaterialSamplerSlot samplers[MATERIAL_MAX_SAMPLERS] = {};
    u32 sampler_count = 0;
};

// Set 0, binding 0: what engine/frame.slang declares. std140: every member
// is 16-byte aligned already.
struct FrameUniforms {
    Matrix4x4 view;
    Matrix4x4 projection;
    Matrix4x4 view_projection;
    Vector4 camera_position;
    // x seconds since start, y last frame's dt.
    Vector4 time;
};

static_assert(sizeof(FrameUniforms) == 3 * 64 + 2 * 16, "FrameUniforms must match frame.slang");

namespace RENDER_COMPONENTS {

// Registers every component above with `world`, names the component
// entities for the editor, and installs the removed hooks that hand Shader
// and Material resources back to `renderer`.
void register_all(World& world, Renderer& renderer);

} // namespace RENDER_COMPONENTS
