#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render_graph/render_graph.hpp"

// The Viewport's ground grid: the y = 0 plane drawn by
// render/internal/grid_editor.slang in a graph pass of its own.
//
// The pass is a RENDER_PASS_CUSTOM named "alpha", with the backbuffer as its
// color attachment (loaded, not cleared) and the forward pass's depth as its
// depth attachment (loaded), so the grid blends over the finished frame and
// is occluded by the scene. It is kept last in the graph's order, after any
// pass other code adds (the demo's post-process pass included), which is the
// slot transparent objects will take later: a scene shader with a
// `#pragma pass alpha ... blend no_depth_write` variant could be drawn by a
// DRAW_SCENE pass tagged "alpha" in that same position, through the same
// pipeline cache. The grid shader itself declares its blend and depth state
// on its pragma line, so the callback only binds the pipeline PIPELINES::get
// hands back and draws six vertices.
//
// The forward pass's depth attachment is stored (the default graph discards
// it) so the grid has something to test against; update() re-applies that
// and follows the forward pass's depth resource each frame, because other
// graph edits (the demo toggles) set it back.
struct EditorGrid {
    // Adds the pass, registers the callback and compiles the shader. False
    // (reported in the renderer log) when the shader did not load; the pass
    // is still there and draws nothing until a reload succeeds.
    bool init(Renderer& renderer);
    // Removes the pass and the callback. Call before Engine::shutdown.
    void shutdown(Renderer& renderer);
    // Once per frame before the engine renders: keeps the pass last and its
    // depth attachment on the forward pass's depth, which is kept stored.
    void update(Renderer& renderer);
    void set_enabled(Renderer& renderer, bool enabled);

    bool enabled = true;
    EntityId shader = 0;
    RenderPassHandle pass;

private:
    static void draw(const RenderPassContext& ctx, void* user_data);

    Renderer* renderer = nullptr;
};
