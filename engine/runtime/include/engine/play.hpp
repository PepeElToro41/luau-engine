#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/scheduler/scheduler.hpp"

// The facts of a running game: a PlayState singleton exists while a game
// runs and not otherwise, so `engine.get_singleton<PlayState>()` is the
// "are we playing" test for any system, and its fields are the game's own
// clock, apart from Engine::time. PlayPlugin creates it (in the play
// instance's store when Play clones the world, in the root's when the game
// runs in place as the standalone does), advances it, hands it to the
// systems of the play pipelines as the run's argument (PLAY::state(ctx))
// and removes it at Stop.
enum PlayMode : u32 {
    PLAY_STOPPED = 0,
    PLAY_PLAYING = 1,
    PLAY_PAUSED = 2,
};

struct PlayState {
    PlayMode mode = PLAY_PLAYING;
    // Seconds of play, scaled by time_scale, pauses excluded.
    f64 time = 0.0;
    // This frame's scaled step; 0 while paused.
    f32 delta_time = 0.0f;
    f32 time_scale = 1.0f;
    // Play updates run so far.
    u64 frame = 0;
    // The first entity id created at runtime, when the game runs in a clone
    // of the authoring world: every id at or above it did not exist before
    // Play. 0 when the game runs in place.
    EntityIdLow fence = 0;
    // Running in a clone, in a store of its own (see Engine::active).
    bool isolated = false;

    bool is_paused() const { return this->mode == PLAY_PAUSED; }
    // Whether `entity` was created after Play (always false in place).
    bool is_runtime_entity(const EntityId entity) const { return this->fence != 0 && ECS::ENTITY_LOW(entity) >= this->fence; }
};

namespace PLAY {

// The PlayState a system of the play pipelines was run with.
inline PlayState& state(SystemContext& ctx) {
    return *static_cast<PlayState*>(ctx.argument);
}

} // namespace PLAY
