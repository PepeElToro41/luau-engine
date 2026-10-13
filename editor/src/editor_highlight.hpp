#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/render/outline.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render_graph/render_graph.hpp"

// The Viewport's selection highlight: the jump-flood outline of
// render/outline.hpp around whatever the editor has selected.
//
// Everything but the mask is the engine's (the same chain a game's outline
// builds, and the same shader), so this holds only what is the editor's:
// the mask is a custom pass whose callback draws the selected entity
// through render/editor/selection.slang, which exists because the selected
// object keeps its own material and so cannot carry the `outline_mask`
// pass itself. The chain is kept before the grid pass, so the grid's faint
// lines draw over the outline rather than the outline sitting under
// nothing.
//
// The occluded side is on: a selection hidden behind geometry still shows,
// fainter, which is the point of a highlight and the one thing a game's
// outline normally leaves off. With nothing selected the whole chain is
// disabled, so the flood costs nothing until something is picked.
struct EditorHighlight {
    // Adds the chain, registers the mask callback and compiles both
    // shaders. False (reported in the log) when a shader did not load; the
    // passes are there and draw nothing until a reload succeeds.
    bool init(Renderer& renderer);
    // Removes the chain and the callback. Call before Engine::shutdown.
    void shutdown(Renderer& renderer);
    // Once per frame before the engine renders: `selected` is the scene
    // entity to outline (0 for none) and `before` the pass the chain must
    // stay in front of (the grid's).
    void update(Renderer& renderer, EntityId selected, RenderPassHandle before);
    void set_enabled(Renderer& renderer, bool enabled);

    // The editor's own switch (Render > Selection Outline); the chain is
    // live only while it is on and something is selected.
    bool enabled = true;
    Outline outline;
    // render/editor/selection.slang, the mask's shader.
    EntityId shader = 0;
    EntityId selected = 0;

private:
    static void draw_mask(const RenderPassContext& ctx, void* user_data);

    Renderer* renderer = nullptr;
};
