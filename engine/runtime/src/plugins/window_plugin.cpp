#include "engine/plugins/window_plugin.hpp"
#include "engine/utils/log.hpp"

#include "engine/engine.hpp"
#include "engine/gpu/gpu.hpp"

#include <cstdio>

bool WindowPlugin::build(Engine& engine) {
    if (!DISPLAY_WINDOW::sdl_initialize()) {
        return false;
    }
    this->sdl_ready = true;
    if (!this->window.initialize(this->desc.title, this->desc.width, this->desc.height)) {
        DISPLAY_WINDOW::sdl_shutdown();
        this->sdl_ready = false;
        return false;
    }
    GpuInitDesc gpu_desc;
    gpu_desc.window = this->window.window;
    gpu_desc.swapchain_format = this->desc.swapchain_format;
    engine.gpu = GPU::init(gpu_desc);
    if (engine.gpu == nullptr) {
        LOG::error(LOG_CATEGORY::WINDOW, "GPU init failed");
        this->window.shutdown();
        DISPLAY_WINDOW::sdl_shutdown();
        this->sdl_ready = false;
        return false;
    }
    engine.runner = &WindowPlugin::run;

    engine.add_system(engine.phases.events, WindowPlugin::NAME, &WindowPlugin::handle_events, this);
    // Releases begin in pre_shutdown; the GPU is made idle one phase earlier.
    this->gpu_idle = engine.scheduler.add_phase(engine.pipelines.shutdown, WindowPlugin::GPU_IDLE_PHASE);
    engine.scheduler.phase_before(this->gpu_idle, engine.phases.pre_shutdown);
    engine.add_system(this->gpu_idle, WindowPlugin::NAME, &WindowPlugin::wait_idle, this);
    engine.add_system(engine.phases.post_shutdown, WindowPlugin::NAME, &WindowPlugin::shutdown, this);
    return true;
}

// --- Frame ---------------------------------------------------------------------------

void WindowPlugin::poll() {
    this->events.clear();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        this->events.push(event);
    }
}

void WindowPlugin::run(Engine& engine) {
    WindowPlugin* self = engine.get_plugin<WindowPlugin>();
    if (self == nullptr || engine.gpu == nullptr) {
        return;
    }
    u64 last = SDL_GetTicksNS();
    while (!engine.quit_requested) {
        const u64 now = SDL_GetTicksNS();
        const f32 dt = static_cast<f32>(now - last) / 1e9f;
        last = now;

        self->poll();
        engine.update(dt);

        GpuFrame frame;
        if (!GPU::begin_frame(engine.gpu, frame)) {
            continue;
        }
        FrameContext ctx;
        ctx.frame = frame;
        ctx.target = frame.backbuffer;
        ctx.target_state = GPU_STATE_UNDEFINED;
        engine.render(ctx);
        GPU::end_frame(engine.gpu, frame);
    }
    GPU::wait_idle(engine.gpu);
}

void WindowPlugin::handle_events(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    WindowPlugin* self = static_cast<WindowPlugin*>(ctx.user_data);
    for (const SDL_Event& event : self->events) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (self->quit_on_close) {
                engine.quit();
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (engine.gpu != nullptr) {
                GPU::mark_resized(engine.gpu);
            }
            break;
        default:
            break;
        }
    }
}

// --- Teardown ------------------------------------------------------------------------

void WindowPlugin::wait_idle(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    if (engine.gpu != nullptr) {
        GPU::wait_idle(engine.gpu);
    }
}

void WindowPlugin::shutdown(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    WindowPlugin* self = static_cast<WindowPlugin*>(ctx.user_data);
    if (engine.gpu != nullptr) {
        GPU::shutdown(engine.gpu);
        engine.gpu = nullptr;
    }
    engine.runner = nullptr;
    self->window.shutdown();
    self->events.free();
    if (self->sdl_ready) {
        DISPLAY_WINDOW::sdl_shutdown();
        self->sdl_ready = false;
    }
}
