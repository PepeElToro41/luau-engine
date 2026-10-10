#include "support/bench.hpp"

#include "engine/ecs/archetype/archetype_matcher.hpp"
#include "engine/ecs/ecs.hpp"
#include "engine/ecs/world.hpp"

#include <bit>
#include <cstdio>

// Baseline for queries: matching a with/without filter against every
// archetype in a world with 255 distinct archetypes (every non-empty subset
// of 8 tags).
BENCH_CASE("ecs/archetype_matcher: matches over 255 archetypes") {
    World world;
    world.init();

    constexpr usz tag_count = 8;
    EntityId tags[tag_count];
    for (usz i = 0; i < tag_count; ++i) {
        tags[i] = world.new_entity();
    }
    for (usz mask = 1; mask < (1u << tag_count); ++mask) {
        const EntityId e = world.new_entity();
        for (usz bit = 0; bit < tag_count; ++bit) {
            if (mask & (1u << bit)) {
                world.add(e, tags[bit]);
            }
        }
    }

    const Id with[] = {tags[0], tags[1]};
    const Id without[] = {tags[7]};
    ArchetypeMatcher matcher = ArchetypeMatcher::create(world.allocator, with, 2, without, 1);

    const usz archetype_count = world.archetypes.alive_count;
    bench.batch(archetype_count).run("with 2, without 1", [&] {
        usz hits = 0;
        for (usz i = 0; i < archetype_count; ++i) {
            const Archetype* archetype = world.archetypes.get_element_alive(world.archetypes.get_alive_id(i));
            hits += matcher.matches(archetype) ? 1 : 0;
        }
        ankerl::nanobench::doNotOptimizeAway(hits);
    });

    const Id wildcard_with[] = {ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)};
    ArchetypeMatcher wildcard = ArchetypeMatcher::create(world.allocator, wildcard_with, 1, nullptr, 0);
    bench.batch(archetype_count).run("with (CHILD_OF, *)", [&] {
        usz hits = 0;
        for (usz i = 0; i < archetype_count; ++i) {
            const Archetype* archetype = world.archetypes.get_element_alive(world.archetypes.get_alive_id(i));
            hits += wildcard.matches(archetype) ? 1 : 0;
        }
        ankerl::nanobench::doNotOptimizeAway(hits);
    });

    wildcard.free();
    matcher.free();
    world.free();
}

// --- Component mask: is the outer word worth it? -------------------------------
//
// Archetypes built from component ids drawn at random from
// [1, ECS::MAX_COMPONENT_ID] so they spread over every mask word (consecutive
// ids would all land in words[0] and the outer word could never reject
// anything). The same signatures are matched through the real
// ArchetypeMatcher and through local copies of its mask checks that drop the
// outer pre-compare, compare every word, or test both sides in one walk.
//
// Findings (2026-10-09, Release, 4 mask words): the outer pre-compare is
// worth keeping, dropping it costs ~0.5 ns per archetype on `with` queries
// since it rejects before the word loop starts; comparing every word ties
// it for `with` but loses on `without` (which only walks the words both
// outers share); the fused walk ties or loses. What did matter was keeping
// the mask compares inline in matches(), which halved component-only
// queries (the out-of-line call cost as much as the compares).

namespace {

struct SplitMix64 {
    u64 state;
    u64 next() {
        this->state += 0x9E3779B97F4A7C15ull;
        u64 z = this->state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // Uniform in [0, bound).
    u64 below(const u64 bound) { return this->next() % bound; }
};

// ComponentMask::contains_all is: outer pre-compare, then only the words the
// query names (countr_zero walk). This drops the pre-compare and goes
// straight to the walk.
bool contains_all_walk(const ComponentMask& a, const ComponentMask& b) {
    u64 remaining = b.outer;
    while (remaining != 0) {
        const usz word = std::countr_zero(remaining);
        remaining &= remaining - 1;
        if ((a.words[word] & b.words[word]) != b.words[word]) {
            return false;
        }
    }
    return true;
}
// intersects already walks only the words both outers share; the walk-only
// variant walks the query's words instead.
bool intersects_walk(const ComponentMask& a, const ComponentMask& b) {
    u64 remaining = b.outer;
    while (remaining != 0) {
        const usz word = std::countr_zero(remaining);
        remaining &= remaining - 1;
        if ((a.words[word] & b.words[word]) != 0) {
            return true;
        }
    }
    return false;
}

// No outer at all: every word, early exit.
bool contains_all_dense(const ComponentMask& a, const ComponentMask& b) {
    for (usz i = 0; i < COMPONENT_MASK_WORDS; i++) {
        if ((a.words[i] & b.words[i]) != b.words[i]) {
            return false;
        }
    }
    return true;
}
bool intersects_dense(const ComponentMask& a, const ComponentMask& b) {
    for (usz i = 0; i < COMPONENT_MASK_WORDS; i++) {
        if ((a.words[i] & b.words[i]) != 0) {
            return true;
        }
    }
    return false;
}

// No outer, no branches: fold every word and test once (vectorizable).
bool contains_all_fold(const ComponentMask& a, const ComponentMask& b) {
    u64 missing = 0;
    for (usz i = 0; i < COMPONENT_MASK_WORDS; i++) {
        missing |= b.words[i] & ~a.words[i];
    }
    return missing == 0;
}
bool intersects_fold(const ComponentMask& a, const ComponentMask& b) {
    u64 shared = 0;
    for (usz i = 0; i < COMPONENT_MASK_WORDS; i++) {
        shared |= a.words[i] & b.words[i];
    }
    return shared != 0;
}

using MaskTest = bool (*)(const ComponentMask&, const ComponentMask&);

// The mask part of ArchetypeMatcher::matches with the checks swapped in.
template <MaskTest contains_all, MaskTest intersects>
usz count_matches(const Archetype* const* archetypes, const usz count, const ArchetypeMatcher& matcher) {
    usz hits = 0;
    for (usz i = 0; i < count; ++i) {
        const ArchetypeSignature& signature = archetypes[i]->signature;
        if (!contains_all(signature.mask, matcher.with_mask)) {
            continue;
        }
        if (intersects(signature.mask, matcher.without_mask)) {
            continue;
        }
        hits++;
    }
    return hits;
}

// Both sides in one walk: the outer pre-compare for `with`, then one pass
// over the words `with` names plus the words `without` shares with the
// archetype, testing both conditions on each word it loads.
usz count_matches_fused(const Archetype* const* archetypes, const usz count, const ArchetypeMatcher& matcher) {
    const ComponentMask& with = matcher.with_mask;
    const ComponentMask& without = matcher.without_mask;
    usz hits = 0;
    for (usz i = 0; i < count; ++i) {
        const ComponentMask& a = archetypes[i]->signature.mask;
        if ((a.outer & with.outer) != with.outer) {
            continue;
        }
        u64 remaining = with.outer | (a.outer & without.outer);
        bool ok = true;
        while (remaining != 0) {
            const usz word = std::countr_zero(remaining);
            remaining &= remaining - 1;
            const u64 w = a.words[word];
            if ((w & with.words[word]) != with.words[word] || (w & without.words[word]) != 0) {
                ok = false;
                break;
            }
        }
        hits += ok ? 1 : 0;
    }
    return hits;
}

struct RandomWorld {
    World world;
    Id* pool = nullptr;
    usz pool_count = 0;
    const Archetype** archetypes = nullptr;
    usz archetype_count = 0;

    // `pool_count` distinct component ids chosen at random, then
    // `entity_count` entities each holding 1..max_ids ids from the pool.
    void init(SplitMix64& rng, const usz pool_count, const usz entity_count, const usz max_ids) {
        this->world.init();
        this->pool_count = pool_count;
        this->pool = this->world.allocator->allocate_array<Id>(pool_count);

        // Partial Fisher-Yates over [1, MAX_COMPONENT_ID].
        Id* all = this->world.allocator->allocate_array<Id>(ECS::MAX_COMPONENT_ID);
        for (usz i = 0; i < ECS::MAX_COMPONENT_ID; i++) {
            all[i] = i + 1;
        }
        for (usz i = 0; i < pool_count; i++) {
            const usz j = i + rng.below(ECS::MAX_COMPONENT_ID - i);
            const Id tmp = all[i];
            all[i] = all[j];
            all[j] = tmp;
            this->pool[i] = all[i];
        }
        this->world.allocator->free(all);

        for (usz e = 0; e < entity_count; e++) {
            const EntityId entity = this->world.new_entity();
            const usz id_count = 1 + rng.below(max_ids);
            for (usz k = 0; k < id_count; k++) {
                this->world.add(entity, this->pool[rng.below(pool_count)]);
            }
        }

        this->archetype_count = this->world.archetypes.alive_count;
        this->archetypes = this->world.allocator->allocate_array<const Archetype*>(this->archetype_count);
        for (usz i = 0; i < this->archetype_count; i++) {
            this->archetypes[i] = this->world.archetypes.get_element_alive(this->world.archetypes.get_alive_id(i));
        }
    }

    void free() {
        this->world.allocator->free(this->archetypes);
        this->world.allocator->free(this->pool);
        this->world.free();
    }
};

void run_mask_variants(ankerl::nanobench::Bench& bench, const RandomWorld& rw, const char* label, const ArchetypeMatcher& matcher) {
    const usz count = rw.archetype_count;
    const Archetype* const* archetypes = rw.archetypes;

    // Every variant must agree with the real matcher.
    usz expected = 0;
    for (usz i = 0; i < count; ++i) {
        expected += matcher.matches(archetypes[i]) ? 1 : 0;
    }
    const usz got_walk = count_matches<contains_all_walk, intersects_walk>(archetypes, count, matcher);
    const usz got_dense = count_matches<contains_all_dense, intersects_dense>(archetypes, count, matcher);
    const usz got_fold = count_matches<contains_all_fold, intersects_fold>(archetypes, count, matcher);
    const usz got_fused = count_matches_fused(archetypes, count, matcher);
    if (got_walk != expected || got_dense != expected || got_fold != expected || got_fused != expected) {
        printf("MISMATCH %s: matcher %zu walk %zu dense %zu fold %zu fused %zu\n", label, expected, got_walk, got_dense, got_fold, got_fused);
    }
    printf("  %s: %zu archetypes, %zu match (%.1f%%)\n", label, count, expected, 100.0 * (f64)expected / (f64)count);

    char name[128];
    snprintf(name, sizeof(name), "%s | matcher (outer + walk)", label);
    bench.batch(count).run(name, [&] {
        usz hits = 0;
        for (usz i = 0; i < count; ++i) {
            hits += matcher.matches(archetypes[i]) ? 1 : 0;
        }
        ankerl::nanobench::doNotOptimizeAway(hits);
    });
    snprintf(name, sizeof(name), "%s | walk only", label);
    bench.batch(count).run(name, [&] {
        ankerl::nanobench::doNotOptimizeAway(count_matches<contains_all_walk, intersects_walk>(archetypes, count, matcher));
    });
    snprintf(name, sizeof(name), "%s | dense early-exit", label);
    bench.batch(count).run(name, [&] {
        ankerl::nanobench::doNotOptimizeAway(count_matches<contains_all_dense, intersects_dense>(archetypes, count, matcher));
    });
    snprintf(name, sizeof(name), "%s | dense fold", label);
    bench.batch(count).run(name, [&] {
        ankerl::nanobench::doNotOptimizeAway(count_matches<contains_all_fold, intersects_fold>(archetypes, count, matcher));
    });
    snprintf(name, sizeof(name), "%s | fused walk", label);
    bench.batch(count).run(name, [&] {
        ankerl::nanobench::doNotOptimizeAway(count_matches_fused(archetypes, count, matcher));
    });
}

void bench_random_world(ankerl::nanobench::Bench& bench, const usz pool_count, const usz entity_count, const usz max_ids) {
    SplitMix64 rng { 0x5EED1234ull };
    RandomWorld rw;
    rw.init(rng, pool_count, entity_count, max_ids);
    printf("pool %zu ids, %zu entities, 1..%zu ids each -> %zu archetypes\n", pool_count, entity_count, max_ids, rw.archetype_count);

    const Id with2[] = {rw.pool[0], rw.pool[1]};
    const Id with1[] = {rw.pool[2]};
    const Id without1[] = {rw.pool[3]};
    const Id with3[] = {rw.pool[4], rw.pool[5], rw.pool[6]};

    ArchetypeMatcher m_with2 = ArchetypeMatcher::create(rw.world.allocator, with2, 2, nullptr, 0);
    ArchetypeMatcher m_with1_without1 = ArchetypeMatcher::create(rw.world.allocator, with1, 1, without1, 1);
    ArchetypeMatcher m_with3 = ArchetypeMatcher::create(rw.world.allocator, with3, 3, nullptr, 0);
    ArchetypeMatcher m_without1 = ArchetypeMatcher::create(rw.world.allocator, nullptr, 0, without1, 1);

    run_mask_variants(bench, rw, "with 2", m_with2);
    run_mask_variants(bench, rw, "with 1 without 1", m_with1_without1);
    run_mask_variants(bench, rw, "with 3", m_with3);
    run_mask_variants(bench, rw, "without 1", m_without1);

    m_without1.free();
    m_with3.free();
    m_with1_without1.free();
    m_with2.free();
    rw.free();
}

} // namespace

BENCH_CASE("ecs/archetype_matcher: component mask, random ids, pool 16") {
    bench_random_world(bench, 16, 2048, 6);
}

BENCH_CASE("ecs/archetype_matcher: component mask, random ids, pool 64") {
    bench_random_world(bench, 64, 4096, 8);
}
