#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/scene/inspector.hpp"

#include <cstring>
#include <type_traits>

struct World;

// What the Inspector panel hands a component's draw function (see
// engine/scene/inspector.hpp): the entity being inspected, the id being
// drawn, and a scratch area that survives from frame to frame for as long
// as the same entity stays selected, so a draw function can keep edit
// state the component itself does not store (the Transform's Euler
// fields). The scratch is zeroed the first time it is used for an
// (entity, id) and must hold trivially copyable data only.
constexpr usz INSPECTOR_SCRATCH_CAPACITY = 128;

struct InspectorContext {
    World* world = nullptr;
    EntityId entity = 0;
    Id id = 0;
    // The panel's scratch slot for (entity, id); `scratch_fresh` is true
    // the first frame it is handed out.
    void* scratch_bytes = nullptr;
    bool scratch_fresh = false;

    // The scratch as a T. Zeroed memory on the first use, so T must treat
    // all-zero bytes as "no state yet".
    template <typename T>
    T* scratch() const {
        static_assert(std::is_trivially_copyable_v<T>, "inspector scratch holds plain data only");
        static_assert(sizeof(T) <= INSPECTOR_SCRATCH_CAPACITY, "inspector scratch is too small for T");
        return static_cast<T*>(this->scratch_bytes);
    }
};
