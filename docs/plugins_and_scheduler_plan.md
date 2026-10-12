# Plugins and the scheduler: plan

Status: implemented 2026-10-12; `engine/runtime/CLAUDE.md`, `engine/core/CLAUDE.md`
and `editor/CLAUDE.md` describe what exists and this file is the design
record. The
decisions below are proposals with their reasoning; the API sketches are a
starting point and may change where the code says otherwise.

Read `CLAUDE.md` and the module files it imports first: the runtime
`Engine` (`engine/runtime/include/engine/engine.hpp`), the two apps
(`standalone/src/app.cpp`, `editor/src/app.cpp`), the singleton store
(`engine/core/include/engine/utils/singletons.hpp`) and the render graph
(`engine/core/include/engine/render_graph/render_graph.hpp`), whose
handle / name / `dirty` conventions the scheduler copies. Follow the C++
conventions there (`struct` only, `this->`, `snake_case`, no destructors,
`SCREAMING_CASE` namespaces, function pointers plus `void* user_data` for
callbacks, doctest tests under `tests/core/`).

## Goal

Two things, and a cleanup they make possible:

- A **plugin system**: the `Engine` holds an ordered list of plugins, each
  with a `build`, built in dependency order; what a plugin tears down is
  systems it adds to the `shutdown` pipeline. Modeled on Bevy's `Plugin` /
  `App::add_plugins`.
- A **scheduler**: named pipelines (the entry points the engine invokes at
  fixed moments: startup, update, render), each a list of phases, each
  phase a list of systems. Phases depend on phases, systems on systems;
  the scheduler orders them and runs them. Modeled on flecs' pipelines and
  phases and Bevy's schedules and system sets.
- With both in place, the code the standalone and the editor duplicate
  today (window + GPU + engine setup and teardown, the run loop, the SDL
  event switch, the project scan, the startup scene, the demo toggles, F5)
  moves into plugins both apps add, and each app becomes a list of plugins
  plus what is really its own.

Out of scope for this round: multi-threaded execution (the dependency
graph is what makes it possible later, the runner stays single-threaded),
a fixed-timestep pipeline (noted below as the natural next pipeline),
query-driven systems (`system<Transform, Velocity>(...)`; a system is a
function pointer for now, Luau and query systems build on top), making
phases or systems ECS entities.

## What exists and what is duplicated

`Engine::init(gpu)` creates the engine's singletons in a fixed order
(provider, World, Scene, AssetEntities, FrameAllocator, Renderer) and
`shutdown()` tears them down in an order that matters (asset entities,
world, renderer, provider, frame allocator: the world first so the Shader /
Material removed hooks hand their GPU resources back while the renderer is
alive). `update(dt)` resets the frame allocator and advances time;
`render(ctx)` pushes clear color / time into the renderer and runs the
graph into `ctx.target`.

Both `App`s then do the same things around it, in the same order:

| Step | standalone `App` | editor `App` |
| --- | --- | --- |
| SDL, window, `GPU::init`, `engine.init(gpu)` | `init` | `init` (swapchain format UNORM) |
| Project directory: walk, `load_asset_file` each asset, `set_project_render_dir` | `scan_project` | `open_test_project` + `scan_project_assets` (also registers shader files as asset entities, which the standalone should do too: a project shader outside `render/` does not resolve there today) |
| Startup scene: `scenes/demo.scene`, else `RENDER_DEMO::spawn` | `load_scene` | `load_startup_scene` |
| Run loop with `SDL_GetTicksNS` timing | `run` | `run` |
| Events: QUIT / CLOSE_REQUESTED, PIXEL_SIZE_CHANGED -> `mark_resized` | `poll_events` | `poll_events` |
| F5 reload shaders, demo post / shadow toggles | keys F5 / F6 / F7 | Render menu + F5 |
| Frame: `update`, `begin_frame`, `FrameContext`, `render`, `end_frame` | `frame` | `frame` |
| Teardown: `wait_idle`, `engine.shutdown`, `GPU::shutdown`, window, SDL | `shutdown` | `shutdown` |

What differs: the standalone draws into the backbuffer and tags the scene
camera; the editor draws into per-slot viewport textures, barriers them to
SAMPLED, then draws ImGui into the backbuffer, flies its own camera, keeps
a grid pass, and has ~800 lines of panel glue (selection, scene dialogs,
Save All, import queue) that is editor logic, not duplication.

The ECS has no notion of systems: nothing in `ecs/` schedules anything,
and the render graph is the only ordered-pass structure in core. Component
entities already carry per-component data (`Inspector`, `Serializer`), so
systems-as-entities would fit later, but it is not needed to get an order.

## Decisions

### Plugins live on the Engine, and the Engine becomes the application object

The two candidates were the per-executable `App` and the `Engine`. The
`Engine` wins: it is the one object both executables share, every plugin
needs its singletons and its scheduler, and a Luau-facing plugin API later
has one place to bind. The `App` structs shrink to `main()` adding plugins.

This reverses one documented decision: "the Engine owns no window or
swapchain". The value behind that decision is kept, the mechanism changes:
the Engine still knows nothing about SDL, the window and the `GpuContext`
are owned by a `WindowPlugin`, and `Engine::render(FrameContext&)` still
draws into whatever target the caller hands it. A headless engine is the
same `Engine` without the window plugin. The run loop moves into the Engine
as `run()`, which calls a *runner* function the window plugin installs
(Bevy's `set_runner`); with no runner `run()` returns at once.

### The engine's own setup becomes plugins too

`Engine::init` stops creating singletons itself. `CorePlugin` creates the
provider, the World, the Scene, the AssetEntities index and the
FrameAllocator; `RenderPlugin` creates the Renderer and registers the
render components. `Engine::init()` builds whatever plugins were added, so
`init(gpu)` becomes `init()` and `Engine::gpu` is set by the window plugin.
A `PLUGINS::add_defaults(engine, desc)` helper adds window + core + render
in one call (Bevy's `DefaultPlugins`); the apps use it.

### Shutdown is a pipeline, not a plugin method

Bevy has no plugin shutdown at all: `Plugin` has `build`, `ready`,
`finish` and `cleanup`, and `cleanup` runs once after every plugin is
built, before the runner, to drop build-time scratch. When the runner
returns, the `World` is dropped and Rust's ownership does the rest: a
resource that still uses another holds an `Arc` to it, so order is
irrelevant, and the few places where order matters anyway (the wgpu
surface before the winit window) are handled by hand. Work that must
happen at exit is a system reacting to the `AppExit` event in `Last`.
So Bevy's answer is "ownership instead of order", which a codebase with
no destructors cannot copy; what it can copy is the second half: exit
work is systems, ordered by the scheduler like everything else.

A `Plugin::shutdown` method ordered by reverse build order breaks as
soon as a plugin built early must outlive one built late: the editor's
UI plugin installs the Output panel as the renderer's log sink, and
`RENDERER::shutdown` logging into a freed panel is a use-after-free that
reverse order produces, not prevents. Fixing it with a second dependency
graph (`shutdown_after<T>()`) makes every plugin name the others it
collides with, which is what the user does not want.

The proposal: the `shutdown` pipeline. `Engine::shutdown()` runs it, then
frees the plugin objects and the singletons. A plugin adds its teardown
systems in `build`, to the phase matching *what kind of thing* it tears
down, so it never names another plugin:

| Phase | What goes there | Examples |
| --- | --- | --- |
| `pre_shutdown` | Detach: take back anything handed to another plugin's state while every plugin is still alive | reset the renderer log sink; unregister the grid's custom pass and remove its graph pass; delete the entities whose removed hooks need the renderer (`Shader`, `Material`); save editor layout |
| `shutdown` | Release what the plugin owns itself | `RENDERER::shutdown`; the index, the World, the provider, the frame allocator; ImGui backends; viewport textures |
| `post_shutdown` | The platform everything else sat on | `GPU::shutdown`, the window, SDL |

Within a phase the default order is reverse insertion (later-added
systems run first), which is reverse build order since plugins add their
systems in `build`; `system_after` / `system_before` override it where a
plugin genuinely depends on another's step, by name
(`find_system(phase, "render")`), without holding the plugin. A pipeline
carries that default as `PipelineOrder::REVERSE_INSERTION`; the other
three keep `INSERTION`.

The rule that makes this sufficient is symmetric responsibility: whoever
registers something into another plugin's state at build time takes it
back in `pre_shutdown`. A plugin only ever talks to the plugins it already
required, and everything is alive during that phase, so the take-back
can never dangle. After it, `shutdown` frees owned resources with nothing
cross-plugin left, and `post_shutdown` pulls the platform. The World case
falls out of it: `RenderPlugin` deletes its `Shader` / `Material` entities
in `pre_shutdown` (collected first, then deleted; the right encapsulation
anyway, the renderer takes back what it created), so `CorePlugin`'s
`world->free()` in `shutdown` runs no hook that needs the renderer,
whichever of the two frees first. Anything a plugin creates in the World
and leaves there dies with the World, hooks and all, so a plugin whose
hooks touch its own state deletes those entities in `pre_shutdown` too.

The GPU must be idle before the first `release_*` / `destroy_*`, which
`pre_shutdown` already causes. The `WindowPlugin` adds a `gpu_idle` phase
with `phase_before(gpu_idle, pre_shutdown)` holding one system,
`GPU::wait_idle`: a plugin adding a phase ahead of the engine's, which is
the ordering tool working as intended. The runner also waits idle before
returning, so the frame in flight is done either way.

`Plugin` therefore has one virtual function, `build`. `PluginRegistry`
keeps ownership, `require` ordering and the final free of the objects
(memory only, since nothing has a destructor).

### Dependencies are declared where they are used: `require<T>()`

Bevy has insertion order and nothing else. The user wants order and
dependencies. The proposal is build-on-demand: inside `build`, a plugin
calls `engine.require<WindowPlugin>()`, which returns the plugin, building
it first if it was added but not built yet, or `nullptr` with an error if
it was never added. The dependency is then written exactly once, at the
point of use, with a typed handle as the reward. Insertion order remains
the default order for plugins that do not require each other; a cycle
(`A` requires `B` requires `A`) is reported and fails `init`. Build order
is what the `shutdown` pipeline's reverse-insertion default inverts, so a
plugin built on demand for another has its steps run after that other's
within each phase.
`get_plugin<T>()` is the optional lookup: it never builds and returns
`nullptr` while `T` is absent (or, during `init`, not built yet).

Why not a virtual `dependencies()` list or `add_plugin<T>().after<U>()`?
Both state the dependency away from the code that relies on it, and both
still need the typed lookup in `build`. `require` is the lookup and the
dependency in one call.

### Plugins are owned by the Engine, typed by `TypeId`, no destructors

`engine.add_plugin<T>(args...)` constructs `T` in its own allocation (as
`Singletons::create` does), keyed by `TYPE_ID::get<T>()` so a type is
added once (a second add is an error, `is_plugin_added<T>()` tells). The
record keeps a `destroy` function per concrete type, so a plugin is never
deleted through the base pointer. A plugin object is plain state plus two
virtual functions (the one place the codebase uses virtuals already is
`RendererBackend`: `init` / `shutdown` overrides, no virtual destructor).

```cpp
// engine/runtime/include/engine/plugin.hpp
struct Plugin {
    // Once, in dependency order, from Engine::init(). Creates singletons,
    // adds phases and systems (its shutdown steps included), requires
    // other plugins. False fails init.
    virtual bool build(Engine& engine) = 0;
};

struct WindowPlugin : Plugin {
    static constexpr const char* NAME = "window";
    WindowDesc desc;
    explicit WindowPlugin(WindowDesc desc) : desc(desc) {}
    bool build(Engine& engine) override;
    DisplayWindow window;
    WindowEvents events;   // this frame's SDL events, see below

private:
    static void wait_idle(SystemContext& ctx);   // phase gpu_idle
    static void shutdown(SystemContext& ctx);    // phase post_shutdown
};
```

`NAME` is the name errors and the scheduler use; `add_plugin<T>` reads it.

### The scheduler is a plain core structure, like the render graph

`engine/core/include/engine/scheduler/scheduler.hpp`, no World, no
Engine: pipelines, phases and systems are records in arrays, handles carry
index + generation, names are looked up by `HASH::fnv1a_str`, ordering
constraints set `dirty` and the next `run` re-sorts. It is testable in
`engine_tests` (`tests/core/scheduler/scheduler_tests.cpp`) with counting
systems, which the Engine-bound alternative is not. The same topological
sort (Kahn's algorithm, ties broken by insertion order so unconstrained
items keep the order they were added in) serves phases within a pipeline
and systems within a phase.

A system is a function pointer plus `user_data`, the codebase's callback
shape (hooks, observers, custom passes, serializers, the log sink). The
context it receives is engine-neutral; the runtime wraps it:

```cpp
// core
struct SystemContext {
    Scheduler* scheduler = nullptr;
    void* host = nullptr;       // the Engine; core does not name it
    void* argument = nullptr;   // the run's argument: the render pipeline's FrameContext
    f32 dt = 0.0f;
    void* user_data = nullptr;  // the system's own
};
using SystemFn = void (*)(SystemContext& ctx);

// runtime, engine/engine.hpp
namespace ENGINE {
Engine& of(SystemContext& ctx);              // ctx.host
FrameContext& frame(SystemContext& ctx);     // ctx.argument, render pipeline only
}
```

Why not an ECS-based scheduler (flecs: systems are entities, phases are
tags, a pipeline is a query)? It is the more uniform end state and the
natural shape for Luau-defined systems, but it needs the ECS to grow
system entities, a `DependsOn` relation and ordered query results first,
and it makes the simplest case (an app calling a function once per frame)
depend on the World. The plain structure gives the order now; when
systems become entities, the entity can hold a `SystemHandle` and the
scheduler stays the executor.

```cpp
struct PipelineHandle { u32 index = 0; u32 generation = 0; };
struct PhaseHandle    { u32 index = 0; u32 generation = 0; };
struct SystemHandle   { u32 index = 0; u32 generation = 0; };

struct Scheduler {
    void init();
    void free();

    // `order` is the tie-break among unconstrained phases and systems:
    // INSERTION (the default) or REVERSE_INSERTION (the shutdown pipeline).
    PipelineHandle add_pipeline(const char* name, PipelineOrder order = PIPELINE_ORDER_INSERTION);
    PipelineHandle find_pipeline(const char* name) const;

    // A phase of `pipeline`; phases without constraints run in insertion
    // order.
    PhaseHandle add_phase(PipelineHandle pipeline, const char* name);
    PhaseHandle find_phase(PipelineHandle pipeline, const char* name) const;
    // Removes an empty phase. False (with a message) when systems remain.
    bool remove_phase(PhaseHandle phase);
    // `phase` runs after / before `other`; both of one pipeline.
    bool phase_after(PhaseHandle phase, PhaseHandle other);
    bool phase_before(PhaseHandle phase, PhaseHandle other);

    SystemHandle add_system(PhaseHandle phase, const char* name, SystemFn fn, void* user_data);
    SystemHandle find_system(PhaseHandle phase, const char* name) const;
    bool remove_system(SystemHandle system);
    // `system` runs after / before `other`; both of one phase.
    bool system_after(SystemHandle system, SystemHandle other);
    bool system_before(SystemHandle system, SystemHandle other);
    bool set_enabled(SystemHandle system, bool enabled);

    // Runs every enabled system of every phase in order. Re-sorts when
    // `dirty`; a cycle is reported through `log` and the pipeline does not
    // run until the constraint is removed (false). `host` and `argument`
    // go into every SystemContext.
    bool run(PipelineHandle pipeline, void* host, void* argument, f32 dt);

    // Reports and tests: the resolved order.
    usz phase_order(PipelineHandle pipeline, DynamicArray<PhaseHandle>& out);
    usz system_order(PhaseHandle phase, DynamicArray<SystemHandle>& out);

    SchedulerLogFn log = nullptr;   // defaults to stderr
    void* log_user_data = nullptr;
    bool dirty = true;
};
```

Removing a system while its pipeline runs is allowed and takes effect
on the next run (the runner walks a copy of the order built on a
`TemporalAllocator`); a removed handle's generation makes later calls
with it return false.

### Pipelines and phases the Engine defines

The Engine creates its pipelines and phases before any plugin builds, so
a plugin can add systems to them in `build`. Names are constants in
`engine/engine.hpp` (`ENGINE_PIPELINE::UPDATE` = `"update"`, ...) and the
handles are members (`engine.pipelines.update`, `engine.phases.post_update`),
so a plugin writes `engine.add_system(engine.phases.update, "fly camera", fn, this)`.

| Pipeline | Invoked by | Phases, in order |
| --- | --- | --- |
| `startup` | `Engine::init`, once, after every plugin built | `pre_startup`, `startup`, `post_startup` |
| `update` | `Engine::update(dt)`, after the FrameAllocator reset and the clock | `events`, `pre_update`, `update`, `post_update` |
| `render` | `Engine::render(FrameContext&)`, the context as `argument` | `pre_render`, `render`, `post_render` |
| `shutdown` | `Engine::shutdown`, once, before the plugins and singletons are freed; reverse-insertion default | `pre_shutdown`, `shutdown`, `post_shutdown` (see "Shutdown is a pipeline") |

A `fixed_update` pipeline (run zero or more times per `update` by an
accumulator, Bevy's `FixedUpdate`) is the obvious next one and changes
nothing here.

A plugin may add its own phases with a constraint: the editor adds `ui`
to the render pipeline after `post_render`, which is exactly the case the
user described ("phases depending on other phases").

### The frame as pipelines

The window plugin's runner is the loop both apps have today, with the
frame split across the pipelines:

```
run():
    while !quit:
        dt from SDL_GetTicksNS
        poll SDL into WindowEvents          (the runner, before update)
        engine.update(dt)                   -> update pipeline
        if GPU::begin_frame:
            ctx.target = backbuffer, UNDEFINED
            engine.render(ctx)              -> render pipeline
            GPU::end_frame
```

Events are data, not a hook: the runner fills `WindowEvents` (a
`DynamicArray<SDL_Event>` cleared each frame) and systems in the `events`
phase read it. The window plugin's own `events` system turns QUIT / close
into `engine.quit()` when `quit_on_close` is set (the standalone's case;
the editor clears the flag and asks about unsaved changes first) and
PIXEL_SIZE_CHANGED into `GPU::mark_resized`.

The render pipeline is where the editor stops being special-cased in a
`frame()` of its own: `ctx` is shared by every system of the run, so

- `RenderPlugin`, phase `render`: clear color and time into the renderer,
  `RENDERER::render(renderer, ctx)` into `ctx.target`, `ctx.target_state`
  updated. (What `Engine::render` does today.)
- `EditorViewportPlugin`, phase `pre_render`: `ctx.target` = this slot's
  viewport texture with its recorded state. Phase `post_render`: barrier it
  to SAMPLED, record the state, `ctx.target` = the backbuffer again,
  UNDEFINED.
- `EditorUiPlugin`, phase `ui` (added after `post_render`): ImGui new
  frame, draw the panels, render the draw data into `ctx.target`.

Before the frame, in `update`: the fly camera (`update`), the grid pass
bookkeeping and the viewport resize (`post_update`, the resize must happen
outside `begin_frame` / `end_frame` since it waits for the GPU).

### The shared plugins

Runtime (`engine/runtime/include/engine/plugins/`), added by both apps:

- `WindowPlugin` (`window`): SDL, `DisplayWindow`, `GPU::init` into
  `Engine::gpu`, the runner, `WindowEvents`, `quit_on_close`.
  `WindowDesc` is title, size, swapchain format (the editor wants UNORM).
- `CorePlugin` (`core`): provider, World, Scene, AssetEntities,
  FrameAllocator. Takes `require_cooked` for the provider (the standalone's
  `--cooked`).
- `RenderPlugin` (`render`): requires `window` and `core`; Renderer,
  `RENDER_COMPONENTS::register_all`, the `render` system, `clear_color`.
- `ProjectPlugin` (`project`): requires `core` and `render`. `Project`
  moves from `editor/src/project.hpp` to runtime (root + name, the
  comment "editor-only" was true until the standalone grew `--project`).
  `build` opens the directory and creates the singleton; `pre_startup`
  scans it: every `.lunaasset` / `.material` / `.scene` through
  `load_asset_file`, every `.slang` / `.glsl` in the engine's `render/`
  and anywhere under the project as a shader asset entity (the editor's
  `register_shader_assets`, moved), `set_project_render_dir`. `PROJECT::scan(engine)`
  is the same function for the editor's Refresh Assets.
- `ScenePlugin` (`scene`): requires `project`. Phase `startup`: load the
  scene path given (`scenes/demo.scene` by default, the standalone's
  `--scene`), else `RENDER_DEMO::spawn`. Option `tag_render_camera`
  (standalone true: tag the first scene camera when none carries
  `RenderCamera`; editor false, it renders from its own camera).
- `DemoPlugin` (`demo`): the `RENDER_DEMO` post / shadow toggles as state
  (`set_post(engine, bool)` ...), F5 reload shaders, F6 / F7 toggles in an
  `events` system, `--post` / `--shadow` as constructor arguments. The
  editor's Render menu reads and writes this plugin through
  `engine.get_plugin<DemoPlugin>()` instead of its own booleans.

Standalone `main.cpp` becomes: parse arguments, add the defaults, project,
scene (tagging the camera), demo, `init`, `run`, `shutdown`.

Editor-only plugins (`editor/src/plugins/`): `EditorCameraPlugin`
(`EditorCameraController` + `Renderer::camera`), `EditorGridPlugin`
(`EditorGrid`), `EditorViewportPlugin` (the viewport textures and the two
render-pipeline systems), `EditorUiPlugin` (ImGui context and backends,
the panels, the menus, selection, scene dialogs, Save All: today's
`App` minus window / GPU / loop, kept as one `Editor` struct the plugin
owns, so the panel code moves rather than changes). `INSPECTORS::register_all`
goes in `EditorUiPlugin::build`. The editor's `App` struct goes away;
`main.cpp` adds the plugins.

### What `Engine` looks like after

```cpp
struct Engine {
    bool init();            // builds plugins in dependency order, runs `startup`
    void run();             // the runner, or returns at once
    void quit();            // asks the runner to stop after this frame
    void shutdown();        // runs `shutdown`, then frees the plugins and the singletons
    void update(f32 dt);    // FrameAllocator reset, clock, `update` pipeline
    void render(FrameContext& ctx);   // `render` pipeline with ctx as argument

    template <typename T, typename... Args> T* add_plugin(Args&&... args);
    template <typename T> T* require();          // build-on-demand, see above
    template <typename T> T* get_plugin() const; // never builds
    template <typename T> bool is_plugin_added() const;

    SystemHandle add_system(PhaseHandle phase, const char* name, SystemFn fn, void* user_data);
    // ... singletons, load_asset_file, the scene functions, unchanged

    Scheduler scheduler;
    struct { PipelineHandle startup, update, render, shutdown; } pipelines;
    struct { PhaseHandle pre_startup, startup, post_startup, events, pre_update, update, post_update, pre_render, render, post_render, pre_shutdown, shutdown, post_shutdown; } phases;
    PluginRegistry plugins;
    EngineRunner runner = nullptr;   // void (*)(Engine&), set by WindowPlugin
    GpuContext* gpu = nullptr;
    f64 time = 0.0;
    f32 last_dt = 0.0f;
};
```

The scene functions (`load_scene`, `save_scene`, ...) stay on the Engine:
they are the engine's scene API, used by plugins and the editor alike, and
nothing about them is per-app.

## Steps

Each a commit, each leaving both apps running:

1. Core: `scheduler/scheduler.hpp` + `.cpp`, tests (insertion and
   reverse-insertion order, `phase_after` / `system_before`, cycle
   reported and `run` refused, disabled systems skipped, remove during
   run, generations).
2. Runtime: `Plugin`, `PluginRegistry` (add / require / get, build order,
   cycle detection, the final free), the Engine's pipelines and phases,
   `Engine::init` building plugins and running `startup`, `update` /
   `render` running their pipelines, `Engine::shutdown` running the
   `shutdown` pipeline. `CorePlugin` and `RenderPlugin` carrying what
   `Engine::init` / `shutdown` / `render` do today, with the Shader /
   Material deletion as the render plugin's `pre_shutdown` system. Both apps
   still own window / GPU / loop and call `engine.init()` after
   `add_plugin<CorePlugin>()` + `add_plugin<RenderPlugin>()`; verified
   under the validation layers (the shutdown order is the risk here).
3. Runtime: `WindowPlugin` with the runner and `WindowEvents`; standalone
   switched to `Engine::run`.
4. Runtime: `ProjectPlugin` (Project moved), `ScenePlugin`, `DemoPlugin`;
   standalone reduced to `main.cpp`.
5. Editor: the editor plugins, `App` dissolved into `main.cpp` + `Editor`.
   The Render menu through `DemoPlugin`, Refresh Assets through
   `PROJECT::scan`.
6. Docs: `engine/runtime/CLAUDE.md` (plugins, pipelines, phases, the
   shared plugins), `engine/core/CLAUDE.md` (scheduler), `editor/CLAUDE.md`
   (the editor plugins), the "owns no window" sentence rewritten.
