#pragma once

#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"

struct Engine;

// A stand-in scene until projects describe their own: a camera, the unlit
// shader, one material and the three primitive shapes (a cube, a sphere
// and a cylinder, each a PrimitiveRenderer entity). Both apps spawn it at
// startup so there is something to look at while the real content pipeline
// is built.
namespace RENDER_DEMO {

// False if the shader could not be loaded or the mesh not uploaded.
bool spawn(Engine& engine);

// Routes the forward pass through a transient scene image and a fullscreen
// "post" pass (render/passthrough.slang) instead of straight into the
// backbuffer, or back again. Exercises the graph's transient resources and
// recompilation. False if the passthrough shader could not be loaded.
bool set_post_enabled(Engine& engine, bool enabled);
bool is_post_enabled(Engine& engine);

// Adds a depth-only "shadow" DRAW_SCENE pass into a transient shadow map
// that the forward pass then takes as an input, or removes it again.
// Exercises vertex-only pipelines and depth resources; nothing samples the
// map yet.
bool set_shadow_enabled(Engine& engine, bool enabled);
bool is_shadow_enabled(Engine& engine);

// Switches every demo entity to render/textured.slang with `texture` (a
// registered texture asset) as its albedo. False if the shader could not be loaded.
bool set_texture(Engine& engine, const AssetGuid& texture);

// Draws every demo entity with the .material asset `material` (registered
// through Engine::load_asset_file; MATERIAL::load). False if it did not load.
bool set_material(Engine& engine, const AssetGuid& material);

} // namespace RENDER_DEMO
