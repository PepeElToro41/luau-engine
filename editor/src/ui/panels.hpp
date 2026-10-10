#pragma once

// Window titles double as ImGui ids and as the dock targets in dock_layout.
// Keep them in one place so renaming a panel cannot break the layout.
namespace PANELS {

inline constexpr const char* OUTPUT = "Output";
inline constexpr const char* EXPLORER = "Explorer";
inline constexpr const char* INSPECTOR = "Inspector";
inline constexpr const char* VIEWPORT = "Viewport";
inline constexpr const char* STATS = "Stats";
inline constexpr const char* ASSET_BROWSER = "Asset Browser";
// The Import panel's title changes to Reimport for a kept original; the
// part after ### is the id, shared so the window keeps its place.
inline constexpr const char* IMPORT = "Import###Import";
inline constexpr const char* REIMPORT = "Reimport###Import";

} // namespace PANELS
