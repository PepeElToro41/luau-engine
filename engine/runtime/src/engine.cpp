#include "engine/engine.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/asset/asset_reader.hpp"
#include "engine/asset/asset_types/scene_asset.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/render/components.hpp"
#include "engine/render/materials.hpp"
#include "engine/render/shader_library.hpp"
#include "engine/scene/scene.hpp"
#include "engine/utils/guid.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifndef ENGINE_RENDER_DIR
#define ENGINE_RENDER_DIR "render"
#endif

bool Engine::init(GpuContext* gpu) {
    this->gpu = gpu;
    this->time = 0.0;
    AssetResourceProvider* assets = this->create_singleton<AssetResourceProvider>();
    if (assets == nullptr) {
        return false;
    }
    World* world = this->create_singleton<World>();
    if (world == nullptr) {
        return false;
    }
    world->init();
    Scene* scene = this->create_singleton<Scene>();
    if (scene == nullptr) {
        return false;
    }
    scene->init(world);
    AssetEntities* asset_entities = this->create_singleton<AssetEntities>();
    if (asset_entities == nullptr) {
        return false;
    }
    asset_entities->init(world);
    FrameAllocator* frame_allocator = this->create_singleton<FrameAllocator>(ENGINE_FRAME_ARENA_BYTES);
    if (frame_allocator == nullptr) {
        return false;
    }
    Renderer* renderer = this->create_singleton<Renderer>();
    if (renderer == nullptr || !RENDERER::init(*renderer, gpu, world, assets, frame_allocator, ENGINE_RENDER_DIR)) {
        return false;
    }
    memcpy(renderer->clear_color, this->clear_color, sizeof(this->clear_color));
    RENDER_COMPONENTS::register_all(*world, *renderer);
    return true;
}

void Engine::shutdown() {
    // Singletons that own memory release it before the store destroys them.
    // The world first: its Shader and Material entities hand their
    // resources back through the removed hooks; then the renderer. The
    // asset entity index owns nothing in the world and goes first, so the
    // world's teardown does not keep it in step entity by entity.
    if (AssetEntities* asset_entities = this->get_singleton<AssetEntities>()) {
        asset_entities->free();
    }
    if (World* world = this->get_singleton<World>()) {
        world->free();
    }
    if (Renderer* renderer = this->get_singleton<Renderer>()) {
        RENDERER::shutdown(*renderer);
    }
    if (AssetResourceProvider* assets = this->get_singleton<AssetResourceProvider>()) {
        assets->free();
    }
    if (FrameAllocator* frame_allocator = this->get_singleton<FrameAllocator>()) {
        frame_allocator->release();
    }
    this->singletons.free();
    this->gpu = nullptr;
}

void Engine::update(const f32 dt) {
    // A new frame: last frame's scratch is gone.
    if (FrameAllocator* frame_allocator = this->get_singleton<FrameAllocator>()) {
        frame_allocator->reset();
    }
    this->time += dt;
    this->last_dt = dt;
}

AssetGuid Engine::load_asset_file(const char* path) {
    AssetResourceProvider* assets = this->get_singleton<AssetResourceProvider>();
    if (assets == nullptr || path == nullptr) {
        return AssetGuid{};
    }
    AssetView view = ASSET_FILE::read_prelude_any(path, MEMORY::heap_allocator());
    if (!view.is_ok()) {
        fprintf(stderr, "[assets] %s: %s\n", path, ASSET_FILE::parse_error_name(view.parse_error));
        ASSET_FILE::free_prelude(&view, MEMORY::heap_allocator());
        return AssetGuid{};
    }
    const AssetGuid guid = view.header->guid;
    if (assets->add(view, path) == nullptr) {
        fprintf(stderr, "[assets] %s: could not register (cooked-only provider, or duplicate GUID)\n", path);
        ASSET_FILE::free_prelude(&view, MEMORY::heap_allocator());
        return AssetGuid{};
    }
    // The asset is also an entity: created now, or found and brought up to
    // date (type, name) when the file was registered before.
    if (AssetEntities* asset_entities = this->get_singleton<AssetEntities>()) {
        asset_entities->add(view, path);
    }
    ASSET_FILE::free_prelude(&view, MEMORY::heap_allocator());
    return guid;
}

void Engine::render(FrameContext& frame) {
    Renderer* renderer = this->get_singleton<Renderer>();
    if (renderer == nullptr) {
        return;
    }
    memcpy(renderer->clear_color, this->clear_color, sizeof(this->clear_color));
    renderer->graph.set_clear_color(renderer->default_forward, 0, this->clear_color);
    renderer->time = static_cast<f32>(this->time);
    renderer->delta_time = this->last_dt;
    frame.target_state = RENDERER::render(*renderer, frame);
}

// --- Scenes -----------------------------------------------------------------------

EntityId Engine::resolve_asset(const AssetGuid& guid, void* user_data) {
    Engine* engine = static_cast<Engine*>(user_data);
    AssetEntities* assets = engine->get_singleton<AssetEntities>();
    World* world = engine->get_singleton<World>();
    Renderer* renderer = engine->get_singleton<Renderer>();
    if (assets == nullptr || world == nullptr) {
        return 0;
    }
    const EntityId entity = assets->find(guid);
    if (entity == 0 || renderer == nullptr) {
        return entity;
    }
    switch (ASSET_ENTITY::type_of(*world, entity)) {
    case ASSET_TYPE::MATERIAL:
        // The material lives on the asset entity itself once loaded; a
        // failed load is 0, which the reader reports.
        return MATERIAL::load(*renderer, guid);
    case ASSET_TYPE::SHADER: {
        // A shader asset is named after its file's stem, which is the name
        // the library loads it by.
        const char* name = SCENE::name(*world, entity);
        return name != nullptr ? SHADER_LIBRARY::load(*renderer, name) : 0;
    }
    default:
        return entity;
    }
}

void Engine::scene_warning(const char* text, void* user_data) {
    Engine* engine = static_cast<Engine*>(user_data);
    if (Renderer* renderer = engine->get_singleton<Renderer>()) {
        RENDERER::log(*renderer, RENDER_LOG_WARNING, "%s", text);
    } else {
        fprintf(stderr, "[scene] warning: %s\n", text);
    }
}

void Engine::serialize_context(SerializeContext& ctx) {
    ctx.world = this->get_singleton<World>();
    ctx.resolve_asset = &Engine::resolve_asset;
    ctx.warn = &Engine::scene_warning;
    ctx.user_data = this;
}

// A message about scenes: the renderer log when there is one (the editor
// shows it), stderr otherwise.
static void scene_log(Engine& engine, const RenderLogLevel level, const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (Renderer* renderer = engine.get_singleton<Renderer>()) {
        RENDERER::log(*renderer, level, "%s", text);
    } else {
        fprintf(stderr, "[scene] %s\n", text);
    }
}

const char* Engine::scene_text(const AssetGuid& guid, usz* out_size, const char** out_path) {
    *out_size = 0;
    *out_path = nullptr;
    AssetResourceProvider* assets = this->get_singleton<AssetResourceProvider>();
    char text_guid[TEXT_ASSET::GUID_TEXT_CAPACITY];
    TEXT_ASSET::format_guid(guid, text_guid);
    if (assets == nullptr) {
        scene_log(*this, RENDER_LOG_ERROR, "scene %s: no asset provider", text_guid);
        return nullptr;
    }
    AssetResource* resource = assets->get(guid);
    if (resource == nullptr) {
        scene_log(*this, RENDER_LOG_ERROR, "scene %s: not registered with the asset provider, or its file could not be read", text_guid);
        return nullptr;
    }
    if (resource->view.header->type != ASSET_TYPE::SCENE || !resource->is_text()) {
        scene_log(*this, RENDER_LOG_ERROR, "%s: not a .scene asset", resource->path);
        return nullptr;
    }
    const ChunkEntry* entry = nullptr;
    const u8* text = resource->find_payload(CHUNK_TYPE::TEXT, &entry);
    *out_size = entry != nullptr ? entry->size : 0;
    *out_path = resource->path;
    // An empty file has no payload buffer; a non-null pointer still means
    // "a scene with no text", which the reader rejects for its missing guid.
    return text != nullptr ? reinterpret_cast<const char*>(text) : "";
}

bool Engine::load_scene(const AssetGuid& guid) {
    World* world = this->get_singleton<World>();
    Scene* scene = this->get_singleton<Scene>();
    AssetResourceProvider* assets = this->get_singleton<AssetResourceProvider>();
    if (world == nullptr || scene == nullptr || assets == nullptr) {
        return false;
    }
    usz size = 0;
    const char* path = nullptr;
    const char* text = this->scene_text(guid, &size, &path);
    if (text == nullptr) {
        return false;
    }
    // The path outlives the read (the provider keeps it) but is copied
    // anyway: loading materials on the way registers nothing new, yet a
    // resolver is free to.
    char path_copy[ASSET_PATH_CAPACITY];
    snprintf(path_copy, sizeof(path_copy), "%s", path);
    scene->unload();
    SerializeContext ctx;
    this->serialize_context(ctx);
    ctx.path = path_copy;
    const SCENE_ASSET::SceneReadResult result = SCENE_ASSET::read(*world, text, size, scene->root, ctx);
    assets->unload(guid);
    if (!result.ok) {
        if (result.line != 0) {
            scene_log(*this, RENDER_LOG_ERROR, "%s:%u: %s", path_copy, result.line, result.error);
        } else {
            scene_log(*this, RENDER_LOG_ERROR, "%s: %s", path_copy, result.error);
        }
        scene->set_current(AssetGuid{}, nullptr);
        return false;
    }
    scene->set_current(guid, path_copy);
    scene_log(*this, RENDER_LOG_INFO, "Loaded scene %s: %llu entities%s", path_copy, static_cast<unsigned long long>(result.entities),
        result.warnings > 0 ? " (with warnings)" : "");
    return true;
}

bool Engine::instantiate_scene(const AssetGuid& guid, const EntityId parent) {
    World* world = this->get_singleton<World>();
    AssetResourceProvider* assets = this->get_singleton<AssetResourceProvider>();
    if (world == nullptr || assets == nullptr || !world->alive(parent)) {
        return false;
    }
    usz size = 0;
    const char* path = nullptr;
    const char* text = this->scene_text(guid, &size, &path);
    if (text == nullptr) {
        return false;
    }
    char path_copy[ASSET_PATH_CAPACITY];
    snprintf(path_copy, sizeof(path_copy), "%s", path);
    SerializeContext ctx;
    this->serialize_context(ctx);
    ctx.path = path_copy;
    const SCENE_ASSET::SceneReadResult result = SCENE_ASSET::read(*world, text, size, parent, ctx);
    assets->unload(guid);
    if (!result.ok) {
        if (result.line != 0) {
            scene_log(*this, RENDER_LOG_ERROR, "%s:%u: %s", path_copy, result.line, result.error);
        } else {
            scene_log(*this, RENDER_LOG_ERROR, "%s: %s", path_copy, result.error);
        }
        return false;
    }
    return true;
}

void Engine::write_scene(DynamicArray<char>& out) {
    World* world = this->get_singleton<World>();
    Scene* scene = this->get_singleton<Scene>();
    if (world == nullptr || scene == nullptr) {
        return;
    }
    SerializeContext ctx;
    this->serialize_context(ctx);
    SCENE_ASSET::write(*world, scene->root, scene->current, ctx, out);
}

bool Engine::save_scene(const char* path) {
    World* world = this->get_singleton<World>();
    Scene* scene = this->get_singleton<Scene>();
    if (world == nullptr || scene == nullptr || path == nullptr || path[0] == '\0') {
        return false;
    }
    const AssetGuid guid = scene->has_current() ? scene->current : GUID::random();
    SerializeContext ctx;
    this->serialize_context(ctx);
    ctx.path = path;
    if (!SCENE_ASSET::write_file(*world, scene->root, guid, ctx, path)) {
        scene_log(*this, RENDER_LOG_ERROR, "Could not write scene %s", path);
        return false;
    }
    // Registered (or refreshed) like any other asset, so the file is an
    // asset entity and a later load_scene finds it.
    if (this->load_asset_file(path).is_null()) {
        scene_log(*this, RENDER_LOG_WARNING, "Scene %s was written but could not be registered", path);
    }
    scene->set_current(guid, path);
    return true;
}

void Engine::new_scene() {
    if (Scene* scene = this->get_singleton<Scene>()) {
        scene->unload();
        scene->set_current(AssetGuid{}, nullptr);
    }
}
