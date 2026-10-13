#include "editor_highlight.hpp"

#include "engine/render/shader_library.hpp"
#include "engine/utils/log.hpp"

static constexpr const char* SELECTION_SHADER_NAME = "editor/selection";

bool EditorHighlight::init(Renderer& renderer) {
    this->renderer = &renderer;

    OutlineDesc desc;
    desc.name = "selection";
    desc.mask = &EditorHighlight::draw_mask;
    desc.mask_user_data = this;
    desc.max_width = 8;
    desc.behind = true;
    // Warm orange, and a quarter of it where the object is occluded.
    this->outline.color[0] = 1.0f;
    this->outline.color[1] = 0.55f;
    this->outline.color[2] = 0.1f;
    this->outline.color[3] = 1.0f;
    this->outline.behind_color[0] = 1.0f;
    this->outline.behind_color[1] = 0.55f;
    this->outline.behind_color[2] = 0.1f;
    this->outline.behind_color[3] = 0.25f;
    this->outline.width = 3.0f;
    // Nothing is selected yet.
    this->outline.enabled = false;
    const bool chain = OUTLINE::init(renderer, this->outline, desc);

    this->shader = SHADER_LIBRARY::load(renderer, SELECTION_SHADER_NAME);
    if (this->shader == 0) {
        LOG::error(LOG_CATEGORY::EDITOR, "selection highlight: render/%s.slang did not load", SELECTION_SHADER_NAME);
    }
    return chain && this->shader != 0;
}

void EditorHighlight::shutdown(Renderer& renderer) {
    OUTLINE::shutdown(renderer, this->outline);
    this->renderer = nullptr;
}

void EditorHighlight::update(Renderer& renderer, const EntityId selected, const RenderPassHandle before) {
    this->selected = selected;
    // Disabling the chain when nothing is selected recompiles the graph on
    // the click that empties the selection and on the one that fills it
    // again, which is cheaper than flooding the screen every frame for an
    // outline nobody asked for.
    OUTLINE::set_enabled(renderer, this->outline, this->enabled && selected != 0);
    OUTLINE::update(renderer, this->outline, before);
}

void EditorHighlight::set_enabled(Renderer& renderer, const bool enabled) {
    this->enabled = enabled;
    OUTLINE::set_enabled(renderer, this->outline, enabled && this->selected != 0);
}

void EditorHighlight::draw_mask(const RenderPassContext& ctx, void* user_data) {
    EditorHighlight& highlight = *static_cast<EditorHighlight*>(user_data);
    if (highlight.renderer == nullptr || highlight.selected == 0) {
        return;
    }
    OUTLINE::draw_mask(*highlight.renderer, ctx, highlight.shader, highlight.selected);
}
