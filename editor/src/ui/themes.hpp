#pragma once

// Editor color themes. Each one overwrites the current ImGui style in full
// (sizes, rounding and colors), so call exactly one after ImGui::CreateContext()
// and before the first frame.
namespace THEMES {

// Catppuccin Mocha: dark lavender-tinted palette with mauve / sapphire accents.
void catppuccin_mocha();

} // namespace THEMES
