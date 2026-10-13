#include "editor_grid.hpp"

#include "engine/ecs/world.hpp"
#include "engine/gpu/gpu.hpp"
#include "engine/render/components.hpp"
#include "engine/render/pipelines.hpp"
#include "engine/render/shader_library.hpp"
#include "engine/utils/hash.hpp"
#include "engine/utils/log.hpp"

static constexpr const char* GRID_PASS_NAME = "alpha";
static constexpr const char* GRID_CALLBACK_NAME = "editor_grid";
static constexpr const char* GRID_SHADER_NAME = "internal/grid_editor";
// The shader's `#pragma pass` tag: the grid's own, not the graph pass name.
static constexpr u64 GRID_PASS_TAG = HASH::fnv1a_str("grid_editor");

bool EditorGrid::init(Renderer& renderer) {
    this->renderer = &renderer;
    RenderGraph& graph = renderer.graph;

    this->pass = graph.add_pass(GRID_PASS_NAME, RENDER_PASS_CUSTOM);
    if (!this->pass.is_valid()) {
        LOG::error(LOG_CATEGORY::EDITOR, "editor grid: a pass named '%s' already exists", GRID_PASS_NAME);
        return false;
    }
    graph.set_color_attachment(this->pass, 0, graph.backbuffer, RENDER_LOAD_LOAD, RENDER_STORE_STORE);
    graph.set_depth_attachment(this->pass, graph.backbuffer_depth, RENDER_LOAD_LOAD, RENDER_STORE_DONT_CARE);
    graph.set_custom_callback(this->pass, GRID_CALLBACK_NAME);
    graph.set_enabled(this->pass, this->enabled);
    RENDERER::register_custom_pass(renderer, GRID_CALLBACK_NAME, &EditorGrid::draw, this);
    this->update(renderer);

    this->shader = SHADER_LIBRARY::load(renderer, GRID_SHADER_NAME);
    return this->shader != 0;
}

void EditorGrid::shutdown(Renderer& renderer) {
    RENDERER::unregister_custom_pass(renderer, GRID_CALLBACK_NAME);
    if (this->pass.is_valid()) {
        renderer.graph.remove_pass(this->pass);
        this->pass = RenderPassHandle{};
    }
    this->renderer = nullptr;
}

void EditorGrid::update(Renderer& renderer) {
    RenderGraph& graph = renderer.graph;
    RenderPassDesc* grid = graph.pass(this->pass);
    if (grid == nullptr) {
        return;
    }
    // Every edit below compares first: the setters mark the graph dirty
    // and a recompile every frame is not wanted.
    if (graph.order.count > 0 && graph.order[graph.order.count - 1] != this->pass) {
        graph.move_pass(this->pass, RenderPassHandle{});
    }
    const RenderPassDesc* forward = graph.pass(renderer.default_forward);
    if (forward == nullptr || !forward->has_depth) {
        return;
    }
    if (forward->depth.store != RENDER_STORE_STORE) {
        graph.set_depth_attachment(renderer.default_forward, forward->depth.resource, forward->depth.load, RENDER_STORE_STORE);
    }
    if (grid->depth.resource != forward->depth.resource) {
        graph.set_depth_attachment(this->pass, forward->depth.resource, RENDER_LOAD_LOAD, RENDER_STORE_DONT_CARE);
    }
}

void EditorGrid::set_enabled(Renderer& renderer, const bool enabled) {
    this->enabled = enabled;
    renderer.graph.set_enabled(this->pass, enabled);
}

void EditorGrid::draw(const RenderPassContext& ctx, void* user_data) {
    EditorGrid& grid = *static_cast<EditorGrid*>(user_data);
    Renderer* renderer = grid.renderer;
    if (renderer == nullptr || renderer->world == nullptr || grid.shader == 0 || !ctx.in_render_pass) {
        return;
    }
    const Shader* shader = renderer->world->get<Shader>(grid.shader);
    if (shader == nullptr) {
        return;
    }
    const GpuPipeline pipeline = PIPELINES::get(*renderer, grid.shader, *shader, GRID_PASS_TAG, ctx.formats, nullptr);
    if (!pipeline.is_valid()) {
        return;
    }
    GPU::cmd_bind_pipeline(ctx.cmd, pipeline);
    GPU::cmd_bind_group(ctx.cmd, 0, ctx.frame_group);
    GPU::cmd_draw(ctx.cmd, 6);
}
