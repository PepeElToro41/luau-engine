#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render_graph/render_graph.hpp"
#include "engine/utils/hash.hpp"

// The jump-flood outline as a piece of render graph: the chain of passes
// that turns "these objects" into an outline drawn over the frame, built
// once here so the editor's selection highlight and a game's own outline
// are the same code with different settings (render/engine/outline.slang
// is the shader side of the same split).
//
//     Outline outline;
//     OutlineDesc desc;
//     desc.mask = &EditorHighlight::draw_mask;   // or null: a DRAW_SCENE mask
//     desc.mask_user_data = this;
//     OUTLINE::init(renderer, outline, desc);
//     ...
//     outline.width = 4.0f;                       // live, no recompile
//     OUTLINE::update(renderer, outline, before); // once a frame, before render
//
// What init() adds to the graph, in order: the mask pass, `flood_count`
// flood passes ping-ponging between two RGBA32F seed buffers, and one or
// two resolve passes that draw the band over the backbuffer. update()
// pushes the live settings into the passes' constants, points the resolve
// passes at the depth the forward pass leaves (which it keeps stored, the
// way the editor's grid does) and keeps the whole block together, right
// before `before` or last.
//
// The mask is the only part that differs between uses. Left null it is a
// DRAW_SCENE pass tagged `outline_mask`, so a game marks an object by
// giving its shader that pass; given a callback it is a raster CUSTOM pass
// with the seed buffer attached, and the callback draws whatever it wants
// through a shader that has the pass (OUTLINE::draw_mask does one entity),
// which is how the editor outlines a selection whose objects keep their
// own materials. `behind` adds the second resolve pass, the fainter
// outline where something occludes the object; a game that does not want
// outlines through walls leaves it off.

// Up to a 4096-pixel first step.
static constexpr u32 OUTLINE_MAX_FLOOD_PASSES = 13;

namespace OUTLINE {

// The `#pragma pass` tag of the mask (render/engine/outline.slang), both
// as the DRAW_SCENE tag and as what a custom mask's pipeline asks for.
static constexpr const char* MASK_TAG_NAME = "outline_mask";
static constexpr u64 MASK_TAG = HASH::fnv1a_str("outline_mask");
// The engine's screen-space outline shader, loaded by name.
static constexpr const char* SHADER_NAME = "outline";

} // namespace OUTLINE

struct OutlineDesc {
    // Prefix of every pass and resource name the chain adds, so two
    // outlines can live in one graph.
    const char* name = "outline";
    // Null for a DRAW_SCENE mask over the `outline_mask` tag; otherwise
    // the callback that draws the mask, in a CUSTOM pass of its own.
    CustomPassFn mask = nullptr;
    void* mask_user_data = nullptr;
    // The widest outline this chain can draw, in pixels: rounded up to a
    // power of two it is the first flood step, which fixes how many flood
    // passes there are. `Outline::width` may be anything up to it.
    u32 max_width = 8;
    // Add the resolve pass for the occluded side of the object.
    bool behind = true;
};

struct Outline {
    // --- Live settings, read by update() every frame -----------------------

    bool enabled = true;
    // rgb the color, a the peak opacity, of the visible side.
    f32 color[4] = {1.0f, 0.55f, 0.1f, 1.0f};
    // The occluded side; normally the same color, much fainter.
    f32 behind_color[4] = {1.0f, 0.55f, 0.1f, 0.3f};
    // Band half-width in pixels, at most `max_width`, and how much of its
    // outer edge is feathered.
    f32 width = 3.0f;
    f32 softness = 1.2f;

    // --- What init() made --------------------------------------------------

    // render/outline.slang; 0 when it did not compile, and the passes then
    // draw nothing until a reload succeeds.
    EntityId shader = 0;
    RenderResourceHandle seeds[2];
    RenderResourceHandle mask_depth;
    RenderPassHandle mask;
    RenderPassHandle flood[OUTLINE_MAX_FLOOD_PASSES];
    u32 flood_count = 0;
    RenderPassHandle resolve;
    // Invalid when OutlineDesc::behind was false.
    RenderPassHandle resolve_behind;
    // The first flood step, the power of two OutlineDesc::max_width rounded
    // up to.
    u32 max_width = 8;
    // The CUSTOM mask pass's callback name, empty for a DRAW_SCENE mask.
    char mask_callback[RENDER_GRAPH::NAME_MAX] = {};
};

namespace OUTLINE {

// Adds the chain to `renderer.graph` and loads the outline shader. False
// (reported through LOG) when a name was taken or the shader did not
// compile; whatever was added is in `outline` either way, so shutdown()
// always undoes it.
bool init(Renderer& renderer, Outline& outline, const OutlineDesc& desc);
// Removes every pass and resource the chain added, and the mask callback.
// Call while the renderer is alive (the editor's pre_shutdown).
void shutdown(Renderer& renderer, Outline& outline);
// Once per frame before the engine renders.  `before` is the pass the
// block must stay in front of (the editor's grid, which keeps itself
// last); an invalid handle puts it last. Every edit compares first, so a
// steady graph is never marked dirty.
void update(Renderer& renderer, Outline& outline, RenderPassHandle before = RenderPassHandle{});
void set_enabled(Renderer& renderer, Outline& outline, bool enabled);

// Draws `entity`'s MeshRenderer or PrimitiveRenderer through `shader`'s
// `outline_mask` pass: what a custom mask callback does for each object it
// outlines. False when the entity has no transform, no geometry, or the
// shader has no pipeline for the pass.
bool draw_mask(Renderer& renderer, const RenderPassContext& ctx, EntityId shader, EntityId entity);

} // namespace OUTLINE
