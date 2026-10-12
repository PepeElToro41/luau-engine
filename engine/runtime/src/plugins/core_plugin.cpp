#include "engine/plugins/core_plugin.hpp"

#include "engine/engine.hpp"

bool CorePlugin::build(Engine& engine) {
    AssetResourceProvider* assets = engine.create_singleton<AssetResourceProvider>();
    if (assets == nullptr) {
        return false;
    }
    assets->require_cooked = this->require_cooked;
    World* world = engine.create_singleton<World>();
    if (world == nullptr) {
        return false;
    }
    world->init();
    Scene* scene = engine.create_singleton<Scene>();
    if (scene == nullptr) {
        return false;
    }
    scene->init(world);
    AssetEntities* asset_entities = engine.create_singleton<AssetEntities>();
    if (asset_entities == nullptr) {
        return false;
    }
    asset_entities->init(world);
    if (engine.create_singleton<FrameAllocator>(ENGINE_FRAME_ARENA_BYTES) == nullptr) {
        return false;
    }
    engine.add_system(engine.phases.shutdown, CorePlugin::NAME, &CorePlugin::shutdown, this);
    return true;
}

void CorePlugin::shutdown(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    if (AssetEntities* asset_entities = engine.get_singleton<AssetEntities>()) {
        asset_entities->free();
    }
    if (World* world = engine.get_singleton<World>()) {
        world->free();
    }
    if (AssetResourceProvider* assets = engine.get_singleton<AssetResourceProvider>()) {
        assets->free();
    }
    if (FrameAllocator* frame_allocator = engine.get_singleton<FrameAllocator>()) {
        frame_allocator->release();
    }
}
