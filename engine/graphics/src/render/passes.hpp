#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/gpu/gpu_types.hpp"

// Private to src/render/: what the graph executor runs inside each pass.

struct Renderer;
struct RenderPassContext;
struct Material;
struct Shader;

namespace RENDER_PASSES {

// DRAW_SCENE: every entity with a Transform and a MeshRenderer or a
// PrimitiveRenderer whose material's shader has a pass for the tag,
// collected, sorted by pipeline and material, then recorded.
void draw_scene(Renderer& renderer, const RenderPassContext& ctx);
// FULLSCREEN: the pass's shader entity over the target.
void fullscreen(Renderer& renderer, const RenderPassContext& ctx);
// CUSTOM: the registered callback.
void custom(Renderer& renderer, const RenderPassContext& ctx);

// The material's set-2 group for this frame, made on first use.
GpuBindGroup material_group(Renderer& renderer, EntityId entity, Material& material, const Shader& shader);
// A set-1 group with the pass's inputs for the shader's pass, or invalid
// when it declares none.
GpuBindGroup input_group(Renderer& renderer, const RenderPassContext& ctx, const Shader& shader, u32 pass_index);

} // namespace RENDER_PASSES
