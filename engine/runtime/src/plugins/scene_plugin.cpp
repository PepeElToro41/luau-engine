#include "engine/plugins/scene_plugin.hpp"

#include "engine/ecs/world.hpp"
#include "engine/engine.hpp"
#include "engine/plugins/project_plugin.hpp"
#include "engine/project.hpp"
#include "engine/render/components.hpp"
#include "engine/render/demo_scene.hpp"
#include "engine/scene/scene.hpp"
#include "engine/utils/log.hpp"

bool ScenePlugin::build(Engine& engine) {
    if (engine.require<ProjectPlugin>() == nullptr) {
        return false;
    }
    engine.add_system(engine.phases.startup, ScenePlugin::NAME, &ScenePlugin::load, this);
    engine.add_system(engine.phases.post_startup, ScenePlugin::NAME, &ScenePlugin::tag_camera, this);
    return true;
}

void ScenePlugin::load(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    ScenePlugin* self = static_cast<ScenePlugin*>(ctx.user_data);
    self->loaded = false;

    const Project* project = engine.get_singleton<Project>();
    std::filesystem::path scene = self->scene_path;
    if (scene.is_relative() && project != nullptr && project->is_open()) {
        scene = project->root / scene;
    }
    const std::string path = scene.string();
    std::error_code error;
    if (!std::filesystem::exists(scene, error) || error) {
        LOG::warning(LOG_CATEGORY::ENGINE, "No startup scene at %s%s", path.c_str(), self->spawn_demo_when_missing ? "; spawning the built-in demo" : "");
    } else {
        const AssetGuid guid = engine.load_asset_file(path.c_str());
        if (guid.is_null()) {
            LOG::error(LOG_CATEGORY::ENGINE, "Scene %s could not be registered", path.c_str());
        } else if (engine.load_scene(guid)) {
            self->loaded = true;
            return;
        }
    }
    if (self->spawn_demo_when_missing && !RENDER_DEMO::spawn(engine)) {
        LOG::error(LOG_CATEGORY::ENGINE, "Demo scene could not be created");
    }
}

void ScenePlugin::tag_camera(SystemContext& ctx) {
    const ScenePlugin* self = static_cast<ScenePlugin*>(ctx.user_data);
    if (self->tag_render_camera) {
        SCENE_PLUGIN::tag_scene_camera(ENGINE::of(ctx));
    }
}

bool SCENE_PLUGIN::tag_scene_camera(Engine& engine) {
    World* world = engine.get_singleton<World>();
    Scene* scene = engine.get_singleton<Scene>();
    if (world == nullptr || scene == nullptr) {
        return false;
    }
    EntityId camera = 0;
    world->query<Transform, Camera>().with<RenderCamera>().each([&](const EntityId entity, Transform&, Camera&) {
        if (camera == 0 && scene->contains(entity)) {
            camera = entity;
        }
    });
    if (camera != 0) {
        return true;
    }
    world->query<Transform, Camera>().each([&](const EntityId entity, Transform&, Camera&) {
        if (camera == 0 && scene->contains(entity)) {
            camera = entity;
        }
    });
    if (camera == 0) {
        LOG::warning(LOG_CATEGORY::ENGINE, "The scene has no camera to render from");
        return false;
    }
    world->add<RenderCamera>(camera);
    return true;
}
