#pragma once

#include "engine/defines.hpp"

#include <imgui.h>

#include <string>
#include <vector>

// The editor's log: everything the engine and editor report, newest at the
// bottom, with a text filter and auto-scroll. Lines are kept until clear().
struct OutputPanel {
    enum struct Level : u8 { INFO, WARNING, ERROR };

    struct Line {
        Level level;
        std::string text;
    };

    void info(const char* format, ...);
    void warning(const char* format, ...);
    void error(const char* format, ...);
    void clear();

    // Submits the window. `open` is the View-menu toggle.
    void draw(bool* open);

    std::vector<Line> lines;
    ImGuiTextFilter filter;
    bool auto_scroll = true;

private:
    void add(Level level, const char* format, va_list args);
    bool scroll_to_bottom = false;
};
