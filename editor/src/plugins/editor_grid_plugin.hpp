#pragma once

#include "editor_grid.hpp"
#include "engine/plugin.hpp"
#include "engine/scheduler/scheduler.hpp"

// The Viewport's ground grid as a plugin (editor_grid.hpp): build adds its
// graph pass and compiles its shader (a failure is reported and the pass
// draws nothing until a reload succeeds); the "post_update" system keeps
// the pass last and on the forward pass's depth before the frame renders;
// the "pre_shutdown" system removes the pass and the callback while the
// renderer is alive. Requires RenderPlugin.
struct EditorGridPlugin : Plugin {
    static constexpr const char* NAME = "editor_grid";

    bool build(Engine& engine) override;

    EditorGrid grid;

private:
    static void update(SystemContext& ctx);
    static void detach(SystemContext& ctx);
};
