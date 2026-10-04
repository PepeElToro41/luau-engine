#include "ui/asset_browser_panel.hpp"

#include "ui/panels.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>

// Writes `bytes` as "1.2 MB" style text into `out`.
static void format_size(const u64 bytes, char* out, const usz out_size) {
    static constexpr const char* UNITS[] = {"B", "KB", "MB", "GB", "TB"};
    f64 value = static_cast<f64>(bytes);
    usz unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(UNITS) / sizeof(UNITS[0])) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        snprintf(out, out_size, "%llu %s", static_cast<unsigned long long>(bytes), UNITS[unit]);
    } else {
        snprintf(out, out_size, "%.1f %s", value, UNITS[unit]);
    }
}

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

void AssetBrowserPanel::draw(bool* open, const Project* project_or_null) {
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

    this->draw_breadcrumbs(project);
    ImGui::Separator();
    this->draw_entries();

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
    if (ImGui::SmallButton(project.name.c_str())) {
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
        if (ImGui::SmallButton(label.c_str())) {
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
            if (entry.is_directory && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                enter = this->current / entry.name;
            }
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
            format_size(entry.size, size_text, sizeof(size_text));
            ImGui::TextUnformatted(size_text);
        }
    }
    ImGui::EndTable();

    if (!enter.empty()) {
        this->navigate(enter);
    }
}
