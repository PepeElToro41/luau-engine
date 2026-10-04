#pragma once

#include "engine/ecs/archetype.hpp"
#include "engine/ecs/archetype_listener.hpp"
#include "engine/ecs/component_record.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/entity.hpp"
#include "engine/ecs/entity_index.hpp"
#include "engine/ecs/hooks.hpp"
#include "engine/ecs/monitor.hpp"
#include "engine/ecs/observer.hpp"
#include "engine/ecs/query_term.hpp"
#include "engine/memory/base_allocator.hpp"
#include "engine/memory/heap_allocator.hpp"
#include "engine/templates/hash_map.hpp"
#include "engine/templates/sparse_list.hpp"
#include "engine/utils/type_id.hpp"

#include <type_traits>

template <typename... Ts>
struct Query;
struct QueryBuilder;

// Owns everything an ECS instance needs: the entity index, every archetype
// and component record, and the lookup tables that find them by type or id.
//
// Archetypes and component records live in sparse lists so their pointers
// stay stable; the two index maps point straight at those elements. Storage
// is lazy and there is no destructor.
//
// Build a world with World::create() and release it with World::destroy():
// archetypes and component records keep a pointer back to their world, so a
// World must never be copied or moved once it is set up. The constructors
// only wire up the allocator and leave every table empty; a world that was
// merely constructed has no root archetype and cannot hold entities.
struct World {
    BaseAllocator* allocator = nullptr;
    // The empty archetype (no ids) every new entity starts in. It is only a
    // marker: entity records point at it, but it stores no rows and no
    // columns, so nothing may insert into or resize it (see entity.cpp).
    Archetype* root_archetype = nullptr;

    EntityIndex entity_index;
    SparseList<Archetype> archetypes;
    SparseList<ComponentRecord> component_records;

    HashMap<ArchetypeType, Archetype*> archetype_index;
    HashMap<Id, ComponentRecord*> component_index;

    // C++ type (see TYPE_ID::get) -> the entity that represents that type as a
    // component in this world. Filled by component<T>().
    HashMap<TypeId, EntityId> type_index;
    // Component entity id -> storage size and alignment, for ids whose
    // TypeInfo cannot be read off the entity itself. Today that is
    // only ECS::COMPONENT: its column stores every other component's TypeInfo,
    // so its own size has to be known before that column can exist.
    HashMap<ComponentId, TypeInfo> type_info_index;
    // Next id component<T>() hands out. Component ids are the pre-registered
    // entities in [1, ECS::MAX_COMPONENT_ID]; init() makes them all alive.
    Id next_component_id = 1;

    // Hooks registered on WILDCARD (every id) and on (*, *) (every pair); all
    // other hooks live on the ComponentRecord of their id. hook_counts[kind]
    // totals every hook of that kind anywhere so dispatch can skip early.
    HookList* any_hooks = nullptr;
    HookList* any_pair_hooks = nullptr;
    u32 hook_counts[HOOK_KIND_COUNT] = {};
    HookId next_hook_id = 1;

    // Every monitor (see monitor.hpp) and observer (see observer.hpp) by id,
    // issued from one counter. The archetypes a monitor / observer matches
    // carry its id in Archetype::observers, which is what fires it.
    HashMap<ObserverId, Monitor> monitors;
    HashMap<ObserverId, Observer> observers;
    ObserverId next_observer_id = 1;

    // Archetype lifecycle listeners (see archetype_listener.hpp) by the key
    // they listen under, plus listener id -> key so remove() and fire() can
    // find a listener by id alone.
    HashMap<Id, ArchetypeListenerList*> archetype_listeners;
    HashMap<ArchetypeListenerId, Id> archetype_listener_keys;
    ArchetypeListenerId next_archetype_listener_id = 1;

    World();
    explicit World(BaseAllocator* allocator);

    // Sets the world up: the root archetype is created, every built-in id in
    // [1, ECS::REST] is registered as an alive entity parked in the root,
    // ECS::COMPONENT is given its TypeInfo so it can store other components'
    // TypeInfo, the built-in relations get their traits (CHILD_OF is
    // exclusive, traversable and deletes its children; IS_A is traversable),
    // every built-in id gets (ON_DELETE, PANIC) so it cannot be deleted, and
    // fresh entities are issued from ECS::REST + 1 onward. Release it with
    // destroy().
    void init();
    // Calls free() and returns the world's memory to the allocator it was
    // created on. Does nothing for nullptr.
    static void destroy(World* world);

    // New entity parked in the root archetype; forwards to ENTITY::create.
    // Returns 0 if the entity index is exhausted.
    EntityId new_entity();
    // Registers `entity` under the exact id and generation given, reviving it
    // if dead; forwards to ENTITY::make_alive.
    void make_alive(EntityId entity);
    // Whether `entity` is alive (right id and generation); forwards to
    // ENTITY::is_alive.
    bool alive(EntityId entity) const;
    // Deletes `entity`; forwards to ENTITY::delete_entity. Every id that
    // refers to the entity is first removed from, or cascades deletion to,
    // the entities holding it according to the ON_DELETE / ON_DELETE_TARGET
    // policies (see entity_cleanup.hpp): children of a CHILD_OF parent are
    // deleted with it, other pairs are simply removed. Then the entity's own
    // row is dropped and its id killed. Returns false and does nothing if the
    // entity is not alive or a PANIC policy protects it: every built-in id
    // has (ON_DELETE, PANIC), and a relation with (ON_DELETE_TARGET, PANIC)
    // protects its targets while pairs with it are held. (Named
    // delete_entity because `delete` is a keyword; destroy() is for the World
    // itself.)
    bool delete_entity(EntityId entity);
    // Removes every id from `entity` but keeps it alive, parked in the root
    // archetype; forwards to ENTITY::clear. Removed hooks fire for each id.
    // Unlike delete_entity nothing cascades: pairs on other entities that
    // point at this one stay. No-op if the entity is not alive.
    void clear(EntityId entity);
    // Restricts the ids new_entity() issues to [min, max]; forwards to
    // EntityIndex::set_range. A max of 0 means unbounded; a min of 0 means
    // "start after the highest id issued so far".
    void set_range(EntityIdLow min, EntityIdLow max);

    // Alive entity (with generation) behind a pair's relation / target, or 0
    // if that entity is not alive. The pair only stores the low ids; see
    // ECS::PAIR_FIRST / ECS::PAIR_SECOND for those.
    EntityId pair_first(Id pair) const;
    EntityId pair_second(Id pair) const;

    // The component entity for T, claiming it on first call: the next id in
    // [1, ECS::MAX_COMPONENT_ID] (already alive since init()) is bound to T's
    // type id and T's TypeInfo (sizeof / alignof) is stored on it as its
    // ECS::COMPONENT data. Empty types register as tags with a zero-length
    // TypeInfo. Later calls return the same entity. Returns 0 and reports an
    // error if every component id has been handed out.
    template <typename T>
    EntityId component();

    // The tag entity for T, claiming it on first call. Same as component<T>()
    // but no ECS::COMPONENT data is stored, so T carries no data regardless of
    // its size. Shares the id counter and type_index with component<T>(): the
    // first of the two called for a T decides what it is, and the other then
    // returns that same entity. Returns 0 and reports an error if every
    // component id has been handed out.
    template <typename T>
    EntityId tag();

    // --- Typed ids -----------------------------------------------------------
    // Every typed operation below resolves its id through these, registering
    // types on first use. Mind the ECS::MAX_COMPONENT_ID cap: a has<T>() on a
    // type never seen before still claims a component id for T.

    // Id for T: component<T>() for types with data, tag<T>() for empty types,
    // and pair<First, Second>() for an ECS::Pair<First, Second>. Returns 0 if
    // the type could not be registered.
    template <typename T>
    Id id();
    // Pair id (First, Second) with both sides resolved through id<>(), or 0.
    template <typename First, typename Second>
    Id pair();
    // Pair id (First, second) with a runtime target, or 0 if either side is 0.
    template <typename First>
    Id pair(EntityId second);
    // Pair id (first, second); same as ECS::PAIR.
    static constexpr Id pair(EntityId first, EntityId second) { return ECS::PAIR(first, second); }

    // --- Entity operations ---------------------------------------------------
    // Forwarders for ENTITY::has / get / add / remove / set. Each comes in the
    // same shapes, modeled on the flecs C++ API:
    //
    //   op(entity, id)                raw entity id (with generation) or pair id
    //   op<T>(entity)                 component / tag, or ECS::Pair<First, Second>
    //   op<First, Second>(entity)     pair of two types
    //   op<First>(entity, second)     typed relation, runtime target
    //   op_second<Second>(entity, first)  runtime relation, typed target
    //
    // For get / set the data type follows ECS::Pair<First, Second>::type;
    // get<First>(entity, second) reads First's data and get_second<Second>()
    // reads Second's. The raw forms are the only ones usable on a const World.

    bool has(EntityId entity, Id id) const;
    template <typename T>
    bool has(EntityId entity);
    template <typename First, typename Second>
    bool has(EntityId entity);
    template <typename First>
    bool has(EntityId entity, EntityId second);
    template <typename Second>
    bool has_second(EntityId entity, EntityId first);

    // Pointer to the entity's data for the id, or nullptr if the entity does
    // not have it or it carries no data. Invalidated by any structural change
    // on the entity's archetype. The typed forms reject data-less types at
    // compile time; get_second<Second>() returns nullptr if the relation
    // `first` carries data itself, since the pair's data is then First's.
    void* get(EntityId entity, Id id) const;
    template <typename T>
    ECS::StorageType<T>* get(EntityId entity);
    template <typename First, typename Second>
    typename ECS::Pair<First, Second>::type* get(EntityId entity);
    template <typename First>
    First* get(EntityId entity, EntityId second);
    template <typename Second>
    Second* get_second(EntityId entity, EntityId first);

    // Adds a tag or data-less pair to the entity (see ENTITY::add). Adding an
    // id that carries data prints a warning and zeroes the value; use set()
    // for components. An id of 0 is ignored.
    void add(EntityId entity, Id id);
    template <typename T>
    void add(EntityId entity);
    template <typename First, typename Second>
    void add(EntityId entity);
    template <typename First>
    void add(EntityId entity, EntityId second);
    template <typename Second>
    void add_second(EntityId entity, EntityId first);

    void remove(EntityId entity, Id id);
    template <typename T>
    void remove(EntityId entity);
    template <typename First, typename Second>
    void remove(EntityId entity);
    template <typename First>
    void remove(EntityId entity, EntityId second);
    template <typename Second>
    void remove_second(EntityId entity, EntityId first);

    // Adds the id if missing and copies the value in (see ENTITY::set). The
    // one-type form deduces T from the value: set(entity, position). The
    // relation / target forms take the value type explicitly so that a raw
    // set(entity, id, pointer) can never bind to them. set_second<Second>()
    // is refused (no-op) if the relation `first` carries data itself.
    void set(EntityId entity, Id id, const void* data);
    template <typename T>
    void set(EntityId entity, const T& value);
    template <typename First, typename Second>
    void set(EntityId entity, const typename ECS::Pair<First, Second>::type& value);
    template <typename First>
    void set(EntityId entity, EntityId second, const std::type_identity_t<First>& value);
    template <typename Second>
    void set_second(EntityId entity, EntityId first, const std::type_identity_t<Second>& value);

    // Fires the changed hooks for an id the entity holds, after its data was
    // written in place through the pointer from get() (see ENTITY::modified).
    // No-op for tags, ids the entity does not have, and wildcard pairs.
    void modified(EntityId entity, Id id);
    template <typename T>
    void modified(EntityId entity);
    template <typename First, typename Second>
    void modified(EntityId entity);
    template <typename First>
    void modified(EntityId entity, EntityId second);
    template <typename Second>
    void modified_second(EntityId entity, EntityId first);

    // --- Hooks ---------------------------------------------------------------
    // Register `callback` to run after `id` is added to an entity, before it
    // is removed (including on delete_entity), or after set() overwrites data
    // the entity already had (a set() that adds the id fires only added, with
    // the data written). `id` may be a component, tag, pair, or a pattern: (R, *) covers
    // every pair with relation R, (*, T) every pair with target T, (*, *)
    // every pair, and WILDCARD every id. ANY is treated as WILDCARD. The
    // callback receives the concrete id that fired. Returns a HookId for
    // unhook(), or 0 if `id` is 0. See hooks.hpp for what a callback may do.
    HookId hook_added(Id id, HookCallback callback, void* user_data = nullptr);
    HookId hook_removed(Id id, HookCallback callback, void* user_data = nullptr);
    HookId hook_changed(Id id, HookCallback callback, void* user_data = nullptr);
    template <typename T>
    HookId hook_added(HookCallback callback, void* user_data = nullptr);
    template <typename T>
    HookId hook_removed(HookCallback callback, void* user_data = nullptr);
    template <typename T>
    HookId hook_changed(HookCallback callback, void* user_data = nullptr);
    // Removes the hook registered under `id` (the same id or pattern given at
    // registration). False if no such hook.
    bool unhook(Id id, HookId hook);

    // --- Queries -------------------------------------------------------------
    // The trivial query (see query.hpp): the types listed are the outputs,
    // with() / without() on the returned handle add constraints, and the
    // handle iterates (each / iter / begin), counts and monitors. `flags`
    // are QueryFlags.
    template <typename... Ts>
    Query<Ts...> query(u32 flags = QUERY_NONE);
    // A builder for an engine-evaluated query (see query_builder.hpp):
    // optionals, variables, traversal, other sources. The builder owns its
    // term list; free() it or build() it.
    QueryBuilder query_build(u32 flags = QUERY_NONE);

    // --- Monitors ------------------------------------------------------------
    // Registers `callback` to run when an entity enters or leaves the result
    // set of the query `terms` describe (see monitor.hpp for the events and
    // their timing). Query<Ts...>::monitor() is the usual way in. Returns an
    // ObserverId for unmonitor(), or 0 with an error for a null callback or a
    // term shape monitors do not support.
    ObserverId monitor(const QueryTerm* terms, usz term_count, MonitorCallback callback, void* user_data = nullptr);
    // Removes the monitor. False if no monitor has that id.
    bool unmonitor(ObserverId id);

    // --- Observers -----------------------------------------------------------
    // Registers `callback` to run when an entity matching the query `terms`
    // describe moves archetype because of one of the query's ids, or has the
    // data of one of its output terms written (see observer.hpp for the
    // events and their timing). Query<Ts...>::observe() is the usual way in.
    // Returns an ObserverId for unobserve(), or 0 with an error for a null
    // callback or a term shape observers do not support.
    ObserverId observe(const QueryTerm* terms, usz term_count, ObserverCallback callback, void* user_data = nullptr);
    // Removes the observer. False if no observer has that id.
    bool unobserve(ObserverId id);

    // TypeInfo stored for `id` in type_info_index, or nullptr for tags and
    // unknown ids. Component data sizes normally come from the
    // entity's ECS::COMPONENT data instead; see ComponentRecord.
    const TypeInfo* get_type_info(ComponentId id) const;

    // Shuts the world down. First the removed hooks fire for every id of
    // every alive entity, built-ins included, as if each entity were cleared:
    // components that own resources release them and shutdown logic runs
    // while the world is still intact. Hooks may touch other entities but
    // not the one they are told about (see hooks.hpp). Then every archetype,
    // component record, entity, hook and index is released. The World itself
    // stays allocated; see destroy().
    void free();

private:
    // Binds the next free component id to `type_id` and returns it, or 0 (with
    // an error) if the component range is exhausted. The id is already alive
    // since init() with generation 0, so the low id is the full entity id.
    EntityId claim_component_id(TypeId type_id);

    // Pair id (first, Second) for the *_second forms, or 0 if either side is 0.
    template <typename Second>
    Id pair_with_second(EntityId first);

    // Whether the alive entity behind `first` (any generation) carries
    // component data, i.e. a pair with it as relation stores First's type.
    bool relation_has_data(EntityId first) const;

    // Shared body of hook_added / hook_removed / hook_changed.
    HookId hook(HookKind kind, Id id, HookCallback callback, void* user_data);
    // The first step of free(): removed hooks for every id of every alive
    // entity. Returns at once if no removed hook is registered.
    void fire_shutdown_hooks();
    // The HookList `id` (already ANY-folded) routes to: the world-wide lists
    // for WILDCARD and (*, *), the id's ComponentRecord otherwise. Allocates
    // it when `create` is set; may return nullptr otherwise.
    HookList* hook_list_for(Id id, bool create);
};

template <typename T>
EntityId World::component() {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "component<T>: pass the plain type, without const / volatile / reference");

    const TypeId type_id = TYPE_ID::get<T>();
    const EntityId* existing = this->type_index.find(type_id);
    if (existing != nullptr) {
        return *existing;
    }

    const EntityId entity = this->claim_component_id(type_id);
    if (entity == 0) {
        return 0;
    }

    TypeInfo type_info { };
    if constexpr (!std::is_empty_v<T>) {
        type_info.length = sizeof(T);
        type_info.alignment = alignof(T);
    }
    ENTITY::set(this, entity, ECS::COMPONENT, &type_info);
    return entity;
}

// --- Typed ids ---------------------------------------------------------------

template <typename T>
Id World::id() {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "id<T>: pass the plain type, without const / volatile / reference");

    if constexpr (ECS::PairTraits<T>::is_pair) {
        return this->pair<typename T::first, typename T::second>();
    } else if constexpr (std::is_empty_v<T>) {
        return this->tag<T>();
    } else {
        return this->component<T>();
    }
}

template <typename First, typename Second>
Id World::pair() {
    static_assert(!ECS::PairTraits<First>::is_pair && !ECS::PairTraits<Second>::is_pair, "pair<First, Second>: a pair cannot be a side of another pair");

    const Id first = this->id<First>();
    const Id second = this->id<Second>();
    if (first == 0 || second == 0) {
        return 0;
    }
    return ECS::PAIR(first, second);
}

template <typename First>
Id World::pair(const EntityId second) {
    static_assert(!ECS::PairTraits<First>::is_pair, "pair<First>(second): a pair cannot be a side of another pair");

    const Id first = this->id<First>();
    if (first == 0 || second == 0) {
        return 0;
    }
    return ECS::PAIR(first, second);
}

template <typename Second>
Id World::pair_with_second(const EntityId first) {
    static_assert(!ECS::PairTraits<Second>::is_pair, "*_second<Second>(first): a pair cannot be a side of another pair");

    const Id second = this->id<Second>();
    if (first == 0 || second == 0) {
        return 0;
    }
    return ECS::PAIR(first, second);
}

// --- has ---------------------------------------------------------------------

template <typename T>
bool World::has(const EntityId entity) {
    return this->has(entity, this->id<T>());
}

template <typename First, typename Second>
bool World::has(const EntityId entity) {
    return this->has(entity, this->pair<First, Second>());
}

template <typename First>
bool World::has(const EntityId entity, const EntityId second) {
    return this->has(entity, this->pair<First>(second));
}

template <typename Second>
bool World::has_second(const EntityId entity, const EntityId first) {
    return this->has(entity, this->pair_with_second<Second>(first));
}

// --- get ---------------------------------------------------------------------

template <typename T>
ECS::StorageType<T>* World::get(const EntityId entity) {
    using Storage = ECS::StorageType<T>;
    static_assert(!std::is_empty_v<Storage>, "get<T>: T carries no data; use has()");
    return static_cast<Storage*>(this->get(entity, this->id<T>()));
}

template <typename First, typename Second>
typename ECS::Pair<First, Second>::type* World::get(const EntityId entity) {
    using Storage = typename ECS::Pair<First, Second>::type;
    static_assert(!std::is_empty_v<Storage>, "get<First, Second>: the pair carries no data; use has()");
    return static_cast<Storage*>(this->get(entity, this->pair<First, Second>()));
}

template <typename First>
First* World::get(const EntityId entity, const EntityId second) {
    static_assert(!std::is_empty_v<First>, "get<First>(entity, second): First carries no data; use has() or get_second()");
    return static_cast<First*>(this->get(entity, this->pair<First>(second)));
}

template <typename Second>
Second* World::get_second(const EntityId entity, const EntityId first) {
    static_assert(!std::is_empty_v<Second>, "get_second<Second>: Second carries no data; use has_second()");
    if (this->relation_has_data(first)) {
        // The pair stores the relation's type, not Second.
        return nullptr;
    }
    return static_cast<Second*>(this->get(entity, this->pair_with_second<Second>(first)));
}

// --- add / remove ------------------------------------------------------------

template <typename T>
void World::add(const EntityId entity) {
    this->add(entity, this->id<T>());
}

template <typename First, typename Second>
void World::add(const EntityId entity) {
    this->add(entity, this->pair<First, Second>());
}

template <typename First>
void World::add(const EntityId entity, const EntityId second) {
    this->add(entity, this->pair<First>(second));
}

template <typename Second>
void World::add_second(const EntityId entity, const EntityId first) {
    this->add(entity, this->pair_with_second<Second>(first));
}

template <typename T>
void World::remove(const EntityId entity) {
    this->remove(entity, this->id<T>());
}

template <typename First, typename Second>
void World::remove(const EntityId entity) {
    this->remove(entity, this->pair<First, Second>());
}

template <typename First>
void World::remove(const EntityId entity, const EntityId second) {
    this->remove(entity, this->pair<First>(second));
}

template <typename Second>
void World::remove_second(const EntityId entity, const EntityId first) {
    this->remove(entity, this->pair_with_second<Second>(first));
}

// --- set ---------------------------------------------------------------------

template <typename T>
void World::set(const EntityId entity, const T& value) {
    static_assert(!ECS::PairTraits<T>::is_pair, "set(entity, value): pass the pair's data, with the pair as explicit template arguments: set<First, Second>(entity, value)");
    static_assert(!std::is_empty_v<T>, "set<T>: T carries no data; use add()");
    this->set(entity, this->id<T>(), &value);
}

template <typename First, typename Second>
void World::set(const EntityId entity, const typename ECS::Pair<First, Second>::type& value) {
    static_assert(!std::is_empty_v<typename ECS::Pair<First, Second>::type>, "set<First, Second>: the pair carries no data; use add()");
    this->set(entity, this->pair<First, Second>(), &value);
}

template <typename First>
void World::set(const EntityId entity, const EntityId second, const std::type_identity_t<First>& value) {
    static_assert(!std::is_empty_v<First>, "set<First>(entity, second, value): First carries no data; use add() or set_second()");
    this->set(entity, this->pair<First>(second), &value);
}

template <typename Second>
void World::set_second(const EntityId entity, const EntityId first, const std::type_identity_t<Second>& value) {
    static_assert(!std::is_empty_v<Second>, "set_second<Second>: Second carries no data; use add_second()");
    if (this->relation_has_data(first)) {
        // The pair stores the relation's type, not Second; refuse the write.
        return;
    }
    this->set(entity, this->pair_with_second<Second>(first), &value);
}

// --- modified ----------------------------------------------------------------

template <typename T>
void World::modified(const EntityId entity) {
    static_assert(!std::is_empty_v<ECS::StorageType<T>>, "modified<T>: T carries no data, so nothing can have changed");
    this->modified(entity, this->id<T>());
}

template <typename First, typename Second>
void World::modified(const EntityId entity) {
    static_assert(!std::is_empty_v<typename ECS::Pair<First, Second>::type>, "modified<First, Second>: the pair carries no data, so nothing can have changed");
    this->modified(entity, this->pair<First, Second>());
}

template <typename First>
void World::modified(const EntityId entity, const EntityId second) {
    static_assert(!std::is_empty_v<First>, "modified<First>(entity, second): First carries no data; use modified_second()");
    this->modified(entity, this->pair<First>(second));
}

template <typename Second>
void World::modified_second(const EntityId entity, const EntityId first) {
    static_assert(!std::is_empty_v<Second>, "modified_second<Second>: Second carries no data, so nothing can have changed");
    this->modified(entity, this->pair_with_second<Second>(first));
}

// --- component / tag ---------------------------------------------------------

template <typename T>
EntityId World::tag() {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "tag<T>: pass the plain type, without const / volatile / reference");

    const TypeId type_id = TYPE_ID::get<T>();
    const EntityId* existing = this->type_index.find(type_id);
    if (existing != nullptr) {
        return *existing;
    }
    return this->claim_component_id(type_id);
}

// --- Hooks -------------------------------------------------------------------

template <typename T>
HookId World::hook_added(const HookCallback callback, void* user_data) {
    return this->hook_added(this->id<T>(), callback, user_data);
}

template <typename T>
HookId World::hook_removed(const HookCallback callback, void* user_data) {
    return this->hook_removed(this->id<T>(), callback, user_data);
}

template <typename T>
HookId World::hook_changed(const HookCallback callback, void* user_data) {
    return this->hook_changed(this->id<T>(), callback, user_data);
}

// The query front ends need the World definition above and define
// World::query / World::query_build; including world.hpp gives the full API.
#include "engine/ecs/query.hpp"         // NOLINT(misc-include-cleaner)
#include "engine/ecs/query_builder.hpp" // NOLINT(misc-include-cleaner)
