#pragma once

#include "engine/plugin.hpp"
#include "engine/scheduler/scheduler.hpp"

#include <filesystem>

// The startup scene: in the startup phase (after ProjectPlugin's scan)
// loads `scene_path` (relative to the project, scenes/demo.scene by
// default) through Engine::load_asset_file + load_scene, and when the file
// is missing or does not load spawns the built-in demo (RENDER_DEMO::spawn)
// if `spawn_demo_when_missing`. In post_startup, if `tag_render_camera`,
// makes sure a scene camera carries the RenderCamera tag: the one the file
// tagged, else the first Transform + Camera under scene_root (what the
// standalone draws from; an editor clears the flag and renders from its
// own camera instead). Requires ProjectPlugin.
struct ScenePlugin : Plugin {
    static constexpr const char* NAME = "scene";

    ScenePlugin() = default;
    explicit ScenePlugin(std::filesystem::path scene_path) : scene_path(std::move(scene_path)) {}

    bool build(Engine& engine) override;

    std::filesystem::path scene_path = "scenes/demo.scene";
    bool spawn_demo_when_missing = true;
    bool tag_render_camera = true;
    // Whether the startup system loaded the file (false when the demo was
    // spawned or nothing could be).
    bool loaded = false;

private:
    static void load(SystemContext& ctx);
    static void tag_camera(SystemContext& ctx);
};

namespace SCENE_PLUGIN {

// Tags a scene camera RenderCamera if none is: the first Transform + Camera
// under scene_root. False (logged) when the scene has no camera at all.
bool tag_scene_camera(Engine& engine);

} // namespace SCENE_PLUGIN
