#include "engine/plugins/demo_plugin.hpp"

#include "engine/engine.hpp"
#include "engine/play.hpp"
#include "engine/plugins/render_plugin.hpp"
#include "engine/plugins/window_plugin.hpp"
#include "engine/render/demo_scene.hpp"
#include "engine/render/shader_library.hpp"
#include "engine/utils/log.hpp"

bool DemoPlugin::build(Engine& engine) {
    if (engine.require<RenderPlugin>() == nullptr) {
        return false;
    }
    engine.add_system(engine.phases.post_startup, DemoPlugin::NAME, &DemoPlugin::apply, this);
    engine.add_system(engine.phases.events, DemoPlugin::NAME, &DemoPlugin::handle_keys, this);
    return true;
}

bool DemoPlugin::set_post(Engine& engine, const bool enabled) {
    if (!RENDER_DEMO::set_post_enabled(engine, enabled)) {
        return false;
    }
    this->post = enabled;
    return true;
}

bool DemoPlugin::set_shadow(Engine& engine, const bool enabled) {
    if (!RENDER_DEMO::set_shadow_enabled(engine, enabled)) {
        return false;
    }
    this->shadow = enabled;
    return true;
}

void DemoPlugin::reload_shaders(Engine& engine) {
    // A play instance borrows the authoring world's programs; reloading
    // them there would free them from under it.
    const PlayState* play = engine.get_singleton<PlayState>();
    if (play != nullptr && play->isolated) {
        LOG::warning(LOG_CATEGORY::ENGINE, "Stop playing to reload shaders");
        return;
    }
    if (Renderer* renderer = engine.get_singleton<Renderer>()) {
        SHADER_LIBRARY::reload_all(*renderer);
    }
}

void DemoPlugin::apply(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    DemoPlugin* self = static_cast<DemoPlugin*>(ctx.user_data);
    if (self->post && !RENDER_DEMO::set_post_enabled(engine, true)) {
        self->post = false;
    }
    if (self->shadow && !RENDER_DEMO::set_shadow_enabled(engine, true)) {
        self->shadow = false;
    }
}

void DemoPlugin::handle_keys(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    DemoPlugin* self = static_cast<DemoPlugin*>(ctx.user_data);
    const WindowPlugin* window = engine.get_plugin<WindowPlugin>();
    if (window == nullptr) {
        return;
    }
    for (const SDL_Event& event : window->events) {
        if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
            continue;
        }
        if (event.key.key == SDLK_F5) {
            self->reload_shaders(engine);
        } else if (event.key.key == SDLK_F6) {
            self->set_post(engine, !self->post);
        } else if (event.key.key == SDLK_F7) {
            self->set_shadow(engine, !self->shadow);
        }
    }
}
