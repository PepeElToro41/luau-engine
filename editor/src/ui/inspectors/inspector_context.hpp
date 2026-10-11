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

// What a draw function may ask the App to do after the draw, since it must
// not add components itself (loading a material or a shader creates or
// fills an entity): the Inspector panel keeps the last request of a draw
// and the App performs it and resets it (App::apply_inspector_request).
enum struct InspectorRequestKind : u8 {
    NONE,
    // Load the .material asset entity `asset` as a Material (MATERIAL::load);
    // `target`'s renderer component already points at it.
    LOAD_MATERIAL,
    // Load the shader asset entity `asset` (SHADER_LIBRARY::load by its name)
    // and make it the shader of the Material on `target` (MATERIAL::set_shader).
    SET_MATERIAL_SHADER,
};

struct InspectorRequest {
    InspectorRequestKind kind = InspectorRequestKind::NONE;
    EntityId target = 0;
    EntityId asset = 0;
};

struct InspectorContext {
    World* world = nullptr;
    EntityId entity = 0;
    Id id = 0;
    // The panel's scratch slot for (entity, id); `scratch_fresh` is true
    // the first frame it is handed out.
    void* scratch_bytes = nullptr;
    bool scratch_fresh = false;
    // Set by the draw function; the panel hands it to the App after the draw.
    InspectorRequest request;

    // The scratch as a T. Zeroed memory on the first use, so T must treat
    // all-zero bytes as "no state yet".
    template <typename T>
    T* scratch() const {
        static_assert(std::is_trivially_copyable_v<T>, "inspector scratch holds plain data only");
        static_assert(sizeof(T) <= INSPECTOR_SCRATCH_CAPACITY, "inspector scratch is too small for T");
        return static_cast<T*>(this->scratch_bytes);
    }
};
