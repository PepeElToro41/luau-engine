#pragma once

#include "engine/plugin.hpp"
#include "engine/scheduler/scheduler.hpp"

// The Renderer as a plugin: requires CorePlugin and Engine::gpu, creates
// the Renderer singleton (RENDERER::init with the engine's render/
// directory) and registers the render components with their serializers
// and removed hooks (RENDER_COMPONENTS::register_all).
//
// Systems:
//   render / "render"           pushes the renderer's clear color and clock
//                               and draws the graph into the frame's target
//                               (ENGINE::frame(ctx)), leaving target_state
//   pre_shutdown / "render"     deletes every Shader and Material entity, so
//                               their removed hooks hand programs, pipelines
//                               and blocks back while the renderer is alive,
//                               whichever plugin frees first afterwards
//   shutdown / "render"         RENDERER::shutdown
struct RenderPlugin : Plugin {
    static constexpr const char* NAME = "render";

    bool build(Engine& engine) override;

private:
    static void render(SystemContext& ctx);
    static void detach(SystemContext& ctx);
    static void shutdown(SystemContext& ctx);
};
