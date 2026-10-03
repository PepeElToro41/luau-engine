#include "engine/ecs/hooks.hpp"

#include "engine/ecs/component_record.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/world.hpp"

#include <new>

void HookList::initialize(BaseAllocator* allocator) {
    for (u32 kind = 0; kind < HOOK_KIND_COUNT; kind++) {
        new (&this->lists[kind]) DynamicArray<Hook>(allocator);
    }
}

void HookList::free() {
    for (u32 kind = 0; kind < HOOK_KIND_COUNT; kind++) {
        this->lists[kind].free();
    }
}

void HookList::add(const HookKind kind, const HookCallback callback, void* user_data, const HookId id) {
    Hook hook;
    hook.callback = callback;
    hook.user_data = user_data;
    hook.id = id;
    this->lists[kind].push(hook);
}

bool HookList::remove(const HookId id, HookKind* out_kind) {
    for (u32 kind = 0; kind < HOOK_KIND_COUNT; kind++) {
        DynamicArray<Hook>& list = this->lists[kind];
        for (usz i = 0; i < list.count; i++) {
            if (list[i].id == id) {
                list.remove_at(i);
                *out_kind = static_cast<HookKind>(kind);
                return true;
            }
        }
    }
    return false;
}

void HookList::fire(const HookKind kind, World* world, const EntityId entity, const Id id) const {
    const DynamicArray<Hook>& list = this->lists[kind];
    // Hook ids grow with registration and the list keeps registration order,
    // so "the next hook" is the first one with an id above the last fired.
    // Re-finding it after every callback keeps the walk correct when a
    // callback removes hooks (itself included) or appends new ones.
    HookId last_fired = 0;
    usz i = 0;
    while (true) {
        while (i < list.count && list[i].id <= last_fired) {
            i++;
        }
        if (i >= list.count) {
            return;
        }
        const Hook hook = list[i];
        last_fired = hook.id;
        hook.callback(world, entity, id, hook.user_data);
        // The callback may have shrunk the list, or removed entries at or
        // before our slot (shifting the next hook into it). Clamp, then step
        // back over anything newer than what we just fired.
        if (i > list.count) {
            i = list.count;
        }
        while (i > 0 && (i == list.count || list[i - 1].id > last_fired)) {
            i--;
        }
    }
}

namespace HOOKS {

static void fire_record(const ComponentRecord* record, const HookKind kind, World* world, const EntityId entity, const Id id) {
    if (record != nullptr && record->hooks != nullptr) {
        record->hooks->fire(kind, world, entity, id);
    }
}

void fire(World* world, const HookKind kind, const EntityId entity, const Id id) {
    if (world->hook_counts[kind] == 0) {
        return;
    }

    const ComponentRecord* record = ComponentRecord::component_record_find(world, id);
    fire_record(record, kind, world, entity, id);

    if (ECS::IS_PAIR(id)) {
        if (record != nullptr) {
            // Concrete pair records cache their wildcard records.
            fire_record(record->first_wildcard, kind, world, entity, id);
            fire_record(record->second_wildcard, kind, world, entity, id);
        } else {
            fire_record(ComponentRecord::component_record_find(world, ECS::PAIR(ECS::PAIR_FIRST(id), ECS::WILDCARD)), kind, world, entity, id);
            fire_record(ComponentRecord::component_record_find(world, ECS::PAIR(ECS::WILDCARD, ECS::PAIR_SECOND(id))), kind, world, entity, id);
        }
        if (world->any_pair_hooks != nullptr) {
            world->any_pair_hooks->fire(kind, world, entity, id);
        }
    }

    if (world->any_hooks != nullptr) {
        world->any_hooks->fire(kind, world, entity, id);
    }
}

} // namespace HOOKS
