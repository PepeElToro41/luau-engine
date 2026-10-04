#pragma once

#include <imgui.h>

namespace DOCK_LAYOUT {

// Submits the full-window dockspace and returns its id. Call once per frame
// before any panel is submitted.
ImGuiID submit_dockspace();

// Resets `dockspace` to the default arrangement:
//
//     +----------+-----------------------+
//     | Explorer |       Viewport        |
//     |          |                       |
//     +----------+-----------------------+
//     |         Output | Stats           |
//     +----------------------------------+
//
// Call right after submit_dockspace() in the frame the layout should apply;
// windows submitted later in that frame land in their slots.
void build_default(ImGuiID dockspace);

// True if `dockspace` has never been split (first run without an imgui.ini),
// in which case build_default() should be applied.
bool is_unset(ImGuiID dockspace);

} // namespace DOCK_LAYOUT
