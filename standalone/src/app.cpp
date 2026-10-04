#include "app.hpp"

#include <cstdio>

bool App::init() {
    if (!DISPLAY_WINDOW::sdl_initialize()) {
        return false;
    }
    if (!this->window.initialize(this->title, this->width, this->height)) {
        return false;
    }
    if (!this->gpu.init(this->window.window)) {
        return false;
    }
    if (!this->presenter.init(&this->gpu, this->window.window)) {
        fprintf(stderr, "[app] presenter init failed\n");
        return false;
    }
    if (!this->engine.init(&this->gpu)) {
        return false;
    }
    // A shipped build only ever loads cooked assets (see docs/asset_format.md).
    this->engine.get_singleton<AssetResourceProvider>()->require_cooked = true;
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
    this->gpu.wait_idle();
    this->engine.shutdown();
    this->presenter.shutdown();
    this->gpu.shutdown();
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
            this->presenter.mark_resized();
            break;
        default:
            break;
        }
    }
}

void App::frame(const f32 dt) {
    this->engine.update(dt);

    FrameContext frame;
    if (!this->presenter.begin(frame)) {
        return;
    }
    this->engine.render(frame);
    this->presenter.end();
}
