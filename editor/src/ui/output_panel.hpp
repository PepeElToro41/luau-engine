#pragma once

#include "engine/defines.hpp"
#include "engine/utils/log.hpp"

#include <imgui.h>

#include <string>
#include <vector>

// The editor's log: a LOG sink (utils/log.hpp) showing everything the
// engine, the editor and scripts report, newest at the bottom, with a text
// filter, a per-category view filter and auto-scroll. Lines are kept until
// clear(). The UI plugin adds `sink` with LOG::add_sink and removes it in
// pre_shutdown; the panel never writes to the log itself.
struct OutputPanel {
    struct Line {
        LogLevel level;
        LogCategory category;
        std::string text;
        // The file and line the message is about, empty / 0 when none.
        std::string file;
        u32 line;
    };

    // The LogSinkFn; `user_data` is the panel.
    static void sink(const LogRecord& record, void* user_data);

    void add(const LogRecord& record);
    void clear();

    // Submits the window. `open` is the View-menu toggle.
    void draw(bool* open);

    std::vector<Line> lines;
    ImGuiTextFilter filter;
    bool auto_scroll = true;
    // Categories hidden from view (by id); the messages are still kept.
    std::vector<bool> hidden;

private:
    bool is_hidden(LogCategory category) const;
    void draw_category_menu();
    bool scroll_to_bottom = false;
};
