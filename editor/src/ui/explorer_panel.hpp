#pragma once

#include "engine/defines.hpp"

#include <imgui.h>

// The scene tree. There is no world to list yet, so it shows the root and
// records which node is selected; entity rows plug in once the engine owns a
// World.
struct ExplorerPanel {
    // Submits the window. `open` is the View-menu toggle.
    void draw(bool* open);

    ImGuiTextFilter filter;
    // 0 means nothing selected; the root is 1 for now.
    u64 selected = 0;
};
