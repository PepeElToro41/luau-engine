#include "ui/dock_layout.hpp"

#include "ui/panels.hpp"

#include <imgui_internal.h>

namespace DOCK_LAYOUT {

ImGuiID submit_dockspace() {
    return ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_None);
}

void build_default(const ImGuiID dockspace) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);

    ImGuiID center = dockspace;
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);

    // The viewport is the central node: it absorbs resizes and keeps no tab
    // bar, so the scene fills it edge to edge.
    ImGuiDockNode* center_node = ImGui::DockBuilderGetNode(center);
    center_node->SetLocalFlags(ImGuiDockNodeFlags_CentralNode | ImGuiDockNodeFlags_NoTabBar);

    ImGui::DockBuilderDockWindow(PANELS::EXPLORER, left);
    ImGui::DockBuilderDockWindow(PANELS::VIEWPORT, center);
    ImGui::DockBuilderDockWindow(PANELS::OUTPUT, bottom);
    ImGui::DockBuilderDockWindow(PANELS::STATS, bottom);
    ImGui::DockBuilderFinish(dockspace);
}

bool is_unset(const ImGuiID dockspace) {
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
    return node == nullptr || node->IsLeafNode();
}

} // namespace DOCK_LAYOUT
