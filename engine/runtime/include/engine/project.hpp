#pragma once

#include <filesystem>
#include <string>

// The project the engine is working on: the directory that holds its assets,
// scripts and settings. It is an engine singleton, so there is at most one and
// everything that needs the project asks the engine for it:
//
//     Project& project = engine.get_singleton<Project>();
//     if (project.open("/path/to/game")) {
//         std::filesystem::path assets = project.root / "assets";
//     }
//
// Nothing is open until open() succeeds; a default-constructed Project is
// closed. The type owns no resources, so there is nothing to release before
// the engine frees its singletons.
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
