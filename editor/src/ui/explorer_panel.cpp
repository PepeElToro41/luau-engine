#include "ui/explorer_panel.hpp"

#include "engine/ecs/world.hpp"
#include "engine/render/components.hpp"
#include "engine/scene/scene.hpp"
#include "ui/panels.hpp"

#include <cfloat>
#include <cstdint>
#include <cstdio>

void ExplorerPanel::draw(bool* open, World* world, Scene* scene, Selection& selection) {
    if (!ImGui::Begin(PANELS::EXPLORER, open)) {
        ImGui::End();
        return;
    }

    this->filter.Draw("##filter", -FLT_MIN);
    ImGui::Separator();

    if (world != nullptr && selection.is_entity() && !world->alive(selection.entity)) {
        selection.clear();
    }

    if (ImGui::BeginChild("tree")) {
        if (world == nullptr || scene == nullptr || !world->alive(scene->root)) {
            ImGui::TextDisabled("(no world loaded)");
        } else {
            DynamicArray<EntityId> stack;
            this->draw_entity(*world, scene->root, true, true, stack, selection);
            // Editor entities: outside the scene, so outside its tree. While
            // filtering they follow the same rule as any node, and the
            // separator goes with them.
            if (this->editor_camera != 0 && world->alive(this->editor_camera)
                && (!this->filter.IsActive() || this->subtree_matches(*world, this->editor_camera, stack))) {
                ImGui::Separator();
                this->draw_entity(*world, this->editor_camera, false, false, stack, selection);
            }
            stack.free();
            // Right-click on the empty space of the tree: a new entity at
            // the top level. The rows are items, so NoOpenOverItems leaves
            // them to their own menus.
            const ImGuiPopupFlags empty_flags = ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems;
            if (ImGui::BeginPopupContextWindow("##tree_context", empty_flags)) {
                if (ImGui::MenuItem("New Entity")) {
                    this->create_entity(*world, scene->root, selection);
                }
                ImGui::EndPopup();
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

void ExplorerPanel::label_of(World& world, const EntityId entity, char* out, const usz capacity) {
    const char* name = SCENE::name(world, entity);
    if (name != nullptr && name[0] != '\0') {
        snprintf(out, capacity, "%s", name);
    } else {
        snprintf(out, capacity, "entity %llu", static_cast<unsigned long long>(ECS::ENTITY_LOW(entity)));
    }
}

bool ExplorerPanel::subtree_matches(World& world, const EntityId entity, DynamicArray<EntityId>& stack) {
    char label[ENTITY_NAME_CAPACITY + 16];
    ExplorerPanel::label_of(world, entity, label, sizeof(label));
    if (this->filter.PassFilter(label)) {
        return true;
    }
    const usz start = stack.count;
    const usz count = SCENE::children(world, entity, stack);
    bool found = false;
    for (usz i = 0; i < count && !found; i++) {
        found = this->subtree_matches(world, stack[start + i], stack);
    }
    stack.count = start;
    return found;
}

void ExplorerPanel::create_entity(World& world, const EntityId parent, Selection& selection) {
    const EntityId entity = SCENE::spawn(world, "New Entity", parent);
    if (entity == 0) {
        return;
    }
    // An empty scene entity still has a place in the world.
    world.set(entity, Transform{});
    selection.select_entity(entity);
    this->reveal = parent;
}

void ExplorerPanel::draw_entity(World& world, const EntityId entity, const bool is_root, const bool in_scene, DynamicArray<EntityId>& stack, Selection& selection) {
    const bool filtering = this->filter.IsActive();
    char label[ENTITY_NAME_CAPACITY + 16];
    ExplorerPanel::label_of(world, entity, label, sizeof(label));

    // While filtering, a node is listed if it or something below it matches,
    // and opened so the match is reachable. The root always stays.
    if (filtering && !is_root && !this->subtree_matches(world, entity, stack)) {
        return;
    }

    const usz start = stack.count;
    const usz child_count = SCENE::children(world, entity, stack);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (child_count == 0) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (is_root) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    if (selection.is_entity() && selection.entity == entity) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (filtering && child_count > 0) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }
    if (this->reveal == entity) {
        // A child was just created under this node: show it.
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        this->reveal = 0;
    }

    // The entity id is the ImGui id, so renames keep the node's open state.
    const bool node_open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entity)), flags, "%s", label);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selection.select_entity(entity);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("id %llu, generation %llu",
            static_cast<unsigned long long>(ECS::ENTITY_LOW(entity)),
            static_cast<unsigned long long>(entity >> ECS::ENTITY_BITS));
    }
    // Right-click: the row's context menu (the tree node's id). Opening it
    // also selects the row, like the Asset Browser does.
    if (in_scene && ImGui::BeginPopupContextItem()) {
        if (!selection.is_entity() || selection.entity != entity) {
            selection.select_entity(entity);
        }
        if (ImGui::MenuItem("New Entity")) {
            this->create_entity(world, entity, selection);
        }
        ImGui::EndPopup();
    }

    if (node_open && child_count > 0) {
        for (usz i = 0; i < child_count; i++) {
            // stack[start + i] stays put: deeper levels only append past it
            // and truncate back before returning.
            this->draw_entity(world, stack[start + i], false, in_scene, stack, selection);
        }
        ImGui::TreePop();
    }
    stack.count = start;
}
