#include "engine/engine.hpp"


#include "engine/asset/asset_entity.hpp"
#include "engine/asset/asset_reader.hpp"
#include "engine/asset/asset_types/scene_asset.hpp"
#include "engine/asset/text_asset.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/render/materials.hpp"
#include "engine/render/shader_library.hpp"
#include "engine/scene/scene.hpp"
#include "engine/utils/guid.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

// --- Lifetime ----------------------------------------------------------------------

void Engine::create_pipelines() {
    Scheduler& s = this->scheduler;
    this->pipelines.startup = s.add_pipeline(ENGINE_PIPELINE::STARTUP);
    this->pipelines.update = s.add_pipeline(ENGINE_PIPELINE::UPDATE);
    this->pipelines.render = s.add_pipeline(ENGINE_PIPELINE::RENDER);
    this->pipelines.shutdown = s.add_pipeline(ENGINE_PIPELINE::SHUTDOWN, PIPELINE_ORDER_REVERSE_INSERTION);

    this->phases.pre_startup = s.add_phase(this->pipelines.startup, ENGINE_PHASE::PRE_STARTUP);
    this->phases.startup = s.add_phase(this->pipelines.startup, ENGINE_PHASE::STARTUP);
    this->phases.post_startup = s.add_phase(this->pipelines.startup, ENGINE_PHASE::POST_STARTUP);
    s.phase_after(this->phases.startup, this->phases.pre_startup);
    s.phase_after(this->phases.post_startup, this->phases.startup);

    this->phases.events = s.add_phase(this->pipelines.update, ENGINE_PHASE::EVENTS);
    this->phases.pre_update = s.add_phase(this->pipelines.update, ENGINE_PHASE::PRE_UPDATE);
    this->phases.update = s.add_phase(this->pipelines.update, ENGINE_PHASE::UPDATE);
    this->phases.post_update = s.add_phase(this->pipelines.update, ENGINE_PHASE::POST_UPDATE);
    s.phase_after(this->phases.pre_update, this->phases.events);
    s.phase_after(this->phases.update, this->phases.pre_update);
    s.phase_after(this->phases.post_update, this->phases.update);

    this->phases.pre_render = s.add_phase(this->pipelines.render, ENGINE_PHASE::PRE_RENDER);
    this->phases.render = s.add_phase(this->pipelines.render, ENGINE_PHASE::RENDER);
    this->phases.post_render = s.add_phase(this->pipelines.render, ENGINE_PHASE::POST_RENDER);
    s.phase_after(this->phases.render, this->phases.pre_render);
    s.phase_after(this->phases.post_render, this->phases.render);

    // The shutdown pipeline reverses insertion within a phase, so its
    // phases are ordered by constraint, not by the order they were added.
    this->phases.pre_shutdown = s.add_phase(this->pipelines.shutdown, ENGINE_PHASE::PRE_SHUTDOWN);
    this->phases.shutdown = s.add_phase(this->pipelines.shutdown, ENGINE_PHASE::SHUTDOWN);
    this->phases.post_shutdown = s.add_phase(this->pipelines.shutdown, ENGINE_PHASE::POST_SHUTDOWN);
    s.phase_after(this->phases.shutdown, this->phases.pre_shutdown);
    s.phase_after(this->phases.post_shutdown, this->phases.shutdown);
}

bool Engine::init() {
    if (this->initialized) {
        fprintf(stderr, "[engine] init called twice\n");
        return false;
    }
    this->time = 0.0;
    this->last_dt = 0.0f;
    this->quit_requested = false;
    this->scheduler.init();
    this->create_pipelines();
    // From here on shutdown() runs the pipeline for whatever got built.
    this->initialized = true;
    if (!this->plugins.build_all(*this)) {
        return false;
    }
    return this->scheduler.run(this->pipelines.startup, this, nullptr, 0.0f);
}

void Engine::run() {
    if (this->runner != nullptr) {
        this->runner(*this);
    }
}

void Engine::quit() {
    this->quit_requested = true;
}

void Engine::shutdown() {
    if (this->initialized) {
        this->scheduler.run(this->pipelines.shutdown, this, nullptr, 0.0f);
    }
    this->plugins.free();
    this->singletons.free();
    this->scheduler.free();
    this->pipelines = EnginePipelines{};
    this->phases = EnginePhases{};
    this->initialized = false;
    this->runner = nullptr;
    this->gpu = nullptr;
}

SystemHandle Engine::add_system(const PhaseHandle phase, const char* name, const SystemFn fn, void* user_data) {
    return this->scheduler.add_system(phase, name, fn, user_data);
}

// --- Frame -------------------------------------------------------------------------

void Engine::update(const f32 dt) {
    // A new frame: last frame's scratch is gone.
    if (FrameAllocator* frame_allocator = this->get_singleton<FrameAllocator>()) {
        frame_allocator->reset();
    }
    this->time += dt;
    this->last_dt = dt;
    this->scheduler.run(this->pipelines.update, this, nullptr, dt);
}

void Engine::render(FrameContext& frame) {
    this->scheduler.run(this->pipelines.render, this, &frame, this->last_dt);
}

// --- Assets ------------------------------------------------------------------------

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

void Engine::log(const RenderLogLevel level, const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (Renderer* renderer = this->get_singleton<Renderer>()) {
        RENDERER::log(*renderer, level, "%s", text);
    } else {
        fprintf(stderr, "[engine] %s\n", text);
    }
}

const char* Engine::scene_text(const AssetGuid& guid, usz* out_size, const char** out_path) {
    *out_size = 0;
    *out_path = nullptr;
    AssetResourceProvider* assets = this->get_singleton<AssetResourceProvider>();
    char text_guid[TEXT_ASSET::GUID_TEXT_CAPACITY];
    TEXT_ASSET::format_guid(guid, text_guid);
    if (assets == nullptr) {
        this->log(RENDER_LOG_ERROR, "scene %s: no asset provider", text_guid);
        return nullptr;
    }
    AssetResource* resource = assets->get(guid);
    if (resource == nullptr) {
        this->log(RENDER_LOG_ERROR, "scene %s: not registered with the asset provider, or its file could not be read", text_guid);
        return nullptr;
    }
    if (resource->view.header->type != ASSET_TYPE::SCENE || !resource->is_text()) {
        this->log(RENDER_LOG_ERROR, "%s: not a .scene asset", resource->path);
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
            this->log(RENDER_LOG_ERROR, "%s:%u: %s", path_copy, result.line, result.error);
        } else {
            this->log(RENDER_LOG_ERROR, "%s: %s", path_copy, result.error);
        }
        scene->set_current(AssetGuid{}, nullptr);
        return false;
    }
    scene->set_current(guid, path_copy);
    this->log(RENDER_LOG_INFO, "Loaded scene %s: %llu entities%s", path_copy, static_cast<unsigned long long>(result.entities),
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
            this->log(RENDER_LOG_ERROR, "%s:%u: %s", path_copy, result.line, result.error);
        } else {
            this->log(RENDER_LOG_ERROR, "%s: %s", path_copy, result.error);
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
        this->log(RENDER_LOG_ERROR, "Could not write scene %s", path);
        return false;
    }
    // Registered (or refreshed) like any other asset, so the file is an
    // asset entity and a later load_scene finds it.
    if (this->load_asset_file(path).is_null()) {
        this->log(RENDER_LOG_WARNING, "Scene %s was written but could not be registered", path);
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
