#pragma once

#include "engine/ecs/ecs_types.hpp"

struct World;

// What happens to the rest of the world when an entity is deleted.
//
// An entity E can appear in other entities' types in three ways: as a plain
// id (component or tag), as the relation of a pair (E, T), or as the target
// of a pair (R, E). ENTITY::delete_entity calls on_delete() before touching E's own
// row, and on_delete() visits every archetype holding such an id and applies
// a policy to the entities in it:
//
//   - ECS::REMOVE (the default): the id is removed from each holder, firing
//     its removed hooks as ENTITY::remove would.
//   - ECS::DELETE: each holder is deleted too, which cascades through this
//     same path.
//   - ECS::PANIC: the deletion is refused. can_delete() checks this before
//     anything is touched: (ON_DELETE, PANIC) on E refuses deleting E at all
//     (every built-in id carries it), (ON_DELETE_TARGET, PANIC) on a relation
//     R refuses deleting a T that is still the target of some (R, T). A
//     refusal prints an error and leaves the world as it was; remove the
//     trait (or the holders) first to go ahead. Inside a cascade a refused
//     entity is kept and only loses the ids that pointed at the deleted one.
//
// For a plain id and for (E, *) the policy is the (ON_DELETE, policy) pair on
// E itself. For (*, E) it is (ON_DELETE_TARGET, policy) on each relation R;
// when an archetype holds several (R, E) with different policies, DELETE
// wins. ECS::CHILD_OF carries (ON_DELETE_TARGET, DELETE), so deleting a
// parent deletes its children. Policies are read off the trait entity when
// the deletion happens, not from the cached ComponentRecord flags, so a trait
// set after the relation was first used is still honored.
//
// Cycles are safe: an entity whose destroy is already running higher up the
// stack is skipped (see ENTITY_RECORD_DELETING), and if it holds an id that
// refers to E that id is removed from it instead.
//
// Emptied archetypes are destroyed, and so are the component records for E
// (keyed by its full id), (E, *), (*, E) and every concrete pair with E on
// either side (keyed by its low id), together with the hooks registered on
// them. Afterwards no live archetype type mentions E, so an entity that later
// recycles E's low id starts clean.
namespace ENTITY_CLEANUP {

// Whether `entity` (alive) may be deleted under the PANIC policies above.
// Prints the reason and returns false if not.
bool can_delete(World* world, EntityId entity);

void on_delete(World* world, EntityId entity);

} // namespace ENTITY_CLEANUP
