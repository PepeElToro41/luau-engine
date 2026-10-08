#pragma once

#include "engine/defines.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/ecs_types.hpp"
#include "engine/ecs/utils/bloom.hpp"
#include "engine/ecs/utils/component_mask.hpp"

// Fixed-size summary of an archetype's ids, built once per archetype so
// ArchetypeMatcher can accept or reject it without touching the id array in
// the common case:
//
//   - `mask` holds every component id (see ComponentMask::fits) exactly.
//   - `bloom` holds every other id: pairs, built-in ids, tag entities. For a
//     pair (R, T) it also holds (R, *) and (*, T) so wildcard patterns can be
//     filtered through it. Pairs with a wildcard side are never held by an
//     entity, so an id in the type is always added as is.
struct ArchetypeSignature {
    ComponentMask mask;
    BloomFilter bloom;

    static ArchetypeSignature build(const Id* ids, const usz id_count) {
        ArchetypeSignature signature;
        for (usz i = 0; i < id_count; i++) {
            signature.add(ids[i]);
        }
        return signature;
    }

    void add(const Id id) {
        if (ComponentMask::fits(id)) {
            this->mask.set(id);
            return;
        }
        this->bloom.add(id);
        if (ECS::IS_PAIR(id)) {
            this->bloom.add(ECS::PAIR(ECS::PAIR_FIRST(id), ECS::WILDCARD));
            this->bloom.add(ECS::PAIR(ECS::WILDCARD, ECS::PAIR_SECOND(id)));
        }
    }
};
