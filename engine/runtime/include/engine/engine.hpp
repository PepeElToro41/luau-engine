#pragma once

#include "engine/asset/asset_entity.hpp"
#include "engine/asset/asset_resource_provider.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/ecs/world.hpp"
#include "engine/gpu/gpu.hpp"
#include "engine/memory/frame_allocator.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/serialize.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "engine/utils/singletons.hpp"

#include <utility>

// Scratch memory per frame (see FrameAllocator).
inline constexpr usz ENGINE_FRAME_ARENA_BYTES = MEMORY::MB * 8;

// The simulation and its renderer. It knows nothing about windows or
// swapchains: the application owns the GpuContext and its frames and tells
// the engine when to render and where, one FrameContext at a time.
//
//     Engine engine;
//     engine.init(gpu);
//     while (running) {
//         engine.update(dt);
//         GpuFrame frame;
//         if (GPU::begin_frame(gpu, frame)) {
//             FrameContext ctx;
//             ctx.frame = frame;
//             ctx.target = frame.backbuffer;    // or any texture, e.g. the editor's viewport
//             engine.render(ctx);               // ctx.target_state tells where it was left
//             GPU::end_frame(gpu, frame);
//         }
//     }
//     GPU::wait_idle(gpu);
//     engine.shutdown();
//
// Engine-wide globals (the world, input state, asset caches, ...) are
// singletons keyed by C++ type: engine.create_singleton<T>(args...) builds T
// once and engine.get_singleton<T>() returns that same object afterwards
// (see singletons.hpp). They are released by shutdown(); anything a singleton
// owns must be released before that, explicitly, like any other engine
// resource. init() creates the engine's own singletons: the
// AssetResourceProvider that streams asset payloads
// (get_singleton<AssetResourceProvider>()), the ECS World every entity lives
// in (get_singleton<World>()), the Scene whose "scene_root" entity every
// scene entity is parented under through CHILD_OF (get_singleton<Scene>(),
// spawn entities with scene->spawn(name, parent)), the AssetEntities index
// that gives every registered asset an unparented entity with AssetUuid,
// an (AssetType, tag) pair and its file's name (get_singleton<AssetEntities>(),
// see asset/asset_entity.hpp; load_asset_file fills it), and the Renderer that
// owns the render graph, the shader and material entities (named but
// unparented, so outside the scene) and draws the frame
// (get_singleton<Renderer>(), driven through RENDERER::, SHADER_LIBRARY::
// and MATERIAL::), and the FrameAllocator (get_singleton<FrameAllocator>(),
// ENGINE_FRAME_ARENA_BYTES of scratch for anything a frame builds and drops:
// reset at the start of every update(), so one update per frame, and what the
// Renderer's draw lists live on). shutdown() frees what they own before
// destroying them, so the app must have waited for the GPU to go idle before
// calling it.
struct Engine {
    // Creates the engine's GPU resources on `gpu`, which must outlive it,
    // and the engine's own singletons.
    bool init(GpuContext* gpu);
    // Destroys every singleton and releases the engine's resources.
    void shutdown();

    // Advances the simulation by `dt` seconds. Also begins the frame: resets
    // the FrameAllocator, so anything allocated from it last frame is gone.
    void update(f32 dt);

    // Registers the asset file at `path` (a .lunaasset, or a text asset
    // such as a .material by its extension) with the AssetResourceProvider
    // and returns its GUID, or a null GUID (with a message) if the file is
    // not a valid asset. Payloads are read later, on first use. The asset
    // also becomes (or already is) an entity:
    // get_singleton<AssetEntities>()->find(guid) is it.
    AssetGuid load_asset_file(const char* path);
    // Records the frame into frame.frame.cmd, drawing into frame.target
    // through the Renderer's graph, and updates frame.target_state.
    void render(FrameContext& frame);

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

    Singletons singletons;
    GpuContext* gpu = nullptr;

private:
    static EntityId resolve_asset(const AssetGuid& guid, void* user_data);
    static void scene_warning(const char* text, void* user_data);
    // The text of the scene asset `guid`, resident in the provider until
    // unload_scene_text; nullptr (with a message) when it is not a scene.
    const char* scene_text(const AssetGuid& guid, usz* out_size, const char** out_path);

public:
    // Applied to the renderer's default forward pass every frame.
    f32 clear_color[4] = {0.05f, 0.05f, 0.08f, 1.0f};
    f64 time = 0.0;
    f32 last_dt = 0.0f;
};
