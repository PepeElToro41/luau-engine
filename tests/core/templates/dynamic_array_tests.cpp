#include "support/test_support.hpp"

#include "engine/memory/arena_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

namespace {

// Non-trivially-copyable element that counts how it was built. It has no
// destructor on purpose (engine rule), so only constructions and moves are
// observable; a moved-from instance carries value -1.
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

static_assert(!std::is_trivially_copyable_v<Tracked>);

} // namespace

TEST_CASE("templates/dynamic_array: a fresh array owns nothing") {
    DynamicArray<int> array;
    CHECK(array.data == nullptr);
    CHECK(array.count == 0);
    CHECK(array.capacity == 0);
    CHECK(array.is_empty());
    CHECK(array.allocator == MEMORY::heap_allocator());
    CHECK(array.begin() == array.end());

    // free() on a never-used array is a valid no-op.
    array.free();
    CHECK(array.data == nullptr);
    CHECK(array.capacity == 0);
}

TEST_CASE("templates/dynamic_array: push grows capacity and preserves elements") {
    DynamicArray<int> array;

    array.push(1);
    CHECK(array.count == 1);
    CHECK(array.capacity == 8);
    CHECK(array.data != nullptr);

    for (int i = 2; i <= 100; ++i) {
        array.push(i);
    }
    CHECK(array.count == 100);
    CHECK(array.capacity >= 100);
    // Capacity doubles from the minimum of 8: 8, 16, 32, 64, 128.
    CHECK(array.capacity == 128);

    for (size_t i = 0; i < array.count; ++i) {
        CHECK(array[i] == static_cast<int>(i + 1));
    }
    CHECK(array.first() == 1);
    CHECK(array.last() == 100);

    array.free();
}

TEST_CASE("templates/dynamic_array: push returns a reference to the stored slot") {
    DynamicArray<int> array;
    int& ref = array.push(5);
    CHECK(&ref == array.data);
    ref = 9;
    CHECK(array[0] == 9);

    Tracked::reset_counters();
    DynamicArray<Tracked> tracked;
    Tracked& emplaced = tracked.emplace(42);
    CHECK(emplaced.value == 42);
    CHECK(&emplaced == &tracked[0]);
    CHECK(Tracked::constructed == 1);
    CHECK(Tracked::copied == 0);

    array.free();
    tracked.free();
}

TEST_CASE("templates/dynamic_array: reserve never shrinks and resize constructs or drops elements") {
    DynamicArray<int> array;

    SUBCASE("reserve grows to the requested capacity exactly") {
        array.reserve(20);
        CHECK(array.capacity == 20);
        CHECK(array.count == 0);

        array.reserve(5);
        CHECK(array.capacity == 20);
    }

    SUBCASE("reserve keeps existing elements") {
        for (int i = 0; i < 10; ++i) {
            array.push(i);
        }
        array.reserve(1000);
        CHECK(array.capacity == 1000);
        CHECK(array.count == 10);
        for (int i = 0; i < 10; ++i) {
            CHECK(array[static_cast<size_t>(i)] == i);
        }
    }

    SUBCASE("resize up value-initializes, resize down keeps the prefix") {
        array.push(7);
        array.resize(4);
        CHECK(array.count == 4);
        CHECK(array[0] == 7);
        CHECK(array[1] == 0);
        CHECK(array[2] == 0);
        CHECK(array[3] == 0);

        array[3] = 3;
        array.resize(2);
        CHECK(array.count == 2);
        CHECK(array[0] == 7);
        CHECK(array[1] == 0);
        CHECK(array.capacity >= 4);
    }

    array.free();
}

TEST_CASE("templates/dynamic_array: pop returns the last element and shrinks the count") {
    DynamicArray<int> array;
    array.push(1);
    array.push(2);
    array.push(3);

    CHECK(array.pop() == 3);
    CHECK(array.count == 2);
    CHECK(array.last() == 2);
    CHECK(array.pop() == 2);
    CHECK(array.pop() == 1);
    CHECK(array.is_empty());
    CHECK(array.capacity == 8);

    array.free();
}

TEST_CASE("templates/dynamic_array: remove_at preserves order, remove_swap fills from the back") {
    DynamicArray<int> array;
    for (int i = 0; i < 6; ++i) {
        array.push(i * 10); // 0 10 20 30 40 50
    }

    SUBCASE("remove_at shifts later elements down") {
        array.remove_at(2);
        CHECK(array.count == 5);
        const int expected[] = {0, 10, 30, 40, 50};
        for (size_t i = 0; i < 5; ++i) {
            CHECK(array[i] == expected[i]);
        }

        array.remove_at(4); // last element
        CHECK(array.count == 4);
        CHECK(array.last() == 40);

        array.remove_at(0);
        CHECK(array.count == 3);
        CHECK(array.first() == 10);
    }

    SUBCASE("remove_swap moves the last element into the hole") {
        array.remove_swap(1);
        CHECK(array.count == 5);
        CHECK(array[1] == 50);
        CHECK(array[4] == 40);

        array.remove_swap(4); // removing the last is a plain pop
        CHECK(array.count == 4);
        CHECK(array.last() == 30);
    }

    array.free();
}

TEST_CASE("templates/dynamic_array: clear keeps the buffer, free releases it") {
    DynamicArray<int> array;
    for (int i = 0; i < 20; ++i) {
        array.push(i);
    }
    int* buffer = array.data;
    size_t capacity = array.capacity;

    array.clear();
    CHECK(array.count == 0);
    CHECK(array.is_empty());
    CHECK(array.data == buffer);
    CHECK(array.capacity == capacity);

    // The kept buffer is reused by the next push.
    array.push(99);
    CHECK(array.data == buffer);
    CHECK(array[0] == 99);

    array.free();
    CHECK(array.count == 0);
    CHECK(array.data == nullptr);
    CHECK(array.capacity == 0);
    CHECK(array.allocator == MEMORY::heap_allocator());

    // A freed array is usable again.
    array.push(1);
    CHECK(array.count == 1);
    array.free();
}

TEST_CASE("templates/dynamic_array: move constructor and assignment leave the source empty") {
    DynamicArray<int> source;
    for (int i = 0; i < 10; ++i) {
        source.push(i);
    }
    int* buffer = source.data;

    SUBCASE("move construction transfers the buffer") {
        DynamicArray<int> target(std::move(source));
        CHECK(target.data == buffer);
        CHECK(target.count == 10);
        CHECK(target.capacity == 16);
        CHECK(target[9] == 9);

        CHECK(source.data == nullptr);
        CHECK(source.count == 0);
        CHECK(source.capacity == 0);

        target.free();
    }

    SUBCASE("move assignment frees the old buffer and takes the new one") {
        DynamicArray<int> target;
        target.push(123);
        target.push(456);

        target = std::move(source);
        CHECK(target.data == buffer);
        CHECK(target.count == 10);
        CHECK(target[0] == 0);
        CHECK(target[9] == 9);

        CHECK(source.data == nullptr);
        CHECK(source.count == 0);
        CHECK(source.capacity == 0);

        target.free();
    }

    // Freeing the moved-from source is a harmless no-op.
    source.free();
}

TEST_CASE("templates/dynamic_array: growth moves non-trivially-copyable elements instead of copying") {
    Tracked::reset_counters();
    DynamicArray<Tracked> array;

    for (int i = 0; i < 9; ++i) {
        array.emplace(i);
    }
    // 9 pushes cross the 8 -> 16 growth boundary once: the 8 elements that
    // were already stored get moved into the fresh buffer, never copied.
    CHECK(array.count == 9);
    CHECK(array.capacity == 16);
    CHECK(Tracked::constructed == 9);
    CHECK(Tracked::copied == 0);
    CHECK(Tracked::moved == 8);
    for (int i = 0; i < 9; ++i) {
        CHECK(array[static_cast<size_t>(i)].value == i);
    }

    SUBCASE("push by const reference copies, push by rvalue moves") {
        Tracked::reset_counters();
        Tracked lvalue(100);
        array.push(lvalue);
        CHECK(Tracked::copied == 1);
        CHECK(lvalue.value == 100);

        array.push(Tracked(200));
        CHECK(Tracked::copied == 1);
        CHECK(array.last().value == 200);
    }

    SUBCASE("pop moves the element out") {
        Tracked::reset_counters();
        Tracked popped = array.pop();
        CHECK(popped.value == 8);
        CHECK(Tracked::copied == 0);
        CHECK(array.count == 8);
    }

    SUBCASE("remove_at moves elements down without copying") {
        Tracked::reset_counters();
        array.remove_at(0);
        CHECK(Tracked::copied == 0);
        CHECK(array.count == 8);
        for (int i = 0; i < 8; ++i) {
            CHECK(array[static_cast<size_t>(i)].value == i + 1);
        }
    }

    array.free();
}

TEST_CASE("templates/dynamic_array: range-for iterates elements in order") {
    DynamicArray<int> array;
    for (int i = 0; i < 5; ++i) {
        array.push(i);
    }

    int expected = 0;
    for (int value : array) {
        CHECK(value == expected);
        expected += 1;
    }
    CHECK(expected == 5);

    const DynamicArray<int>& view = array;
    CHECK(view.end() - view.begin() == 5);
    CHECK(view.first() == 0);
    CHECK(view.last() == 4);

    array.free();
}

TEST_CASE("templates/dynamic_array: an explicit allocator backs every allocation") {
    alignas(64) static char buffer[64 * 1024];
    ArenaAllocator arena(buffer, sizeof(buffer));

    DynamicArray<int> array(&arena);
    CHECK(array.allocator == &arena);
    CHECK(arena.offset == 0);

    array.push(1);
    CHECK(arena.offset >= 8 * sizeof(int));
    CHECK(reinterpret_cast<char*>(array.data) >= buffer);
    CHECK(reinterpret_cast<char*>(array.data) < buffer + sizeof(buffer));

    // Growing allocates a fresh block from the arena (arenas cannot
    // reallocate) and the contents survive the move.
    usz before_growth = arena.offset;
    for (int i = 2; i <= 9; ++i) {
        array.push(i);
    }
    CHECK(array.capacity == 16);
    CHECK(arena.offset > before_growth);
    CHECK(reinterpret_cast<char*>(array.data) >= buffer);
    CHECK(reinterpret_cast<char*>(array.data) < buffer + sizeof(buffer));
    for (int i = 0; i < 9; ++i) {
        CHECK(array[static_cast<size_t>(i)] == i + 1);
    }

    // The allocator pointer survives a move.
    DynamicArray<int> moved(std::move(array));
    CHECK(moved.allocator == &arena);
    CHECK(array.allocator == &arena);

    moved.free();
    array.free();
    arena.reset();
    CHECK(arena.offset == 0);
}

TEST_CASE("templates/dynamic_array: a temporal allocator reclaims the buffer when its scope ends") {
    {
        TemporalAllocator temp = TemporalAllocator::create();
        DynamicArray<int> scratch(&temp);
        for (int i = 0; i < 100; ++i) {
            scratch.push(i);
        }
        CHECK(scratch.count == 100);
        CHECK(MAIN_ARENA.offset > 0);
        // No free() needed: the scope exit rewinds MAIN_ARENA.
    }
    CHECK_ARENA_CLEAN();
}
