#include "engine/scene/inspector.hpp"

#include "engine/ecs/ecs.hpp"
#include "engine/memory/temporal_allocator.hpp"

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

void INSPECTOR::addable(World& world, const Id id, const InspectorInitFn init) {
    if (id == 0 || ECS::IS_PAIR(id) || !world.alive(id)) {
        return;
    }
    Addable addable;
    addable.init = init;
    world.set(id, addable);
}

const Addable* INSPECTOR::addable_of(World& world, const Id id) {
    if (id == 0 || ECS::IS_PAIR(id) || !world.alive(id)) {
        return nullptr;
    }
    return world.get<Addable>(id);
}

bool INSPECTOR::can_add(World& world, const EntityId entity, const Id id) {
    return INSPECTOR::addable_of(world, id) != nullptr && world.alive(entity) && !world.has(entity, id);
}

bool INSPECTOR::add(World& world, const EntityId entity, const Id id) {
    const Addable* addable = INSPECTOR::addable_of(world, id);
    if (addable == nullptr || !world.alive(entity) || world.has(entity, id)) {
        return false;
    }
    // Copied out: init may touch the world and move the component entity.
    const InspectorInitFn init = addable->init;
    // A component's size is its ECS::COMPONENT data (see ComponentRecord);
    // a tag has none.
    const TypeInfo* info = static_cast<const TypeInfo*>(world.get(id, ECS::COMPONENT));
    if (info == nullptr || info->length == 0) {
        world.add(entity, id);
        return true;
    }
    TemporalAllocator temp = TemporalAllocator::create();
    void* data = temp.allocate(info->length, info->alignment);
    memset(data, 0, info->length);
    if (init != nullptr) {
        init(world, entity, data);
    }
    world.set(entity, id, data);
    return true;
}
