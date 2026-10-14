#include "plugins/editor_grid_plugin.hpp"

#include "engine/engine.hpp"
#include "engine/plugins/render_plugin.hpp"
#include "engine/render/renderer.hpp"
#include "engine/utils/log.hpp"

bool EditorGridPlugin::build(Engine& engine) {
    if (engine.require<RenderPlugin>() == nullptr) {
        return false;
    }
    Renderer* renderer = engine.get_singleton<Renderer>();
    if (!this->grid.init(*renderer)) {
        LOG::warning(LOG_CATEGORY::EDITOR, "The grid shader did not load; see the messages above");
    }
    engine.add_system(engine.phases.post_update, EditorGridPlugin::NAME, &EditorGridPlugin::update, this);
    engine.add_system(engine.phases.pre_shutdown, EditorGridPlugin::NAME, &EditorGridPlugin::detach, this);
    return true;
}

// The grid is an authoring tool: its pass lives in the root renderer's
// graph, never in a play instance's, so the Viewport shows the game without
// it while playing.
void EditorGridPlugin::update(SystemContext& ctx) {
    EditorGridPlugin* self = static_cast<EditorGridPlugin*>(ctx.user_data);
    if (Renderer* renderer = ENGINE::of(ctx).root.get<Renderer>()) {
        self->grid.update(*renderer);
    }
}

void EditorGridPlugin::detach(SystemContext& ctx) {
    EditorGridPlugin* self = static_cast<EditorGridPlugin*>(ctx.user_data);
    if (Renderer* renderer = ENGINE::of(ctx).root.get<Renderer>()) {
        self->grid.shutdown(*renderer);
    }
}
