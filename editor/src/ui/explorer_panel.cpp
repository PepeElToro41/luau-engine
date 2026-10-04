#include "ui/explorer_panel.hpp"

#include "ui/panels.hpp"

#include <cfloat>

static constexpr u64 ROOT_ID = 1;

void ExplorerPanel::draw(bool* open) {
    if (!ImGui::Begin(PANELS::EXPLORER, open)) {
        ImGui::End();
        return;
    }

    this->filter.Draw("##filter", -FLT_MIN);
    ImGui::Separator();

    if (ImGui::BeginChild("tree")) {
        ImGuiTreeNodeFlags root_flags = ImGuiTreeNodeFlags_OpenOnArrow
                                      | ImGuiTreeNodeFlags_DefaultOpen
                                      | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (this->selected == ROOT_ID) {
            root_flags |= ImGuiTreeNodeFlags_Selected;
        }

        const bool root_open = ImGui::TreeNodeEx("Scene", root_flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            this->selected = ROOT_ID;
        }
        if (root_open) {
            ImGui::TextDisabled("(no world loaded)");
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();

    ImGui::End();
}
