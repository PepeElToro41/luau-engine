#include "app.hpp"

#include "engine/ecs/world.hpp"
#include "engine/render/components.hpp"
#include "engine/render/demo_scene.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/scene.hpp"

#include <cstdio>

// Draws the frame from the scene's first camera: the standalone has no
// camera of its own, so the one under scene_root gets the RenderCamera tag.
static bool tag_scene_camera(Engine& engine) {
    World* world = engine.get_singleton<World>();
    Scene* scene = engine.get_singleton<Scene>();
    if (world == nullptr || scene == nullptr) {
        return false;
    }
    EntityId camera = 0;
    world->query<Transform, Camera>().each([&](const EntityId entity, Transform&, Camera&) {
        if (camera == 0 && scene->contains(entity)) {
            camera = entity;
        }
    });
    if (camera == 0) {
        return false;
    }
    world->add<RenderCamera>(camera);
    return true;
}

bool App::init() {
    if (!DISPLAY_WINDOW::sdl_initialize()) {
        return false;
    }
    if (!this->window.initialize(this->title, this->width, this->height)) {
        return false;
    }
    GpuInitDesc gpu_desc;
    gpu_desc.window = this->window.window;
    this->gpu = GPU::init(gpu_desc);
    if (this->gpu == nullptr) {
        fprintf(stderr, "[app] GPU init failed\n");
        return false;
    }
    if (!this->engine.init(this->gpu)) {
        return false;
    }
    // A shipped build only ever loads cooked assets (see docs/asset_format.md).
    this->engine.get_singleton<AssetResourceProvider>()->require_cooked = true;
    if (!RENDER_DEMO::spawn(this->engine)) {
        fprintf(stderr, "[app] demo scene could not be created\n");
    }
    if (!tag_scene_camera(this->engine)) {
        fprintf(stderr, "[app] the scene has no camera to render from\n");
    }
    if (this->demo_post) {
        RENDER_DEMO::set_post_enabled(this->engine, true);
    }
    if (this->demo_shadow) {
        RENDER_DEMO::set_shadow_enabled(this->engine, true);
    }
    this->running = true;
    return true;
}

void App::run() {
    u64 last = SDL_GetTicksNS();
    while (this->running) {
        const u64 now = SDL_GetTicksNS();
        const f32 dt = static_cast<f32>(now - last) / 1e9f;
        last = now;

        this->poll_events();
        this->frame(dt);
    }
}

void App::shutdown() {
    this->running = false;
    if (this->gpu != nullptr) {
        GPU::wait_idle(this->gpu);
    }
    this->engine.shutdown();
    if (this->gpu != nullptr) {
        GPU::shutdown(this->gpu);
        this->gpu = nullptr;
    }
    this->window.shutdown();
    DISPLAY_WINDOW::sdl_shutdown();
}

void App::poll_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            this->running = false;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            GPU::mark_resized(this->gpu);
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.repeat) {
                break;
            }
            if (event.key.key == SDLK_F5) {
                SHADER_LIBRARY::reload_all(*this->engine.get_singleton<Renderer>());
            } else if (event.key.key == SDLK_F6) {
                RENDER_DEMO::set_post_enabled(this->engine, !RENDER_DEMO::is_post_enabled(this->engine));
            } else if (event.key.key == SDLK_F7) {
                RENDER_DEMO::set_shadow_enabled(this->engine, !RENDER_DEMO::is_shadow_enabled(this->engine));
            }
            break;
        default:
            break;
        }
    }
}

void App::frame(const f32 dt) {
    this->engine.update(dt);

    GpuFrame frame;
    if (!GPU::begin_frame(this->gpu, frame)) {
        return;
    }
    FrameContext ctx;
    ctx.frame = frame;
    ctx.target = frame.backbuffer;
    ctx.target_state = GPU_STATE_UNDEFINED;
    this->engine.render(ctx);
    GPU::end_frame(this->gpu, frame);
}
