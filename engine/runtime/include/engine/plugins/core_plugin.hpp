#pragma once

#include "engine/plugin.hpp"
#include "engine/scheduler/scheduler.hpp"

// The engine's own state, as a plugin so the order it is created and torn
// down in is the plugins' order: the AssetResourceProvider, the ECS World,
// the Scene (its "scene_root"), the AssetEntities index and the
// FrameAllocator, each an engine singleton (see engine.hpp). Every other
// plugin that touches the world requires this one.
//
// Teardown is one system in the "shutdown" phase: the index first (it
// owns nothing in the world and would otherwise be kept in step entity by
// entity), then the World (its removed hooks run, so a plugin whose hooks
// need its own state deletes those entities in pre_shutdown), then the
// provider and the frame allocator.
struct CorePlugin : Plugin {
    static constexpr const char* NAME = "core";

    // `require_cooked` makes the provider refuse uncooked assets, as a
    // shipped build would (docs/asset_format.md).
    CorePlugin() = default;
    explicit CorePlugin(const bool require_cooked) : require_cooked(require_cooked) {}

    bool build(Engine& engine) override;

    bool require_cooked = false;

private:
    static void shutdown(SystemContext& ctx);
};
