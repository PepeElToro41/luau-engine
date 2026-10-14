#include "engine/render/components.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/ecs/component_registry.hpp"
#include "engine/ecs/world.hpp"
#include "engine/ecs/world_clone.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/serialize.hpp"

#include <cstring>

static void shader_removed(World* world, const EntityId entity, Id, void* user_data) {
    Renderer* renderer = static_cast<Renderer*>(user_data);
    if (ECS::ENTITY_LOW(entity) < renderer->owns_from) {
        // Borrowed from the world this one was cloned from: the program and
        // the pipelines keyed by this id are the original's to free.
        return;
    }
    if (Shader* shader = world->get<Shader>(entity)) {
        PIPELINES::release_shader(*renderer, entity);
        shader->program.free();
    }
}

// A cloned Material gets its own parameter block: edits made while playing
// die with the play world and the authoring material is untouched.
static void material_clone(const void* source, void* destination, const usz size, void*) {
    memcpy(destination, source, size);
    Material* material = static_cast<Material*>(destination);
    const Material* original = static_cast<const Material*>(source);
    if (original->params != nullptr && original->param_size > 0) {
        material->params = MEMORY::heap_allocator()->allocate_array<u8>(original->param_size);
        memcpy(material->params, original->params, original->param_size);
    } else {
        material->params = nullptr;
        material->param_size = 0;
    }
}

static void material_removed(World* world, const EntityId entity, Id, void* user_data) {
    Renderer* renderer = static_cast<Renderer*>(user_data);
    if (Material* material = world->get<Material>(entity)) {
        MEMORY::heap_allocator()->free(material->params);
        material->params = nullptr;
        material->param_size = 0;
        if (!material->asset.is_null()) {
            const EntityId* loaded = renderer->materials.find(material->asset);
            if (loaded != nullptr && *loaded == entity) {
                renderer->materials.remove(material->asset);
            }
        }
    }
}

// A write to a Material that came from a file (the Inspector writing the
// block and calling World::modified, MATERIAL::set_*, set_shader) puts the
// file behind the entity: tag it AssetDirty so an editor's Save All finds
// it. Only CHANGED counts: the MOVED a set() raises is the load itself,
// and MATERIAL::load / reload / save clear the tag when file and entity
// agree again. The observer lives as long as the World.
static void material_changed(World* world, const EntityId entity, const ObserverEvent event, Id, void*) {
    if (event != OBSERVER_CHANGED) {
        return;
    }
    const Material* material = world->get<Material>(entity);
    if (material != nullptr && !material->asset.is_null()) {
        world->add<AssetDirty>(entity);
    }
}

// --- Serializers ---------------------------------------------------------------
//
// How the scene components read and write in a .scene file (see
// engine/scene/serialize.hpp). Rotations are saved as the quaternion so a
// round trip is lossless, the field of view in radians as stored, a
// primitive's shape by name and materials as entity references (the
// material's asset GUID, resolved by the Engine on load).

static void transform_write(SerializeContext&, const void* data, TextWriter& out) {
    const Transform* transform = static_cast<const Transform*>(data);
    SERIALIZE::write_vec3(out, "position", transform->position);
    SERIALIZE::write_quat(out, "rotation", transform->rotation);
    SERIALIZE::write_vec3(out, "scale", transform->scale);
}

static bool transform_read(SerializeContext&, void* data, const char* field, const usz field_length, const char* value, const usz value_length) {
    Transform* transform = static_cast<Transform*>(data);
    if (TEXT_ASSET::word_is(field, field_length, "position")) {
        return SERIALIZE::read_vec3(value, value_length, &transform->position);
    }
    if (TEXT_ASSET::word_is(field, field_length, "rotation")) {
        return SERIALIZE::read_quat(value, value_length, &transform->rotation);
    }
    if (TEXT_ASSET::word_is(field, field_length, "scale")) {
        return SERIALIZE::read_vec3(value, value_length, &transform->scale);
    }
    return false;
}

static void camera_write(SerializeContext&, const void* data, TextWriter& out) {
    const Camera* camera = static_cast<const Camera*>(data);
    SERIALIZE::write_f32(out, "fov_y", camera->fov_y);
    SERIALIZE::write_f32(out, "near_plane", camera->near_plane);
    SERIALIZE::write_f32(out, "far_plane", camera->far_plane);
}

static bool camera_read(SerializeContext&, void* data, const char* field, const usz field_length, const char* value, const usz value_length) {
    Camera* camera = static_cast<Camera*>(data);
    if (TEXT_ASSET::word_is(field, field_length, "fov_y")) {
        return SERIALIZE::read_f32(value, value_length, &camera->fov_y);
    }
    if (TEXT_ASSET::word_is(field, field_length, "near_plane")) {
        return SERIALIZE::read_f32(value, value_length, &camera->near_plane);
    }
    if (TEXT_ASSET::word_is(field, field_length, "far_plane")) {
        return SERIALIZE::read_f32(value, value_length, &camera->far_plane);
    }
    return false;
}

static void mesh_renderer_write(SerializeContext& ctx, const void* data, TextWriter& out) {
    const MeshRenderer* renderer = static_cast<const MeshRenderer*>(data);
    SERIALIZE::write_guid(out, "mesh", renderer->mesh);
    SERIALIZE::write_entity_ref(ctx, out, "material", renderer->material);
}

static bool mesh_renderer_read(SerializeContext& ctx, void* data, const char* field, const usz field_length, const char* value, const usz value_length) {
    MeshRenderer* renderer = static_cast<MeshRenderer*>(data);
    if (TEXT_ASSET::word_is(field, field_length, "mesh")) {
        return SERIALIZE::read_guid(value, value_length, &renderer->mesh);
    }
    if (TEXT_ASSET::word_is(field, field_length, "material")) {
        return SERIALIZE::read_entity_ref(ctx, value, value_length, &renderer->material);
    }
    return false;
}

static void primitive_renderer_write(SerializeContext& ctx, const void* data, TextWriter& out) {
    const PrimitiveRenderer* renderer = static_cast<const PrimitiveRenderer*>(data);
    SERIALIZE::write_word(out, "shape", PRIMITIVES::name(renderer->shape));
    SERIALIZE::write_entity_ref(ctx, out, "material", renderer->material);
}

static bool primitive_renderer_read(SerializeContext& ctx, void* data, const char* field, const usz field_length, const char* value, const usz value_length) {
    PrimitiveRenderer* renderer = static_cast<PrimitiveRenderer*>(data);
    if (TEXT_ASSET::word_is(field, field_length, "shape")) {
        // By name, never by number: the enum's values are an implementation detail.
        for (u32 i = 0; i < PRIMITIVE_COUNT; ++i) {
            const PrimitiveShape shape = static_cast<PrimitiveShape>(i);
            if (TEXT_ASSET::word_is(value, value_length, PRIMITIVES::name(shape))) {
                renderer->shape = shape;
                return true;
            }
        }
        return false;
    }
    if (TEXT_ASSET::word_is(field, field_length, "material")) {
        return SERIALIZE::read_entity_ref(ctx, value, value_length, &renderer->material);
    }
    return false;
}

void RENDER_COMPONENTS::register_components(World& world) {
    // Process-wide ids, so a world cloned from this one and this one agree
    // on every render component without copying anything over.
    COMPONENT_REGISTRY::preregister<Transform>("Transform");
    COMPONENT_REGISTRY::preregister<Camera>("Camera");
    COMPONENT_REGISTRY::preregister<RenderCamera>("RenderCamera");
    COMPONENT_REGISTRY::preregister<MeshRenderer>("MeshRenderer");
    COMPONENT_REGISTRY::preregister<PrimitiveRenderer>("PrimitiveRenderer");
    COMPONENT_REGISTRY::preregister<Shader>("Shader");
    COMPONENT_REGISTRY::preregister<Material>("Material");
    // Named so the editor can list them by name even where nothing draws them.
    SCENE::set_name(world, world.component<Transform>(), "Transform");
    SCENE::set_name(world, world.component<Camera>(), "Camera");
    SCENE::set_name(world, world.tag<RenderCamera>(), "RenderCamera");
    SCENE::set_name(world, world.component<MeshRenderer>(), "MeshRenderer");
    SCENE::set_name(world, world.component<PrimitiveRenderer>(), "PrimitiveRenderer");
    SCENE::set_name(world, world.component<Shader>(), "Shader");
    SCENE::set_name(world, world.component<Material>(), "Material");
    // Scene content, as a .scene file stores it. Shader and Material are not
    // scene content: a scene refers to a material by its asset GUID.
    SERIALIZE::expose<Transform>(world, "Transform", &transform_write, &transform_read, 0);
    SERIALIZE::expose<Camera>(world, "Camera", &camera_write, &camera_read, 10);
    SERIALIZE::expose_tag<RenderCamera>(world, "RenderCamera");
    SERIALIZE::expose<MeshRenderer>(world, "MeshRenderer", &mesh_renderer_write, &mesh_renderer_read, 20);
    SERIALIZE::expose<PrimitiveRenderer>(world, "PrimitiveRenderer", &primitive_renderer_write, &primitive_renderer_read, 20);
    // A Shader has no Cloner: the program is shared by the clone, which the
    // removed hook's owns_from check accounts for.
    CLONE::expose<Material>(world, &material_clone);
}

void RENDER_COMPONENTS::install_hooks(World& world, Renderer& renderer) {
    world.hook_removed<Shader>(shader_removed, &renderer);
    world.hook_removed<Material>(material_removed, &renderer);
    world.query<Material>().observe(material_changed);
}

void RENDER_COMPONENTS::register_all(World& world, Renderer& renderer) {
    RENDER_COMPONENTS::register_components(world);
    RENDER_COMPONENTS::install_hooks(world, renderer);
}
