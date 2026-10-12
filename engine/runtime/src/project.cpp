#include "engine/project.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/engine.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/shader_library.hpp"

#include <cctype>
#include <string>
#include <unordered_map>

// --- Project ----------------------------------------------------------------------

bool Project::open(const std::filesystem::path& root) {
    return this->open(root, std::string());
}

bool Project::open(const std::filesystem::path& root, std::string name) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(root, error);
    if (error) {
        return false;
    }
    absolute = absolute.lexically_normal();
    // lexically_normal keeps a trailing separator ("game/" has an empty
    // filename); drop it so filename() is the directory's own name.
    if (absolute.has_filename() == false && absolute.has_parent_path()) {
        absolute = absolute.parent_path();
    }
    if (!std::filesystem::is_directory(absolute, error) || error) {
        return false;
    }

    if (name.empty()) {
        name = absolute.filename().string();
    }
    this->root = std::move(absolute);
    this->name = std::move(name);
    return true;
}

void Project::close() {
    this->root.clear();
    this->name.clear();
}

// --- PROJECT ----------------------------------------------------------------------

static std::string lower_extension(const std::filesystem::path& file) {
    std::string extension = file.extension().string();
    for (char& c : extension) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return extension;
}

bool PROJECT::open(Engine& engine, const std::filesystem::path& root) {
    Project* project = engine.get_singleton<Project>();
    if (project == nullptr) {
        project = engine.create_singleton<Project>();
        if (project == nullptr) {
            return false;
        }
    }
    if (!project->open(root)) {
        engine.log(RENDER_LOG_ERROR, "Project directory not found: %s", root.string().c_str());
        return false;
    }
    // The whole project is shader source: its root is the include root and
    // where a name falls back to when no asset entity carries a path, and
    // its render/ overrides the engine's shaders by name.
    if (Renderer* renderer = engine.get_singleton<Renderer>()) {
        const std::string render_dir = project->root.string();
        RENDERER::set_project_render_dir(*renderer, render_dir.c_str());
    }
    engine.log(RENDER_LOG_INFO, "Project '%s' opened at %s", project->name.c_str(), project->root.string().c_str());
    return true;
}

// Registers every .slang / .glsl in `directory` (its subfolders too when
// `recursive`) as a shader asset entity named after its stem
// (SHADER_LIBRARY::asset_guid) carrying the file's path, so an inspector
// can list it before it is compiled and the shader library finds it
// wherever it sits. `seen` maps stem -> path across calls, to report a
// second file of one name: the name is the GUID, so only one can win.
static u32 register_shader_assets(Engine& engine, const std::filesystem::path& directory, const bool recursive,
    std::unordered_map<std::string, std::string>& seen) {
    AssetEntities* assets = engine.get_singleton<AssetEntities>();
    if (assets == nullptr) {
        return 0;
    }
    std::error_code error;
    u32 registered = 0;
    const auto visit = [&](const std::filesystem::path& file) {
        const std::string extension = lower_extension(file);
        if (extension != ".slang" && extension != ".glsl") {
            return;
        }
        const std::string stem = file.stem().string();
        const std::string path = file.string();
        const auto [where, inserted] = seen.emplace(stem, path);
        if (!inserted) {
            engine.log(RENDER_LOG_WARNING, "Shader %s is ignored: %s already registered the name '%s'", path.c_str(), where->second.c_str(), stem.c_str());
            return;
        }
        if (assets->add(SHADER_LIBRARY::asset_guid(stem.c_str()), ASSET_TYPE::SHADER, stem.c_str(), path.c_str()) != 0) {
            registered += 1;
        } else {
            engine.log(RENDER_LOG_WARNING, "Shader %s could not be registered", path.c_str());
        }
    };
    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(directory, error), end; it != end; it.increment(error)) {
            if (!error && it->is_regular_file(error)) {
                visit(it->path());
            }
        }
    } else {
        for (std::filesystem::directory_iterator it(directory, error), end; it != end; it.increment(error)) {
            if (!error && it->is_regular_file(error)) {
                visit(it->path());
            }
        }
    }
    return registered;
}

bool PROJECT::scan(Engine& engine, ProjectScanResult* result) {
    const Project* project = engine.get_singleton<Project>();
    if (project == nullptr || !project->is_open()) {
        return false;
    }
    ProjectScanResult counts;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(project->root, error), end; it != end; it.increment(error)) {
        if (error || !it->is_regular_file(error)) {
            continue;
        }
        const std::string extension = lower_extension(it->path());
        if (extension != ASSET_FILE::EXTENSION && TEXT_ASSET::type_of_path(extension.c_str()) == 0) {
            continue;
        }
        const std::string path = it->path().string();
        if (!engine.load_asset_file(path.c_str()).is_null()) {
            counts.assets += 1;
        } else {
            engine.log(RENDER_LOG_WARNING, "Asset %s could not be registered", path.c_str());
        }
    }
    engine.log(RENDER_LOG_INFO, "%u asset files registered", counts.assets);

    // Engine shaders first: a project file of the same name has the same
    // GUID and points the entity at its own path, which is the override.
    std::unordered_map<std::string, std::string> seen;
    const Renderer* renderer = engine.get_singleton<Renderer>();
    if (renderer != nullptr && renderer->engine_render_dir[0] != '\0') {
        counts.shaders += register_shader_assets(engine, renderer->engine_render_dir, false, seen);
    }
    // The engine's files are not duplicates of themselves when the project
    // overrides one, so the project walk starts a fresh name set.
    seen.clear();
    counts.shaders += register_shader_assets(engine, project->root, true, seen);
    engine.log(RENDER_LOG_INFO, "%u shader files registered", counts.shaders);
    if (result != nullptr) {
        *result = counts;
    }
    return true;
}
