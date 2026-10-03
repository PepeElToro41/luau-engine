#pragma once

#include <SDL3/SDL.h>

#include <string>


namespace DISPLAY_WINDOW {

bool sdl_initialize();
void sdl_shutdown();

} // namespace DISPLAY_WINDOW

struct DisplayWindow {
    bool initialize(const std::string& title, int width, int height);
    void shutdown();

    SDL_Window* window = nullptr;
    std::string title;
};
