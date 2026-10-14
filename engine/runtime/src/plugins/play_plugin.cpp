#include "engine/plugins/play_plugin.hpp"

#include "engine/asset/asset_entity.hpp"
#include "engine/ecs/world.hpp"
#include "engine/ecs/world_clone.hpp"
#include "engine/engine.hpp"
#include "engine/memory/frame_allocator.hpp"
#include "engine/plugins/core_plugin.hpp"
#include "engine/plugins/render_plugin.hpp"
#include "engine/render/components.hpp"
#include "engine/render/materials.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/scene.hpp"
#include "engine/utils/log.hpp"

#include <cstring>

namespace {

constexpr const char* PIPELINE_BEGIN = "play_begin";
constexpr const char* PIPELINE_UPDATE = "play_update";
constexpr const char* PIPELINE_END = "play_end";

// The camera a game is drawn from: the scene's own, under the scene root,
// tagged RenderCamera if one is, else the first with a Transform. 0 when
// the scene has none, and the renderer falls back to whatever carries the
// tag (in the editor, the fly camera the clone brought along).
EntityId scene_camera(World& world, Scene& scene) {
    EntityId camera = 0;
    world.query<Transform, Camera>().with<RenderCamera>().each([&](const EntityId entity, Transform&, Camera&) {
        if (camera == 0 && scene.contains(entity)) {
            camera = entity;
        }
    });
    if (camera != 0) {
        return camera;
    }
    world.query<Transform, Camera>().each([&](const EntityId entity, Transform&, Camera&) {
        if (camera == 0 && scene.contains(entity)) {
            camera = entity;
        }
    });
    return camera;
}

} // namespace

// --- Build ---------------------------------------------------------------------

bool PlayPlugin::build(Engine& engine) {
    if (engine.require<CorePlugin>() == nullptr) {
        return false;
    }
    // Built after the render plugin when there is one, so that in the
    // shutdown pipeline (reverse insertion order) the instance is stopped
    // before the render plugin deletes the Shader and Material entities of
    // the active world, which must be the authoring one by then.
    if (engine.is_plugin_added<RenderPlugin>() && engine.require<RenderPlugin>() == nullptr) {
        return false;
    }
    SINGLETON_SCOPE::declare<PlayState>(SCOPE_LOCAL, "PlayState");
    Scheduler& s = engine.scheduler;
    this->pipelines.begin = s.add_pipeline(PIPELINE_BEGIN);
    this->pipelines.update = s.add_pipeline(PIPELINE_UPDATE);
    this->pipelines.end = s.add_pipeline(PIPELINE_END, PIPELINE_ORDER_REVERSE_INSERTION);
    if (!this->pipelines.begin.is_valid() || !this->pipelines.update.is_valid() || !this->pipelines.end.is_valid()) {
        return false;
    }
    this->phases.begin = s.add_phase(this->pipelines.begin, "begin");
    this->phases.pre_update = s.add_phase(this->pipelines.update, "pre_update");
    this->phases.update = s.add_phase(this->pipelines.update, "update");
    this->phases.post_update = s.add_phase(this->pipelines.update, "post_update");
    this->phases.end = s.add_phase(this->pipelines.end, "end");
    s.phase_after(this->phases.update, this->phases.pre_update);
    s.phase_after(this->phases.post_update, this->phases.update);

    // Requests first, then the game, both inside the engine's update so the
    // window's events of the frame are in and nothing is being recorded.
    engine.add_system(engine.phases.pre_update, PlayPlugin::NAME, &PlayPlugin::control, this);
    engine.add_system(engine.phases.update, PlayPlugin::NAME, &PlayPlugin::tick, this);
    engine.add_system(engine.phases.post_startup, PlayPlugin::NAME, &PlayPlugin::autostart, this);
    engine.add_system(engine.phases.pre_shutdown, PlayPlugin::NAME, &PlayPlugin::teardown, this);
    return true;
}

PlayState* PlayPlugin::state(Engine& engine) const {
    if (this->mode == PLAY_STOPPED) {
        return nullptr;
    }
    return this->isolate ? this->instance.get<PlayState>() : engine.root.get<PlayState>();
}

// --- The instance ------------------------------------------------------------------

bool PlayPlugin::begin_instance(Engine& engine) {
    Singletons& root = engine.root;
    World* source = root.get<World>();
    Scene* source_scene = root.get<Scene>();
    if (source == nullptr || source_scene == nullptr) {
        LOG::error(LOG_CATEGORY::ENGINE, "play: the engine has no World and Scene to clone");
        return false;
    }
    this->instance.parent = &root;

    World* world = this->instance.create<World>();
    world->init();
    world->strict_components = this->strict_components;
    const WorldCloneResult cloned = WORLD_CLONE::clone(*source, *world);
    if (!cloned.ok) {
        LOG::error(LOG_CATEGORY::ENGINE, "play: the world could not be cloned: %s", cloned.error != nullptr ? cloned.error : "?");
        world->free();
        this->instance.free();
        this->instance.parent = nullptr;
        return false;
    }

    // The scene root came over under its id; the index is rebuilt from the
    // asset entities that came with it.
    Scene* scene = this->instance.create<Scene>();
    scene->init(world, source_scene->root);
    scene->set_current(source_scene->current, source_scene->current_path);
    AssetEntities* assets = this->instance.create<AssetEntities>();
    assets->init(world);
    assets->rebuild();
    FrameAllocator* frame_allocator = this->instance.create<FrameAllocator>(ENGINE_FRAME_ARENA_BYTES);

    // A view of the clone with the shared GPU state. The hooks it installs
    // own only what is created above the fence: the programs below it are
    // the authoring world's, reached through the same ids.
    const Renderer* source_renderer = root.get<Renderer>();
    RenderResources* resources = root.get<RenderResources>();
    if (source_renderer != nullptr && resources != nullptr) {
        Renderer* renderer = this->instance.create<Renderer>();
        if (!RENDERER::init(*renderer, resources, world, root.get<AssetResourceProvider>(), frame_allocator)) {
            LOG::error(LOG_CATEGORY::ENGINE, "play: the instance renderer could not be made");
            this->end_instance(engine);
            return false;
        }
        renderer->owns_from = cloned.fence;
        memcpy(renderer->clear_color, source_renderer->clear_color, sizeof(renderer->clear_color));
        RENDER_COMPONENTS::install_hooks(*world, *renderer);
        MATERIAL::index_loaded(*renderer);
        renderer->camera = scene_camera(*world, *scene);
    }

    PlayState* play = this->instance.create<PlayState>();
    play->fence = cloned.fence;
    play->isolated = true;
    if (!this->instance.check_locals()) {
        LOG::error(LOG_CATEGORY::ENGINE, "play: the instance lacks a local singleton; not starting");
        this->end_instance(engine);
        return false;
    }
    engine.active = &this->instance;
    LOG::info(LOG_CATEGORY::ENGINE, "Play: %llu entities cloned, runtime ids start at %llu", static_cast<unsigned long long>(cloned.entities),
        static_cast<unsigned long long>(cloned.fence));
    return true;
}

void PlayPlugin::end_instance(Engine& engine) {
    engine.active = &engine.root;
    // The world first: its removed hooks reach the renderer (blocks,
    // pipelines of shaders loaded during play) and the index, which must
    // still be there. Then the view, its transient textures released at the
    // end of the frames in flight, since the GPU may be drawing the last
    // play frame.
    if (World* world = this->instance.get<World>()) {
        world->free();
    }
    if (Renderer* renderer = this->instance.get<Renderer>()) {
        RENDERER::shutdown(*renderer, false);
    }
    if (AssetEntities* assets = this->instance.get<AssetEntities>()) {
        assets->free();
    }
    if (FrameAllocator* frame_allocator = this->instance.get<FrameAllocator>()) {
        frame_allocator->release();
    }
    this->instance.free();
    this->instance.parent = nullptr;
}

// --- Controls ----------------------------------------------------------------------

bool PlayPlugin::play(Engine& engine) {
    if (this->mode != PLAY_STOPPED) {
        return false;
    }
    if (this->isolate) {
        if (!this->begin_instance(engine)) {
            return false;
        }
    } else {
        PlayState* play = engine.root.create<PlayState>();
        if (play == nullptr) {
            return false;
        }
        LOG::info(LOG_CATEGORY::ENGINE, "Play");
    }
    // state() reads the mode, so it comes first.
    this->mode = PLAY_PLAYING;
    this->step_pending = false;
    PlayState* play = this->state(engine);
    play->mode = PLAY_PLAYING;
    play->time_scale = this->time_scale;
    engine.scheduler.run(this->pipelines.begin, &engine, play, 0.0f);
    return true;
}

bool PlayPlugin::stop(Engine& engine) {
    if (this->mode == PLAY_STOPPED) {
        return false;
    }
    PlayState* play = this->state(engine);
    engine.scheduler.run(this->pipelines.end, &engine, play, 0.0f);
    if (this->isolate) {
        this->end_instance(engine);
    } else {
        engine.root.remove<PlayState>();
    }
    this->mode = PLAY_STOPPED;
    this->step_pending = false;
    LOG::info(LOG_CATEGORY::ENGINE, "Stop");
    return true;
}

bool PlayPlugin::pause(Engine& engine) {
    if (this->mode != PLAY_PLAYING) {
        return false;
    }
    this->mode = PLAY_PAUSED;
    if (PlayState* play = this->state(engine)) {
        play->mode = PLAY_PAUSED;
        play->delta_time = 0.0f;
    }
    return true;
}

bool PlayPlugin::resume(Engine& engine) {
    if (this->mode != PLAY_PAUSED) {
        return false;
    }
    this->mode = PLAY_PLAYING;
    this->step_pending = false;
    if (PlayState* play = this->state(engine)) {
        play->mode = PLAY_PLAYING;
    }
    return true;
}

bool PlayPlugin::step(Engine&) {
    if (this->mode != PLAY_PAUSED) {
        return false;
    }
    this->step_pending = true;
    return true;
}

void PlayPlugin::request(const Request request) {
    this->requests.push(static_cast<u32>(request));
}

// --- Systems -----------------------------------------------------------------------

void PlayPlugin::control(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    PlayPlugin* self = static_cast<PlayPlugin*>(ctx.user_data);
    for (usz i = 0; i < self->requests.count; ++i) {
        switch (static_cast<Request>(self->requests[i])) {
        case REQUEST_PLAY:
            self->play(engine);
            break;
        case REQUEST_STOP:
            self->stop(engine);
            break;
        case REQUEST_PAUSE:
            self->pause(engine);
            break;
        case REQUEST_RESUME:
            self->resume(engine);
            break;
        case REQUEST_STEP:
            self->step(engine);
            break;
        case REQUEST_TOGGLE:
            if (self->mode == PLAY_STOPPED) {
                self->play(engine);
            } else {
                self->stop(engine);
            }
            break;
        }
    }
    self->requests.clear();
}

void PlayPlugin::tick(SystemContext& ctx) {
    Engine& engine = ENGINE::of(ctx);
    PlayPlugin* self = static_cast<PlayPlugin*>(ctx.user_data);
    if (self->mode == PLAY_STOPPED) {
        return;
    }
    PlayState* play = self->state(engine);
    if (play == nullptr) {
        return;
    }
    play->time_scale = self->time_scale;
    const bool stepping = self->mode == PLAY_PAUSED && self->step_pending;
    if (self->mode != PLAY_PLAYING && !stepping) {
        play->delta_time = 0.0f;
        return;
    }
    const f32 dt = stepping ? self->step_dt : ctx.dt;
    self->step_pending = false;
    play->delta_time = dt * play->time_scale;
    play->time += play->delta_time;
    play->frame += 1;
    engine.scheduler.run(self->pipelines.update, &engine, play, play->delta_time);
}

void PlayPlugin::autostart(SystemContext& ctx) {
    PlayPlugin* self = static_cast<PlayPlugin*>(ctx.user_data);
    if (self->autoplay) {
        self->play(ENGINE::of(ctx));
    }
}

void PlayPlugin::teardown(SystemContext& ctx) {
    PlayPlugin* self = static_cast<PlayPlugin*>(ctx.user_data);
    self->stop(ENGINE::of(ctx));
    self->requests.free();
}
