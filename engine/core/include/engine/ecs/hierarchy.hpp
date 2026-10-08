#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/ecs_types.hpp"
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

// Cache on the component record of a traversable concrete pair (R, T),
// allocated by HIERARCHY:: when the record is created and freed with it.
struct HierarchyNode {
    // Depth of every entity holding the pair: depth of T along R plus one.
    // Valid when `dirty` is clear.
    u32 depth = 0;
    bool dirty = true;
    // HIERARCHY::depth is computing this node further up the stack; meeting
    // it again means the relation has a cycle.
    bool computing = false;
    // The (R, c) records of the entities c that hold (R, T) and are targets
    // themselves: the nodes whose depth depends on this one. Keyed by pair id.
    HashMap<Id, ComponentRecord*> children;

    explicit HierarchyNode(BaseAllocator* allocator) : children(allocator) {}
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

// Marks the node of `record` dirty, and every node below it. No-op without a
// node or when already dirty.
void invalidate(World* world, ComponentRecord* record);

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
