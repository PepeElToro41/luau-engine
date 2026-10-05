#pragma once

#include "engine/asset/asset_types/mesh_asset.hpp"
#include "engine/asset/asset_types/texture_asset.hpp"
#include "engine/defines.hpp"
#include "project.hpp"
#include "ui/output_panel.hpp"

#include <filesystem>
#include <string>
#include <vector>

// The import dialog: the source files waiting to be imported, one shown at a
// time, with what the editor knows about the front one (path, kind, size),
// the import settings for its kind and where the .lunaasset goes. Import
// writes the front file through OBJ::import / IMAGE::import and moves on to
// the next; Skip drops it; Cancel empties the queue. Settings are kept
// between files, so a batch of textures only needs them set once.
//
// Files get here from File > Import... (the OS picker) and from
// double-clicking an importable file in the Asset Browser. The panel opens
// itself when something is queued: `open` is set by open() and the window's
// close button clears the queue.
struct ImportPanel {
    enum struct Kind : u8 { UNSUPPORTED, MESH, TEXTURE };

    struct Item {
        std::filesystem::path source; // absolute
        std::string extension;        // lowercase, with the dot
        Kind kind = Kind::UNSUPPORTED;
        u64 size = 0;                 // bytes; 0 if it could not be read
    };

    // Queues `source` for import into `folder` (relative to the project
    // root; the Asset Browser's current folder) and sets `*open`. The folder
    // and the destination name are reset only when the queue was empty, so
    // a batch picked together lands in one place.
    void open(const std::filesystem::path& source, const std::filesystem::path& destination_folder, bool* open);

    // Submits the window while something is queued. Returns true when a file
    // was written this frame, so the caller can refresh the Asset Browser.
    bool draw(bool* open, const Project* project, OutputPanel& output);

    bool has_items() const { return !this->queue.empty(); }
    void clear() { this->queue.clear(); }

    static Kind kind_of(const std::string& extension);
    static const char* kind_name(Kind kind);

    std::vector<Item> queue; // front is the one shown
    MeshImportOptions mesh_options;
    TextureImportOptions texture_options;
    char folder[512] = {}; // destination folder, relative to the project root
    char name[256] = {};   // destination file name without the extension

private:
    void show_front();
    void pop_front();
    std::filesystem::path destination(const Project& project) const;
    bool will_overwrite(const Project& project) const;
    bool import_front(const Project& project, OutputPanel& output);
    void draw_source(const Item& item);
    void draw_settings(const Item& item);
    void draw_destination(const Project& project);
    bool draw_overwrite_popup(const Project& project, OutputPanel& output);
};
