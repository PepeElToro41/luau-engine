#pragma once

#include "engine/asset/asset_resource_provider.hpp"
#include "engine/asset/asset_view.hpp"
#include "engine/defines.hpp"
#include "engine/ecs/world.hpp"
#include "engine/gpu/gpu.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/scene.hpp"
#include "engine/utils/singletons.hpp"

#include <utility>

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
// spawn entities with scene->spawn(name, parent)), and the Renderer that
// owns the render graph, the shader and material entities (named but
// unparented, so outside the scene) and draws the frame
// (get_singleton<Renderer>(), driven through RENDERER::, SHADER_LIBRARY::
// and MATERIAL::). shutdown() frees what they own before destroying them,
// so the app must have waited for the GPU to go idle before calling it.
struct Engine {
    // Creates the engine's GPU resources on `gpu`, which must outlive it,
    // and the engine's own singletons.
    bool init(GpuContext* gpu);
    // Destroys every singleton and releases the engine's resources.
    void shutdown();

    // Advances the simulation by `dt` seconds.
    void update(f32 dt);

    // Registers the asset file at `path` (a .lunaasset, or a text asset
    // such as a .material by its extension) with the AssetResourceProvider
    // and returns its GUID, or a null GUID (with a message) if the file is
    // not a valid asset. Payloads are read later, on first use.
    AssetGuid load_asset_file(const char* path);
    // Records the frame into frame.frame.cmd, drawing into frame.target
    // through the Renderer's graph, and updates frame.target_state.
    void render(FrameContext& frame);

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
    // Applied to the renderer's default forward pass every frame.
    f32 clear_color[4] = {0.05f, 0.05f, 0.08f, 1.0f};
    f64 time = 0.0;
    f32 last_dt = 0.0f;
};
