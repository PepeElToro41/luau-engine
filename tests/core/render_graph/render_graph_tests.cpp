#include "support/test_support.hpp"

#include "engine/render_graph/render_graph.hpp"

#include <cstring>

TEST_CASE("render_graph/graph: init creates the backbuffer resources") {
    RenderGraph graph;
    graph.init();

    CHECK(graph.backbuffer.is_valid());
    CHECK(graph.backbuffer_depth.is_valid());
    CHECK(graph.backbuffer != graph.backbuffer_depth);
    CHECK(graph.find_resource("backbuffer") == graph.backbuffer);
    CHECK(graph.find_resource("backbuffer_depth") == graph.backbuffer_depth);

    const RenderResourceDesc* color = graph.resource(graph.backbuffer);
    REQUIRE(color != nullptr);
    CHECK(color->kind == RENDER_RESOURCE_BACKBUFFER);
    CHECK(color->aspect == RENDER_ASPECT_COLOR);
    const RenderResourceDesc* depth = graph.resource(graph.backbuffer_depth);
    REQUIRE(depth != nullptr);
    CHECK(depth->kind == RENDER_RESOURCE_BACKBUFFER_DEPTH);
    CHECK(depth->aspect == RENDER_ASPECT_DEPTH);

    SUBCASE("the backbuffer resources cannot be removed") {
        CHECK_FALSE(graph.remove_resource(graph.backbuffer));
        CHECK_FALSE(graph.remove_resource(graph.backbuffer_depth));
        CHECK(graph.resource(graph.backbuffer) != nullptr);
    }
    SUBCASE("the graph starts dirty") {
        CHECK(graph.dirty);
    }

    graph.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("render_graph/graph: resources are added, found and removed by name and handle") {
    RenderGraph graph;
    graph.init();
    graph.dirty = false;

    RenderResourceDesc desc;
    desc.aspect = RENDER_ASPECT_COLOR;
    desc.size_mode = RENDER_SIZE_RELATIVE;
    desc.scale = 0.5f;
    desc.kind = RENDER_RESOURCE_BACKBUFFER; // overwritten: add_resource makes transients
    const RenderResourceHandle scene = graph.add_resource("scene_color", desc);
    REQUIRE(scene.is_valid());
    CHECK(graph.dirty);

    const RenderResourceDesc* stored = graph.resource(scene);
    REQUIRE(stored != nullptr);
    CHECK(strcmp(stored->name, "scene_color") == 0);
    CHECK(stored->kind == RENDER_RESOURCE_TRANSIENT);
    CHECK(stored->scale == 0.5f);
    CHECK(graph.find_resource("scene_color") == scene);

    SUBCASE("duplicate, empty and overlong names are refused") {
        CHECK_FALSE(graph.add_resource("scene_color", desc).is_valid());
        CHECK_FALSE(graph.add_resource("", desc).is_valid());
        CHECK_FALSE(graph.add_resource(nullptr, desc).is_valid());
        char long_name[RENDER_GRAPH::NAME_MAX + 2];
        memset(long_name, 'x', sizeof(long_name) - 1);
        long_name[sizeof(long_name) - 1] = '\0';
        CHECK_FALSE(graph.add_resource(long_name, desc).is_valid());
    }
    SUBCASE("removing invalidates the handle and frees the name") {
        CHECK(graph.remove_resource(scene));
        CHECK(graph.resource(scene) == nullptr);
        CHECK_FALSE(graph.find_resource("scene_color").is_valid());
        CHECK_FALSE(graph.remove_resource(scene));

        const RenderResourceHandle again = graph.add_resource("scene_color", desc);
        CHECK(again.is_valid());
        CHECK(again != scene);
        CHECK(graph.resource(scene) == nullptr);
    }
    SUBCASE("unknown names and invalid handles find nothing") {
        CHECK_FALSE(graph.find_resource("nope").is_valid());
        CHECK_FALSE(graph.find_resource(nullptr).is_valid());
        CHECK(graph.resource(RenderResourceHandle{}) == nullptr);
    }

    graph.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("render_graph/graph: passes keep their order and can be moved") {
    RenderGraph graph;
    graph.init();

    const RenderPassHandle shadow = graph.add_pass("shadow", RENDER_PASS_DRAW_SCENE);
    const RenderPassHandle forward = graph.add_pass("forward", RENDER_PASS_DRAW_SCENE);
    const RenderPassHandle post = graph.add_pass("post", RENDER_PASS_FULLSCREEN);
    REQUIRE(shadow.is_valid());
    REQUIRE(forward.is_valid());
    REQUIRE(post.is_valid());
    REQUIRE(graph.order.count == 3);
    CHECK(graph.order[0] == shadow);
    CHECK(graph.order[1] == forward);
    CHECK(graph.order[2] == post);
    CHECK(graph.find_pass("forward") == forward);
    CHECK(graph.pass(post)->kind == RENDER_PASS_FULLSCREEN);
    CHECK(graph.pass(post)->enabled);

    SUBCASE("a duplicate name is refused") {
        CHECK_FALSE(graph.add_pass("forward", RENDER_PASS_CUSTOM).is_valid());
        CHECK(graph.order.count == 3);
    }
    SUBCASE("move before another pass") {
        graph.dirty = false;
        CHECK(graph.move_pass(post, shadow));
        CHECK(graph.dirty);
        REQUIRE(graph.order.count == 3);
        CHECK(graph.order[0] == post);
        CHECK(graph.order[1] == shadow);
        CHECK(graph.order[2] == forward);
    }
    SUBCASE("move to the end") {
        CHECK(graph.move_pass(shadow, RenderPassHandle{}));
        REQUIRE(graph.order.count == 3);
        CHECK(graph.order[0] == forward);
        CHECK(graph.order[1] == post);
        CHECK(graph.order[2] == shadow);
    }
    SUBCASE("moving before itself or a stale pass fails") {
        CHECK_FALSE(graph.move_pass(forward, forward));
        CHECK(graph.remove_pass(post));
        CHECK_FALSE(graph.move_pass(forward, post));
        CHECK_FALSE(graph.move_pass(post, forward));
        CHECK(graph.order.count == 2);
    }
    SUBCASE("removing drops the pass from the order and invalidates the handle") {
        CHECK(graph.remove_pass(forward));
        CHECK(graph.pass(forward) == nullptr);
        CHECK_FALSE(graph.find_pass("forward").is_valid());
        REQUIRE(graph.order.count == 2);
        CHECK(graph.order[0] == shadow);
        CHECK(graph.order[1] == post);
        CHECK_FALSE(graph.remove_pass(forward));
    }

    graph.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("render_graph/graph: attachments and inputs are edited through the structural setters") {
    RenderGraph graph;
    graph.init();
    RenderResourceDesc color_desc;
    const RenderResourceHandle a = graph.add_resource("a", color_desc);
    const RenderResourceHandle b = graph.add_resource("b", color_desc);
    RenderResourceDesc depth_desc;
    depth_desc.aspect = RENDER_ASPECT_DEPTH;
    const RenderResourceHandle d = graph.add_resource("d", depth_desc);
    const RenderPassHandle pass = graph.add_pass("pass", RENDER_PASS_DRAW_SCENE);
    graph.dirty = false;

    SUBCASE("color attachments append in order and may be replaced in place") {
        CHECK(graph.set_color_attachment(pass, 0, a, RENDER_LOAD_CLEAR, RENDER_STORE_STORE));
        CHECK(graph.dirty);
        CHECK(graph.set_color_attachment(pass, 1, b, RENDER_LOAD_LOAD, RENDER_STORE_DONT_CARE));
        const RenderPassDesc* desc = graph.pass(pass);
        REQUIRE(desc->color_count == 2);
        CHECK(desc->color[0].resource == a);
        CHECK(desc->color[1].resource == b);
        CHECK(desc->color[1].load == RENDER_LOAD_LOAD);
        CHECK(desc->color[1].store == RENDER_STORE_DONT_CARE);

        CHECK(graph.set_color_attachment(pass, 0, b, RENDER_LOAD_DONT_CARE, RENDER_STORE_STORE));
        CHECK(desc->color_count == 2);
        CHECK(desc->color[0].resource == b);
        CHECK(desc->color[0].load == RENDER_LOAD_DONT_CARE);

        SUBCASE("index must be at most color_count") {
            CHECK_FALSE(graph.set_color_attachment(pass, 3, a, RENDER_LOAD_CLEAR, RENDER_STORE_STORE));
            CHECK(desc->color_count == 2);
        }
        SUBCASE("remove shifts the rest down") {
            CHECK(graph.remove_color_attachment(pass, 0));
            REQUIRE(desc->color_count == 1);
            CHECK(desc->color[0].resource == b);
            CHECK(desc->color[0].load == RENDER_LOAD_LOAD);
            CHECK_FALSE(graph.remove_color_attachment(pass, 1));
        }
    }
    SUBCASE("a stale resource is refused") {
        CHECK(graph.remove_resource(b));
        CHECK_FALSE(graph.set_color_attachment(pass, 0, b, RENDER_LOAD_CLEAR, RENDER_STORE_STORE));
        CHECK_FALSE(graph.add_input(pass, b));
        CHECK_FALSE(graph.set_depth_attachment(pass, b, RENDER_LOAD_CLEAR, RENDER_STORE_STORE));
        CHECK(graph.pass(pass)->color_count == 0);
    }
    SUBCASE("depth attachment") {
        CHECK(graph.set_depth_attachment(pass, d, RENDER_LOAD_CLEAR, RENDER_STORE_DONT_CARE));
        const RenderPassDesc* desc = graph.pass(pass);
        CHECK(desc->has_depth);
        CHECK(desc->depth.resource == d);
        CHECK(desc->depth.clear[0] == 1.0f);
        CHECK(graph.clear_depth_attachment(pass));
        CHECK_FALSE(desc->has_depth);
        CHECK_FALSE(graph.clear_depth_attachment(pass));
    }
    SUBCASE("inputs are a set") {
        CHECK(graph.add_input(pass, a));
        CHECK_FALSE(graph.add_input(pass, a));
        CHECK(graph.add_input(pass, b));
        const RenderPassDesc* desc = graph.pass(pass);
        REQUIRE(desc->input_count == 2);
        CHECK(graph.remove_input(pass, a));
        REQUIRE(desc->input_count == 1);
        CHECK(desc->inputs[0] == b);
        CHECK_FALSE(desc->inputs[1].is_valid());
        CHECK_FALSE(graph.remove_input(pass, a));
    }
    SUBCASE("enabled only dirties on change") {
        CHECK(graph.set_enabled(pass, true));
        CHECK_FALSE(graph.dirty);
        CHECK(graph.set_enabled(pass, false));
        CHECK(graph.dirty);
        CHECK_FALSE(graph.pass(pass)->enabled);
    }
    SUBCASE("every setter fails on a stale pass") {
        CHECK(graph.remove_pass(pass));
        CHECK_FALSE(graph.set_color_attachment(pass, 0, a, RENDER_LOAD_CLEAR, RENDER_STORE_STORE));
        CHECK_FALSE(graph.set_depth_attachment(pass, d, RENDER_LOAD_CLEAR, RENDER_STORE_STORE));
        CHECK_FALSE(graph.add_input(pass, a));
        CHECK_FALSE(graph.set_enabled(pass, false));
        CHECK_FALSE(graph.set_draw_scene_tag(pass, "forward"));
        CHECK_FALSE(graph.set_constants(pass, nullptr, 0));
        CHECK_FALSE(graph.set_fullscreen_shader(pass, 1));
        CHECK_FALSE(graph.set_custom_callback(pass, "x"));
        CHECK_FALSE(graph.set_clear_target(pass, a));
        CHECK_FALSE(graph.set_blit(pass, a, b, true));
    }

    graph.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("render_graph/graph: live edits never dirty the graph") {
    RenderGraph graph;
    graph.init();
    RenderResourceDesc color_desc;
    const RenderResourceHandle a = graph.add_resource("a", color_desc);
    const RenderPassHandle pass = graph.add_pass("pass", RENDER_PASS_DRAW_SCENE);
    graph.set_color_attachment(pass, 0, a, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
    graph.set_depth_attachment(pass, graph.backbuffer_depth, RENDER_LOAD_CLEAR, RENDER_STORE_DONT_CARE);
    graph.dirty = false;
    const u32 version = graph.version;
    RenderPassDesc* desc = graph.pass(pass);

    const f32 red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    CHECK(graph.set_clear_color(pass, 0, red));
    CHECK(desc->color[0].clear[0] == 1.0f);
    CHECK_FALSE(graph.set_clear_color(pass, 1, red));

    CHECK(graph.set_clear_depth(pass, 0.0f, 3));
    CHECK(desc->depth.clear[0] == 0.0f);
    CHECK(desc->depth.clear_stencil == 3);

    const u32 constants[2] = {7, 9};
    CHECK(graph.set_constants(pass, constants, sizeof(constants)));
    CHECK(desc->constant_size == 8);
    CHECK(memcmp(desc->constants, constants, 8) == 0);
    CHECK_FALSE(graph.set_constants(pass, constants, RENDER_GRAPH::MAX_CONSTANT_BYTES + 1));
    CHECK(graph.set_constants(pass, nullptr, 0));
    CHECK(desc->constant_size == 0);

    CHECK(graph.set_draw_scene_tag(pass, "forward"));
    CHECK(strcmp(desc->tag, "forward") == 0);
    CHECK(desc->tag_hash != 0);
    CHECK_FALSE(graph.set_draw_scene_tag(pass, ""));

    CHECK(graph.set_fullscreen_shader(pass, 42));
    CHECK(desc->shader == 42);

    CHECK(graph.set_custom_callback(pass, "debug_lines"));
    CHECK(strcmp(desc->callback, "debug_lines") == 0);
    CHECK(desc->callback_hash != 0);

    CHECK_FALSE(graph.dirty);
    CHECK(graph.version == version);

    SUBCASE("a clear pass takes its values through the same setters") {
        const RenderPassHandle clear = graph.add_pass("clear", RENDER_PASS_CLEAR);
        CHECK(graph.set_clear_target(clear, a));
        CHECK(graph.set_clear_color(clear, 0, red));
        CHECK(graph.set_clear_depth(clear, 0.5f, 1));
        const RenderPassDesc* clear_desc = graph.pass(clear);
        CHECK(clear_desc->clear_target == a);
        CHECK(clear_desc->clear_color[0] == 1.0f);
        CHECK(clear_desc->clear_depth == 0.5f);
    }

    graph.free();
    CHECK_ARENA_CLEAN();
}
