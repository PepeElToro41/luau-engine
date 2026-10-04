#include "project.hpp"

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
