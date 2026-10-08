#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/archetype/archetype.hpp"
#include "engine/ecs/archetype/archetype_signature.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/utils/bloom.hpp"
#include "engine/ecs/utils/component_mask.hpp"
#include "engine/memory/base_allocator.hpp"

// Decides whether an archetype holds every id in `with` and none in `without`.
//
// Each side is split by how it can be tested:
//   - Component ids go into a ComponentMask and are matched exactly against
//     the archetype's mask with a handful of word compares.
//   - Everything else (pairs, built-in ids, tag entities, wildcard patterns)
//     goes into a BloomFilter plus a sorted id list. The bloom rejects (for
//     `with`) or accepts (for `without`) most archetypes outright; only the
//     ones it cannot decide are checked against the archetype's ids.
//
// Patterns follow ECS::ID_MATCHES: (R, *) matches any pair with relation R,
// (*, T) any pair with target T, and (*, *) any pair at all. ANY is treated
// like WILDCARD. Ids of 0 are ignored. A matcher with no ids matches every
// archetype, including the root.
//
// The id lists live on `allocator`; call free() when the matcher is dropped.
struct ArchetypeMatcher {
    BaseAllocator* allocator = nullptr;

    ComponentMask with_mask;
    ComponentMask without_mask;

    BloomFilter with_bloom;
    BloomFilter without_bloom;

    // Non-component ids, sorted ascending. Patterns whose bloom test would be
    // meaningless ((*, *), plain WILDCARD / ANY) are in the list but not in
    // the bloom.
    Id* with_ids = nullptr;
    usz with_count = 0;
    Id* without_ids = nullptr;
    usz without_count = 0;

    static ArchetypeMatcher create(BaseAllocator* allocator, const Id* with, usz with_count, const Id* without, usz without_count);
    void free();

    bool matches(const Archetype* archetype) const {
        return this->matches(archetype->signature, archetype->type);
    }
    // `type` is the archetype's ids (sorted ascending), `signature` their summary.
    bool matches(const ArchetypeSignature& signature, const ArchetypeType& type) const;

    // Whether `type` (sorted ascending) holds an id matching `pattern`.
    static bool type_matches(const ArchetypeType& type, Id pattern);

private:
    // Splits `ids` between `mask` / `bloom` / a sorted list on `allocator`.
    static void build_side(BaseAllocator* allocator, const Id* ids, usz count, ComponentMask& mask, BloomFilter& bloom, Id*& out_ids, usz& out_count);
};
