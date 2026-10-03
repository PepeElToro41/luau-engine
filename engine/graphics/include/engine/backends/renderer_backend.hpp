#pragma once

#include <cstdint>

struct SDL_Window;

// Abstract interface every rendering backend implements. The engine talks to
// the active backend only through this type so the graphics API can be swapped
// without touching engine code.
struct RendererBackend {
    // Returns false if the backend could not be brought up. The window must
    // have been created with the flags the backend needs (e.g. SDL_WINDOW_VULKAN).
    virtual bool init(SDL_Window* window) = 0;
    virtual void shutdown() = 0;

    virtual void begin_frame() = 0;
    virtual void end_frame() = 0;

    virtual void resize(uint32_t width, uint32_t height) = 0;
};
