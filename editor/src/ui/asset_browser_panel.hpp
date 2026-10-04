#pragma once

#include "engine/defines.hpp"
#include "project.hpp"

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

// The project's files: one folder of the open Project at a time, with a
// breadcrumb path to climb back up, a name filter and a Name / Type / Size
// table. Double-clicking a folder enters it. The listing is read from disk
// when the folder changes, the project changes or Refresh is pressed, not
// every frame.
//
// It only lists for now: no previews, no drag and drop, no import. Those
// plug in once the engine has asset types to show.
struct AssetBrowserPanel {
    struct Entry {
        std::string name;
        std::string extension; // lowercase, with the dot; empty for folders
        bool is_directory = false;
        u64 size = 0;          // bytes; 0 for folders
    };

    // Submits the window. `open` is the View-menu toggle. `project` is the
    // engine's Project singleton; null or closed shows an empty panel.
    void draw(bool* open, const Project* project);

    // Shows `folder`, relative to the project root (empty for the root
    // itself). Takes effect on the next draw().
    void navigate(const std::filesystem::path& folder);
    // Re-reads the current folder on the next draw().
    void refresh() { this->dirty = true; }

    // Folder shown, relative to the project root; empty is the root.
    std::filesystem::path current;
    // Contents of `current`, folders first, each group sorted by name.
    std::vector<Entry> entries;
    // Name of the selected entry in `current`; empty for none.
    std::string selected;
    ImGuiTextFilter filter;
    bool show_hidden = false; // entries whose name starts with '.'

private:
    void rescan(const Project& project);
    void draw_breadcrumbs(const Project& project);
    void draw_entries();

    bool dirty = true;
    // Root the entries were read from, so a different project (or a reopened
    // one) triggers a rescan and resets the folder.
    std::filesystem::path scanned_root;
    bool scanned_hidden = false;
};
