#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs_types.hpp"

#include <type_traits>

// Id layout and the hardcoded ids the ECS relies on.
//
// An EntityId packs a 32-bit entity id (low) and a 32-bit generation (high).
// The generation is bumped whenever an entity is deleted, so a stale id no
// longer matches once its slot has been recycled. Id 0 is never valid.
//
// An id that appears in an archetype type is either a plain entity id or a
// pair. A plain id (component or tag) is stored as the full EntityId,
// generation included, so a stale id never aliases the entity that recycles
// its slot and only an alive entity can be added. A pair has no room for
// generations: it packs two low ids with a flag on top so it can be told apart
// from a plain id:
//
//     bit 63        ID_FLAG_PAIR
//     bits 32..59   first  (relation), low id only
//     bits  0..31   second (target),   low id only
//
// The top four bits are reserved for id flags, so a low id used as a pair's
// first must fit in 28 bits. PAIR() strips the generation off both sides; use
// World::pair_first() / pair_second() to get the alive entities back.
namespace ECS {

// --- Id layout ---------------------------------------------------------------

constexpr u64 ENTITY_BITS = 32;
constexpr u64 ENTITY_SIZE = 1ull << ENTITY_BITS;
constexpr u64 ENTITY_MASK = ENTITY_SIZE - 1;

constexpr u64 GENERATION_BITS = 32;
constexpr u64 GENERATION_SIZE = 1ull << GENERATION_BITS;

constexpr u64 ID_FLAGS_MASK = 0xFull << 60;
constexpr u64 ID_COMPONENT_MASK = ~ID_FLAGS_MASK;
constexpr u64 ID_FLAG_PAIR = 1ull << 63;

// Component ids are handed out below this; everything above is a regular
// entity id. Keeping components low keeps per-component lookups dense.
constexpr Id MAX_COMPONENT_ID = 256;

// --- Built-in ids ------------------------------------------------------------
// Hardcoded so they are stable across worlds. They start right after the
// component range. World::create() registers every id up to and including
// REST and starts handing out fresh entities after it, so no created entity
// collides with them.

constexpr Id WILDCARD         = MAX_COMPONENT_ID + 1;  // matches any id: (R, *), (*, T)
constexpr Id ANY              = MAX_COMPONENT_ID + 2;  // like WILDCARD but matches at most once per entity
constexpr Id THIS             = MAX_COMPONENT_ID + 3;  // query variable for the matched entity
constexpr Id COMPONENT        = MAX_COMPONENT_ID + 4;  // component holding a TypeInfo
constexpr Id EXCLUSIVE        = MAX_COMPONENT_ID + 5;  // trait: an entity holds at most one (R, *)
constexpr Id TRAVERSABLE      = MAX_COMPONENT_ID + 6;  // trait: queries may walk up through R
// Deletion policies (see entity_cleanup.hpp). Both trait relations are
// exclusive; without one the policy is REMOVE. Every built-in id, component
// ids included, carries (ON_DELETE, PANIC) so it cannot be deleted.
constexpr Id ON_DELETE        = MAX_COMPONENT_ID + 7;  // (ON_DELETE, policy) on id E: what happens to holders of E / (E, *) when E is deleted
constexpr Id ON_DELETE_TARGET = MAX_COMPONENT_ID + 8;  // (ON_DELETE_TARGET, policy) on relation R: what happens to holders of (R, T) when T is deleted
constexpr Id REMOVE           = MAX_COMPONENT_ID + 9;  // policy: remove the id from its holders (the default)
constexpr Id DELETE           = MAX_COMPONENT_ID + 10; // policy: delete the holders too
constexpr Id PANIC            = MAX_COMPONENT_ID + 11; // policy: the deletion is an error and does not happen
constexpr Id CHILD_OF         = MAX_COMPONENT_ID + 12; // (CHILD_OF, parent); exclusive, traversable, (ON_DELETE_TARGET, DELETE)
constexpr Id IS_A             = MAX_COMPONENT_ID + 13; // (IS_A, base); traversable
constexpr Id REST             = IS_A + 1;              // last built-in id; user entities start after it

// --- Utilities ---------------------------------------------------------------

// Low 32 bits of an entity id: the id without its generation.
constexpr EntityIdLow ENTITY_LOW(const EntityId id) {
    return id & ENTITY_MASK;
}

constexpr bool IS_PAIR(const Id id) {
    return (id & ID_FLAG_PAIR) != 0;
}

// Pair (first, second). Both ids are stripped to their low id first.
constexpr Id PAIR(const EntityId first, const EntityId second) {
    return ID_FLAG_PAIR | (ENTITY_LOW(first) << ENTITY_BITS) | ENTITY_LOW(second);
}

// Low id of the pair's relation. Use World::pair_first() to get the alive
// entity with its generation.
constexpr EntityIdLow PAIR_FIRST(const Id pair) {
    return (pair & ID_COMPONENT_MASK) >> ENTITY_BITS;
}

// Low id of the pair's target. Use World::pair_second() to get the alive
// entity with its generation.
constexpr EntityIdLow PAIR_SECOND(const Id pair) {
    return pair & ENTITY_MASK;
}

// Whether a low id is one of the wildcard ids (WILDCARD or ANY).
constexpr bool IS_WILDCARD(const EntityIdLow id) {
    return id == WILDCARD || id == ANY;
}

// Whether either side of `pair` is a wildcard: (R, *), (*, T), (R, _), ...
// Such a pair only matches concrete pairs and can never be held by an entity.
constexpr bool PAIR_HAS_WILDCARD(const Id pair) {
    return IS_WILDCARD(PAIR_FIRST(pair)) || IS_WILDCARD(PAIR_SECOND(pair));
}

// `pattern` with every ANY side rewritten as WILDCARD. The two match the same
// ids, so hooks and matchers index by the WILDCARD spelling only.
constexpr Id FOLD_ANY(const Id pattern) {
    if (!IS_PAIR(pattern)) {
        return pattern == ANY ? WILDCARD : pattern;
    }
    EntityIdLow first = PAIR_FIRST(pattern);
    EntityIdLow second = PAIR_SECOND(pattern);
    if (first == ANY) {
        first = WILDCARD;
    }
    if (second == ANY) {
        second = WILDCARD;
    }
    return PAIR(first, second);
}

// Whether `id` (an id held by an entity, so never a wildcard) matches
// `pattern`. A plain pattern matches on equality, with WILDCARD / ANY
// matching any id. A pair pattern matches a pair id side by side, with
// WILDCARD / ANY on either side matching any low id: (R, *) matches every
// pair with relation R, (*, T) every pair with target T.
constexpr bool ID_MATCHES(const Id pattern, const Id id) {
    if (pattern == id) {
        return true;
    }
    if (!IS_PAIR(pattern)) {
        return IS_WILDCARD(pattern);
    }
    if (!IS_PAIR(id)) {
        return false;
    }
    const EntityIdLow first = PAIR_FIRST(pattern);
    const EntityIdLow second = PAIR_SECOND(pattern);
    return (IS_WILDCARD(first) || first == PAIR_FIRST(id))
        && (IS_WILDCARD(second) || second == PAIR_SECOND(id));
}

// --- Compile-time pairs ------------------------------------------------------
// A pair (First, Second) spelled as types, for the templated World API
// (World::has<First, Second>(), World::get<Pair<First, Second>>(), ...).
//
// A pair stores data from one side only, following the rule ComponentRecord
// applies at runtime: the relation's data wins, and a tag relation takes the
// target's data. Pair<First, Second>::type is that side:
//   - First is non-empty              -> First
//   - First is empty, Second is not   -> Second
//   - both empty                      -> First (the pair is a tag)

template <typename First, typename Second>
struct Pair {
    using first = First;
    using second = Second;
    using type = std::conditional_t<!std::is_empty_v<First> || std::is_empty_v<Second>, First, Second>;
};

// PairTraits<T>::is_pair tells a Pair<> apart from a plain component type;
// PairTraits<T>::type is the type whose data is stored for T: T itself, or
// the pair's data side.
template <typename T>
struct PairTraits {
    static constexpr bool is_pair = false;
    using type = T;
};

template <typename First, typename Second>
struct PairTraits<Pair<First, Second>> {
    static constexpr bool is_pair = true;
    using type = typename Pair<First, Second>::type;
};

template <typename T>
using StorageType = typename PairTraits<T>::type;

} // namespace ECS
