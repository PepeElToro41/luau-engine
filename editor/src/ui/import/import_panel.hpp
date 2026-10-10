#pragma once

#include "engine/asset/asset_types/mesh_asset.hpp"
#include "engine/asset/asset_types/texture_asset.hpp"
#include "engine/defines.hpp"
#include "import/importer.hpp"
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
// between files, so a batch of textures only needs them set once. "Keep
// original" (MeshImportOptions / TextureImportOptions::keep_source) stores
// the source bytes in the asset's SRC chunk, which is what a re-import
// reads later.
//
// Files get here from File > Import... (the OS picker) and from
// double-clicking an importable file in the Asset Browser. A re-import
// (open_reimport, from the Asset Browser's Reimport) queues the original
// kept inside a .lunaasset instead of a file: the window is titled
// Reimport, the header names the asset and its original, the destination
// starts as the asset itself, and importing over it keeps the asset's GUID
// so references stay valid (any other destination gets a fresh one, two
// files must not share a GUID). The panel opens itself when something is
// queued: `open` is set by open() and the window's close button clears
// the queue.
struct ImportPanel {
    enum struct Kind : u8 { UNSUPPORTED, MESH, TEXTURE };

    struct Item {
        ImportSource source;         // where the bytes come from (a file, or a range of an asset)
        std::string extension;       // of source.name; lowercase, with the dot
        Kind kind = Kind::UNSUPPORTED;
        u64 size = 0;                // bytes to import; 0 if it could not be read
        // For a re-import: the asset the original was read from, relative
        // to the project root. Empty for a plain file.
        std::filesystem::path asset;

        bool is_reimport() const { return !this->asset.empty(); }
    };

    // Queues `source` for import into `folder` (relative to the project
    // root; the Asset Browser's current folder) and sets `*open`. The folder
    // and the destination name are reset only when the queue was empty, so
    // a batch picked together lands in one place.
    void open(const std::filesystem::path& source, const std::filesystem::path& destination_folder, bool* open);

    // Queues the original kept in an asset (`source` from IMPORT::find_source,
    // `asset` the file it came from, relative to the project root) and sets
    // `*open`. When it comes to the front, the destination is set to the
    // asset itself.
    void open_reimport(const ImportSource& source, const std::filesystem::path& asset, bool* open);

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
    // The front item's own asset, absolute and normalized; empty for a plain file.
    std::filesystem::path reimport_target(const Project& project) const;
    // Whether importing would replace a file other than the asset being
    // re-imported in place.
    bool will_overwrite(const Project& project) const;
    bool import_front(const Project& project, OutputPanel& output);
    void draw_source(const Item& item);
    void draw_settings(const Item& item);
    void draw_keep_original(const Item& item, bool* keep_source);
    void draw_destination(const Project& project);
    bool draw_overwrite_popup(const Project& project, OutputPanel& output);
};
