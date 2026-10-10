#include "engine/scene/inspector.hpp"

#include "engine/ecs/ecs.hpp"

#include <cstring>

void INSPECTOR::expose(World& world, const Id id, const char* name, const InspectorDrawFn draw, const u32 order) {
    if (id == 0 || ECS::IS_PAIR(id) || draw == nullptr || !world.alive(id)) {
        return;
    }
    Inspector inspector;
    if (name != nullptr) {
        strncpy(inspector.name, name, ENTITY_NAME_CAPACITY - 1);
    }
    inspector.draw = draw;
    inspector.order = order;
    world.set(id, inspector);
}

const Inspector* INSPECTOR::of(World& world, const Id id) {
    if (id == 0 || ECS::IS_PAIR(id) || !world.alive(id)) {
        return nullptr;
    }
    return world.get<Inspector>(id);
}
