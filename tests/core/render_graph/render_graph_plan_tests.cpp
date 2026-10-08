#include "support/test_support.hpp"

#include "engine/render_graph/render_graph_plan.hpp"

#include <cstring>

namespace {

// Arbitrary stand-ins for VkFormat values.
constexpr RenderFormat COLOR_FORMAT = 44;   // "B8G8R8A8_SRGB"
constexpr RenderFormat DEPTH_FORMAT = 126;  // "D32_SFLOAT"
constexpr RenderFormat OTHER_FORMAT = 97;   // "R16G16B16A16_SFLOAT"

RenderBackbufferInfo backbuffer_info(const u32 width = 1280, const u32 height = 720) {
    RenderBackbufferInfo info;
    info.color_format = COLOR_FORMAT;
    info.depth_format = DEPTH_FORMAT;
    info.width = width;
    info.height = height;
    return info;
}

// The default graph the renderer builds: one forward pass into the target.
RenderPassHandle add_backbuffer_pass(RenderGraph& graph, const char* name = "forward") {
    const RenderPassHandle pass = graph.add_pass(name, RENDER_PASS_DRAW_SCENE);
    graph.set_draw_scene_tag(pass, "forward");
    graph.set_color_attachment(pass, 0, graph.backbuffer, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
    graph.set_depth_attachment(pass, graph.backbuffer_depth, RENDER_LOAD_CLEAR, RENDER_STORE_DONT_CARE);
    return pass;
}

RenderResourceHandle add_color(RenderGraph& graph, const char* name, const f32 scale = 1.0f, const RenderFormat format = RENDER_FORMAT_BACKBUFFER) {
    RenderResourceDesc desc;
    desc.aspect = RENDER_ASPECT_COLOR;
    desc.format = format;
    desc.scale = scale;
    return graph.add_resource(name, desc);
}

RenderResourceHandle add_depth(RenderGraph& graph, const char* name) {
    RenderResourceDesc desc;
    desc.aspect = RENDER_ASPECT_DEPTH;
    return graph.add_resource(name, desc);
}

struct Compiled {
    RenderGraphPlan plan;
    DynamicArray<RenderDiagnostic> diagnostics;
    bool ok = false;

    void run(const RenderGraph& graph, const RenderBackbufferInfo& info = backbuffer_info()) {
        this->plan.init();
        this->ok = RENDER_GRAPH::compile(graph, info, this->plan, &this->diagnostics);
    }
    u32 errors() const {
        u32 n = 0;
        for (const RenderDiagnostic& d : this->diagnostics) {
            n += d.severity == RENDER_DIAG_ERROR;
        }
        return n;
    }
    u32 warnings() const { return static_cast<u32>(this->diagnostics.count) - this->errors(); }
    bool has_message(const char* text) const {
        for (const RenderDiagnostic& d : this->diagnostics) {
            if (strstr(d.message, text) != nullptr) {
                return true;
            }
        }
        return false;
    }
    void free() {
        this->plan.free();
        this->diagnostics.free();
    }
};

} // namespace

TEST_CASE("render_graph/plan: the default forward graph compiles to one borrowed pass") {
    RenderGraph graph;
    graph.init();
    const RenderPassHandle forward = add_backbuffer_pass(graph);

    Compiled c;
    c.run(graph);
    CHECK(c.ok);
    CHECK(c.diagnostics.count == 0);
    REQUIRE(c.plan.passes.count == 1);
    CHECK(c.plan.backbuffer_pass == 0);
    CHECK(c.plan.graph_version == graph.version);

    const PlannedPass& pass = c.plan.passes[0];
    CHECK(pass.handle == forward);
    CHECK(pass.is_raster);
    CHECK(pass.writes_backbuffer);
    CHECK(pass.width == 1280);
    CHECK(pass.height == 720);
    REQUIRE(pass.color_count == 1);
    CHECK(pass.color[0].format == COLOR_FORMAT);
    CHECK(pass.color[0].final == RENDER_STATE_EXTERNAL);
    CHECK(pass.has_depth);
    CHECK(pass.depth.format == DEPTH_FORMAT);
    CHECK(pass.before_count == 0);
    CHECK(pass.compat_key == RENDER_GRAPH::target_compat_key(backbuffer_info()));
    REQUIRE(c.plan.compat_keys.count == 1);
    CHECK(c.plan.compat_keys[0] == pass.compat_key);

    SUBCASE("the backbuffer resources are resolved from the backbuffer info") {
        const u32 color = c.plan.find_resource(graph.backbuffer);
        REQUIRE(color != RENDER_GRAPH::NO_RESOURCE);
        CHECK(c.plan.resources[color].format == COLOR_FORMAT);
        CHECK(c.plan.resources[color].width == 1280);
        CHECK(c.plan.resources[color].used);
        CHECK(c.plan.resources[color].usage == RENDER_USAGE_COLOR);
        const u32 depth = c.plan.find_resource(graph.backbuffer_depth);
        REQUIRE(depth != RENDER_GRAPH::NO_RESOURCE);
        CHECK(c.plan.resources[depth].format == DEPTH_FORMAT);
        CHECK(c.plan.resources[depth].usage == RENDER_USAGE_DEPTH);
    }
    SUBCASE("a backbuffer pass without depth is fine too") {
        graph.clear_depth_attachment(forward);
        c.run(graph);
        CHECK(c.ok);
        CHECK_FALSE(c.plan.passes[0].has_depth);
        // The target's render pass is the same with or without the depth
        // attachment in the graph, so the key is too.
        CHECK(c.plan.passes[0].compat_key == RENDER_GRAPH::target_compat_key(backbuffer_info()));
    }

    c.free();
    graph.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("render_graph/plan: the backbuffer must be written by exactly one pass") {
    RenderGraph graph;
    graph.init();
    Compiled c;

    SUBCASE("none") {
        graph.add_pass("nothing", RENDER_PASS_CUSTOM);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("no enabled pass writes the backbuffer"));
        CHECK(c.plan.passes.count == 0);
    }
    SUBCASE("a disabled writer does not count") {
        const RenderPassHandle forward = add_backbuffer_pass(graph);
        graph.set_enabled(forward, false);
        c.run(graph);
        CHECK_FALSE(c.ok);
    }
    SUBCASE("two") {
        add_backbuffer_pass(graph, "a");
        add_backbuffer_pass(graph, "b");
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("more than one pass writes the backbuffer"));
    }
    SUBCASE("the backbuffer pass must clear and store a single color attachment") {
        const RenderPassHandle forward = add_backbuffer_pass(graph);
        graph.set_color_attachment(forward, 0, graph.backbuffer, RENDER_LOAD_LOAD, RENDER_STORE_STORE);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("cleared and stored"));
    }
    SUBCASE("the backbuffer pass cannot have a second color attachment") {
        const RenderPassHandle forward = add_backbuffer_pass(graph);
        graph.set_color_attachment(forward, 1, add_color(graph, "extra"), RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        c.run(graph);
        CHECK_FALSE(c.ok);
    }
    SUBCASE("the backbuffer pass's depth must be backbuffer_depth, cleared, not stored") {
        const RenderPassHandle forward = add_backbuffer_pass(graph);
        graph.set_depth_attachment(forward, add_depth(graph, "own_depth"), RENDER_LOAD_CLEAR, RENDER_STORE_DONT_CARE);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("backbuffer_depth, cleared and not stored"));
        graph.set_depth_attachment(forward, graph.backbuffer_depth, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        c.run(graph);
        CHECK_FALSE(c.ok);
    }
    SUBCASE("backbuffer_depth is reserved for the backbuffer pass") {
        add_backbuffer_pass(graph);
        const RenderPassHandle shadow = graph.add_pass("shadow", RENDER_PASS_DRAW_SCENE);
        graph.set_draw_scene_tag(shadow, "shadow");
        graph.set_depth_attachment(shadow, graph.backbuffer_depth, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("belongs to the pass that draws the backbuffer"));
    }
    SUBCASE("the backbuffer cannot be read") {
        add_backbuffer_pass(graph);
        const RenderPassHandle post = graph.add_pass("post", RENDER_PASS_FULLSCREEN);
        graph.set_color_attachment(post, 0, add_color(graph, "x"), RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        graph.add_input(post, graph.backbuffer);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("the backbuffer cannot be read"));
    }
    SUBCASE("the backbuffer cannot be cleared or blitted") {
        add_backbuffer_pass(graph);
        const RenderPassHandle clear = graph.add_pass("clear", RENDER_PASS_CLEAR);
        graph.set_clear_target(clear, graph.backbuffer);
        c.run(graph);
        CHECK_FALSE(c.ok);
        graph.remove_pass(clear);
        const RenderPassHandle blit = graph.add_pass("blit", RENDER_PASS_BLIT);
        graph.set_blit(blit, add_color(graph, "x"), graph.backbuffer, true);
        c.run(graph);
        CHECK_FALSE(c.ok);
    }
    SUBCASE("a backbuffer with no size fails") {
        add_backbuffer_pass(graph);
        c.run(graph, backbuffer_info(0, 0));
        CHECK_FALSE(c.ok);
    }

    c.free();
    graph.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("render_graph/plan: attachment and input validation") {
    RenderGraph graph;
    graph.init();
    add_backbuffer_pass(graph);
    const RenderResourceHandle full = add_color(graph, "full");
    const RenderResourceHandle half = add_color(graph, "half", 0.5f);
    const RenderResourceHandle depth = add_depth(graph, "depth");
    const RenderPassHandle pass = graph.add_pass("gbuffer", RENDER_PASS_DRAW_SCENE);
    graph.set_draw_scene_tag(pass, "gbuffer");
    graph.move_pass(pass, graph.find_pass("forward"));
    Compiled c;

    SUBCASE("a stale resource handle is an error") {
        graph.set_color_attachment(pass, 0, full, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        graph.remove_resource(full);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("removed resource"));
    }
    SUBCASE("aspects must match the slot") {
        graph.set_color_attachment(pass, 0, depth, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("color attachment needs a color resource"));
        graph.remove_color_attachment(pass, 0);
        graph.set_color_attachment(pass, 0, full, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        graph.set_depth_attachment(pass, half, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("depth attachment needs a depth resource"));
    }
    SUBCASE("every attachment must have the same size") {
        graph.set_color_attachment(pass, 0, full, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        graph.set_color_attachment(pass, 1, half, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("same size"));
    }
    SUBCASE("a resource cannot be sampled and attached by one pass") {
        graph.set_color_attachment(pass, 0, full, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        graph.add_input(pass, full);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("sampled and attached"));
    }
    SUBCASE("draw and fullscreen passes need attachments and a draw pass needs a tag") {
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("no attachments"));
        graph.set_color_attachment(pass, 0, full, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        graph.pass(pass)->tag[0] = '\0';
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("no tag"));
    }
    SUBCASE("clear and blit passes take no attachments; compute is rejected") {
        const RenderPassHandle clear = graph.add_pass("clear", RENDER_PASS_CLEAR);
        graph.set_clear_target(clear, full);
        graph.set_color_attachment(clear, 0, half, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        graph.set_color_attachment(pass, 0, full, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("take no attachments"));
        graph.remove_pass(clear);
        graph.add_pass("compute", RENDER_PASS_COMPUTE);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("compute"));
    }
    SUBCASE("a valid multi-attachment pass compiles with its own key and usage") {
        graph.set_color_attachment(pass, 0, full, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        const RenderResourceHandle normals = add_color(graph, "normals", 1.0f, OTHER_FORMAT);
        graph.set_color_attachment(pass, 1, normals, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        graph.set_depth_attachment(pass, depth, RENDER_LOAD_CLEAR, RENDER_STORE_DONT_CARE);
        c.run(graph);
        CHECK(c.ok);
        REQUIRE(c.plan.passes.count == 2);
        const PlannedPass& gbuffer = c.plan.passes[0];
        CHECK(gbuffer.handle == pass);
        CHECK_FALSE(gbuffer.writes_backbuffer);
        CHECK(gbuffer.color_count == 2);
        CHECK(gbuffer.compat_key == RENDER_GRAPH::pass_compat_key(gbuffer));
        CHECK(gbuffer.compat_key != c.plan.passes[1].compat_key);
        CHECK(c.plan.compat_keys.count == 2);
        CHECK(c.plan.resources[c.plan.find_resource(normals)].usage == RENDER_USAGE_COLOR);
        CHECK(c.plan.resources[c.plan.find_resource(depth)].usage == RENDER_USAGE_DEPTH);
        CHECK(c.plan.resources[c.plan.find_resource(half)].used == false);
    }

    c.free();
    graph.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("render_graph/plan: resources resolve their format and size from the backbuffer") {
    RenderGraph graph;
    graph.init();
    add_backbuffer_pass(graph);
    const RenderResourceHandle half = add_color(graph, "half", 0.5f);
    const RenderResourceHandle tiny = add_color(graph, "tiny", 0.0001f);
    const RenderResourceHandle depth = add_depth(graph, "depth");
    RenderResourceDesc absolute;
    absolute.size_mode = RENDER_SIZE_ABSOLUTE;
    absolute.width = 2048;
    absolute.height = 2048;
    absolute.format = OTHER_FORMAT;
    const RenderResourceHandle shadow = graph.add_resource("shadow", absolute);

    Compiled c;
    c.run(graph, backbuffer_info(1001, 601));
    REQUIRE(c.ok);

    const PlannedResource& half_resource = c.plan.resources[c.plan.find_resource(half)];
    CHECK(half_resource.format == COLOR_FORMAT);
    CHECK(half_resource.width == 501);
    CHECK(half_resource.height == 301);
    CHECK_FALSE(half_resource.used);

    const PlannedResource& tiny_resource = c.plan.resources[c.plan.find_resource(tiny)];
    CHECK(tiny_resource.width == 1);
    CHECK(tiny_resource.height == 1);

    const PlannedResource& depth_resource = c.plan.resources[c.plan.find_resource(depth)];
    CHECK(depth_resource.format == DEPTH_FORMAT);

    const PlannedResource& shadow_resource = c.plan.resources[c.plan.find_resource(shadow)];
    CHECK(shadow_resource.format == OTHER_FORMAT);
    CHECK(shadow_resource.width == 2048);
    CHECK(shadow_resource.height == 2048);

    SUBCASE("an unused resource with no size only fails once a pass uses it") {
        RenderResourceDesc empty;
        empty.size_mode = RENDER_SIZE_ABSOLUTE;
        const RenderResourceHandle zero = graph.add_resource("zero", empty);
        c.run(graph);
        CHECK(c.ok);
        const RenderPassHandle post = graph.add_pass("post", RENDER_PASS_FULLSCREEN);
        graph.set_color_attachment(post, 0, zero, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("no size"));
    }

    c.free();
    graph.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("render_graph/plan: a forward + post graph routes the scene image through sampled") {
    RenderGraph graph;
    graph.init();
    const RenderResourceHandle scene_color = add_color(graph, "scene_color");
    const RenderResourceHandle scene_depth = add_depth(graph, "scene_depth");

    const RenderPassHandle forward = graph.add_pass("forward", RENDER_PASS_DRAW_SCENE);
    graph.set_draw_scene_tag(forward, "forward");
    graph.set_color_attachment(forward, 0, scene_color, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
    graph.set_depth_attachment(forward, scene_depth, RENDER_LOAD_CLEAR, RENDER_STORE_DONT_CARE);

    const RenderPassHandle post = graph.add_pass("post", RENDER_PASS_FULLSCREEN);
    graph.set_fullscreen_shader(post, 1);
    graph.add_input(post, scene_color);
    graph.set_color_attachment(post, 0, graph.backbuffer, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);

    Compiled c;
    c.run(graph);
    REQUIRE(c.ok);
    CHECK(c.diagnostics.count == 0);
    REQUIRE(c.plan.passes.count == 2);
    CHECK(c.plan.backbuffer_pass == 1);

    const PlannedPass& p_forward = c.plan.passes[0];
    const PlannedPass& p_post = c.plan.passes[1];
    const u32 color_index = c.plan.find_resource(scene_color);

    SUBCASE("the forward pass ends its color in SAMPLED for the post pass") {
        CHECK(p_forward.color[0].final == RENDER_STATE_SAMPLED);
        CHECK(p_forward.color[0].initial == RENDER_STATE_UNDEFINED);
        CHECK(p_forward.depth.final == RENDER_STATE_DEPTH_ATTACHMENT);
        // The resource ends the frame sampled and so starts the next one there.
        CHECK(c.plan.resources[color_index].frame_end_state == RENDER_STATE_SAMPLED);
        CHECK(c.plan.resources[color_index].frame_start_state == RENDER_STATE_SAMPLED);
        // Since the render pass does the transition, the forward pass's
        // previous state is what last frame left: sampled.
        CHECK(p_forward.color[0].previous == RENDER_STATE_SAMPLED);
    }
    SUBCASE("the post pass requires its input sampled") {
        REQUIRE(p_post.before_count == 1);
        CHECK(p_post.before[0].resource == color_index);
        CHECK(p_post.before[0].state == RENDER_STATE_SAMPLED);
        REQUIRE(p_post.input_count == 1);
        CHECK(p_post.inputs[0] == color_index);
    }
    SUBCASE("usage is the union of the uses") {
        CHECK(c.plan.resources[color_index].usage == (RENDER_USAGE_COLOR | RENDER_USAGE_SAMPLED));
        CHECK(c.plan.resources[c.plan.find_resource(scene_depth)].usage == RENDER_USAGE_DEPTH);
    }
    SUBCASE("disabling the post pass drops it from the plan but keeps the resources") {
        graph.set_enabled(post, false);
        c.run(graph);
        CHECK_FALSE(c.ok); // nothing writes the backbuffer any more
        graph.set_enabled(post, true);
        graph.set_enabled(forward, false);
        graph.remove_input(post, scene_color);
        c.run(graph);
        CHECK(c.ok);
        CHECK(c.plan.passes.count == 1);
        CHECK(c.plan.resources.count == 4);
        CHECK_FALSE(c.plan.resources[color_index].used);
    }
    SUBCASE("reading before writing in the frame is a warning, not an error") {
        graph.move_pass(post, forward);
        c.run(graph);
        CHECK(c.ok);
        CHECK(c.warnings() >= 1);
        CHECK(c.has_message("before any pass writes it"));
        // post runs first now, reads what last frame left, and forward
        // ends the frame with the image sampled-ready again.
        CHECK(c.plan.passes[1].color[0].final == RENDER_STATE_COLOR_ATTACHMENT);
        CHECK(c.plan.resources[color_index].frame_end_state == RENDER_STATE_COLOR_ATTACHMENT);
        CHECK(c.plan.passes[0].before[0].state == RENDER_STATE_SAMPLED);
    }
    SUBCASE("a LOAD on first use starts from the frame end state") {
        graph.set_color_attachment(forward, 0, scene_color, RENDER_LOAD_LOAD, RENDER_STORE_STORE);
        c.run(graph);
        CHECK(c.ok);
        CHECK(c.has_message("loads contents no pass wrote"));
        CHECK(c.plan.passes[0].color[0].initial == RENDER_STATE_SAMPLED);
        CHECK(c.plan.passes[0].color[0].previous == RENDER_STATE_SAMPLED);
    }
    SUBCASE("a depth read later but not stored warns") {
        graph.add_input(post, scene_depth);
        c.run(graph);
        CHECK(c.ok);
        CHECK(c.has_message("read later but not stored"));
        CHECK(c.plan.passes[0].depth.final == RENDER_STATE_SAMPLED);
    }

    c.free();
    graph.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("render_graph/plan: clear and blit passes become explicit requirements") {
    RenderGraph graph;
    graph.init();
    const RenderResourceHandle a = add_color(graph, "a");
    const RenderResourceHandle b = add_color(graph, "b");

    const RenderPassHandle clear = graph.add_pass("clear", RENDER_PASS_CLEAR);
    graph.set_clear_target(clear, a);
    const RenderPassHandle blit = graph.add_pass("blit", RENDER_PASS_BLIT);
    graph.set_blit(blit, a, b, true);
    const RenderPassHandle post = graph.add_pass("post", RENDER_PASS_FULLSCREEN);
    graph.set_fullscreen_shader(post, 1);
    graph.add_input(post, b);
    graph.set_color_attachment(post, 0, graph.backbuffer, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);

    Compiled c;
    c.run(graph);
    REQUIRE(c.ok);
    REQUIRE(c.plan.passes.count == 3);
    const u32 ia = c.plan.find_resource(a);
    const u32 ib = c.plan.find_resource(b);

    const PlannedPass& p_clear = c.plan.passes[0];
    CHECK_FALSE(p_clear.is_raster);
    CHECK(p_clear.clear_target == ia);
    REQUIRE(p_clear.before_count == 1);
    CHECK(p_clear.before[0].state == RENDER_STATE_TRANSFER_DST);

    const PlannedPass& p_blit = c.plan.passes[1];
    CHECK(p_blit.blit_src == ia);
    CHECK(p_blit.blit_dst == ib);
    REQUIRE(p_blit.before_count == 2);
    CHECK(p_blit.before[0].resource == ia);
    CHECK(p_blit.before[0].state == RENDER_STATE_TRANSFER_SRC);
    CHECK(p_blit.before[1].resource == ib);
    CHECK(p_blit.before[1].state == RENDER_STATE_TRANSFER_DST);

    CHECK(c.plan.passes[2].before[0].state == RENDER_STATE_SAMPLED);
    CHECK(c.plan.resources[ia].usage == (RENDER_USAGE_TRANSFER_DST | RENDER_USAGE_TRANSFER_SRC));
    CHECK(c.plan.resources[ib].usage == (RENDER_USAGE_TRANSFER_DST | RENDER_USAGE_SAMPLED));
    CHECK(c.plan.compat_keys.count == 1);

    SUBCASE("blit needs two distinct resources of one aspect") {
        graph.set_blit(blit, a, a, true);
        c.run(graph);
        CHECK_FALSE(c.ok);
        graph.set_blit(blit, a, add_depth(graph, "d"), true);
        c.run(graph);
        CHECK_FALSE(c.ok);
        CHECK(c.has_message("same aspect"));
    }

    c.free();
    graph.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("render_graph/plan: an owned pass with the backbuffer's formats still has its own key") {
    // Same attachments as the target, but the backend builds this render
    // pass with its own dependencies: Vulkan does not consider the two
    // compatible, so the keys must differ.
    RenderGraph graph;
    graph.init();
    const RenderResourceHandle scene_color = add_color(graph, "scene_color");
    const RenderResourceHandle scene_depth = add_depth(graph, "scene_depth");
    const RenderPassHandle forward = graph.add_pass("forward", RENDER_PASS_DRAW_SCENE);
    graph.set_draw_scene_tag(forward, "forward");
    graph.set_color_attachment(forward, 0, scene_color, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
    graph.set_depth_attachment(forward, scene_depth, RENDER_LOAD_CLEAR, RENDER_STORE_DONT_CARE);
    const RenderPassHandle post = graph.add_pass("post", RENDER_PASS_FULLSCREEN);
    graph.set_fullscreen_shader(post, 1);
    graph.add_input(post, scene_color);
    graph.set_color_attachment(post, 0, graph.backbuffer, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);

    Compiled c;
    c.run(graph);
    REQUIRE(c.ok);
    REQUIRE(c.plan.passes.count == 2);
    CHECK(c.plan.passes[0].compat_key != c.plan.passes[1].compat_key);
    CHECK(c.plan.compat_keys.count == 2);

    SUBCASE("a change in what follows the pass changes its dependencies and its key") {
        const u64 sampled_key = c.plan.passes[0].compat_key;
        graph.remove_input(post, scene_color);
        c.run(graph);
        REQUIRE(c.ok);
        CHECK(c.plan.passes[0].color[0].final == RENDER_STATE_COLOR_ATTACHMENT);
        CHECK(c.plan.passes[0].compat_key != sampled_key);
    }

    c.free();
    graph.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("render_graph/plan: a depth-only shadow pass feeds the forward pass") {
    RenderGraph graph;
    graph.init();
    const RenderPassHandle forward = add_backbuffer_pass(graph);
    RenderResourceDesc desc;
    desc.aspect = RENDER_ASPECT_DEPTH;
    desc.size_mode = RENDER_SIZE_ABSOLUTE;
    desc.width = 1024;
    desc.height = 1024;
    const RenderResourceHandle shadow_map = graph.add_resource("shadow_map", desc);
    const RenderPassHandle shadow = graph.add_pass("shadow", RENDER_PASS_DRAW_SCENE);
    graph.set_draw_scene_tag(shadow, "shadow");
    graph.set_depth_attachment(shadow, shadow_map, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
    graph.move_pass(shadow, forward);
    graph.add_input(forward, shadow_map);

    Compiled c;
    c.run(graph);
    REQUIRE(c.ok);
    CHECK(c.diagnostics.count == 0);
    REQUIRE(c.plan.passes.count == 2);
    const PlannedPass& p_shadow = c.plan.passes[0];
    CHECK(p_shadow.is_raster);
    CHECK(p_shadow.color_count == 0);
    CHECK(p_shadow.has_depth);
    CHECK(p_shadow.width == 1024);
    CHECK(p_shadow.depth.format == DEPTH_FORMAT);
    CHECK(p_shadow.depth.final == RENDER_STATE_SAMPLED);
    CHECK(p_shadow.compat_key == RENDER_GRAPH::pass_compat_key(p_shadow));
    CHECK(p_shadow.compat_key != c.plan.passes[1].compat_key);
    const u32 index = c.plan.find_resource(shadow_map);
    CHECK(c.plan.resources[index].usage == (RENDER_USAGE_DEPTH | RENDER_USAGE_SAMPLED));
    REQUIRE(c.plan.passes[1].before_count == 1);
    CHECK(c.plan.passes[1].before[0].resource == index);
    CHECK(c.plan.passes[1].before[0].state == RENDER_STATE_SAMPLED);

    c.free();
    graph.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("render_graph/plan: compat_key depends only on the attachment formats") {
    const RenderFormat one[1] = {COLOR_FORMAT};
    const RenderFormat two[2] = {COLOR_FORMAT, OTHER_FORMAT};
    const RenderFormat swapped[2] = {OTHER_FORMAT, COLOR_FORMAT};
    CHECK(RENDER_GRAPH::compat_key(1, one, DEPTH_FORMAT) == RENDER_GRAPH::compat_key(1, one, DEPTH_FORMAT));
    CHECK(RENDER_GRAPH::compat_key(1, one, DEPTH_FORMAT) != RENDER_GRAPH::compat_key(1, one, RENDER_FORMAT_UNDEFINED));
    CHECK(RENDER_GRAPH::compat_key(2, two, DEPTH_FORMAT) != RENDER_GRAPH::compat_key(1, one, DEPTH_FORMAT));
    CHECK(RENDER_GRAPH::compat_key(2, two, DEPTH_FORMAT) != RENDER_GRAPH::compat_key(2, swapped, DEPTH_FORMAT));
    CHECK(RENDER_GRAPH::compat_key(0, nullptr, DEPTH_FORMAT) != RENDER_GRAPH::compat_key(0, nullptr, RENDER_FORMAT_UNDEFINED));
}

TEST_CASE("render_graph/plan: a custom pass is raster only with attachments") {
    RenderGraph graph;
    graph.init();
    add_backbuffer_pass(graph);
    const RenderPassHandle readback = graph.add_pass("readback", RENDER_PASS_CUSTOM);
    graph.set_custom_callback(readback, "readback");
    const RenderPassHandle lines = graph.add_pass("lines", RENDER_PASS_CUSTOM);
    graph.set_custom_callback(lines, "lines");
    graph.set_color_attachment(lines, 0, add_color(graph, "overlay"), RENDER_LOAD_LOAD, RENDER_STORE_STORE);

    Compiled c;
    c.run(graph);
    REQUIRE(c.ok);
    REQUIRE(c.plan.passes.count == 3);
    CHECK_FALSE(c.plan.passes[1].is_raster);
    CHECK(c.plan.passes[1].compat_key == 0);
    CHECK(c.plan.passes[2].is_raster);
    CHECK(c.plan.passes[2].compat_key != 0);
    // Passes after the backbuffer pass are allowed.
    CHECK(c.plan.backbuffer_pass == 0);

    c.free();
    graph.free();
    CHECK_ARENA_CLEAN();
}
