#include "engine/ecs/archetype/archetype_matcher.hpp"

#include "engine/ecs/ecs.hpp"

// Index of the first id in `type` that is >= `value`, or id_count.
static usz lower_bound(const ArchetypeType& type, const Id value) {
    usz low = 0;
    usz high = type.id_count;
    while (low < high) {
        const usz middle = low + (high - low) / 2;
        if (type.ids[middle] < value) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

// Patterns that match whole classes of ids ((*, *), plain WILDCARD) are not
// in any archetype's bloom, so testing them there would reject everything.
static bool pattern_in_bloom(const Id pattern) {
    if (!ECS::IS_PAIR(pattern)) {
        return !ECS::IS_WILDCARD(pattern);
    }
    return !(ECS::IS_WILDCARD(ECS::PAIR_FIRST(pattern)) && ECS::IS_WILDCARD(ECS::PAIR_SECOND(pattern)));
}

void ArchetypeMatcher::build_side(BaseAllocator* allocator, const Id* ids, const usz count, ComponentMask& mask, BloomFilter& bloom, Id*& out_ids, usz& out_count) {
    out_ids = nullptr;
    out_count = 0;

    usz slow_count = 0;
    for (usz i = 0; i < count; i++) {
        const Id id = ids[i];
        if (id == 0) {
            continue;
        }
        if (ComponentMask::fits(id)) {
            mask.set(id);
        } else {
            slow_count++;
        }
    }
    if (slow_count == 0) {
        return;
    }

    out_ids = allocator->allocate_array<Id>(slow_count);
    for (usz i = 0; i < count; i++) {
        const Id id = ids[i];
        if (id == 0 || ComponentMask::fits(id)) {
            continue;
        }
        const Id pattern = ECS::FOLD_ANY(id);
        if (pattern_in_bloom(pattern)) {
            bloom.add(pattern);
        }

        // Insertion sort: the lists are a few entries long.
        usz position = out_count;
        while (position > 0 && out_ids[position - 1] > pattern) {
            out_ids[position] = out_ids[position - 1];
            position--;
        }
        out_ids[position] = pattern;
        out_count++;
    }
}

ArchetypeMatcher ArchetypeMatcher::create(BaseAllocator* allocator, const Id* with, const usz with_count, const Id* without, const usz without_count) {
    ArchetypeMatcher matcher;
    matcher.allocator = allocator;
    ArchetypeMatcher::build_side(allocator, with, with_count, matcher.with_mask, matcher.with_bloom, matcher.with_ids, matcher.with_count);
    ArchetypeMatcher::build_side(allocator, without, without_count, matcher.without_mask, matcher.without_bloom, matcher.without_ids, matcher.without_count);
    return matcher;
}

void ArchetypeMatcher::free() {
    if (this->allocator != nullptr) {
        this->allocator->free(this->with_ids);
        this->allocator->free(this->without_ids);
    }
    this->with_ids = nullptr;
    this->with_count = 0;
    this->without_ids = nullptr;
    this->without_count = 0;
}

bool ArchetypeMatcher::type_matches(const ArchetypeType& type, const Id pattern) {
    if (!ECS::IS_PAIR(pattern)) {
        if (ECS::IS_WILDCARD(pattern)) {
            return type.id_count > 0;
        }
        const usz index = lower_bound(type, pattern);
        return index < type.id_count && type.ids[index] == pattern;
    }

    const EntityIdLow first = ECS::PAIR_FIRST(pattern);
    const EntityIdLow second = ECS::PAIR_SECOND(pattern);
    const bool first_any = ECS::IS_WILDCARD(first);
    const bool second_any = ECS::IS_WILDCARD(second);

    // Pairs carry bit 63, so they sort after every plain id and group by
    // relation: all (R, t) are contiguous, ordered by t.
    if (!first_any) {
        const Id start = second_any ? ECS::PAIR(first, 0) : pattern;
        const usz index = lower_bound(type, start);
        return index < type.id_count && ECS::ID_MATCHES(pattern, type.ids[index]);
    }

    // (*, T) or (*, *): walk the pair block at the end of the type.
    for (usz i = lower_bound(type, ECS::ID_FLAG_PAIR); i < type.id_count; i++) {
        if (second_any || ECS::PAIR_SECOND(type.ids[i]) == second) {
            return true;
        }
    }
    return false;
}

bool ArchetypeMatcher::matches(const ArchetypeSignature& signature, const ArchetypeType& type) const {
    if (!signature.mask.contains_all(this->with_mask)) {
        return false;
    }
    if (signature.mask.intersects(this->without_mask)) {
        return false;
    }

    if (this->with_count > 0) {
        if (!signature.bloom.test(this->with_bloom)) {
            return false;
        }
        for (usz i = 0; i < this->with_count; i++) {
            if (!ArchetypeMatcher::type_matches(type, this->with_ids[i])) {
                return false;
            }
        }
    }

    if (this->without_count > 0) {
        // The bloom can only prove the sets are disjoint when every pattern
        // is in it; (*, *) and plain wildcards are not, so those are always
        // checked exactly.
        bool need_exact = signature.bloom.intersects(this->without_bloom);
        if (!need_exact) {
            for (usz i = 0; i < this->without_count; i++) {
                if (!pattern_in_bloom(this->without_ids[i])) {
                    need_exact = true;
                    break;
                }
            }
        }
        if (need_exact) {
            for (usz i = 0; i < this->without_count; i++) {
                if (ArchetypeMatcher::type_matches(type, this->without_ids[i])) {
                    return false;
                }
            }
        }
    }

    return true;
}
