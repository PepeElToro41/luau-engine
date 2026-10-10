#include "ui/inspector_panel.hpp"

#include "engine/ecs/archetype/archetype.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/world.hpp"
#include "engine/scene/inspector.hpp"
#include "ui/format.hpp"
#include "ui/panels.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>

void InspectorPanel::draw(bool* open, World* world, const Project* project, const Selection& selection) {
    this->file_action = InspectorFileAction::NONE;
    if (!ImGui::Begin(PANELS::INSPECTOR, open)) {
        ImGui::End();
        return;
    }

    switch (selection.kind) {
    case SelectionKind::ENTITY:
        if (world == nullptr || !world->alive(selection.entity)) {
            ImGui::TextDisabled("(nothing selected)");
        } else {
            this->draw_entity(*world, selection.entity);
        }
        break;
    case SelectionKind::FILE:
        this->draw_file(world, project, selection);
        break;
    case SelectionKind::NONE:
        ImGui::TextDisabled("(nothing selected)");
        break;
    }

    ImGui::End();
}

void InspectorPanel::draw_entity(World& world, const EntityId entity) {
    if (entity != this->scratch_entity) {
        this->scratch_entity = entity;
        this->scratch_count = 0;
    }
    this->draw_header(world, entity);
    ImGui::Separator();
    if (ImGui::BeginChild("components")) {
        this->draw_components(world, entity);
        this->draw_add_component(world, entity);
    }
    ImGui::EndChild();
}

// The kind shown for a file, from its extension.
static const char* file_kind(const std::filesystem::path& file, const bool is_directory) {
    if (is_directory) {
        return "Folder";
    }
    std::string extension = file.extension().string();
    for (char& c : extension) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    if (extension == ".material") {
        return "Material";
    }
    if (extension == ".lunaasset") {
        return "Asset";
    }
    if (extension == ".slang" || extension == ".glsl") {
        return "Shader";
    }
    if (extension == ".obj") {
        return "Mesh source";
    }
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" || extension == ".tga") {
        return "Image";
    }
    return "File";
}

void InspectorPanel::draw_file(World* world, const Project* project, const Selection& selection) {
    if (selection.file != this->file_shown) {
        this->file_shown = selection.file;
        this->file_exists = false;
        this->file_is_directory = false;
        this->file_size = 0;
        if (project != nullptr && project->is_open()) {
            std::error_code error;
            const std::filesystem::path absolute = project->root / selection.file;
            this->file_exists = std::filesystem::exists(absolute, error) && !error;
            this->file_is_directory = this->file_exists && std::filesystem::is_directory(absolute, error) && !error;
            if (this->file_exists && !this->file_is_directory) {
                const std::uintmax_t size = std::filesystem::file_size(absolute, error);
                this->file_size = error ? 0 : static_cast<u64>(size);
            }
        }
    }

    const std::string name = selection.file.filename().string();
    const std::string folder = selection.file.parent_path().generic_string();
    ImGui::TextUnformatted(name.c_str());
    ImGui::TextDisabled("%s", file_kind(selection.file, this->file_is_directory));
    ImGui::TextDisabled("in %s", folder.empty() ? "(project root)" : folder.c_str());
    if (!this->file_exists) {
        ImGui::TextDisabled("(not found on disk)");
    } else if (!this->file_is_directory) {
        char size_text[32];
        UI::format_size(this->file_size, size_text, sizeof(size_text));
        ImGui::TextDisabled("%s", size_text);
    }

    const bool loaded = world != nullptr && selection.entity != 0 && world->alive(selection.entity);
    if (!loaded) {
        return;
    }
    ImGui::Spacing();
    if (ImGui::Button("Save")) {
        this->file_action = InspectorFileAction::SAVE;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        this->file_action = InspectorFileAction::RELOAD;
    }
    ImGui::SameLine();
    const char* entity_name = SCENE::name(*world, selection.entity);
    ImGui::TextDisabled("entity %s", entity_name != nullptr && entity_name[0] != '\0' ? entity_name : "(unnamed)");

    if (selection.entity != this->scratch_entity) {
        this->scratch_entity = selection.entity;
        this->scratch_count = 0;
    }
    ImGui::Separator();
    if (ImGui::BeginChild("components")) {
        this->draw_components(*world, selection.entity);
    }
    ImGui::EndChild();
}

void InspectorPanel::draw_header(World& world, const EntityId entity) {
    if (entity != this->name_entity) {
        const char* name = SCENE::name(world, entity);
        snprintf(this->name_buffer, sizeof(this->name_buffer), "%s", name != nullptr ? name : "");
        this->name_entity = entity;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##name", this->name_buffer, sizeof(this->name_buffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
        SCENE::set_name(world, entity, this->name_buffer);
    }
    if (!ImGui::IsItemActive()) {
        // Not being typed into: track renames made elsewhere.
        const char* name = SCENE::name(world, entity);
        snprintf(this->name_buffer, sizeof(this->name_buffer), "%s", name != nullptr ? name : "");
    }

    ImGui::TextDisabled("id %llu, generation %llu",
        static_cast<unsigned long long>(ECS::ENTITY_LOW(entity)),
        static_cast<unsigned long long>(entity >> ECS::ENTITY_BITS));
    const EntityId parent = SCENE::parent(world, entity);
    if (parent != 0) {
        const char* parent_name = SCENE::name(world, parent);
        if (parent_name != nullptr && parent_name[0] != '\0') {
            ImGui::TextDisabled("child of %s", parent_name);
        } else {
            ImGui::TextDisabled("child of entity %llu", static_cast<unsigned long long>(ECS::ENTITY_LOW(parent)));
        }
    }
}

void InspectorPanel::draw_components(World& world, const EntityId entity) {
    const EntityRecord* record = world.entity_index.get_record_alive(entity);
    if (record == nullptr || record->archetype == nullptr) {
        return;
    }
    // Copy the ids out: a draw function may not change the archetype, but
    // the Name field above already could have, and this keeps the walk
    // independent of the type's storage either way.
    const ArchetypeType& type = record->archetype->type;
    const usz id_count = type.id_count;
    Id ids[ECS::MAX_COMPONENT_ID];
    const usz count = id_count < ECS::MAX_COMPONENT_ID ? id_count : ECS::MAX_COMPONENT_ID;
    memcpy(ids, type.ids, count * sizeof(Id));

    struct Exposed {
        Id id;
        const Inspector* inspector;
    };
    Exposed exposed[ECS::MAX_COMPONENT_ID];
    usz exposed_count = 0;
    Id other[ECS::MAX_COMPONENT_ID];
    usz other_count = 0;
    const Id name_id = world.id<Name>();
    for (usz i = 0; i < count; i++) {
        const Id id = ids[i];
        if (id == name_id) {
            continue; // edited in the header
        }
        const Inspector* inspector = INSPECTOR::of(world, id);
        if (inspector != nullptr && inspector->draw != nullptr) {
            exposed[exposed_count++] = {id, inspector};
        } else {
            other[other_count++] = id;
        }
    }
    std::sort(exposed, exposed + exposed_count, [](const Exposed& a, const Exposed& b) {
        return a.inspector->order != b.inspector->order ? a.inspector->order < b.inspector->order : a.id < b.id;
    });

    for (usz i = 0; i < exposed_count; i++) {
        const Exposed& item = exposed[i];
        ImGui::PushID(static_cast<int>(item.id & 0x7fffffff));
        ImGui::PushID(static_cast<int>(item.id >> 32));
        if (ImGui::CollapsingHeader(item.inspector->name, ImGuiTreeNodeFlags_DefaultOpen)) {
            void* data = world.get(entity, item.id);
            if (data != nullptr) {
                InspectorContext ctx;
                ctx.world = &world;
                ctx.entity = entity;
                ctx.id = item.id;
                ctx.scratch_bytes = this->scratch_for(item.id, &ctx.scratch_fresh);
                if (item.inspector->draw(ctx, data)) {
                    world.modified(entity, item.id);
                }
            } else {
                ImGui::TextDisabled("(no data)");
            }
        }
        ImGui::PopID();
        ImGui::PopID();
    }

    if (other_count > 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("Other");
        for (usz i = 0; i < other_count; i++) {
            char label[2 * ENTITY_NAME_CAPACITY + 16];
            InspectorPanel::label_of_id(world, other[i], label, sizeof(label));
            ImGui::BulletText("%s", label);
        }
    }
}

void InspectorPanel::draw_add_component(World& world, const EntityId entity) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Add Component", ImVec2(-FLT_MIN, 0.0f))) {
        ImGui::OpenPopup("add_component");
    }
    if (!ImGui::BeginPopup("add_component")) {
        return;
    }

    // Every component with an Addable, by its Inspector header (or its
    // entity's Name when it has no Inspector), in Inspector order.
    struct Entry {
        Id id;
        u32 order;
        char label[2 * ENTITY_NAME_CAPACITY + 16];
    };
    Entry entries[ECS::MAX_COMPONENT_ID];
    usz count = 0;
    world.query<Addable>().each([&](const EntityId id, Addable&) {
        if (count == ECS::MAX_COMPONENT_ID) {
            return;
        }
        Entry& entry = entries[count++];
        entry.id = id;
        const Inspector* inspector = INSPECTOR::of(world, id);
        entry.order = inspector != nullptr ? inspector->order : 100;
        if (inspector != nullptr && inspector->name[0] != '\0') {
            snprintf(entry.label, sizeof(entry.label), "%s", inspector->name);
        } else {
            InspectorPanel::label_of_id(world, id, entry.label, sizeof(entry.label));
        }
    });
    std::sort(entries, entries + count, [](const Entry& a, const Entry& b) {
        if (a.order != b.order) {
            return a.order < b.order;
        }
        const int by_name = strcmp(a.label, b.label);
        return by_name != 0 ? by_name < 0 : a.id < b.id;
    });

    Id chosen = 0;
    if (count == 0) {
        ImGui::TextDisabled("(no addable components)");
    }
    for (usz i = 0; i < count; i++) {
        const Entry& entry = entries[i];
        ImGui::PushID(static_cast<int>(entry.id & 0x7fffffff));
        ImGui::PushID(static_cast<int>(entry.id >> 32));
        ImGui::BeginDisabled(!INSPECTOR::can_add(world, entity, entry.id));
        if (ImGui::MenuItem(entry.label)) {
            chosen = entry.id;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        ImGui::PopID();
    }
    ImGui::EndPopup();

    if (chosen != 0) {
        INSPECTOR::add(world, entity, chosen);
    }
}

void* InspectorPanel::scratch_for(const Id id, bool* fresh) {
    for (usz i = 0; i < this->scratch_count; i++) {
        if (this->scratch[i].id == id) {
            *fresh = false;
            return this->scratch[i].bytes;
        }
    }
    *fresh = true;
    if (this->scratch_count == SCRATCH_SLOTS) {
        memset(this->scratch_spill, 0, sizeof(this->scratch_spill));
        return this->scratch_spill;
    }
    Scratch& slot = this->scratch[this->scratch_count++];
    slot.id = id;
    memset(slot.bytes, 0, sizeof(slot.bytes));
    return slot.bytes;
}

// Text for one side of a pair or a plain id: a built-in's name, the
// entity's Name, or its number.
static void label_of_entity(World& world, const EntityId entity, char* out, const usz capacity) {
    switch (ECS::ENTITY_LOW(entity)) {
    case ECS::CHILD_OF:
        snprintf(out, capacity, "CHILD_OF");
        return;
    case ECS::IS_A:
        snprintf(out, capacity, "IS_A");
        return;
    default:
        break;
    }
    const char* name = SCENE::name(world, entity);
    if (name != nullptr && name[0] != '\0') {
        snprintf(out, capacity, "%s", name);
    } else {
        snprintf(out, capacity, "entity %llu", static_cast<unsigned long long>(ECS::ENTITY_LOW(entity)));
    }
}

void InspectorPanel::label_of_id(World& world, const Id id, char* out, const usz capacity) {
    if (ECS::IS_PAIR(id)) {
        char first[ENTITY_NAME_CAPACITY];
        char second[ENTITY_NAME_CAPACITY];
        label_of_entity(world, world.pair_first(id) != 0 ? world.pair_first(id) : ECS::PAIR_FIRST(id), first, sizeof(first));
        label_of_entity(world, world.pair_second(id) != 0 ? world.pair_second(id) : ECS::PAIR_SECOND(id), second, sizeof(second));
        snprintf(out, capacity, "(%s, %s)", first, second);
        return;
    }
    const char* name = SCENE::name(world, id);
    if (name != nullptr && name[0] != '\0') {
        snprintf(out, capacity, "%s", name);
    } else {
        snprintf(out, capacity, "component %llu", static_cast<unsigned long long>(ECS::ENTITY_LOW(id)));
    }
}
