#include "app.hpp"

#include "engine/asset/asset_view.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/ecs/world.hpp"
#include "engine/render/components.hpp"
#include "engine/render/demo_scene.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/scene.hpp"

#include <cctype>
#include <cstdio>

#ifndef STANDALONE_DEFAULT_PROJECT_DIR
#define STANDALONE_DEFAULT_PROJECT_DIR "."
#endif

// Draws the frame from the scene's camera: the one tagged RenderCamera by
// the scene file, else the first one under scene_root, which gets the tag.
static bool tag_scene_camera(Engine& engine) {
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
    // A shipped build only ever loads cooked assets (see docs/asset_format.md);
    // a development project's imports are not cooked yet, so that is opt-in.
    this->engine.get_singleton<AssetResourceProvider>()->require_cooked = this->cooked_only;
    if (this->project_dir.empty()) {
        this->project_dir = STANDALONE_DEFAULT_PROJECT_DIR;
    }
    this->scan_project();
    if (!this->load_scene() && !RENDER_DEMO::spawn(this->engine)) {
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

void App::scan_project() {
    std::error_code error;
    if (!std::filesystem::is_directory(this->project_dir, error) || error) {
        fprintf(stderr, "[app] project directory %s not found\n", this->project_dir.string().c_str());
        return;
    }
    const std::string root = this->project_dir.string();
    RENDERER::set_project_render_dir(*this->engine.get_singleton<Renderer>(), root.c_str());
    u32 registered = 0;
    for (std::filesystem::recursive_directory_iterator it(this->project_dir, error), end; it != end; it.increment(error)) {
        if (error || !it->is_regular_file(error)) {
            continue;
        }
        std::string extension = it->path().extension().string();
        for (char& c : extension) {
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
        if (extension != ASSET_FILE::EXTENSION && TEXT_ASSET::type_of_path(extension.c_str()) == 0) {
            continue;
        }
        const std::string path = it->path().string();
        if (!this->engine.load_asset_file(path.c_str()).is_null()) {
            registered += 1;
        }
    }
    printf("[app] project %s: %u asset files registered\n", root.c_str(), registered);
}

bool App::load_scene() {
    std::filesystem::path scene = this->scene_path;
    if (scene.is_relative()) {
        scene = this->project_dir / scene;
    }
    const std::string path = scene.string();
    std::error_code error;
    if (!std::filesystem::exists(scene, error) || error) {
        fprintf(stderr, "[app] scene %s not found\n", path.c_str());
        return false;
    }
    const AssetGuid guid = this->engine.load_asset_file(path.c_str());
    if (guid.is_null()) {
        fprintf(stderr, "[app] scene %s could not be registered\n", path.c_str());
        return false;
    }
    return this->engine.load_scene(guid);
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
