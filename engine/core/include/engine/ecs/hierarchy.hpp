#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/templates/dynamic_array.hpp"
#include "engine/templates/hash_map.hpp"

struct World;
struct Archetype;
struct EntityRecord;
struct ComponentRecord;

// Cached hierarchy depth along traversable relations.
//
// An entity's depth along a traversable relation R (CHILD_OF, IS_A, ...) is
// the length of the longest chain of (R, target) pairs leading up from it: 0
// without the pair, 1 under a root, 2 under that, and so on. The depth is a
// property of the pair: every holder of (R, P) sits at depth(P) + 1, whatever
// else it holds. So the cache lives on the component record of each
// traversable concrete pair (R, T), as a HierarchyNode: the depth of every
// entity holding (R, T), and the records whose depth depends on it. An
// archetype's depth along R is the deepest of its (R, *) pairs' nodes (an
// IS_A entity with two bases sorts after both), 0 if it holds none; the
// node of each (R, *) column is one record lookup away.
//
// The nodes form the hierarchy at the record level: node (R, T) is a child of
// node (R, P) when T holds (R, P), and `children` on (R, P) lists exactly the
// nodes whose depth it feeds. ENTITY::move calls on_move() for every move,
// which does nothing for an entity nothing points at
// (ENTITY_RECORD_TRAVERSABLE_TARGET is clear; it is set while some
// traversable (R, T) record exists for T). For a target T it visits the
// relations T is a parent through (trav_records on the (*, T) record), and
// for each one whose (R, *) pairs differ between the two archetypes it
// re-links node (R, T) under its new parent nodes and then, only if T's
// depth actually changed, marks the node dirty. Dirtying walks the children
// recursively and stops at a node that is already dirty (its subtree is too),
// which also makes cycles terminate; archetypes are never touched, since
// they read their depth off the nodes. Reparenting under a parent at the
// same depth, or a parent gaining an unrelated component, invalidates
// nothing.
//
// Nodes are computed lazily by depth() and every invalidation bumps
// World::hierarchy_generation, so whoever keeps archetypes sorted by depth (a
// cascade query) can re-sort only when the counter moved.
//
// A cycle (an entity that is its own ancestor) has no depth; depth() reports
// it once per recompute and treats the link that closes the cycle as depth 0.
//
// Each node also caches what the holders of its pair can reach by walking
// up: `reachable` lists the ids of T itself and of every ancestor of T along
// R, sorted by id, each with the entity that holds it (`source`), its
// column there, and its `rank` in the depth-first walk over T's parents (0
// for T, 1 for the first parent, then that parent's ancestors, then the
// next parent, ...). An id held at several levels keeps the nearest, so an
// up() term reads "the first ancestor holding X" with one search instead of
// a walk (see query_vm.hpp). The ids are kept in their own array, apart
// from the entries, so that the search touches a few contiguous cache lines
// of 8-byte keys and reads one entry at the end: the sets are cold by the
// time a query comes back to them. The set is built from nodes alone: the node
// keeps `target_archetype` (T's archetype, refreshed by on_move) for T's
// own ids and `parents` (the records whose nodes it is linked under, the
// mirror of `children`) whose sets are merged in order. Any move of a
// target changes what its holders reach, so on_move marks the node and
// everything below `reachable_dirty` (stopping at dirty nodes, like depth)
// and reachable() recomputes on the next request. A cycle is reported once
// per recompute and the link that closes it contributes nothing.

// Where one reachable id is found: see ReachableSet.
struct ReachableEntry {
    // The nearest entity, from the pair's target upwards, holding the id.
    EntityId source = 0;
    // Column of the id in the source's archetype.
    u32 column = 0;
    // Position of `source` in the depth-first walk from the target: 0 is
    // the target, 1 its first parent, and so on. Every id of one source
    // shares the rank.
    u32 rank = 0;
};

// The ids reachable up from the holders of a pair, see HierarchyNode:
// `ids` sorted ascending, `entries[i]` saying where ids[i] is found.
struct ReachableSet {
    DynamicArray<Id> ids;
    DynamicArray<ReachableEntry> entries;

    explicit ReachableSet(BaseAllocator* allocator) : ids(allocator), entries(allocator) {}

    usz count() const { return this->ids.count; }
    void clear() {
        this->ids.clear();
        this->entries.clear();
    }
    void push(const Id id, const ReachableEntry& entry) {
        this->ids.push(id);
        this->entries.push(entry);
    }
    void free() {
        this->ids.free();
        this->entries.free();
    }
};

// Cache on the component record of a traversable concrete pair (R, T),
// allocated by HIERARCHY:: when the record is created and freed with it.
struct HierarchyNode {
    // The fields a query reads come first, so they share the node's first
    // cache line: the reachable flags, then the set's arrays.
    //
    // Whether `reachable` is stale, and whether it is being computed further
    // up the stack (meeting it again means the relation has a cycle).
    bool reachable_dirty = true;
    bool reachable_computing = false;
    // Depth of every entity holding the pair: depth of T along R plus one.
    // Valid when `dirty` is clear; `computing` as for the set.
    bool dirty = true;
    bool computing = false;
    u32 depth = 0;
    // What the holders of (R, T) reach walking up. Valid when
    // `reachable_dirty` is clear; `reachable_ranks` is the number of ranks
    // used (the length of the depth-first walk), which a child offsets its
    // parents' ranks by.
    ReachableSet reachable;
    u32 reachable_ranks = 0;

    // T's archetype, kept current by on_move; nullptr while T has none.
    Archetype* target_archetype = nullptr;
    // The (R, P) records of T's parents P, in T's column order: the nodes
    // this one is linked under (the mirror of their `children`).
    DynamicArray<ComponentRecord*> parents;
    // The (R, c) records of the entities c that hold (R, T) and are targets
    // themselves: the nodes whose depth depends on this one. Keyed by pair id.
    HashMap<Id, ComponentRecord*> children;

    explicit HierarchyNode(BaseAllocator* allocator) : reachable(allocator), parents(allocator), children(allocator) {}
};

namespace HIERARCHY {

// Depth of the entities holding the pair of `record`, computing and caching
// it if dirty. 0 if the record carries no node (not a traversable pair).
u32 depth(World* world, ComponentRecord* record);

// Depth of `archetype` along `relation` (low id; the generation is ignored):
// the deepest (relation, *) pair it holds, 0 without one (the root included).
u32 depth(World* world, Archetype* archetype, EntityIdLow relation);

// Depth of the entity's archetype, or 0 if the entity is not alive.
u32 depth(World* world, EntityId entity, EntityIdLow relation);

// Fills `order` (count entries) with the indices of `archetypes` sorted by
// their depth along `relation`, shallowest first, or deepest first when
// `descending`; archetypes of one depth keep their relative order. Since a
// child is always one deeper than its deepest parent, the ascending order
// visits every archetype before those of its descendants. One counting pass
// over the depths, which are small integers; its scratch lives on a
// TemporalAllocator of its own, so the caller must not hold an inner one
// across the call. The order of a cascade() query.
void order_by_depth(World* world, Archetype* const* archetypes, usz count, EntityIdLow relation, bool descending, u32* order);

// Marks the node of `record` dirty, and every node below it. No-op without a
// node or when already dirty.
void invalidate(World* world, ComponentRecord* record);

// The reachable set of `record`'s pair (see above), recomputed if dirty.
// nullptr when the record has no node, or when the set is being computed
// further up the stack: the relation has a cycle, reported once.
const ReachableSet* reachable(World* world, ComponentRecord* record);

// Index of the first id of `set` at or after `start` matching `pattern` (a
// concrete id, or a wildcard pattern as ECS::ID_MATCHES reads it), or
// set.count(). A concrete id is found by binary search, an (R, *) pattern
// starts at its range; the rest is a scan.
usz find_reachable(const ReachableSet& set, Id pattern, usz start);

// Marks the reachable set of `record` dirty, and every node's below it.
// No-op without a node or when already dirty.
void invalidate_reachable(ComponentRecord* record);

// Called by ENTITY::move after `entity` left `source` for `destination`
// (either may be the root): re-links and invalidates the nodes that depend
// on the entity's position, as described above.
void on_move(World* world, EntityId entity, const EntityRecord* record, Archetype* source, Archetype* destination);

// Called by ComponentRecord when a traversable concrete pair record (R, T) is
// created or is about to be deleted: allocates / frees its node, links it
// under T's current parent nodes / unlinks it, and maintains
// ENTITY_RECORD_TRAVERSABLE_TARGET on T.
void on_target_record_created(World* world, ComponentRecord* record);
void on_target_record_deleted(World* world, ComponentRecord* record);

// Releases the node without touching anything else, for ComponentRecord::destroy
// when the whole world goes away.
void free_node(ComponentRecord* record);

} // namespace HIERARCHY
