#include "engine/render/demo_scene.hpp"

#include "engine/ecs/world.hpp"
#include "engine/engine.hpp"
#include "engine/render/components.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/scene.hpp"

#include <cstdio>

bool RENDER_DEMO::spawn(Engine& engine) {
    World* world = engine.get_singleton<World>();
    Scene* scene = engine.get_singleton<Scene>();
    Renderer* renderer = engine.get_singleton<Renderer>();
    if (world == nullptr || scene == nullptr || renderer == nullptr) {
        return false;
    }

    const EntityId unlit = SHADER_LIBRARY::load(*renderer, "unlit");
    if (unlit == 0) {
        return false;
    }
    const EntityId orange = MATERIAL::create(*renderer, unlit);
    MATERIAL::set_vec4(*renderer, orange, "color", Vector4(1.0f, 0.5f, 0.15f, 1.0f));

    const EntityId camera = scene->spawn("camera");
    Transform camera_transform;
    camera_transform.position = Vector3(0.0f, 1.5f, 4.5f);
    // Look at the origin: the camera's -Z must point from the eye to it.
    camera_transform.rotation = Quaternion::from_to(Vector3::forward(), (Vector3::zero() - camera_transform.position).normalized());
    world->set(camera, camera_transform);
    world->set(camera, Camera{});

    // One of each primitive in a row, the cube in the middle. The shapes
    // are unit-sized; the Transform's scale makes the cylinder taller.
    const EntityId cube = scene->spawn("cube");
    Transform cube_transform;
    cube_transform.rotation = Quaternion::rotation_y(MATH::radians(30.0f));
    world->set(cube, cube_transform);
    world->set(cube, PrimitiveRenderer{PRIMITIVE_CUBE, orange});

    const EntityId sphere = scene->spawn("sphere");
    Transform sphere_transform;
    sphere_transform.position = Vector3(-1.75f, 0.0f, 0.0f);
    world->set(sphere, sphere_transform);
    world->set(sphere, PrimitiveRenderer{PRIMITIVE_SPHERE, orange});

    const EntityId cylinder = scene->spawn("cylinder");
    Transform cylinder_transform;
    cylinder_transform.position = Vector3(1.75f, 0.0f, 0.0f);
    cylinder_transform.rotation = Quaternion::rotation_y(MATH::radians(-20.0f));
    cylinder_transform.scale = Vector3(1.0f, 1.5f, 1.0f);
    world->set(cylinder, cylinder_transform);
    world->set(cylinder, PrimitiveRenderer{PRIMITIVE_CYLINDER, orange});
    return true;
}

bool RENDER_DEMO::set_post_enabled(Engine& engine, const bool enabled) {
    Renderer* renderer = engine.get_singleton<Renderer>();
    if (renderer == nullptr) {
        return false;
    }
    RenderGraph& graph = renderer->graph;
    const RenderPassHandle forward = graph.find_pass("forward");
    if (!forward.is_valid()) {
        return false;
    }

    RenderResourceHandle scene_color = graph.find_resource("scene_color");
    RenderResourceHandle scene_depth = graph.find_resource("scene_depth");
    RenderPassHandle post = graph.find_pass("post");
    if (enabled) {
        if (!scene_color.is_valid()) {
            RenderResourceDesc color;
            color.aspect = RENDER_ASPECT_COLOR;
            scene_color = graph.add_resource("scene_color", color);
        }
        if (!scene_depth.is_valid()) {
            RenderResourceDesc depth;
            depth.aspect = RENDER_ASPECT_DEPTH;
            scene_depth = graph.add_resource("scene_depth", depth);
        }
        if (!post.is_valid()) {
            const EntityId passthrough = SHADER_LIBRARY::load(*renderer, "passthrough");
            if (passthrough == 0) {
                return false;
            }
            post = graph.add_pass("post", RENDER_PASS_FULLSCREEN);
            graph.set_fullscreen_shader(post, passthrough);
            graph.add_input(post, scene_color);
            graph.set_color_attachment(post, 0, graph.backbuffer, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        }
        graph.set_color_attachment(forward, 0, scene_color, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        graph.set_clear_color(forward, 0, renderer->clear_color);
        graph.set_depth_attachment(forward, scene_depth, RENDER_LOAD_CLEAR, RENDER_STORE_DONT_CARE);
        graph.set_enabled(post, true);
    } else {
        graph.set_color_attachment(forward, 0, graph.backbuffer, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
        graph.set_clear_color(forward, 0, renderer->clear_color);
        graph.set_depth_attachment(forward, graph.backbuffer_depth, RENDER_LOAD_CLEAR, RENDER_STORE_DONT_CARE);
        if (post.is_valid()) {
            graph.set_enabled(post, false);
        }
    }
    return true;
}

bool RENDER_DEMO::is_post_enabled(Engine& engine) {
    Renderer* renderer = engine.get_singleton<Renderer>();
    if (renderer == nullptr) {
        return false;
    }
    const RenderPassDesc* post = renderer->graph.pass(renderer->graph.find_pass("post"));
    return post != nullptr && post->enabled;
}

bool RENDER_DEMO::set_shadow_enabled(Engine& engine, const bool enabled) {
    Renderer* renderer = engine.get_singleton<Renderer>();
    if (renderer == nullptr) {
        return false;
    }
    RenderGraph& graph = renderer->graph;
    const RenderPassHandle forward = graph.find_pass("forward");
    if (!forward.is_valid()) {
        return false;
    }
    RenderResourceHandle shadow_map = graph.find_resource("shadow_map");
    RenderPassHandle shadow = graph.find_pass("shadow");
    if (enabled) {
        if (!shadow_map.is_valid()) {
            RenderResourceDesc desc;
            desc.aspect = RENDER_ASPECT_DEPTH;
            desc.size_mode = RENDER_SIZE_ABSOLUTE;
            desc.width = 1024;
            desc.height = 1024;
            shadow_map = graph.add_resource("shadow_map", desc);
        }
        if (!shadow.is_valid()) {
            shadow = graph.add_pass("shadow", RENDER_PASS_DRAW_SCENE);
            graph.set_draw_scene_tag(shadow, "shadow");
            graph.set_depth_attachment(shadow, shadow_map, RENDER_LOAD_CLEAR, RENDER_STORE_STORE);
            graph.move_pass(shadow, forward);
        }
        graph.set_enabled(shadow, true);
        graph.add_input(forward, shadow_map);
    } else {
        if (shadow.is_valid()) {
            graph.set_enabled(shadow, false);
        }
        graph.remove_input(forward, shadow_map);
    }
    return true;
}

bool RENDER_DEMO::is_shadow_enabled(Engine& engine) {
    Renderer* renderer = engine.get_singleton<Renderer>();
    if (renderer == nullptr) {
        return false;
    }
    const RenderPassDesc* shadow = renderer->graph.pass(renderer->graph.find_pass("shadow"));
    return shadow != nullptr && shadow->enabled;
}

bool RENDER_DEMO::set_texture(Engine& engine, const AssetGuid& texture) {
    World* world = engine.get_singleton<World>();
    Renderer* renderer = engine.get_singleton<Renderer>();
    if (world == nullptr || renderer == nullptr) {
        return false;
    }
    const EntityId textured = SHADER_LIBRARY::load(*renderer, "textured");
    if (textured == 0) {
        return false;
    }
    const EntityId material = MATERIAL::create(*renderer, textured);
    MATERIAL::set_vec4(*renderer, material, "tint", Vector4::one());
    MATERIAL::set_texture(*renderer, material, "albedo", texture);
    world->query<Transform, MeshRenderer>().each([&](const EntityId, Transform&, MeshRenderer& mesh_renderer) { mesh_renderer.material = material; });
    world->query<Transform, PrimitiveRenderer>().each([&](const EntityId, Transform&, PrimitiveRenderer& primitive) { primitive.material = material; });
    return true;
}

bool RENDER_DEMO::set_material(Engine& engine, const AssetGuid& asset) {
    World* world = engine.get_singleton<World>();
    Renderer* renderer = engine.get_singleton<Renderer>();
    if (world == nullptr || renderer == nullptr) {
        return false;
    }
    const EntityId material = MATERIAL::load(*renderer, asset);
    if (material == 0) {
        return false;
    }
    world->query<Transform, MeshRenderer>().each([&](const EntityId, Transform&, MeshRenderer& mesh_renderer) { mesh_renderer.material = material; });
    world->query<Transform, PrimitiveRenderer>().each([&](const EntityId, Transform&, PrimitiveRenderer& primitive) { primitive.material = material; });
    return true;
}
