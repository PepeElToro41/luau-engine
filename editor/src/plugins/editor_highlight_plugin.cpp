#include "plugins/editor_highlight_plugin.hpp"

#include "engine/engine.hpp"
#include "engine/plugins/render_plugin.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/scene.hpp"
#include "engine/utils/log.hpp"
#include "plugins/editor_grid_plugin.hpp"
#include "plugins/editor_ui_plugin.hpp"

bool EditorHighlightPlugin::build(Engine& engine) {
    if (engine.require<RenderPlugin>() == nullptr) {
        return false;
    }
    Renderer* renderer = engine.get_singleton<Renderer>();
    if (!this->highlight.init(*renderer)) {
        LOG::warning(LOG_CATEGORY::EDITOR, "The selection outline did not load; see the messages above");
    }
    engine.add_system(engine.phases.post_update, EditorHighlightPlugin::NAME, &EditorHighlightPlugin::update, this);
    engine.add_system(engine.phases.pre_shutdown, EditorHighlightPlugin::NAME, &EditorHighlightPlugin::detach, this);
    return true;
}

void EditorHighlightPlugin::update(SystemContext& ctx) {
    EditorHighlightPlugin* self = static_cast<EditorHighlightPlugin*>(ctx.user_data);
    Engine& engine = ENGINE::of(ctx);
    // An authoring tool, like the grid: the root renderer's graph only.
    Renderer* renderer = engine.root.get<Renderer>();
    if (renderer == nullptr) {
        return;
    }
    // Only scene content is outlined: a picked file's Material entity and
    // the editor's own camera are not things the Viewport shows.
    EntityId selected = 0;
    if (const EditorUiPlugin* ui = engine.get_plugin<EditorUiPlugin>()) {
        if (ui->editor.selection.is_entity()) {
            selected = ui->editor.selection.entity;
        }
    }
    Scene* scene = engine.root.get<Scene>();
    if (selected != 0 && (scene == nullptr || !scene->contains(selected))) {
        selected = 0;
    }
    // In front of the grid, which keeps itself last.
    RenderPassHandle before;
    if (const EditorGridPlugin* grid = engine.get_plugin<EditorGridPlugin>()) {
        before = grid->grid.pass;
    }
    self->highlight.update(*renderer, selected, before);
}

void EditorHighlightPlugin::detach(SystemContext& ctx) {
    EditorHighlightPlugin* self = static_cast<EditorHighlightPlugin*>(ctx.user_data);
    if (Renderer* renderer = ENGINE::of(ctx).root.get<Renderer>()) {
        self->highlight.shutdown(*renderer);
    }
}
