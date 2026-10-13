#pragma once

#include "editor_highlight.hpp"
#include "engine/plugin.hpp"
#include "engine/scheduler/scheduler.hpp"

// The Viewport's selection highlight as a plugin (editor_highlight.hpp):
// build adds its outline chain and compiles its shaders (a failure is
// reported and the passes draw nothing until a reload succeeds); the
// "post_update" system hands it the selected entity and keeps the chain in
// front of the grid pass before the frame renders; the "pre_shutdown"
// system removes the chain while the renderer is alive. Requires
// RenderPlugin, and reads the selection off EditorUiPlugin when there is
// one, so it works with or without the UI.
//
// Add it after EditorGridPlugin: both order themselves in "post_update",
// the grid by keeping itself last, and running second lets the highlight
// settle in one frame rather than two.
struct EditorHighlightPlugin : Plugin {
    static constexpr const char* NAME = "editor_highlight";

    bool build(Engine& engine) override;

    EditorHighlight highlight;

private:
    static void update(SystemContext& ctx);
    static void detach(SystemContext& ctx);
};
