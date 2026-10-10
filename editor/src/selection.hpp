#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"

#include <filesystem>

// What the editor has selected, shared by the Explorer, the Asset Browser
// and the Inspector: one thing at a time, an entity of the World or a file
// of the project. Clicking in either panel replaces it, and the other panel
// drops its highlight. The App owns the one instance and hands it to the
// panels.
//
// A file selection can also carry an entity: the one the file is loaded
// as in the World (a .material is a Material entity once MATERIAL::load
// has seen it; the App resolves that after the browser's click). The
// Inspector then shows the file's facts under its name and the entity's
// components below, so a material's values are edited where every other
// component is, while Save and Reload keep the path to act on.
enum struct SelectionKind : u8 {
    NONE,
    ENTITY,
    FILE,
};

struct Selection {
    SelectionKind kind = SelectionKind::NONE;
    // ENTITY: the entity. FILE: the entity the file is loaded as, 0 when it
    // has none (a texture, a folder, a material that did not load).
    EntityId entity = 0;
    // FILE: the path relative to the project root.
    std::filesystem::path file;

    void select_entity(const EntityId id) {
        this->kind = id != 0 ? SelectionKind::ENTITY : SelectionKind::NONE;
        this->entity = id;
        this->file.clear();
    }
    void select_file(const std::filesystem::path& path, const EntityId loaded_as = 0) {
        this->kind = SelectionKind::FILE;
        this->entity = loaded_as;
        this->file = path;
    }
    void clear() {
        this->kind = SelectionKind::NONE;
        this->entity = 0;
        this->file.clear();
    }

    bool is_empty() const { return this->kind == SelectionKind::NONE; }
    bool is_entity() const { return this->kind == SelectionKind::ENTITY; }
    bool is_file() const { return this->kind == SelectionKind::FILE; }
    bool is_file(const std::filesystem::path& path) const { return this->is_file() && this->file == path; }
};
