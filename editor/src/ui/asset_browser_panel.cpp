#include "ui/asset_browser_panel.hpp"

#include "engine/asset/asset_types/material_asset.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/asset/asset_writer.hpp"
#include "ui/format.hpp"
#include "ui/import/import_panel.hpp"
#include "ui/panels.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>

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
static const char* NAME_POPUP = "##name_popup";
static const char* SAVE_ORIGINAL_POPUP = "Save Original";

// What Create > Shader writes: the unlit shader's shape with one color
// param, ready to be loaded by name from the project's render/ folder.
static const char* SHADER_TEMPLATE =
    "#pragma pass forward vertex fragment\n"
    "#pragma pass shadow vertex\n"
    "\n"
    "#include \"engine/frame.slang\"\n"
    "#include \"engine/object.slang\"\n"
    "#include \"engine/vertex.slang\"\n"
    "#include \"engine/bindings.slang\"\n"
    "\n"
    "struct MaterialParams {\n"
    "    float4 color;\n"
    "};\n"
    "MATERIAL(0) ConstantBuffer<MaterialParams> material;\n"
    "\n"
    "struct VertexInput {\n"
    "    VERTEX_POSITION float3 position;\n"
    "};\n"
    "\n"
    "struct VertexOutput {\n"
    "    float4 clip_position : SV_Position;\n"
    "};\n"
    "\n"
    "[shader(\"vertex\")]\n"
    "VertexOutput vertex(VertexInput v) {\n"
    "    VertexOutput o;\n"
    "    o.clip_position = mul(frame.view_projection, mul(object.model, float4(v.position, 1.0)));\n"
    "    return o;\n"
    "}\n"
    "\n"
    "[shader(\"fragment\")]\n"
    "float4 fragment(VertexOutput v) : SV_Target {\n"
    "    return material.color;\n"
    "}\n";

// What Create > Code > Luau writes.
static const char* LUAU_TEMPLATE =
    "--!strict\n"
    "\n"
    "local function main()\n"
    "end\n"
    "\n"
    "return main\n";

// What Create > Code > Cpp writes.
static const char* CPP_TEMPLATE =
    "#include \"engine/defines.hpp\"\n"
    "\n"
    "void main() {\n"
    "}\n";

const char* AssetBrowserPanel::create_extension(const CreateKind kind) {
    switch (kind) {
    case CreateKind::MATERIAL: return ".material";
    case CreateKind::SHADER: return ".slang";
    case CreateKind::LUAU: return ".luau";
    case CreateKind::CPP: return ".cpp";
    }
    return "";
}

const char* AssetBrowserPanel::create_label(const CreateKind kind) {
    switch (kind) {
    case CreateKind::MATERIAL: return "material";
    case CreateKind::SHADER: return "shader";
    case CreateKind::LUAU: return "Luau script";
    case CreateKind::CPP: return "C++ source";
    }
    return "file";
}

void AssetBrowserPanel::draw_create_items() {
    CreateKind picked = CreateKind::MATERIAL;
    bool any = false;
    if (ImGui::MenuItem("Material")) {
        picked = CreateKind::MATERIAL;
        any = true;
    }
    if (ImGui::MenuItem("Shader")) {
        picked = CreateKind::SHADER;
        any = true;
    }
    if (ImGui::BeginMenu("Code")) {
        if (ImGui::MenuItem("Luau")) {
            picked = CreateKind::LUAU;
            any = true;
        }
        if (ImGui::MenuItem("Cpp")) {
            picked = CreateKind::CPP;
            any = true;
        }
        ImGui::EndMenu();
    }
    if (any) {
        this->create_kind = picked;
        this->create_requested = true;
    }
}

void AssetBrowserPanel::draw(bool* open, const Project* project_or_null, OutputPanel& output, Selection& selection) {
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
    this->reimport_asset.clear();
    // The highlight follows the Selection: a file of this folder keeps it,
    // anything else (an entity, another folder's file) drops it.
    if (!this->selected.empty() && !selection.is_file(this->current / this->selected)) {
        this->selected.clear();
    }
    this->draw_breadcrumbs(project);
    ImGui::Separator();
    this->draw_entries(project, selection);
    this->draw_delete_popup(project, output, selection);
    this->draw_name_popup(project, output, selection);
    this->draw_save_original_popup(project, output);

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
    // Create dropdown: a button that opens a menu right under itself.
    if (ImGui::Button("Create")) {
        ImGui::OpenPopup("##create_menu");
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##create_menu")) {
        this->draw_create_items();
        ImGui::EndPopup();
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

void AssetBrowserPanel::draw_entries(const Project& project, Selection& selection) {
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
    bool request_new_folder = false; // New Folder... picked from a context menu this frame
    bool request_rename = false; // Rename... picked from a context menu (or F2) this frame
    bool request_save_original = false; // Save Original... picked from a context menu this frame
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
            if (!is_selected) {
                selection.select_file(this->current / entry.name);
            }
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
            if (!is_selected) {
                this->selected = entry.name;
                selection.select_file(this->current / entry.name);
            }
            const bool importable = !entry.is_directory && ImportPanel::kind_of(entry.extension) != ImportPanel::Kind::UNSUPPORTED;
            const bool is_asset = !entry.is_directory && entry.extension == ASSET_FILE::EXTENSION;
            if (ImGui::IsWindowAppearing()) {
                // Probe the asset once per opening: does it keep its original?
                this->context_entry = entry.name;
                this->context_has_source = false;
                this->context_source = ImportSource{};
                if (is_asset) {
                    std::string error;
                    this->context_has_source = IMPORT::find_source(project.root / this->current / entry.name, &this->context_source, &error);
                }
            }
            if (importable && ImGui::MenuItem("Import")) {
                this->activated = this->current / entry.name; // same as a double-click
            }
            if (is_asset) {
                const bool has_source = this->context_has_source && this->context_entry == entry.name;
                ImGui::BeginDisabled(!has_source);
                if (ImGui::MenuItem("Reimport")) {
                    this->reimport_source = this->context_source;
                    this->reimport_asset = this->current / entry.name;
                }
                if (ImGui::MenuItem("Save Original...")) {
                    request_save_original = true;
                    this->begin_save_original(project, entry);
                }
                ImGui::EndDisabled();
                if (!has_source && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("No original was kept in this asset");
                }
            }
            if (ImGui::MenuItem("Rename...", "F2")) {
                this->renaming = entry;
                request_rename = true;
            }
            if (ImGui::MenuItem("Delete")) {
                this->deleting = entry;
                request_delete = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("New Folder...")) {
                request_new_folder = true;
            }
            if (ImGui::BeginMenu("Create")) {
                this->draw_create_items();
                ImGui::EndMenu();
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
    // Right-click on the empty space of the listing (the table's scrolling
    // child is the current window here; its rows are items, so
    // NoOpenOverItems leaves them to their own menus).
    const ImGuiPopupFlags empty_flags = ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems;
    if (ImGui::BeginPopupContextWindow("##folder_context", empty_flags)) {
        if (ImGui::MenuItem("New Folder...")) {
            request_new_folder = true;
        }
        if (ImGui::BeginMenu("Create")) {
            this->draw_create_items();
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Refresh")) {
            this->refresh();
        }
        ImGui::EndPopup();
    }
    // F2 renames the selected entry while the listing has focus.
    if (!this->selected.empty() && ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive()
        && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        for (const Entry& entry : this->entries) {
            if (entry.name == this->selected) {
                this->renaming = entry;
                request_rename = true;
                break;
            }
        }
    }
    ImGui::EndTable();

    if (!enter.empty()) {
        this->navigate(enter);
    }
    // Opened here, outside the table, so the modals in draw_delete_popup()
    // and draw_name_popup() begin at the same id-stack level.
    if (request_delete) {
        ImGui::OpenPopup(DELETE_POPUP);
    }
    if (request_rename) {
        this->begin_rename(this->renaming);
        ImGui::OpenPopup(NAME_POPUP);
    } else if (request_new_folder) {
        this->begin_new_folder();
        ImGui::OpenPopup(NAME_POPUP);
    } else if (this->create_requested) {
        this->begin_create(this->create_kind);
        ImGui::OpenPopup(NAME_POPUP);
    }
    this->create_requested = false;
    if (request_save_original) {
        ImGui::OpenPopup(SAVE_ORIGINAL_POPUP);
    }
}

void AssetBrowserPanel::begin_save_original(const Project& project, const Entry& entry) {
    this->saving = this->context_source;
    this->saving_asset = entry.name;
    // Next to the asset, under the original's name.
    const std::string suggested = (project.root / this->current / this->saving.name).lexically_normal().string();
    std::snprintf(this->save_path, sizeof(this->save_path), "%s", suggested.c_str());
}

void AssetBrowserPanel::draw_save_original_popup(const Project& project, OutputPanel& output) {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(SAVE_ORIGINAL_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    char size_text[32];
    UI::format_size(this->saving.size, size_text, sizeof(size_text));
    ImGui::Text("Save the original of %s", (this->current / this->saving_asset).generic_string().c_str());
    ImGui::TextDisabled("%s, %s", this->saving.name.c_str(), size_text);
    ImGui::Spacing();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Save to");
    ImGui::SameLine();
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(420.0f);
    bool confirmed = ImGui::InputText("##save_path", this->save_path, sizeof(this->save_path), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled("Absolute, or relative to %s", project.root.string().c_str());

    const char* problem = this->save_path[0] == '\0' ? "Enter a path." : nullptr;
    std::error_code error;
    std::filesystem::path target(this->save_path);
    if (target.is_relative()) {
        target = project.root / target;
    }
    if (problem == nullptr && std::filesystem::is_directory(target, error)) {
        problem = "That is a folder; name a file.";
    }
    if (problem != nullptr) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", problem);
    } else if (std::filesystem::exists(target, error)) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Replaces the existing file");
    } else {
        ImGui::TextDisabled(" ");
    }
    ImGui::Separator();

    ImGui::BeginDisabled(problem != nullptr);
    confirmed = ImGui::Button("Save", ImVec2(100.0f, 0.0f)) || confirmed;
    ImGui::EndDisabled();
    if (confirmed && problem == nullptr) {
        if (this->save_original(project, this->save_path, output)) {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool AssetBrowserPanel::save_original(const Project& project, const char* path, OutputPanel& output) {
    std::filesystem::path target(path);
    if (target.is_relative()) {
        target = project.root / target;
    }
    target = target.lexically_normal();
    std::error_code fs_error;
    std::filesystem::create_directories(target.parent_path(), fs_error);
    if (fs_error) {
        output.error("Save Original: cannot create %s: %s", target.parent_path().string().c_str(), fs_error.message().c_str());
        return false;
    }
    std::string error;
    if (!IMPORT::save_source(this->saving, target, &error)) {
        output.error("Save Original of %s failed: %s", this->saving_asset.c_str(), error.c_str());
        return false;
    }
    output.info("Saved the original of %s (%s) to %s", this->saving_asset.c_str(), this->saving.name.c_str(), target.string().c_str());
    this->refresh(); // it may have landed in the folder shown
    return true;
}

void AssetBrowserPanel::begin_new_folder() {
    // Default to "New Folder", or the first "New Folder N" not taken in the
    // current listing.
    this->name_mode = NameMode::NEW_FOLDER;
    std::snprintf(this->name_buffer, sizeof(this->name_buffer), "New Folder");
    for (int n = 2; this->name_problem(this->name_buffer, {}) != nullptr && n < 1000; ++n) {
        std::snprintf(this->name_buffer, sizeof(this->name_buffer), "New Folder %d", n);
    }
}

void AssetBrowserPanel::begin_rename(const Entry& entry) {
    this->name_mode = NameMode::RENAME;
    this->renaming = entry;
    std::snprintf(this->name_buffer, sizeof(this->name_buffer), "%s", entry.name.c_str());
}

void AssetBrowserPanel::begin_create(const CreateKind kind) {
    // Default to "new_<kind>", or the first "new_<kind>_N" not taken in the
    // current listing (with the extension; shader and script names end up
    // as identifiers, so no spaces).
    static const char* stems[] = {"new_material", "new_shader", "new_script", "new_source"};
    const char* stem = stems[static_cast<int>(kind)];
    this->name_mode = NameMode::NEW_FILE;
    this->create_kind = kind;
    std::snprintf(this->name_buffer, sizeof(this->name_buffer), "%s", stem);
    for (int n = 2; this->name_problem(this->create_file_name().c_str(), {}) != nullptr && n < 1000; ++n) {
        std::snprintf(this->name_buffer, sizeof(this->name_buffer), "%s_%d", stem, n);
    }
}

std::string AssetBrowserPanel::create_file_name() const {
    std::string name = this->name_buffer;
    const char* extension = AssetBrowserPanel::create_extension(this->create_kind);
    const usz extension_length = std::strlen(extension);
    if (name.size() >= extension_length) {
        std::string tail = name.substr(name.size() - extension_length);
        for (char& c : tail) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (tail == extension) {
            return name;
        }
    }
    return name + extension;
}

const char* AssetBrowserPanel::name_problem(const char* name, const std::string& except) const {
    if (name[0] == '\0') {
        return "Enter a name.";
    }
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
        return "That name is reserved.";
    }
    if (std::strchr(name, '/') != nullptr || std::strchr(name, '\\') != nullptr) {
        return "A name cannot contain / or \\.";
    }
    for (const Entry& entry : this->entries) {
        if (entry.name == name && entry.name != except) {
            return entry.is_directory ? "A folder with that name already exists." : "A file with that name already exists.";
        }
    }
    return nullptr;
}

void AssetBrowserPanel::draw_name_popup(const Project& project, OutputPanel& output, Selection& selection) {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(NAME_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const bool rename = this->name_mode == NameMode::RENAME;
    const bool new_file = this->name_mode == NameMode::NEW_FILE;

    if (rename) {
        const std::string shown = (this->current / this->renaming.name).generic_string();
        ImGui::Text("Rename %s %s", this->renaming.is_directory ? "the folder" : "the file", shown.c_str());
    } else {
        const std::string shown = this->current.empty() ? project.name : (project.name / this->current).generic_string();
        ImGui::Text("Create a %s in %s", new_file ? AssetBrowserPanel::create_label(this->create_kind) : "folder", shown.c_str());
    }
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    const ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue;
    ImGui::SetNextItemWidth(300.0f);
    bool confirmed = ImGui::InputText("##name", this->name_buffer, sizeof(this->name_buffer), input_flags);
    // The file name the modal will make, with the kind's extension shown
    // after the field (typing it yourself is fine, it is not doubled).
    std::string file_name;
    if (new_file) {
        file_name = this->create_file_name();
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::TextDisabled("%s", AssetBrowserPanel::create_extension(this->create_kind));
        if (this->create_kind == CreateKind::SHADER && this->current != std::filesystem::path("render")) {
            ImGui::TextDisabled("Shaders load by name from the project's render folder.");
        }
    }

    const std::string& except = rename ? this->renaming.name : std::string();
    const char* problem = this->name_problem(new_file ? file_name.c_str() : this->name_buffer, except);
    // Renaming to the same name is a no-op, not an error.
    const bool unchanged = rename && this->renaming.name == this->name_buffer;
    if (problem != nullptr) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", problem);
    } else {
        ImGui::TextDisabled(" ");
    }
    ImGui::Separator();

    ImGui::BeginDisabled(problem != nullptr || unchanged);
    confirmed = ImGui::Button(rename ? "Rename" : "Create", ImVec2(100.0f, 0.0f)) || confirmed;
    ImGui::EndDisabled();
    if (confirmed && problem == nullptr) {
        bool done = true;
        if (unchanged) {
            // nothing to do
        } else if (rename) {
            done = this->rename_entry(project, this->renaming, this->name_buffer, output, selection);
        } else if (new_file) {
            done = this->create_file(project, file_name.c_str(), output, selection);
        } else {
            done = this->create_folder(project, this->name_buffer, output, selection);
        }
        if (done) {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool AssetBrowserPanel::rename_entry(const Project& project, const Entry& entry, const char* name, OutputPanel& output, Selection& selection) {
    const std::filesystem::path from = (project.root / this->current / entry.name).lexically_normal();
    const std::filesystem::path to = (project.root / this->current / name).lexically_normal();
    const std::string shown_from = (this->current / entry.name).generic_string();
    const std::string shown_to = (this->current / name).generic_string();
    std::error_code error;
    // rename() would silently replace an existing file; refuse instead, the
    // listing may be stale.
    if (std::filesystem::exists(to, error)) {
        output.error("Rename %s failed: %s already exists", shown_from.c_str(), shown_to.c_str());
        this->refresh();
        return false;
    }
    std::filesystem::rename(from, to, error);
    if (error) {
        output.error("Rename %s failed: %s", shown_from.c_str(), error.message().c_str());
        this->refresh();
        return false;
    }
    output.info("Renamed %s to %s", shown_from.c_str(), shown_to.c_str());
    this->selected = name;
    selection.select_file(this->current / name);
    this->refresh();
    return true;
}

bool AssetBrowserPanel::create_folder(const Project& project, const char* name, OutputPanel& output, Selection& selection) {
    const std::filesystem::path target = (project.root / this->current / name).lexically_normal();
    const std::string shown = (this->current / name).generic_string();
    std::error_code error;
    const bool created = std::filesystem::create_directory(target, error);
    if (error) {
        output.error("Create folder %s failed: %s", shown.c_str(), error.message().c_str());
        return false;
    }
    if (!created) {
        // Exists on disk but not in the listing: it appeared since the scan.
        output.warning("Folder %s already exists", shown.c_str());
    } else {
        output.info("Created folder %s", shown.c_str());
    }
    this->selected = name;
    selection.select_file(this->current / name);
    this->refresh();
    return true;
}

bool AssetBrowserPanel::create_file(const Project& project, const char* name, OutputPanel& output, Selection& selection) {
    const std::filesystem::path target = (project.root / this->current / name).lexically_normal();
    const std::string shown = (this->current / name).generic_string();
    const char* label = AssetBrowserPanel::create_label(this->create_kind);
    std::error_code error;
    // The writers replace an existing file; refuse instead, the listing
    // may be stale.
    if (std::filesystem::exists(target, error)) {
        output.error("Create %s %s failed: it already exists", label, shown.c_str());
        this->refresh();
        return false;
    }
    const std::string path = target.string();
    bool ok = false;
    if (this->create_kind == CreateKind::MATERIAL) {
        MaterialAsset material;
        material.guid = IMPORT::random_guid();
        std::snprintf(material.shader, sizeof(material.shader), "unlit");
        const f64 color[4] = {1.0, 1.0, 1.0, 1.0};
        material.set_param("color", color, 4);
        ok = MATERIAL_ASSET::write_file(material, path.c_str());
    } else {
        const char* text = this->create_kind == CreateKind::SHADER ? SHADER_TEMPLATE
                         : this->create_kind == CreateKind::LUAU   ? LUAU_TEMPLATE
                                                                   : CPP_TEMPLATE;
        ok = ASSET_FILE::write_file(path.c_str(), text, std::strlen(text));
    }
    if (!ok) {
        output.error("Create %s %s failed: cannot write the file", label, shown.c_str());
        this->refresh();
        return false;
    }
    output.info("Created %s %s", label, shown.c_str());
    this->selected = name;
    selection.select_file(this->current / name);
    this->refresh();
    return true;
}

void AssetBrowserPanel::draw_delete_popup(const Project& project, OutputPanel& output, Selection& selection) {
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
        this->delete_entry(project, this->deleting, output, selection);
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

void AssetBrowserPanel::delete_entry(const Project& project, const Entry& entry, OutputPanel& output, Selection& selection) {
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
            selection.clear();
        }
    }
    this->refresh();
}
