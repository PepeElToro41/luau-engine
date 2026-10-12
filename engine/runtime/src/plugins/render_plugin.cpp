#include "engine/plugins/render_plugin.hpp"

#include "engine/engine.hpp"
#include "engine/plugins/core_plugin.hpp"
#include "engine/render/components.hpp"
#include "engine/render/renderer.hpp"

#include <cstdio>

#ifndef ENGINE_RENDER_DIR
#define ENGINE_RENDER_DIR "render"
#endif

bool RenderPlugin::build(Engine& engine) {
    if (engine.require<CorePlugin>() == nullptr) {
        return false;
    }
    if (engine.gpu == nullptr) {
        fprintf(stderr, "[render] Engine::gpu is not set; the render plugin needs a GPU context\n");
        return false;
    }
    World* world = engine.get_singleton<World>();
    AssetResourceProvider* assets = engine.get_singleton<AssetResourceProvider>();
    FrameAllocator* frame_allocator = engine.get_singleton<FrameAllocator>();
    Renderer* renderer = engine.create_singleton<Renderer>();
    if (renderer == nullptr || !RENDERER::init(*renderer, engine.gpu, world, assets, frame_allocator, ENGINE_RENDER_DIR)) {
        return false;
    }
    RENDER_COMPONENTS::register_all(*world, *renderer);
    engine.add_system(engine.phases.render, RenderPlugin::NAME, &RenderPlugin::render, this);
    engine.add_system(engine.phases.pre_shutdown, RenderPlugin::NAME, &RenderPlugin::detach, this);
    engine.add_system(engine.phases.shutdown, RenderPlugin::NAME, &RenderPlugin::shutdown, this);
    return true;
}

void RenderPlugin::render(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    Renderer* renderer = engine.get_singleton<Renderer>();
    if (renderer == nullptr) {
        return;
    }
    FrameContext& frame = ENGINE::frame(ctx);
    renderer->graph.set_clear_color(renderer->default_forward, 0, renderer->clear_color);
    renderer->time = static_cast<f32>(engine.time);
    renderer->delta_time = engine.last_dt;
    frame.target_state = RENDERER::render(*renderer, frame);
}

void RenderPlugin::detach(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    World* world = engine.get_singleton<World>();
    if (world == nullptr) {
        return;
    }
    // Collected first: deleting moves entities out of the archetype being
    // iterated. Materials before shaders, the way they were created.
    DynamicArray<EntityId> entities;
    world->query<Material>().each([&](const EntityId entity, Material&) { entities.push(entity); });
    world->query<Shader>().each([&](const EntityId entity, Shader&) { entities.push(entity); });
    for (const EntityId entity : entities) {
        world->delete_entity(entity);
    }
    entities.free();
}

void RenderPlugin::shutdown(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    if (Renderer* renderer = engine.get_singleton<Renderer>()) {
        RENDERER::shutdown(*renderer);
    }
}
