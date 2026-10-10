#include "ui/import/import_panel.hpp"

#include "import/image_importer.hpp"
#include "import/importer.hpp"
#include "import/obj_importer.hpp"
#include "ui/format.hpp"
#include "ui/panels.hpp"

#include <filesystem>
#include <imgui.h>

#include <cctype>
#include <cfloat>
#include <cstring>

ImportPanel::Kind ImportPanel::kind_of(const std::string& extension) {
    if (extension == ".obj") {
        return Kind::MESH;
    }
    if (IMAGE::is_supported_extension(extension.c_str())) {
        return Kind::TEXTURE;
    }
    return Kind::UNSUPPORTED;
}

const char* ImportPanel::kind_name(const Kind kind) {
    switch (kind) {
    case Kind::MESH: return "Mesh (Wavefront OBJ)";
    case Kind::TEXTURE: return "Texture (image)";
    default: return "Unsupported";
    }
}

static void copy_text(char* out, const usz out_size, const std::string& text) {
    const usz count = text.size() < out_size - 1 ? text.size() : out_size - 1;
    memcpy(out, text.data(), count);
    out[count] = '\0';
}

static std::string lowercase_extension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension;
}

void ImportPanel::open(const std::filesystem::path& source, const std::filesystem::path& destination_folder, bool* open) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(source, error).lexically_normal();
    if (error) {
        absolute = source;
    }
    Item item;
    item.source = ImportSource::file(absolute);
    item.extension = lowercase_extension(absolute);
    item.kind = kind_of(item.extension);
    const std::uintmax_t size = std::filesystem::file_size(absolute, error);
    item.size = error ? 0 : static_cast<u64>(size);

    const bool was_empty = this->queue.empty();
    this->queue.push_back(std::move(item));
    if (was_empty) {
        copy_text(this->folder, sizeof(this->folder), destination_folder.generic_string());
        this->show_front();
    }
    *open = true;
}

void ImportPanel::open_reimport(const ImportSource& source, const std::filesystem::path& asset, bool* open) {
    Item item;
    item.source = source;
    item.extension = lowercase_extension(std::filesystem::path(source.name));
    item.kind = kind_of(item.extension);
    item.size = source.size;
    item.asset = asset.lexically_normal();

    const bool was_empty = this->queue.empty();
    this->queue.push_back(std::move(item));
    if (was_empty) {
        this->show_front();
    }
    *open = true;
}

void ImportPanel::show_front() {
    if (this->queue.empty()) {
        return;
    }
    const Item& front = this->queue.front();
    if (front.is_reimport()) {
        // Back over the asset it came from, unless the user retargets it.
        copy_text(this->folder, sizeof(this->folder), front.asset.parent_path().generic_string());
        copy_text(this->name, sizeof(this->name), front.asset.stem().string());
    } else {
        copy_text(this->name, sizeof(this->name), std::filesystem::path(front.source.name).stem().string());
    }
}

void ImportPanel::pop_front() {
    this->queue.erase(this->queue.begin());
    this->show_front();
}

std::filesystem::path ImportPanel::destination(const Project& project) const {
    std::filesystem::path path = project.root;
    if (this->folder[0] != '\0') {
        path /= std::filesystem::path(this->folder);
    }
    path /= std::string(this->name) + ".lunaasset";
    return path.lexically_normal();
}

std::filesystem::path ImportPanel::reimport_target(const Project& project) const {
    const Item& item = this->queue.front();
    if (!item.is_reimport()) {
        return {};
    }
    return (project.root / item.asset).lexically_normal();
}

bool ImportPanel::import_front(const Project& project, OutputPanel& output) {
    const Item& item = this->queue.front();
    const std::filesystem::path target = this->destination(project);
    // Writing the asset back over itself keeps its GUID, so everything
    // referencing it still resolves. Anywhere else is a new asset: two
    // files must never share a GUID.
    const bool in_place = item.is_reimport() && target == this->reimport_target(project);
    const AssetGuid guid = in_place ? item.source.guid : IMPORT::random_guid();

    std::error_code fs_error;
    std::filesystem::create_directories(target.parent_path(), fs_error);
    if (fs_error) {
        output.error("Import: cannot create %s: %s", target.parent_path().string().c_str(), fs_error.message().c_str());
        return false;
    }

    std::string error;
    bool ok = false;
    switch (item.kind) {
    case Kind::MESH:
        ok = OBJ::import(item.source, target, this->mesh_options, guid, &error);
        break;
    case Kind::TEXTURE:
        ok = IMAGE::import(item.source, target, this->texture_options, guid, &error);
        break;
    default:
        error = "no importer for " + item.extension;
        break;
    }
    const std::string shown = item.source.describe();
    if (!ok) {
        output.error("%s %s failed: %s", item.is_reimport() ? "Reimport" : "Import", shown.c_str(), error.c_str());
        return false;
    }
    const std::string relative = std::filesystem::relative(target, project.root, fs_error).generic_string();
    if (in_place) {
        output.info("Reimported %s from its original %s", relative.c_str(), item.source.name.c_str());
    } else {
        output.info("Imported %s -> %s", shown.c_str(), relative.c_str());
    }
    return true;
}

bool ImportPanel::will_overwrite(const Project& project) const {
    const std::filesystem::path target = this->destination(project);
    if (target == this->reimport_target(project)) {
        return false; // replacing the asset in place is what a re-import is for
    }
    std::error_code error;
    return std::filesystem::exists(target, error);
}

static const char* OVERWRITE_POPUP = "Replace existing file?";

bool ImportPanel::draw(bool* open, const Project* project_or_null, OutputPanel& output) {
    if (this->queue.empty()) {
        *open = false;
        return false;
    }
    Item& item = this->queue.front();
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
    // Both titles share the id after "###", so the window keeps its place
    // when a re-import follows an import.
    if (!ImGui::Begin(item.is_reimport() ? PANELS::REIMPORT : PANELS::IMPORT, open, flags)) {
        ImGui::End();
        if (!*open) {
            this->clear();
        }
        return false;
    }
    if (this->queue.size() > 1) {
        ImGui::TextDisabled("%zu files queued", this->queue.size());
        ImGui::Separator();
    }

    this->draw_source(item);
    ImGui::Separator();
    this->draw_settings(item);
    ImGui::Separator();

    const bool has_project = project_or_null != nullptr && project_or_null->is_open();
    if (has_project) {
        this->draw_destination(*project_or_null);
    } else {
        ImGui::TextDisabled("(no project open: nowhere to import to)");
    }
    ImGui::Separator();

    bool written = false;
    const bool can_import = has_project && item.kind != Kind::UNSUPPORTED && this->name[0] != '\0';
    ImGui::BeginDisabled(!can_import);
    if (ImGui::Button("Import", ImVec2(100.0f, 0.0f))) {
        if (this->will_overwrite(*project_or_null)) {
            ImGui::OpenPopup(OVERWRITE_POPUP);
        } else {
            written = this->import_front(*project_or_null, output);
            if (written) {
                this->pop_front();
            }
        }
    }
    ImGui::EndDisabled();
    if (has_project && this->draw_overwrite_popup(*project_or_null, output)) {
        written = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip", ImVec2(100.0f, 0.0f))) {
        this->pop_front();
    }
    ImGui::SameLine();
    if (ImGui::Button(this->queue.size() > 1 ? "Cancel All" : "Cancel", ImVec2(100.0f, 0.0f))) {
        this->clear();
    }

    ImGui::End();
    if (!*open || this->queue.empty()) {
        this->clear();
        *open = false;
    }
    return written;
}

// The modal asking before an import replaces an existing .lunaasset. Opened by
// the Import button; Overwrite imports the front file, Cancel (or Escape)
// returns to the panel. Returns true when a file was written.
bool ImportPanel::draw_overwrite_popup(const Project& project, OutputPanel& output) {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(OVERWRITE_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return false;
    }

    std::error_code error;
    const std::string target = std::filesystem::relative(this->destination(project), project.root, error).generic_string();
    ImGui::Text("%s already exists.", target.c_str());
    ImGui::TextUnformatted("Replace it with the imported file?");
    ImGui::Separator();

    bool written = false;
    if (ImGui::Button("Overwrite", ImVec2(100.0f, 0.0f))) {
        written = this->import_front(project, output);
        if (written) {
            this->pop_front();
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return written;
}

void ImportPanel::draw_source(const Item& item) {
    char size_text[32];
    UI::format_size(item.size, size_text, sizeof(size_text));

    if (ImGui::BeginTable("source", 2, ImGuiTableFlags_SizingFixedFit)) {
        if (item.is_reimport()) {
            // The asset the original was read from, then the original's name.
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Asset");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(item.asset.generic_string().c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", item.source.path.string().c_str());
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Original");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(item.source.name.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Kept inside the asset at import time");
            }
        } else {
            const std::string dir = item.source.path.parent_path().string();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("File");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(item.source.name.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", item.source.path.string().c_str());
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Folder");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(dir.c_str());
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Type");
        ImGui::TableNextColumn();
        if (item.kind == Kind::UNSUPPORTED) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                "No importer for %s",
                item.extension.empty() ? "files without an extension" : item.extension.c_str()
            );
        } else {
            ImGui::TextUnformatted(kind_name(item.kind));
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Size");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(size_text);
        ImGui::EndTable();
    }
}

void ImportPanel::draw_settings(const Item& item) {
    switch (item.kind) {
    case Kind::MESH:
        ImGui::SeparatorText("Mesh settings");
        ImGui::Checkbox("Compact indices", &this->mesh_options.compact_indices);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Write 16-bit indices when every index fits, 32-bit otherwise.\nOff always writes 32-bit.");
        }
        this->draw_keep_original(item, &this->mesh_options.keep_source);
        break;
    case Kind::TEXTURE:
        ImGui::SeparatorText("Texture settings");
        ImGui::Checkbox("sRGB (color data)", &this->texture_options.srgb);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Store the pixels in the sRGB variant of the format.\nOn for albedo and UI images; off for normal maps, masks and data.");
        }
        ImGui::Checkbox("Generate mips", &this->texture_options.generate_mips);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Write the full box-filtered mip chain instead of mip 0 alone.");
        }
        this->draw_keep_original(item, &this->texture_options.keep_source);
        break;
    default:
        ImGui::TextDisabled("Importable formats: .obj, .png, .jpg, .jpeg, .bmp, .tga");
        break;
    }
}

void ImportPanel::draw_keep_original(const Item& item, bool* keep_source) {
    ImGui::Checkbox("Keep original", keep_source);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Store the source file inside the asset.\n"
                          "The asset grows by the source's size, but it can then be reimported\n"
                          "with other settings and the original saved back out from the Asset Browser.");
    }
    if (item.is_reimport() && !*keep_source) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Drops the kept original: no further reimport or Save Original");
    }
}

void ImportPanel::draw_destination(const Project& project) {
    ImGui::SeparatorText("Destination");
    const f32 label_width = ImGui::CalcTextSize("Folder").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Folder");
    ImGui::SameLine(label_width);
    ImGui::TextUnformatted(project.name.c_str());
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted("/");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##folder", this->folder, sizeof(this->folder));

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Name");
    ImGui::SameLine(label_width);
    const f32 suffix_width = ImGui::CalcTextSize(".lunaasset").x + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(-suffix_width);
    ImGui::InputText("##name", this->name, sizeof(this->name));
    ImGui::SameLine();
    ImGui::TextUnformatted(".lunaasset");

    const std::filesystem::path target = this->destination(project);
    const std::filesystem::path own = this->reimport_target(project);
    if (!own.empty() && target == own) {
        ImGui::TextDisabled("Replaces the asset in place, keeping its GUID");
    } else if (this->will_overwrite(project)) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Replaces the existing file");
    } else if (!own.empty()) {
        ImGui::TextDisabled("Writes a new asset with a new GUID; the reimported asset is left as is");
    }
}
