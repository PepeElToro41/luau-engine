#pragma once

#include "engine/defines.hpp"
#include "project.hpp"
#include "selection.hpp"
#include "ui/output_panel.hpp"

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
// Clicking an entry makes it the editor's Selection (selection.hpp) as a
// file, relative to the project root; the Inspector shows it, and the App
// resolves a `.material` to its Material entity. A selection made
// elsewhere (the Explorer) drops the highlight here, and renaming or
// deleting the selected entry follows it. Double-clicking a file reports
// it in `activated`; the App opens source files (.obj, images) in the
// Import panel from there. Right-clicking an
// entry opens a context menu: Import (importable files only) does the same
// as a double-click; Delete removes the file (or the folder and its
// contents) from disk after a confirmation modal; Rename... (or F2 on the
// selected entry) asks for a new name in a modal; New Folder... asks for a
// name in a modal and creates it in the current folder. Right-clicking the
// empty space of the listing opens a menu with New Folder... and Refresh.
// No previews or drag and drop yet.
struct AssetBrowserPanel {
    struct Entry {
        std::string name;
        std::string extension; // lowercase, with the dot; empty for folders
        bool is_directory = false;
        u64 size = 0;          // bytes; 0 for folders
    };

    // Submits the window. `open` is the View-menu toggle. `project` is the
    // engine's Project singleton; null or closed shows an empty panel.
    // Deletions, renames, folder creations and their failures are reported
    // to `output`.
    void draw(bool* open, const Project* project, OutputPanel& output, Selection& selection);

    // Shows `folder`, relative to the project root (empty for the root
    // itself). Takes effect on the next draw().
    void navigate(const std::filesystem::path& folder);
    // Re-reads the current folder on the next draw().
    void refresh() { this->dirty = true; }

    // Folder shown, relative to the project root; empty is the root.
    std::filesystem::path current;
    // Contents of `current`, folders first, each group sorted by name.
    std::vector<Entry> entries;
    // Name of the highlighted entry in `current`; empty for none. Mirrors
    // the Selection while it points at a file of this folder.
    std::string selected;
    // File double-clicked during the last draw(), relative to the project
    // root; empty when none was. Valid until the next draw().
    std::filesystem::path activated;
    ImGuiTextFilter filter;
    bool show_hidden = false; // entries whose name starts with '.'

private:
    void rescan(const Project& project);
    void draw_breadcrumbs(const Project& project);
    void draw_entries(Selection& selection);
    void draw_delete_popup(const Project& project, OutputPanel& output, Selection& selection);
    void delete_entry(const Project& project, const Entry& entry, OutputPanel& output, Selection& selection);
    // The one name modal, used by New Folder... and Rename...
    enum struct NameMode { NEW_FOLDER, RENAME };
    void begin_new_folder();
    void begin_rename(const Entry& entry);
    void draw_name_popup(const Project& project, OutputPanel& output, Selection& selection);
    bool create_folder(const Project& project, const char* name, OutputPanel& output, Selection& selection);
    bool rename_entry(const Project& project, const Entry& entry, const char* name, OutputPanel& output, Selection& selection);
    // Why `name` cannot be used in `current`, or null when it can. `except`
    // is a listing name that does not count as taken (the entry being
    // renamed).
    const char* name_problem(const char* name, const std::string& except) const;

    bool dirty = true;
    // Entry whose Delete is awaiting confirmation; name is empty when none.
    // A copy, since a rescan may replace `entries` while the modal is up.
    Entry deleting;
    // The name modal: what it does, the name being typed and, for a rename,
    // a copy of the entry it renames (a rescan may replace `entries`).
    NameMode name_mode = NameMode::NEW_FOLDER;
    char name_buffer[128] = {};
    Entry renaming;
    // Root the entries were read from, so a different project (or a reopened
    // one) triggers a rescan and resets the folder.
    std::filesystem::path scanned_root;
    bool scanned_hidden = false;
};
