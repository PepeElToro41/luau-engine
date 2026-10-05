#include "ui/asset_browser_panel.hpp"

#include "ui/format.hpp"
#include "ui/import/import_panel.hpp"
#include "ui/panels.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>

static std::string lowercase_extension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension;
}

void AssetBrowserPanel::navigate(const std::filesystem::path& folder) {
    this->current = folder.lexically_normal();
    if (this->current == ".") {
        this->current.clear();
    }
    this->selected.clear();
    this->dirty = true;
}

void AssetBrowserPanel::rescan(const Project& project) {
    this->entries.clear();
    this->dirty = false;
    this->scanned_root = project.root;
    this->scanned_hidden = this->show_hidden;
    if (!project.is_open()) {
        return;
    }

    const std::filesystem::path folder = project.root / this->current;
    std::error_code error;
    std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) {
        return;
    }

    for (const std::filesystem::directory_entry& item : it) {
        Entry entry;
        entry.name = item.path().filename().string();
        if (entry.name.empty() || (!this->show_hidden && entry.name[0] == '.')) {
            continue;
        }
        entry.is_directory = item.is_directory(error);
        if (error) {
            error.clear();
            entry.is_directory = false;
        }
        if (!entry.is_directory) {
            entry.extension = lowercase_extension(item.path());
            const std::uintmax_t size = item.file_size(error);
            entry.size = error ? 0 : static_cast<u64>(size);
            error.clear();
        }
        this->entries.push_back(std::move(entry));
    }

    std::sort(this->entries.begin(), this->entries.end(), [](const Entry& a, const Entry& b) {
        if (a.is_directory != b.is_directory) {
            return a.is_directory;
        }
        return a.name < b.name;
    });
}

static const char* DELETE_POPUP = "Delete?";

void AssetBrowserPanel::draw(bool* open, const Project* project_or_null, OutputPanel& output) {
    if (!ImGui::Begin(PANELS::ASSET_BROWSER, open)) {
        ImGui::End();
        return;
    }

    if (project_or_null == nullptr || !project_or_null->is_open()) {
        if (!this->scanned_root.empty()) {
            // The project was closed under us: forget where we were.
            this->navigate({});
            this->entries.clear();
            this->scanned_root.clear();
        }
        ImGui::TextDisabled("(no project open)");
        ImGui::End();
        return;
    }
    const Project& project = *project_or_null;

    if (project.root != this->scanned_root) {
        this->navigate({});
    }
    if (this->dirty || this->show_hidden != this->scanned_hidden) {
        this->rescan(project);
    }

    this->activated.clear();
    this->draw_breadcrumbs(project);
    ImGui::Separator();
    this->draw_entries();
    this->draw_delete_popup(project, output);

    ImGui::End();
}

void AssetBrowserPanel::draw_breadcrumbs(const Project& project) {
    // Toolbar: [Up] [Refresh]  project > folder > subfolder        [hidden] [filter]
    const bool at_root = this->current.empty();
    ImGui::BeginDisabled(at_root);
    if (ImGui::Button("Up")) {
        this->navigate(this->current.parent_path());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        this->refresh();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // One button per path component; clicking one jumps to that folder.
    if (ImGui::Button(project.name.c_str())) {
        this->navigate({});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", project.root.string().c_str());
    }
    std::filesystem::path walked;
    for (const std::filesystem::path& component : this->current) {
        walked /= component;
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextDisabled(">");
        ImGui::SameLine(0.0f, 0.0f);
        const std::string label = component.string();
        ImGui::PushID(label.c_str());
        if (ImGui::Button(label.c_str())) {
            this->navigate(walked);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }

    ImGui::SameLine();
    const f32 filter_width = 200.0f;
    const f32 hidden_width = ImGui::CalcTextSize("Hidden").x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const f32 right_edge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const f32 controls_x = right_edge - filter_width - hidden_width - ImGui::GetStyle().ItemSpacing.x;
    if (controls_x > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(controls_x);
    }
    ImGui::Checkbox("Hidden", &this->show_hidden);
    ImGui::SameLine();
    this->filter.Draw("##filter", filter_width);
}

void AssetBrowserPanel::draw_entries() {
    const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg
                                      | ImGuiTableFlags_BordersInnerV
                                      | ImGuiTableFlags_Resizable
                                      | ImGuiTableFlags_ScrollY
                                      | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("entries", 3, table_flags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.2f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.2f);
    ImGui::TableHeadersRow();

    std::filesystem::path enter; // folder double-clicked this frame, if any
    bool request_delete = false; // Delete picked from a context menu this frame
    char size_text[32];
    for (const Entry& entry : this->entries) {
        if (!this->filter.PassFilter(entry.name.c_str())) {
            continue;
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        const bool is_selected = this->selected == entry.name;
        const ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;
        if (ImGui::Selectable(entry.name.c_str(), is_selected, flags)) {
            this->selected = entry.name;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (entry.is_directory) {
                    enter = this->current / entry.name;
                } else {
                    this->activated = this->current / entry.name;
                }
            }
        }
        // Right-click: context menu on the row (the Selectable's id).
        if (ImGui::BeginPopupContextItem()) {
            this->selected = entry.name;
            const bool importable = !entry.is_directory && ImportPanel::kind_of(entry.extension) != ImportPanel::Kind::UNSUPPORTED;
            if (importable && ImGui::MenuItem("Import")) {
                this->activated = this->current / entry.name; // same as a double-click
            }
            if (ImGui::MenuItem("Delete")) {
                this->deleting = entry;
                request_delete = true;
            }
            ImGui::EndPopup();
        }

        ImGui::TableNextColumn();
        if (entry.is_directory) {
            ImGui::TextDisabled("Folder");
        } else if (entry.extension.empty()) {
            ImGui::TextDisabled("File");
        } else {
            ImGui::TextUnformatted(entry.extension.c_str() + 1);
        }

        ImGui::TableNextColumn();
        if (!entry.is_directory) {
            UI::format_size(entry.size, size_text, sizeof(size_text));
            ImGui::TextUnformatted(size_text);
        }
    }
    ImGui::EndTable();

    if (!enter.empty()) {
        this->navigate(enter);
    }
    // Opened here, outside the table, so the modal in draw_delete_popup()
    // begins at the same id-stack level.
    if (request_delete) {
        ImGui::OpenPopup(DELETE_POPUP);
    }
}

void AssetBrowserPanel::draw_delete_popup(const Project& project, OutputPanel& output) {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(DELETE_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (this->deleting.name.empty()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const std::string shown = (this->current / this->deleting.name).generic_string();
    if (this->deleting.is_directory) {
        ImGui::Text("Delete the folder %s and everything in it?", shown.c_str());
    } else {
        ImGui::Text("Delete %s?", shown.c_str());
    }
    ImGui::TextDisabled("This cannot be undone.");
    ImGui::Separator();

    if (ImGui::Button("Delete", ImVec2(100.0f, 0.0f))) {
        this->delete_entry(project, this->deleting, output);
        this->deleting = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        this->deleting = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void AssetBrowserPanel::delete_entry(const Project& project, const Entry& entry, OutputPanel& output) {
    const std::filesystem::path target = (project.root / this->current / entry.name).lexically_normal();
    std::error_code error;
    if (entry.is_directory) {
        std::filesystem::remove_all(target, error);
    } else {
        std::filesystem::remove(target, error);
    }
    const std::string shown = (this->current / entry.name).generic_string();
    if (error) {
        output.error("Delete %s failed: %s", shown.c_str(), error.message().c_str());
    } else {
        output.info("Deleted %s", shown.c_str());
        if (this->selected == entry.name) {
            this->selected.clear();
        }
    }
    this->refresh();
}
