#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/gpu/descriptor.hpp"
#include "engine/gpu/render_target.hpp"
#include "engine/gpu/resource_manager.hpp"
#include "engine/gpu/shaders/program.hpp"
#include "engine/math/math.hpp"

// The ECS components the renderer reads and the ones it manages. All are
// plain data the World copies by value, so none holds a container: what a
// Shader or Material owns is released by the removed hooks the Renderer
// installs (RENDER_COMPONENTS::register_all).
//
// Scene side, set by the app or scripts:
//
//     Transform     where an entity is (world = local for now)
//     Camera        with a Transform, the view the frame is drawn from
//     MeshRenderer  a mesh asset drawn with a material entity
//
// Renderer side, created through Renderer::load_shader / create_material:
//
//     Shader        a loaded shader file: compiled passes and reflection
//     Material      a shader entity plus the values of its set-2 interface

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

struct MeshRenderer {
    AssetGuid mesh;
    // A Material entity; 0 draws nothing. One material covers every submesh
    // for now.
    EntityId material = 0;
};

struct Shader {
    char name[64] = {};
    ShaderProgram program;
    // Bumped by Renderer::reload_shader; materials compare it to theirs to
    // know their descriptor sets need rebuilding.
    u32 generation = 0;
};

static constexpr u32 MATERIAL_MAX_TEXTURES = 8;

struct MaterialTextureSlot {
    // Binding in set 2.
    u32 binding = 0;
    // Null draws the renderer's white texture.
    AssetGuid texture;
    SamplerDesc sampler;
};

struct Material {
    EntityId shader = 0;
    u32 shader_generation = 0;
    // CPU copy of the material block (std140, laid out by the shader's
    // reflection). Heap memory of the Renderer's allocator.
    u8* params = nullptr;
    u32 param_size = 0;
    MaterialTextureSlot textures[MATERIAL_MAX_TEXTURES] = {};
    u32 texture_count = 0;
    // Host-visible, FRAMES_IN_FLIGHT regions of `slot_stride` bytes; region
    // `slot` is what sets[slot] points at.
    GpuBuffer uniform;
    u32 slot_stride = 0;
    DescriptorSetHandle sets[FRAMES_IN_FLIGHT] = {};
    // Bit per frame slot: that slot's region and set are stale.
    u32 dirty_mask = 0;
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

// Registers every component above with `world` and installs the removed
// hooks that hand Shader and Material resources back to `renderer`.
void register_all(World& world, Renderer& renderer);

} // namespace RENDER_COMPONENTS
