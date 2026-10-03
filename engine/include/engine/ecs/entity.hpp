#pragma once

#include "engine/ecs/ecs_types.hpp"

struct World;

// Entity-level operations on a World: lifecycle plus the get/has/add/remove/set
// family that moves an entity between archetypes and reads or writes its
// component data. Every function takes the world first. An entity that is not
// alive is a no-op for mutations and reports "missing" for queries.
namespace ENTITY {

// New entity parked in the root archetype (no row is stored there).
// Returns 0 if the entity index is exhausted.
EntityId create(World* world);

// Registers `entity` under the exact id (and generation) given, reviving it if
// dead. A new or revived entity is parked in the root archetype; one that is
// already alive keeps its archetype. Use this instead of
// EntityIndex::make_alive so the record always has an archetype.
void make_alive(World* world, EntityId entity);

// Deletes the entity. First every other entity that holds an id referring to
// it (the entity as component or tag, as a pair's relation, or as a pair's
// target) is cleaned up following the ON_DELETE / ON_DELETE_TARGET policies
// (see entity_cleanup.hpp), which may delete those entities in turn. Then
// removed hooks fire for the entity's own ids, its row is dropped from its
// archetype and its id is killed. Returns false and does nothing if the
// entity is not alive, is already being deleted further up the stack, or is
// protected by a PANIC policy (an error is printed for that last case).
bool delete_entity(World* world, EntityId entity);

// Removes every id from the entity and parks it in the root archetype, alive
// and reusable. Removed hooks fire for each id first, with the data still
// readable. Nothing cascades and no deletion policy applies: only this
// entity's own ids go, pairs held by other entities that point at it stay.
// No-op if the entity is not alive or already holds nothing.
void clear(World* world, EntityId entity);

bool is_alive(const World* world, EntityId entity);

// Whether the entity's archetype contains `id` (component, tag or pair).
bool has(const World* world, EntityId entity, Id id);

// Pointer to the entity's data for `id`, or nullptr if the entity does not
// have it or `id` is a tag. The pointer is invalidated by any structural change
// (add / remove / destroy) on any entity of the same archetype.
void* get(const World* world, EntityId entity, Id id);

// Adds `id` to the entity, moving it to the archetype with `id`. Meant for
// tags and pairs without data: if `id` carries data a warning is printed and
// the new value is zeroed, so nothing reads the bytes the row held before.
// Use set() to add a component with a value. No-op if the entity already has
// `id`.
//
// A plain id is an entity id, generation included, and must be alive or
// nothing happens: types and component records key on the full id, so a
// stale id is never silently bound to whatever recycled its slot. WILDCARD,
// ANY and THIS are patterns and may not be added; debug builds assert on it,
// release builds do not check.
//
// Pairs (R, T): both R and T must be alive and neither may be a wildcard, or
// nothing happens. If R is exclusive and the entity already holds (R, T'),
// that pair is replaced by (R, T) instead of a second one being added; the
// data for the old pair is dropped.
//
// Adding EXCLUSIVE or TRAVERSABLE to an entity that is already used as an id
// prints a warning (the trait still gets added): the records and archetypes
// that exist for it keep behaving as before. Set traits before first use.
void add(World* world, EntityId entity, Id id);

// Removes `id` from the entity, moving it to the archetype without `id`.
// No-op if the entity does not have `id`.
void remove(World* world, EntityId entity, Id id);

// Adds `id` if missing and copies `data` into the entity's column for it.
// `data` must point at type_info.length bytes. Tags get added but store nothing.
// Pairs follow add(): a wildcard or dead-ended pair is a no-op, and an
// exclusive relation swaps the target before writing. Writing through a
// wildcard like (R, *) is refused even if the entity holds some (R, T).
void set(World* world, EntityId entity, Id id, const void* data);

// Fires the changed hooks for `id` on the entity, for data written in place
// through the pointer from get(). No-op if the entity does not have `id`, if
// `id` is a tag (nothing to change) or if it is a wildcard pair, since that
// would be ambiguous about which pair changed.
void modified(World* world, EntityId entity, Id id);

} // namespace ENTITY
