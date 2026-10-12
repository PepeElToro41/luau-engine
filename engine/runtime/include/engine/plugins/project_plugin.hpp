#pragma once

#include "engine/plugin.hpp"
#include "engine/scheduler/scheduler.hpp"

#include <filesystem>

// Opens a project directory (engine/project.hpp) and registers what is in
// it: build creates the Project singleton, opens `root` and makes it the
// renderer's project render directory; the pre_startup system runs
// PROJECT::scan, so by the startup phase every asset and shader file under
// the project is an asset entity and a scene can reference them. Requires
// CorePlugin and RenderPlugin. A root that is not a directory fails the
// build.
struct ProjectPlugin : Plugin {
    static constexpr const char* NAME = "project";

    explicit ProjectPlugin(std::filesystem::path root) : root(std::move(root)) {}

    bool build(Engine& engine) override;

    std::filesystem::path root;

private:
    static void scan(SystemContext& ctx);
};
