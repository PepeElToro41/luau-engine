#pragma once

#include "engine/defines.hpp"
#include "engine/play.hpp"
#include "engine/plugin.hpp"
#include "engine/scheduler/scheduler.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "engine/utils/singletons.hpp"

struct Engine;

// Playtesting: Play, Pause, Step and Stop (docs/playtesting_plan.md).
//
// Play in the editor creates a play instance: a child store of the
// engine's root (Engine::active points at it while the game runs) holding
// its own World, cloned from the authoring one id for id
// (WORLD_CLONE::clone, so every entity, component and asset keeps its id
// and generation), its own Scene (adopting the cloned scene root),
// AssetEntities (rebuilt from the clone), FrameAllocator, Renderer (drawing
// the clone with the shared RenderResources, so pipelines and GPU assets
// are not rebuilt) and PlayState. Everything shared, the asset provider,
// the project and the GPU caches, keeps resolving to the root's. The game
// then runs in the clone: scripts, physics and gameplay systems in the
// play pipelines, the renderer drawing from the scene's camera, every
// `engine.get_singleton<World>()` the play world. Stop runs the end
// pipeline, points `active` back at the root and frees the instance, world
// first (its removed hooks hand programs, blocks and pipelines back), and
// the authoring world is exactly as it was: it was only ever read.
//
// Without `isolate` (the standalone) Play runs the game in place, in the
// root store, with a PlayState there and no clone; `autoplay` starts it in
// post_startup.
//
// Pipelines, created by build() and run by this plugin only:
//
//   play_begin   once at Play, after the instance is active: `begin`
//   play_update  every frame while PLAYING (and once per Step while
//                PAUSED), from the engine's "update" phase, with the
//                PlayState as the run's argument (PLAY::state(ctx)) and the
//                scaled step as dt: `pre_update`, `update`, `post_update`
//   play_end     once at Stop, before the instance goes: `end`
//                (reverse insertion order, like the engine's shutdown)
//
// A gameplay plugin requires this one and adds its systems to `phases`;
// they only ever see the world being played. The editor and other UI call
// the request_* functions, which take effect at the next frame's
// pre_update, a point where nothing of a frame is being recorded; code that
// knows it is at such a point (a post_startup system) calls play() / stop()
// directly.
//
// Teardown: the pre_shutdown system stops a running game, so by the time
// the render and core plugins release their state the root store is
// active again and the instance is gone; build() requires RenderPlugin
// when it was added, so this plugin is built after it and its system runs
// before the render plugin's in the reverse-ordered shutdown pipeline.
struct PlayPlugin : Plugin {
    static constexpr const char* NAME = "play";

    struct Pipelines {
        PipelineHandle begin;
        PipelineHandle update;
        PipelineHandle end;
    };

    struct Phases {
        PhaseHandle begin;
        PhaseHandle pre_update;
        PhaseHandle update;
        PhaseHandle post_update;
        PhaseHandle end;
    };

    PlayPlugin() = default;
    PlayPlugin(const bool isolate, const bool autoplay) : isolate(isolate), autoplay(autoplay) {}

    bool build(Engine& engine) override;

    // --- Controls, immediate ---------------------------------------------------
    // Only from a point where no frame is being recorded (a system of the
    // update pipeline, a startup system).

    // STOPPED -> PLAYING. False (logged) if already running or the instance
    // could not be made; nothing is left behind then.
    bool play(Engine& engine);
    // PLAYING / PAUSED -> STOPPED. False if nothing runs.
    bool stop(Engine& engine);
    // PLAYING -> PAUSED, and back.
    bool pause(Engine& engine);
    bool resume(Engine& engine);
    // While PAUSED: run one play update of `step_dt` on the next frame.
    bool step(Engine& engine);

    // --- Controls, deferred ----------------------------------------------------
    // Performed in order at the next frame's pre_update.

    void request_play() { this->request(REQUEST_PLAY); }
    void request_stop() { this->request(REQUEST_STOP); }
    void request_pause() { this->request(REQUEST_PAUSE); }
    void request_resume() { this->request(REQUEST_RESUME); }
    void request_step() { this->request(REQUEST_STEP); }
    // Play when stopped, stop otherwise.
    void request_toggle() { this->request(REQUEST_TOGGLE); }

    // --- State -----------------------------------------------------------------

    PlayMode mode = PLAY_STOPPED;
    bool is_stopped() const { return this->mode == PLAY_STOPPED; }
    bool is_playing() const { return this->mode == PLAY_PLAYING; }
    bool is_paused() const { return this->mode == PLAY_PAUSED; }
    // The running game's state, or nullptr while stopped.
    PlayState* state(Engine& engine) const;

    // Play clones the world into an instance of its own; off, the game runs
    // in place in the root store.
    bool isolate = true;
    // Start playing in post_startup.
    bool autoplay = false;
    // The play world refuses component types nobody preregistered
    // (World::strict_components).
    bool strict_components = false;
    // Applied to the play clock every frame.
    f32 time_scale = 1.0f;
    // The step a Step while paused advances by.
    f32 step_dt = 1.0f / 60.0f;

    Pipelines pipelines;
    Phases phases;
    // The play instance's store while an isolated game runs: a child of
    // Engine::root. Empty and parentless otherwise.
    Singletons instance;

private:
    enum Request : u32 {
        REQUEST_PLAY,
        REQUEST_STOP,
        REQUEST_PAUSE,
        REQUEST_RESUME,
        REQUEST_STEP,
        REQUEST_TOGGLE,
    };

    void request(Request request);
    bool begin_instance(Engine& engine);
    void end_instance(Engine& engine);

    static void control(SystemContext& ctx);
    static void tick(SystemContext& ctx);
    static void autostart(SystemContext& ctx);
    static void teardown(SystemContext& ctx);

    DynamicArray<u32> requests;
    bool step_pending = false;
};
