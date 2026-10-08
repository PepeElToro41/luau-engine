#include "engine/ecs/world.hpp"

#include "engine/ecs/archetype/archetype_listener.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/entity.hpp"
#include "engine/ecs/hierarchy.hpp"
#include "engine/ecs/hooks.hpp"
#include "engine/ecs/query/monitor.hpp"
#include "engine/ecs/query/observer.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/memory/temporal_allocator.hpp"

#include <cstdio>
#include <new>

World::World(BaseAllocator* allocator) :
    allocator(allocator),
    entity_index(allocator),
    archetypes(allocator),
    component_records(allocator),
    archetype_index(allocator),
    component_index(allocator),
    type_index(allocator),
    type_info_index(allocator),
    monitors(allocator),
    observers(allocator),
    archetype_listeners(allocator),
    archetype_listener_keys(allocator)
{}

World::World() : World(MEMORY::heap_allocator()) {}

void World::init() {
    this->root_archetype = Archetype::create_archetype(this, ArchetypeType());

    for (Id id = 1; id <= ECS::REST; ++id) {
        this->make_alive(id);
    }
    this->set_range(ECS::REST + 1, 0);

    // ECS::COMPONENT stores a TypeInfo per component entity. Its own TypeInfo
    // has to be known before its first column is created (ComponentRecord
    // falls back to type_info_index for it), and it is then set on the
    // entity too so COMPONENT describes itself like any other component.
    const TypeInfo component_type_info { sizeof(TypeInfo), alignof(TypeInfo) };
    this->type_info_index.insert(ECS::COMPONENT, component_type_info);
    ENTITY::set(this, ECS::COMPONENT, ECS::COMPONENT, &component_type_info);

    // Traits of the built-in relations. A relation's traits are snapshotted
    // into its ComponentRecords when they are first created, so these go in
    // before any pair with these relations exists: the policy relations are
    // made exclusive before CHILD_OF receives its policy pair.
    ENTITY::add(this, ECS::ON_DELETE, ECS::EXCLUSIVE);
    ENTITY::add(this, ECS::ON_DELETE_TARGET, ECS::EXCLUSIVE);
    ENTITY::add(this, ECS::CHILD_OF, ECS::EXCLUSIVE);
    ENTITY::add(this, ECS::CHILD_OF, ECS::TRAVERSABLE);
    ENTITY::add(this, ECS::CHILD_OF, ECS::PAIR(ECS::ON_DELETE_TARGET, ECS::DELETE));
    ENTITY::add(this, ECS::IS_A, ECS::TRAVERSABLE);

    // Nothing built in may be deleted: not the component ids (type_index
    // would keep pointing at a dead entity) nor the ids the ECS relies on.
    for (Id id = 1; id <= ECS::REST; ++id) {
        ENTITY::add(this, id, ECS::PAIR(ECS::ON_DELETE, ECS::PANIC));
    }
}

void World::destroy(World* world) {
    if (world == nullptr) {
        return;
    }
    BaseAllocator* allocator = world->allocator;
    world->free();
    allocator->free(world);
}

EntityId World::new_entity() {
    return ENTITY::create(this);
}

void World::make_alive(const EntityId entity) {
    ENTITY::make_alive(this, entity);
}

bool World::alive(const EntityId entity) const {
    return ENTITY::is_alive(this, entity);
}

bool World::delete_entity(const EntityId entity) {
    return ENTITY::delete_entity(this, entity);
}

void World::clear(const EntityId entity) {
    ENTITY::clear(this, entity);
}

void World::set_range(const EntityIdLow min, const EntityIdLow max) {
    this->entity_index.set_range(min, max);
}

bool World::has(const EntityId entity, const Id id) const {
    return ENTITY::has(this, entity, id);
}

void* World::get(const EntityId entity, const Id id) const {
    return ENTITY::get(this, entity, id);
}

void World::add(const EntityId entity, const Id id) {
    if (id == 0) {
        return;
    }
    ENTITY::add(this, entity, id);
}

void World::remove(const EntityId entity, const Id id) {
    if (id == 0) {
        return;
    }
    ENTITY::remove(this, entity, id);
}

void World::set(const EntityId entity, const Id id, const void* data) {
    if (id == 0) {
        return;
    }
    ENTITY::set(this, entity, id, data);
}

void World::modified(const EntityId entity, const Id id) {
    if (id == 0) {
        return;
    }
    ENTITY::modified(this, entity, id);
}

bool World::relation_has_data(const EntityId first) const {
    const EntityId entity = this->entity_index.get_current(first);
    if (entity == 0) {
        return false;
    }
    const TypeInfo* type_info = static_cast<const TypeInfo*>(ENTITY::get(this, entity, ECS::COMPONENT));
    return type_info != nullptr && type_info->length != 0;
}

EntityId World::pair_first(const Id pair) const {
    return this->entity_index.get_current(ECS::PAIR_FIRST(pair));
}

EntityId World::pair_second(const Id pair) const {
    return this->entity_index.get_current(ECS::PAIR_SECOND(pair));
}

u32 World::depth(const EntityId entity, const Id relation) {
    if (relation == 0) {
        return 0;
    }
    return HIERARCHY::depth(this, entity, ECS::ENTITY_LOW(relation));
}

const TypeInfo* World::get_type_info(const ComponentId id) const {
    const TypeInfo* type_info = this->type_info_index.find(id);
    if (type_info == nullptr || type_info->length == 0) {
        return nullptr;
    }
    return type_info;
}

EntityId World::claim_component_id(const TypeId type_id) {
    if (this->next_component_id > ECS::MAX_COMPONENT_ID) {
        fprintf(stderr, "[ecs] error: out of component ids (max %llu); cannot register a new component or tag type\n",
            ECS::MAX_COMPONENT_ID);
        return 0;
    }
    const EntityId entity = this->next_component_id;
    this->next_component_id += 1;
    this->type_index.insert(type_id, entity);
    return entity;
}

HookList* World::hook_list_for(const Id id, const bool create) {
    HookList** slot = nullptr;
    if (id == ECS::WILDCARD) {
        slot = &this->any_hooks;
    } else if (id == ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD)) {
        slot = &this->any_pair_hooks;
    } else {
        if (create) {
            return ComponentRecord::component_record_ensure(this, id)->ensure_hooks();
        }
        const ComponentRecord* record = ComponentRecord::component_record_find(this, id);
        return record != nullptr ? record->hooks : nullptr;
    }

    if (*slot == nullptr && create) {
        *slot = this->allocator->allocate_array<HookList>(1);
        (*slot)->initialize(this->allocator);
    }
    return *slot;
}

HookId World::hook(const HookKind kind, const Id id, const HookCallback callback, void* user_data) {
    if (id == 0 || callback == nullptr) {
        return 0;
    }
    HookList* list = this->hook_list_for(ECS::FOLD_ANY(id), true);
    const HookId hook_id = this->next_hook_id++;
    list->add(kind, callback, user_data, hook_id);
    this->hook_counts[kind]++;
    return hook_id;
}

HookId World::hook_added(const Id id, const HookCallback callback, void* user_data) {
    return this->hook(HOOK_ADDED, id, callback, user_data);
}

HookId World::hook_removed(const Id id, const HookCallback callback, void* user_data) {
    return this->hook(HOOK_REMOVED, id, callback, user_data);
}

HookId World::hook_changed(const Id id, const HookCallback callback, void* user_data) {
    return this->hook(HOOK_CHANGED, id, callback, user_data);
}

bool World::unhook(const Id id, const HookId hook) {
    if (id == 0 || hook == 0) {
        return false;
    }
    HookList* list = this->hook_list_for(ECS::FOLD_ANY(id), false);
    if (list == nullptr) {
        return false;
    }
    HookKind kind;
    if (!list->remove(hook, &kind)) {
        return false;
    }
    this->hook_counts[kind]--;
    return true;
}

ObserverId World::monitor(const QueryTerm* terms, const usz term_count, const MonitorCallback callback, void* user_data) {
    return MONITOR::create(this, terms, term_count, callback, user_data);
}

bool World::unmonitor(const ObserverId id) {
    return MONITOR::destroy(this, id);
}

ObserverId World::observe(const QueryTerm* terms, const usz term_count, const ObserverCallback callback, void* user_data) {
    return OBSERVER::create(this, terms, term_count, callback, user_data);
}

bool World::unobserve(const ObserverId id) {
    return OBSERVER::destroy(this, id);
}

void World::fire_shutdown_hooks() {
    if (this->hook_counts[HOOK_REMOVED] == 0 || this->root_archetype == nullptr) {
        return;
    }

    // Hooks may change other entities while this runs, which reorders the
    // index, so walk a snapshot of the alive ids. It is one id per entity and
    // can outgrow the scratch arena, hence the world's allocator.
    const usz count = this->entity_index.count();
    EntityId* entities = this->allocator->allocate_array<EntityId>(count);
    for (usz i = 0; i < count; i++) {
        entities[i] = this->entity_index.get_alive_id(i);
    }

    for (usz i = 0; i < count; i++) {
        const EntityId entity = entities[i];
        const EntityRecord* record = this->entity_index.get_record_alive(entity);
        if (record == nullptr || record->archetype == nullptr) {
            // Deleted by an earlier hook, or never given an archetype.
            continue;
        }
        // A hook may move this entity indirectly (deleting another entity
        // it points at), so fire from a copy of its ids, not the live type.
        TemporalAllocator temp = TemporalAllocator::create();
        const ArchetypeType ids = record->archetype->type.clone(&temp);
        for (usz j = 0; j < ids.id_count; j++) {
            HOOKS::fire(this, HOOK_REMOVED, entity, ids.ids[j]);
        }
    }

    this->allocator->free(entities);
}

void World::free() {
    this->fire_shutdown_hooks();

    // Every archetype goes away at once, so skip destroy()'s edge and record
    // unlinking (and its empty-table check) and just release what each owns.
    for (usz i = 0; i < this->archetypes.alive_count; ++i) {
        const SparseId id = this->archetypes.get_alive_id(i);
        this->archetypes.get_element_any(id)->free();
    }
    this->archetypes.free();
    this->root_archetype = nullptr;

    // Component records own maps (and possibly a pair record); release those
    // before dropping the list that holds them.
    for (usz i = 0; i < this->component_records.alive_count; ++i) {
        const SparseId id = this->component_records.get_alive_id(i);
        this->component_records.get_element_any(id)->destroy();
    }
    this->component_records.free();

    this->entity_index.free();
    this->archetype_index.free();
    this->component_index.free();
    this->type_index.free();
    this->type_info_index.free();
    this->next_component_id = 1;

    if (this->any_hooks != nullptr) {
        this->any_hooks->free();
        this->allocator->free(this->any_hooks);
        this->any_hooks = nullptr;
    }
    if (this->any_pair_hooks != nullptr) {
        this->any_pair_hooks->free();
        this->allocator->free(this->any_pair_hooks);
        this->any_pair_hooks = nullptr;
    }
    for (u32 kind = 0; kind < HOOK_KIND_COUNT; kind++) {
        this->hook_counts[kind] = 0;
    }

    // The archetype lists holding monitor and observer ids went with the
    // archetypes.
    for (auto& entry : this->monitors) {
        entry.value.matcher.free();
    }
    this->monitors.free();
    for (auto& entry : this->observers) {
        entry.value.matcher.free();
        this->allocator->free(entry.value.terms);
    }
    this->observers.free();
    this->next_observer_id = 1;
    this->hierarchy_generation = 0;

    // Nothing fires for the archetypes that just went away; whoever
    // registered a listener is being torn down with the world.
    ARCHETYPE_LISTENER::free_all(this);
}
