#include "engine/engine.h"

#include "engine/asset/asset_reader.hpp"
#include "engine/memory/heap_allocator.hpp"

#include <cstdio>
#include <cstring>

#ifndef ENGINE_RENDER_DIR
#define ENGINE_RENDER_DIR "render"
#endif

bool Engine::init(GpuDevice* gpu) {
    this->gpu = gpu;
    this->time = 0.0;
    AssetResourceProvider* assets = this->create_singleton<AssetResourceProvider>();
    if (assets == nullptr) {
        return false;
    }
    GpuResourceManager* resources = this->create_singleton<GpuResourceManager>();
    if (resources == nullptr || !resources->init(gpu)) {
        return false;
    }
    World* world = this->create_singleton<World>();
    if (world == nullptr) {
        return false;
    }
    world->init();
    Renderer* renderer = this->create_singleton<Renderer>();
    if (renderer == nullptr || !renderer->init(gpu, resources, world, assets, ENGINE_RENDER_DIR)) {
        return false;
    }
    memcpy(renderer->clear_color, this->clear_color, sizeof(this->clear_color));
    RENDER_COMPONENTS::register_all(*world, *renderer);
    return true;
}

void Engine::shutdown() {
    // Singletons that own memory release it before the store destroys them.
    // The world first: its Shader and Material entities hand their GPU
    // objects back to the renderer through the removed hooks; then the
    // renderer, then the resource manager that defers destruction for both.
    if (World* world = this->get_singleton<World>()) {
        world->free();
    }
    if (Renderer* renderer = this->get_singleton<Renderer>()) {
        renderer->shutdown();
    }
    if (GpuResourceManager* resources = this->get_singleton<GpuResourceManager>()) {
        resources->shutdown();
    }
    if (AssetResourceProvider* assets = this->get_singleton<AssetResourceProvider>()) {
        assets->free();
    }
    this->singletons.free();
    this->gpu = nullptr;
}

void Engine::update(const f32 dt) {
    this->time += dt;
    this->last_dt = dt;
}

AssetGuid Engine::load_asset_file(const char* path) {
    AssetResourceProvider* assets = this->get_singleton<AssetResourceProvider>();
    if (assets == nullptr || path == nullptr) {
        return AssetGuid{};
    }
    AssetView view = ASSET_FILE::read_prelude(path, MEMORY::heap_allocator());
    if (!view.is_ok()) {
        fprintf(stderr, "[assets] %s: %s\n", path, ASSET_FILE::parse_error_name(view.parse_error));
        ASSET_FILE::free_prelude(&view, MEMORY::heap_allocator());
        return AssetGuid{};
    }
    const AssetGuid guid = view.header->guid;
    if (assets->add(view, path) == nullptr) {
        fprintf(stderr, "[assets] %s: could not register (cooked-only provider, or duplicate GUID)\n", path);
        ASSET_FILE::free_prelude(&view, MEMORY::heap_allocator());
        return AssetGuid{};
    }
    ASSET_FILE::free_prelude(&view, MEMORY::heap_allocator());
    return guid;
}

void Engine::render(const FrameContext& frame) {
    // frame.slot's fence was waited on by FrameScheduler::begin, so whatever
    // was released when this slot was last current can go now.
    GpuResourceManager* resources = this->get_singleton<GpuResourceManager>();
    if (resources != nullptr) {
        resources->begin_frame(frame.slot);
    }
    Renderer* renderer = this->get_singleton<Renderer>();
    if (renderer == nullptr) {
        return;
    }
    memcpy(renderer->clear_color, this->clear_color, sizeof(this->clear_color));
    renderer->graph.set_clear_color(renderer->default_forward, 0, this->clear_color);
    renderer->time = static_cast<f32>(this->time);
    renderer->delta_time = this->last_dt;
    renderer->render(frame);
}
