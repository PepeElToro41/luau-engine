#include "support/test_support.hpp"

#include "engine/ecs/ecs.hpp"
#include "engine/ecs/utils/component_mask.hpp"

static_assert(COMPONENT_MASK_WORDS * 64 >= ECS::MAX_COMPONENT_ID, "every component id gets a bit");
static_assert(ComponentMask::fits(1));
static_assert(ComponentMask::fits(ECS::MAX_COMPONENT_ID));
static_assert(!ComponentMask::fits(0));
static_assert(!ComponentMask::fits(ECS::MAX_COMPONENT_ID + 1));
static_assert(!ComponentMask::fits(ECS::WILDCARD));
static_assert(!ComponentMask::fits(ECS::PAIR(1, 2)));

TEST_CASE("ecs/component_mask: fits covers exactly the component id range") {
    CHECK(ComponentMask::fits(1));
    CHECK(ComponentMask::fits(64));
    CHECK(ComponentMask::fits(65));
    CHECK(ComponentMask::fits(ECS::MAX_COMPONENT_ID));
    CHECK_FALSE(ComponentMask::fits(0));
    CHECK_FALSE(ComponentMask::fits(ECS::MAX_COMPONENT_ID + 1));
    CHECK_FALSE(ComponentMask::fits(ECS::WILDCARD));
    CHECK_FALSE(ComponentMask::fits(ECS::REST));
    CHECK_FALSE(ComponentMask::fits(ECS::PAIR(1, 2)));
    // A component id carrying a generation is out of range.
    CHECK_FALSE(ComponentMask::fits(1 | (1ull << ECS::ENTITY_BITS)));
}

TEST_CASE("ecs/component_mask: a fresh mask is empty") {
    ComponentMask mask;
    CHECK(mask.is_empty());
    CHECK(mask.outer == 0);
    for (usz i = 0; i < COMPONENT_MASK_WORDS; i++) {
        CHECK(mask.words[i] == 0);
    }
    for (Id id = 1; id <= ECS::MAX_COMPONENT_ID; id++) {
        CHECK_FALSE(mask.has(id));
    }
}

TEST_CASE("ecs/component_mask: set makes only that id visible") {
    ComponentMask mask;
    mask.set(3);
    CHECK(mask.has(3));
    CHECK_FALSE(mask.has(2));
    CHECK_FALSE(mask.has(4));
    CHECK_FALSE(mask.is_empty());

    SUBCASE("setting twice is idempotent") {
        const u64 outer = mask.outer;
        const u64 word = mask.words[0];
        mask.set(3);
        CHECK(mask.outer == outer);
        CHECK(mask.words[0] == word);
    }
}

TEST_CASE("ecs/component_mask: ids map to words at 64-id boundaries") {
    ComponentMask mask;

    SUBCASE("id 1 is bit 0 of word 0") {
        mask.set(1);
        CHECK(mask.words[0] == 1ull);
        CHECK(mask.outer == 1ull);
    }
    SUBCASE("id 64 is the last bit of word 0") {
        mask.set(64);
        CHECK(mask.words[0] == (1ull << 63));
        CHECK(mask.outer == 1ull);
        CHECK(mask.has(64));
        CHECK_FALSE(mask.has(65));
    }
    SUBCASE("id 65 is the first bit of word 1") {
        mask.set(65);
        CHECK(mask.words[0] == 0);
        CHECK(mask.words[1] == 1ull);
        CHECK(mask.outer == 2ull);
        CHECK(mask.has(65));
        CHECK_FALSE(mask.has(64));
        CHECK_FALSE(mask.has(1));
    }
    SUBCASE("MAX_COMPONENT_ID lands in the last word") {
        mask.set(ECS::MAX_COMPONENT_ID);
        const usz last_word = (ECS::MAX_COMPONENT_ID - 1) / 64;
        CHECK(mask.words[last_word] != 0);
        CHECK(mask.outer == (1ull << last_word));
        CHECK(mask.has(ECS::MAX_COMPONENT_ID));
        CHECK_FALSE(mask.has(ECS::MAX_COMPONENT_ID - 1));
    }
}

TEST_CASE("ecs/component_mask: outer has one bit per non-empty word") {
    ComponentMask mask;
    mask.set(2);
    mask.set(70);
    mask.set(71);
    CHECK(mask.outer == 0b11ull);
    mask.set(ECS::MAX_COMPONENT_ID);
    CHECK(mask.outer == (0b11ull | (1ull << ((ECS::MAX_COMPONENT_ID - 1) / 64))));
}

TEST_CASE("ecs/component_mask: contains_all accepts subsets") {
    ComponentMask held;
    held.set(1);
    held.set(5);
    held.set(100);
    held.set(200);

    SUBCASE("an empty mask is always contained") {
        ComponentMask empty;
        CHECK(held.contains_all(empty));
        CHECK(empty.contains_all(empty));
    }
    SUBCASE("a mask contains itself") {
        CHECK(held.contains_all(held));
    }
    SUBCASE("a strict subset across words") {
        ComponentMask subset;
        subset.set(5);
        subset.set(200);
        CHECK(held.contains_all(subset));
        CHECK_FALSE(subset.contains_all(held));
    }
    SUBCASE("one missing id in a word that is otherwise matched") {
        ComponentMask other;
        other.set(5);
        other.set(6);
        CHECK_FALSE(held.contains_all(other));
    }
    SUBCASE("an id in a word the holder has empty") {
        ComponentMask other;
        other.set(1);
        other.set(65);
        CHECK_FALSE(held.contains_all(other));
    }
    SUBCASE("an empty mask contains nothing non-empty") {
        ComponentMask empty;
        ComponentMask one;
        one.set(1);
        CHECK_FALSE(empty.contains_all(one));
    }
}

TEST_CASE("ecs/component_mask: intersects needs one shared id") {
    ComponentMask a;
    a.set(10);
    a.set(130);

    SUBCASE("empty masks never intersect") {
        ComponentMask empty;
        CHECK_FALSE(a.intersects(empty));
        CHECK_FALSE(empty.intersects(a));
        CHECK_FALSE(empty.intersects(empty));
    }
    SUBCASE("a mask intersects itself") {
        CHECK(a.intersects(a));
    }
    SUBCASE("same word, different bits") {
        ComponentMask b;
        b.set(11);
        b.set(131);
        CHECK_FALSE(a.intersects(b));
        CHECK_FALSE(b.intersects(a));
    }
    SUBCASE("one shared id in the second word") {
        ComponentMask b;
        b.set(11);
        b.set(130);
        CHECK(a.intersects(b));
        CHECK(b.intersects(a));
    }
    SUBCASE("different words only") {
        ComponentMask b;
        b.set(70);
        CHECK_FALSE(a.intersects(b));
    }
}

TEST_CASE("ecs/component_mask: every id in the range round-trips") {
    ComponentMask mask;
    for (Id id = 1; id <= ECS::MAX_COMPONENT_ID; id += 3) {
        mask.set(id);
    }
    for (Id id = 1; id <= ECS::MAX_COMPONENT_ID; id++) {
        CHECK(mask.has(id) == ((id - 1) % 3 == 0));
    }
    CHECK(mask.outer == (1ull << COMPONENT_MASK_WORDS) - 1);
}
