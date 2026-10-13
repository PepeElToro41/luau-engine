#include "plugins/editor_camera_plugin.hpp"

#include "engine/ecs/world.hpp"
#include "engine/engine.hpp"
#include "engine/plugins/core_plugin.hpp"
#include "engine/plugins/render_plugin.hpp"
#include "engine/plugins/window_plugin.hpp"
#include "engine/render/renderer.hpp"
#include "engine/utils/log.hpp"

bool EditorCameraPlugin::build(Engine& engine) {
    if (engine.require<WindowPlugin>() == nullptr || engine.require<CorePlugin>() == nullptr || engine.require<RenderPlugin>() == nullptr) {
        return false;
    }
    World* world = engine.get_singleton<World>();
    Renderer* renderer = engine.get_singleton<Renderer>();
    if (!this->controller.init(*world)) {
        LOG::error(LOG_CATEGORY::EDITOR, "Editor camera could not be created");
        return false;
    }
    // The Viewport is always drawn from the fly camera, whichever scene
    // camera carries the RenderCamera tag for the standalone.
    renderer->camera = this->controller.entity;
    engine.add_system(engine.phases.events, EditorCameraPlugin::NAME, &EditorCameraPlugin::handle_events, this);
    engine.add_system(engine.phases.update, EditorCameraPlugin::NAME, &EditorCameraPlugin::update, this);
    return true;
}

void EditorCameraPlugin::handle_events(SystemContext& ctx) {
    EditorCameraPlugin* self = static_cast<EditorCameraPlugin*>(ctx.user_data);
    const WindowPlugin* window = ENGINE::of(ctx).get_plugin<WindowPlugin>();
    for (const SDL_Event& event : window->events) {
        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            self->controller.add_wheel(event.wheel.y);
        }
    }
}

void EditorCameraPlugin::update(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    EditorCameraPlugin* self = static_cast<EditorCameraPlugin*>(ctx.user_data);
    WindowPlugin* window = engine.get_plugin<WindowPlugin>();
    World* world = engine.get_singleton<World>();
    if (window == nullptr || world == nullptr) {
        return;
    }
    self->controller.update(*world, window->window.window, self->viewport_hovered, ctx.dt);
}
