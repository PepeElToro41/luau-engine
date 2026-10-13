#pragma once

#include "engine/asset/asset_entity.hpp"
#include "engine/utils/log.hpp"
#include "engine/asset/asset_resource_provider.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/ecs/world.hpp"
#include "engine/gpu/gpu.hpp"
#include "engine/memory/frame_allocator.hpp"
#include "engine/plugin.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/serialize.hpp"
#include "engine/scheduler/scheduler.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "engine/utils/singletons.hpp"

#include <utility>

// Scratch memory per frame (see FrameAllocator).
inline constexpr usz ENGINE_FRAME_ARENA_BYTES = MEMORY::MB * 8;

// The engine is a list of plugins and a scheduler. An application adds the
// plugins it wants (plugin.hpp), calls init() to build them, drives the
// frame with update(dt) and render(ctx), and calls shutdown() at the end:
//
//     Engine engine;
//     engine.add_plugin<WindowPlugin>(desc);   // SDL, the window, Engine::gpu and the runner
//     engine.add_plugin<CorePlugin>();         // world, scene, assets, frame allocator
//     engine.add_plugin<RenderPlugin>();       // renderer, render components
//     engine.add_plugin<MyPlugin>(args...);
//     if (engine.init()) {                     // builds the plugins, runs "startup"
//         engine.run();                        // the window plugin's loop, until quit()
//     }
//     engine.shutdown();                       // runs "shutdown", frees plugins and singletons
//
// run() hands over to the runner a plugin installed (`runner`; the window
// plugin's loop polls events, calls update(dt), then render(ctx) with the
// backbuffer as the target inside a GPU frame, and returns with the GPU
// idle once quit() was called); without one it returns at once. An
// application that owns its own window and loop sets `gpu` itself and
// calls update() and render() directly:
//
//     engine.gpu = gpu;
//     engine.add_plugin<CorePlugin>();
//     engine.add_plugin<RenderPlugin>();
//     if (!engine.init()) { ... }
//     while (running) {
//         engine.update(dt);                   // runs "update"
//         GpuFrame frame;
//         if (GPU::begin_frame(gpu, frame)) {
//             FrameContext ctx;
//             ctx.frame = frame;
//             ctx.target = frame.backbuffer;    // or any texture, e.g. the editor's viewport
//             engine.render(ctx);               // runs "render"; ctx.target_state tells where it was left
//             GPU::end_frame(gpu, frame);
//         }
//     }
//     GPU::wait_idle(gpu);
//     engine.shutdown();
//
// The scheduler (core scheduler/scheduler.hpp) holds four pipelines the
// engine creates before any plugin builds, so a plugin adds its systems to
// them in build() through add_system(phase, name, fn, user_data):
//
//   startup   once, from init(), after every plugin built:
//               pre_startup, startup, post_startup
//   update    from update(dt), after the FrameAllocator reset and the clock:
//               events, pre_update, update, post_update
//   render    from render(ctx), the FrameContext as the run's argument
//             (ENGINE::frame(ctx) reads it back):
//               pre_render, render, post_render
//   shutdown  once, from shutdown(), before the plugins and singletons are
//             freed; reverse-insertion order, so the last built is the first
//             torn down within each phase:
//               pre_shutdown   detach: take back what was handed to another
//                              plugin's state while everything is still alive
//                              (a callback, a graph pass, entities whose hooks
//                              need a plugin)
//               shutdown       release what the plugin owns itself
//               post_shutdown  the platform the rest sat on (GPU, window)
//
// The handles are `pipelines` / `phases` and the names ENGINE_PIPELINE:: /
// ENGINE_PHASE::. A plugin may add phases of its own against these
// (scheduler.phase_before(mine, phases.pre_shutdown)). A system gets the
// Engine back with ENGINE::of(ctx).
//
// Engine-wide globals (the world, input state, asset caches, ...) are
// singletons keyed by C++ type: engine.create_singleton<T>(args...) builds T
// once and engine.get_singleton<T>() returns that same object afterwards
// (see singletons.hpp). They are released by shutdown() after the shutdown
// pipeline ran; anything a singleton owns is released there, explicitly,
// by a system of the plugin that created it. CorePlugin creates the
// AssetResourceProvider that streams asset payloads, the ECS World every
// entity lives in, the Scene whose "scene_root" entity every scene entity
// is parented under (spawn entities with scene->spawn(name, parent)), the
// AssetEntities index that gives every registered asset an unparented
// entity with AssetUuid, an (AssetType, tag) pair and its file's name
// (load_asset_file fills it), and the FrameAllocator
// (ENGINE_FRAME_ARENA_BYTES of scratch for anything a frame builds and
// drops: reset at the start of every update(), so one update per frame).
// RenderPlugin creates the Renderer that owns the render graph, the shader
// and material entities (named but unparented, so outside the scene) and
// draws the frame (driven through RENDERER::, SHADER_LIBRARY:: and
// MATERIAL::).

namespace ENGINE_PIPELINE {
constexpr const char* STARTUP = "startup";
constexpr const char* UPDATE = "update";
constexpr const char* RENDER = "render";
constexpr const char* SHUTDOWN = "shutdown";
} // namespace ENGINE_PIPELINE

namespace ENGINE_PHASE {
constexpr const char* PRE_STARTUP = "pre_startup";
constexpr const char* STARTUP = "startup";
constexpr const char* POST_STARTUP = "post_startup";
constexpr const char* EVENTS = "events";
constexpr const char* PRE_UPDATE = "pre_update";
constexpr const char* UPDATE = "update";
constexpr const char* POST_UPDATE = "post_update";
constexpr const char* PRE_RENDER = "pre_render";
constexpr const char* RENDER = "render";
constexpr const char* POST_RENDER = "post_render";
constexpr const char* PRE_SHUTDOWN = "pre_shutdown";
constexpr const char* SHUTDOWN = "shutdown";
constexpr const char* POST_SHUTDOWN = "post_shutdown";
} // namespace ENGINE_PHASE

struct EnginePipelines {
    PipelineHandle startup;
    PipelineHandle update;
    PipelineHandle render;
    PipelineHandle shutdown;
};

struct EnginePhases {
    PhaseHandle pre_startup;
    PhaseHandle startup;
    PhaseHandle post_startup;
    PhaseHandle events;
    PhaseHandle pre_update;
    PhaseHandle update;
    PhaseHandle post_update;
    PhaseHandle pre_render;
    PhaseHandle render;
    PhaseHandle post_render;
    PhaseHandle pre_shutdown;
    PhaseHandle shutdown;
    PhaseHandle post_shutdown;
};

using EngineRunner = void (*)(Engine& engine);

struct Engine {
    // Creates the pipelines and phases, builds every added plugin in
    // dependency order and runs the startup pipeline. False (with messages)
    // if a plugin failed to build; shutdown() is still the way out.
    bool init();
    // Calls `runner` and returns when it does; at once if there is none.
    void run();
    // Asks the runner to return after the current frame.
    void quit();
    // Runs the shutdown pipeline, then frees the plugins, the singletons and
    // the scheduler. The GPU must be idle.
    void shutdown();

    // Advances the simulation by `dt` seconds: resets the FrameAllocator
    // (anything allocated from it last frame is gone), advances `time`, and
    // runs the update pipeline.
    void update(f32 dt);
    // Runs the render pipeline with `frame` as its argument. RenderPlugin's
    // system records the frame into frame.frame.cmd, drawing into
    // frame.target, and updates frame.target_state.
    void render(FrameContext& frame);

    // --- Plugins -------------------------------------------------------------

    // Adds T(args...) to the list; before init() only. nullptr (with a
    // message) if T was added before or the engine is initialized.
    template <typename T, typename... Args>
    T* add_plugin(Args&&... args);
    // From a build(): T's plugin, built first if it is not yet. nullptr
    // (with a message) if T was never added or cannot be built.
    template <typename T>
    T* require() { return this->plugins.require<T>(*this); }
    // T's plugin if added, built or not. Never builds.
    template <typename T>
    T* get_plugin() const { return this->plugins.get<T>(); }
    template <typename T>
    bool is_plugin_added() const { return this->plugins.is_added<T>(); }

    // scheduler.add_system, for the common case.
    SystemHandle add_system(PhaseHandle phase, const char* name, SystemFn fn, void* user_data);

    // --- Assets --------------------------------------------------------------

    // Registers the asset file at `path` (a .lunaasset, or a text asset
    // such as a .material by its extension) with the AssetResourceProvider
    // and returns its GUID, or a null GUID (with a message) if the file is
    // not a valid asset. Payloads are read later, on first use. The asset
    // also becomes (or already is) an entity:
    // get_singleton<AssetEntities>()->find(guid) is it.
    AssetGuid load_asset_file(const char* path);

    // --- Scenes ----------------------------------------------------------------
    //
    // A .scene file (asset/asset_types/scene_asset.hpp) registered through
    // load_asset_file is loaded under the Scene's root. Loading is
    // synchronous: the old content is deleted and the new one created on
    // this thread, in the same call. Line-level problems in the file are
    // warnings in the renderer log (the editor's Output panel); only a
    // structurally broken file fails.

    // Unloads what is under scene_root and instantiates the scene asset
    // `guid` there; Scene::current then names it. False (with messages)
    // when the asset is unknown, not a .scene, or structurally broken. The
    // old content is only unloaded once the text is in hand, so an unknown
    // asset leaves the scene as it was; a broken text still empties it.
    bool load_scene(const AssetGuid& guid);
    // Instantiates the scene asset `guid` under `parent` (an alive entity)
    // without unloading anything: a prefab. Scene::current is untouched.
    bool instantiate_scene(const AssetGuid& guid, EntityId parent);
    // Writes the content under scene_root to `path` with Scene::current as
    // its guid (a fresh random one when the scene has none), registers the
    // file (so it is an asset entity like any other scene) and makes it
    // Scene::current. False when the file cannot be written.
    bool save_scene(const char* path);
    // The text save_scene would write, appended to `out`: for an app that
    // wants to compare the scene with its file. The guid is Scene::current
    // (null for a new scene).
    void write_scene(DynamicArray<char>& out);
    // Unloads the scene and forgets Scene::current.
    void new_scene();
    // Fills `ctx` with the engine's asset resolver (a material GUID loads
    // the material, a shader GUID the shader, any other asset is its entity)
    // and the renderer log as the warning sink, for anything that
    // serializes on its own.
    void serialize_context(SerializeContext& ctx);

    // --- Singletons ----------------------------------------------------------

    // Creates T's singleton as T(args...) and returns it. The pointer stays
    // valid until remove_singleton<T>() or shutdown(). Returns nullptr (with
    // an error) if T already has a singleton.
    template <typename T, typename... Args>
    T* create_singleton(Args&&... args) { return this->singletons.create<T>(std::forward<Args>(args)...); }
    // The singleton for T, or nullptr if it was never created. Never creates.
    template <typename T>
    T* get_singleton() const { return this->singletons.get<T>(); }
    // Whether T's singleton exists.
    template <typename T>
    bool has_singleton() const { return this->singletons.has<T>(); }
    // Destroys T's singleton. False if there is none.
    template <typename T>
    bool remove_singleton() { return this->singletons.remove<T>(); }

    // --- Members -------------------------------------------------------------

    Singletons singletons;
    PluginRegistry plugins;
    Scheduler scheduler;
    EnginePipelines pipelines;
    EnginePhases phases;
    // The GPU the render plugin draws with. Set by whoever created it: a
    // window plugin in its build, or the application before init().
    GpuContext* gpu = nullptr;
    // The loop run() hands over to; set by a window plugin.
    EngineRunner runner = nullptr;
    bool quit_requested = false;
    bool initialized = false;
    f64 time = 0.0;
    f32 last_dt = 0.0f;

private:
    static EntityId resolve_asset(const AssetGuid& guid, void* user_data);
    // The text of the scene asset `guid`, resident in the provider until
    // unload_scene_text; nullptr (with a message) when it is not a scene.
    const char* scene_text(const AssetGuid& guid, usz* out_size, const char** out_path);
    // The four pipelines and their phases, in order.
    void create_pipelines();
};

template <typename T, typename... Args>
T* Engine::add_plugin(Args&&... args) {
    if (this->initialized) {
        LOG::error(LOG_CATEGORY::ENGINE, "plugin '%s' added after init; plugins are added before it", T::NAME);
        return nullptr;
    }
    return this->plugins.add<T>(std::forward<Args>(args)...);
}

namespace ENGINE {

// The Engine a system is running in (the run's host).
inline Engine& of(SystemContext& ctx) {
    return *static_cast<Engine*>(ctx.host);
}

// The render pipeline's FrameContext (the run's argument). Only meaningful
// in a system of the render pipeline.
inline FrameContext& frame(SystemContext& ctx) {
    return *static_cast<FrameContext*>(ctx.argument);
}

} // namespace ENGINE
