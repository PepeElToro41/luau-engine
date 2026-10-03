#include "engine/display_window.hpp"

#include <cstdio>

namespace DISPLAY_WINDOW {
    bool sdl_initialize() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            fprintf(stderr, "[sdl] SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        return true;
    }

    void sdl_shutdown() {
        SDL_Quit();
    }
}

bool DisplayWindow::initialize(const std::string& title, const int width, const int height) {
    this->title = title;

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    this->window = SDL_CreateWindow(this->title.c_str(), width, height, flags);
    if (this->window == nullptr) {
        fprintf(stderr, "[sdl] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

void DisplayWindow::shutdown() {
    if (this->window != nullptr) {
        SDL_DestroyWindow(this->window);
        this->window = nullptr;
    }
}
