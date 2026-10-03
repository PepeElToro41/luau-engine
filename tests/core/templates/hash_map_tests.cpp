#include "support/test_support.hpp"

#include "engine/memory/arena_allocator.hpp"
#include "engine/templates/hash_map.hpp"

#include <string>

namespace {

// Every key lands on the same home slot, so inserts probe linearly and
// removals leave tombstones in the middle of probe chains.
struct CollidingHash {
    size_t operator()(const int) const { return 0; }
};

// Non-trivially-copyable value that counts how it was built. No destructor
// (engine rule): only constructions and moves are observable.
struct Tracked {
    static inline int constructed = 0;
    static inline int copied = 0;
    static inline int moved = 0;

    int value = 0;

    Tracked() { constructed += 1; }
    explicit Tracked(const int value) : value(value) { constructed += 1; }
    Tracked(const Tracked& other) : value(other.value) { copied += 1; }
    Tracked(Tracked&& other) noexcept : value(other.value) {
        other.value = -1;
        moved += 1;
    }
    Tracked& operator=(const Tracked& other) {
        this->value = other.value;
        copied += 1;
        return *this;
    }
    Tracked& operator=(Tracked&& other) noexcept {
        this->value = other.value;
        other.value = -1;
        moved += 1;
        return *this;
    }

    static void reset_counters() {
        constructed = 0;
        copied = 0;
        moved = 0;
    }
};

} // namespace

TEST_CASE("templates/hash_map: a fresh map owns nothing and finds nothing") {
    HashMap<int, int> map;
    CHECK(map.entries == nullptr);
    CHECK(map.states == nullptr);
    CHECK(map.count == 0);
    CHECK(map.capacity == 0);
    CHECK(map.tombstones == 0);
    CHECK(map.is_empty());
    CHECK(map.allocator == MEMORY::heap_allocator());

    CHECK(map.find(1) == nullptr);
    CHECK_FALSE(map.contains(1));
    CHECK_FALSE(map.remove(1));
    CHECK(map.begin() == map.end());

    // clear() and free() on a never-used map are valid no-ops.
    map.clear();
    map.free();
    CHECK(map.capacity == 0);
}

TEST_CASE("templates/hash_map: insert stores values that find and contains report") {
    HashMap<int, int> map;

    int& stored = map.insert(1, 10);
    CHECK(stored == 10);
    CHECK(map.count == 1);
    CHECK(map.capacity == 8);
    CHECK(map.entries != nullptr);

    map.insert(2, 20);
    map.insert(3, 30);
    CHECK(map.count == 3);

    REQUIRE(map.find(1) != nullptr);
    CHECK(*map.find(1) == 10);
    CHECK(*map.find(2) == 20);
    CHECK(*map.find(3) == 30);
    CHECK(map.contains(2));

    SUBCASE("missing keys are reported as absent") {
        CHECK(map.find(4) == nullptr);
        CHECK(map.find(-1) == nullptr);
        CHECK(map.find(0) == nullptr);
        CHECK_FALSE(map.contains(100));
    }

    SUBCASE("inserting an existing key overwrites the value and keeps the count") {
        int& overwritten = map.insert(2, 99);
        CHECK(overwritten == 99);
        CHECK(*map.find(2) == 99);
        CHECK(map.count == 3);
    }

    SUBCASE("find on a const map returns a const pointer") {
        const HashMap<int, int>& view = map;
        const int* value = view.find(3);
        REQUIRE(value != nullptr);
        CHECK(*value == 30);
        CHECK(view.find(42) == nullptr);
    }

    map.free();
}

TEST_CASE("templates/hash_map: operator[] inserts a default value for absent keys") {
    HashMap<int, int> map;

    int& fresh = map[7];
    CHECK(fresh == 0);
    CHECK(map.count == 1);
    CHECK(map.contains(7));

    fresh = 70;
    CHECK(*map.find(7) == 70);

    // A second access returns the existing slot, not a new default.
    CHECK(map[7] == 70);
    CHECK(map.count == 1);

    map[8] += 5;
    CHECK(map[8] == 5);
    CHECK(map.count == 2);

    SUBCASE("default construction runs the value's constructor") {
        Tracked::reset_counters();
        HashMap<int, Tracked> tracked;
        Tracked& value = tracked[1];
        CHECK(value.value == 0);
        CHECK(Tracked::constructed == 1);
        tracked[1];
        CHECK(Tracked::constructed == 1);
        tracked.free();
    }

    map.free();
}

TEST_CASE("templates/hash_map: remove leaves a tombstone and reports whether anything was removed") {
    HashMap<int, int> map;
    map.insert(1, 10);
    map.insert(2, 20);
    map.insert(3, 30);

    CHECK(map.remove(2));
    CHECK(map.count == 2);
    CHECK(map.tombstones == 1);
    CHECK(map.find(2) == nullptr);
    CHECK_FALSE(map.contains(2));
    CHECK(*map.find(1) == 10);
    CHECK(*map.find(3) == 30);

    // Removing again is a no-op.
    CHECK_FALSE(map.remove(2));
    CHECK(map.count == 2);
    CHECK(map.tombstones == 1);

    // The key can be inserted again, which reclaims a tombstone.
    map.insert(2, 22);
    CHECK(map.count == 3);
    CHECK(map.tombstones == 0);
    CHECK(*map.find(2) == 22);

    map.free();
}

TEST_CASE("templates/hash_map: lookups probe past tombstones in a colliding chain") {
    HashMap<int, int, CollidingHash> map;
    map.insert(1, 10);
    map.insert(2, 20);
    map.insert(3, 30);
    map.insert(4, 40);

    // Key 2 sits between 1 and 3/4 in the probe chain; removing it must not
    // cut the chain short.
    CHECK(map.remove(2));
    CHECK(*map.find(3) == 30);
    CHECK(*map.find(4) == 40);
    CHECK(*map.find(1) == 10);
    CHECK(map.find(2) == nullptr);
    CHECK(map.find(5) == nullptr);

    // Removing the head and inserting a new key reuses a tombstone slot while
    // keeping every survivor reachable.
    CHECK(map.remove(1));
    CHECK(map.tombstones == 2);
    map.insert(5, 50);
    CHECK(map.tombstones == 1);
    CHECK(*map.find(3) == 30);
    CHECK(*map.find(4) == 40);
    CHECK(*map.find(5) == 50);
    CHECK(map.count == 3);

    map.free();
}

TEST_CASE("templates/hash_map: capacity grows as a power of two under the load limit") {
    HashMap<int, int> map;

    for (int i = 0; i < 6; ++i) {
        map.insert(i, i);
    }
    // 6 of 8 slots is exactly 75%: still allowed.
    CHECK(map.capacity == 8);

    map.insert(6, 6);
    CHECK(map.capacity == 16);
    CHECK(map.count == 7);

    for (int i = 7; i < 1000; ++i) {
        map.insert(i, i * 2);
    }
    CHECK(map.count == 1000);
    CHECK(map.capacity == 2048);
    CHECK((map.capacity & (map.capacity - 1)) == 0);

    for (int i = 0; i < 1000; ++i) {
        const int* value = map.find(i);
        REQUIRE(value != nullptr);
        CHECK(*value == (i < 7 ? i : i * 2));
    }
    CHECK(map.find(1000) == nullptr);

    map.free();
}

TEST_CASE("templates/hash_map: reserve pre-sizes the table so inserts do not rehash") {
    HashMap<int, int> map;
    map.reserve(100);
    CHECK(map.capacity == 256);
    CHECK(map.count == 0);
    typename HashMap<int, int>::Entry* storage = map.entries;

    for (int i = 0; i < 100; ++i) {
        map.insert(i, i);
    }
    CHECK(map.entries == storage);
    CHECK(map.capacity == 256);

    // Reserving less than the current capacity changes nothing.
    map.reserve(10);
    CHECK(map.capacity == 256);
    CHECK(map.entries == storage);

    map.free();
}

TEST_CASE("templates/hash_map: churn of inserts and removes rehashes tombstones away") {
    HashMap<int, int> map;

    // Each round fills the table with fresh keys and removes them all, so
    // tombstones alone would fill the table if they were never reclaimed.
    for (int round = 0; round < 50; ++round) {
        for (int i = 0; i < 6; ++i) {
            map.insert(round * 100 + i, round);
        }
        CHECK(map.count == 6);
        for (int i = 0; i < 6; ++i) {
            CHECK(map.remove(round * 100 + i));
        }
        CHECK(map.count == 0);
        CHECK(map.tombstones <= 6);
    }
    // Only live entries count toward growth, so the table never had to grow.
    CHECK(map.capacity == 8);

    // An in-place rehash happens once tombstones push past the load limit,
    // and the table is fully usable afterwards.
    map.insert(1, 1);
    CHECK(map.count == 1);
    CHECK(map.tombstones == 0);
    CHECK(*map.find(1) == 1);

    SUBCASE("a long mixed workload keeps every live key findable") {
        map.clear();
        for (int i = 0; i < 5000; ++i) {
            map.insert(i, i);
            if (i % 3 == 0) {
                CHECK(map.remove(i));
            }
        }
        size_t expected = 0;
        for (int i = 0; i < 5000; ++i) {
            if (i % 3 == 0) {
                CHECK(map.find(i) == nullptr);
            } else {
                const int* value = map.find(i);
                REQUIRE(value != nullptr);
                CHECK(*value == i);
                expected += 1;
            }
        }
        CHECK(map.count == expected);
        CHECK(map.count + map.tombstones <= map.capacity * 3 / 4);
    }

    map.free();
}

TEST_CASE("templates/hash_map: clear keeps the storage, free releases it") {
    HashMap<int, int> map;
    for (int i = 0; i < 20; ++i) {
        map.insert(i, i);
    }
    map.remove(3);
    typename HashMap<int, int>::Entry* storage = map.entries;
    size_t capacity = map.capacity;

    map.clear();
    CHECK(map.count == 0);
    CHECK(map.tombstones == 0);
    CHECK(map.is_empty());
    CHECK(map.entries == storage);
    CHECK(map.capacity == capacity);
    CHECK(map.find(0) == nullptr);
    CHECK(map.find(19) == nullptr);
    CHECK(map.begin() == map.end());

    // The kept storage is reused.
    map.insert(5, 55);
    CHECK(map.entries == storage);
    CHECK(*map.find(5) == 55);

    map.free();
    CHECK(map.entries == nullptr);
    CHECK(map.states == nullptr);
    CHECK(map.count == 0);
    CHECK(map.capacity == 0);
    CHECK(map.find(5) == nullptr);

    // A freed map is usable again.
    map.insert(1, 1);
    CHECK(map.count == 1);
    map.free();
}

TEST_CASE("templates/hash_map: iteration visits every live entry exactly once") {
    HashMap<int, int> map;
    for (int i = 0; i < 50; ++i) {
        map.insert(i, i * 3);
    }
    for (int i = 0; i < 50; i += 5) {
        map.remove(i);
    }

    bool seen[50] = {};
    size_t visited = 0;
    for (auto& entry : map) {
        CHECK(entry.value == entry.key * 3);
        CHECK_FALSE(seen[entry.key]);
        seen[entry.key] = true;
        visited += 1;
    }
    CHECK(visited == 40);
    CHECK(visited == map.count);
    for (int i = 0; i < 50; ++i) {
        CHECK(seen[i] == (i % 5 != 0));
    }

    SUBCASE("const iteration works on a const view") {
        const HashMap<int, int>& view = map;
        size_t const_visited = 0;
        for (const auto& entry : view) {
            CHECK(entry.value == entry.key * 3);
            const_visited += 1;
        }
        CHECK(const_visited == 40);
    }

    SUBCASE("mutation through the iterator is visible to find") {
        for (auto it = map.begin(); it != map.end(); ++it) {
            it->value = -1;
        }
        CHECK(*map.find(1) == -1);
        CHECK(*map.find(49) == -1);
    }

    map.free();
}

TEST_CASE("templates/hash_map: move constructor and assignment leave the source empty") {
    HashMap<int, int> source;
    for (int i = 0; i < 10; ++i) {
        source.insert(i, i);
    }
    source.remove(4);
    typename HashMap<int, int>::Entry* storage = source.entries;

    SUBCASE("move construction transfers the storage") {
        HashMap<int, int> target(std::move(source));
        CHECK(target.entries == storage);
        CHECK(target.count == 9);
        CHECK(target.capacity == 16);
        CHECK(target.tombstones == 1);
        CHECK(*target.find(9) == 9);
        CHECK(target.find(4) == nullptr);

        CHECK(source.entries == nullptr);
        CHECK(source.states == nullptr);
        CHECK(source.count == 0);
        CHECK(source.capacity == 0);
        CHECK(source.tombstones == 0);
        CHECK(source.find(9) == nullptr);

        target.free();
    }

    SUBCASE("move assignment frees the old storage and takes the new one") {
        HashMap<int, int> target;
        target.insert(100, 100);

        target = std::move(source);
        CHECK(target.entries == storage);
        CHECK(target.count == 9);
        CHECK(target.find(100) == nullptr);
        CHECK(*target.find(0) == 0);

        CHECK(source.entries == nullptr);
        CHECK(source.count == 0);
        CHECK(source.capacity == 0);

        target.free();
    }

    source.free();
}

TEST_CASE("templates/hash_map: non-trivially-copyable values are moved, not copied, on rehash") {
    Tracked::reset_counters();
    HashMap<int, Tracked> map;

    for (int i = 0; i < 6; ++i) {
        map.insert(i, Tracked(i));
    }
    CHECK(map.capacity == 8);
    CHECK(Tracked::constructed == 6);
    CHECK(Tracked::copied == 0);
    // Each temporary is moved once into its slot.
    CHECK(Tracked::moved == 6);

    Tracked::reset_counters();
    map.insert(6, Tracked(6)); // triggers the 8 -> 16 rehash
    CHECK(map.capacity == 16);
    CHECK(Tracked::copied == 0);
    // Six entries relocated plus the new temporary moved into place.
    CHECK(Tracked::moved == 7);

    for (int i = 0; i < 7; ++i) {
        const Tracked* value = map.find(i);
        REQUIRE(value != nullptr);
        CHECK(value->value == i);
    }

    SUBCASE("insert by const reference copies the value") {
        Tracked::reset_counters();
        Tracked lvalue(50);
        map.insert(50, lvalue);
        CHECK(Tracked::copied == 1);
        CHECK(lvalue.value == 50);
        CHECK(map.find(50)->value == 50);
    }

    map.free();
}

TEST_CASE("templates/hash_map: string keys hash and compare by content") {
    HashMap<std::string, int> map;
    map.insert("alpha", 1);
    map.insert(std::string("beta"), 2);

    std::string lookup = "alp";
    lookup += "ha";
    REQUIRE(map.find(lookup) != nullptr);
    CHECK(*map.find(lookup) == 1);
    CHECK(*map.find("beta") == 2);
    CHECK(map.find("gamma") == nullptr);

    map["gamma"] = 3;
    CHECK(map.count == 3);
    CHECK(map.remove("alpha"));
    CHECK(map.find("alpha") == nullptr);

    map.free();
}

TEST_CASE("templates/hash_map: an explicit allocator backs the storage") {
    alignas(64) static char buffer[64 * 1024];
    ArenaAllocator arena(buffer, sizeof(buffer));

    HashMap<int, int> map(&arena);
    CHECK(map.allocator == &arena);
    CHECK(arena.offset == 0);

    map.insert(1, 1);
    CHECK(arena.offset > 0);
    CHECK(reinterpret_cast<char*>(map.entries) >= buffer);
    CHECK(reinterpret_cast<char*>(map.entries) < buffer + sizeof(buffer));
    CHECK(reinterpret_cast<char*>(map.states) >= buffer);
    CHECK(reinterpret_cast<char*>(map.states) < buffer + sizeof(buffer));

    // Growing allocates a fresh block from the arena and keeps every entry.
    usz before_growth = arena.offset;
    for (int i = 2; i <= 50; ++i) {
        map.insert(i, i);
    }
    CHECK(arena.offset > before_growth);
    CHECK(reinterpret_cast<char*>(map.entries) >= buffer);
    CHECK(reinterpret_cast<char*>(map.entries) < buffer + sizeof(buffer));
    for (int i = 1; i <= 50; ++i) {
        REQUIRE(map.find(i) != nullptr);
        CHECK(*map.find(i) == i);
    }

    HashMap<int, int> moved(std::move(map));
    CHECK(moved.allocator == &arena);

    moved.free();
    map.free();
    arena.reset();

    SUBCASE("a temporal allocator reclaims the storage when its scope ends") {
        {
            TemporalAllocator temp = TemporalAllocator::create();
            HashMap<int, int> scratch(&temp);
            for (int i = 0; i < 100; ++i) {
                scratch.insert(i, i);
            }
            CHECK(scratch.count == 100);
            CHECK(MAIN_ARENA.offset > 0);
        }
        CHECK_ARENA_CLEAN();
    }
}
