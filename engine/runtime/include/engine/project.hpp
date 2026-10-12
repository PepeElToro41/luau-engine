#pragma once

#include "engine/defines.hpp"

#include <filesystem>
#include <string>

struct Engine;

// The open project: the directory that holds a game's assets, scripts and
// settings. Both apps open one (the standalone's `--project`, the editor's
// test project until it has an open-project page) and register it as an
// engine singleton, so every panel and plugin reaches the same object:
//
//     PROJECT::open(engine, "/path/to/game");      // creates the singleton, opens it
//     PROJECT::scan(engine);                        // registers its assets and shaders
//     engine.get_singleton<Project>()->root / "assets"
//
// Nothing is open until open() succeeds; a default-constructed Project is
// closed. The type owns no resources, so there is nothing to release before
// the engine frees its singletons. ProjectPlugin does the two calls above
// from build and pre_startup.
struct Project {
    // Points the project at `root`, which must be an existing directory. The
    // path is made absolute and normalized, and `name` becomes the directory's
    // name unless one is given. Returns false, leaving the project as it was,
    // if `root` does not exist or is not a directory.
    bool open(const std::filesystem::path& root);
    bool open(const std::filesystem::path& root, std::string name);
    // Forgets the current project. Safe to call when nothing is open.
    void close();

    bool is_open() const { return !this->root.empty(); }

    // Absolute, normalized directory of the project, without a trailing
    // separator. Empty while closed.
    std::filesystem::path root;
    // Display name of the project. Empty while closed.
    std::string name;
};

struct ProjectScanResult {
    u32 assets = 0;
    u32 shaders = 0;
};

namespace PROJECT {

// Creates the Project singleton if there is none, opens `root` and makes
// the project the renderer's project render directory (the shader include
// root and where a shader name falls back to). False (logged) if `root` is
// not a directory; the project is then left closed.
bool open(Engine& engine, const std::filesystem::path& root);

// Registers every asset file under the open project with the engine
// (.lunaasset containers and the text assets: .material, .scene), so
// references by GUID resolve, and every shader file as a shader asset
// entity: the engine's render/ first, then anywhere under the project (a
// project file of an engine file's name shares its GUID and so its entity,
// and its path replaces the engine's, which is the override; two project
// files of one stem keep the first met and warn). Registering a file twice
// only refreshes it, so this is also the editor's Refresh Assets. Counts
// and problems go through the engine log. False if no project is open.
bool scan(Engine& engine, ProjectScanResult* result = nullptr);

} // namespace PROJECT
