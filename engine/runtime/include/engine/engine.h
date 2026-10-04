#pragma once

#include "engine/defines.hpp"
#include "engine/gpu/device.hpp"
#include "engine/gpu/render_target.hpp"
#include "engine/utils/singletons.hpp"

#include <utility>

// The simulation and its renderer. It knows nothing about windows, swapchains
// or frame pacing: the application owns those (see WindowPresenter) and tells
// the engine when to render and where, one FrameContext at a time.
//
//     Engine engine;
//     engine.init(&gpu);
//     while (running) {
//         engine.update(dt);
//         if (presenter.begin(frame)) {
//             engine.render(frame);     // frame.target may be any RenderTarget
//             presenter.end();
//         }
//     }
//     engine.shutdown();
//
// Engine-wide globals (the world, input state, asset caches, ...) are
// singletons keyed by C++ type: engine.create_singleton<T>(args...) builds T
// once and engine.get_singleton<T>() returns that same object afterwards
// (see singletons.hpp). They are released by shutdown(); anything a singleton
// owns must be released before that, explicitly, like any other engine
// resource.
struct Engine {
    // Creates the engine's GPU resources on `gpu`, which must outlive it.
    bool init(GpuDevice* gpu);
    // Destroys every singleton and releases the engine's resources.
    void shutdown();

    // Advances the simulation by `dt` seconds.
    void update(f32 dt);
    // Records the frame into frame.cmd, drawing into frame.target. Resources
    // written per frame are indexed by frame.slot.
    void render(const FrameContext& frame);

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
    GpuDevice* gpu = nullptr;
    f32 clear_color[4] = {0.05f, 0.05f, 0.08f, 1.0f};
    f64 time = 0.0;
};
