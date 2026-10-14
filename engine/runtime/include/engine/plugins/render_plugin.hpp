#pragma once

#include "engine/plugin.hpp"
#include "engine/scheduler/scheduler.hpp"

// The renderer as a plugin: requires CorePlugin and Engine::gpu, creates
// the shared RenderResources (SCOPE_SHARED: the pipeline and GPU asset
// caches, set 0, the samplers, the engine's render/ directory) and the
// authoring world's Renderer (SCOPE_LOCAL: a play instance makes its own
// against the same resources), and registers the render components with
// their serializers, cloner and removed hooks (RENDER_COMPONENTS::register_all).
//
// Systems:
//   render / "render"           pushes the renderer's clear color and clock
//                               (the play clock while a PlayState exists)
//                               and draws the active Renderer's graph into
//                               the frame's target (ENGINE::frame(ctx)),
//                               leaving target_state
//   pre_shutdown / "render"     deletes every Shader and Material entity, so
//                               their removed hooks hand programs, pipelines
//                               and blocks back while the renderer is alive,
//                               whichever plugin frees first afterwards
//   shutdown / "render"         RENDERER::shutdown, then RENDER_RESOURCES::shutdown
struct RenderPlugin : Plugin {
    static constexpr const char* NAME = "render";

    bool build(Engine& engine) override;

private:
    static void render(SystemContext& ctx);
    static void detach(SystemContext& ctx);
    static void shutdown(SystemContext& ctx);
};
