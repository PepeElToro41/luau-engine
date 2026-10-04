#include "support/test_support.hpp"

#include "engine/memory/arena_allocator.hpp"
#include "engine/utils/singletons.hpp"

namespace {

struct Clock {
    f64 time = 0.0;
    f64 rate = 1.0;

    Clock() = default;
    Clock(const f64 time, const f64 rate) : time(time), rate(rate) {}
};

struct Settings {
    i32 width = 1280;
    i32 height = 720;
};

// Only constructible with arguments, so create<T>(args...) must forward them.
struct Named {
    const char* name;
    i32 id;

    Named(const char* name, const i32 id) : name(name), id(id) {}
};

// Counts constructions and destructions so the store's lifecycle is
// observable. A destructor is fine here: it is a test-only type standing in
// for user types the store may hold.
struct Tracked {
    static inline int constructed = 0;
    static inline int destroyed = 0;

    int value = 7;

    Tracked() { constructed += 1; }
    explicit Tracked(const int value) : value(value) { constructed += 1; }
    ~Tracked() { destroyed += 1; }

    static void reset_counters() {
        constructed = 0;
        destroyed = 0;
    }
};

struct alignas(64) Aligned {
    f32 values[16] = {};
};

} // namespace

TEST_CASE("utils/singletons: a fresh store holds nothing and get never creates") {
    Singletons singletons;
    CHECK(singletons.count() == 0);
    CHECK_FALSE(singletons.has<Clock>());
    CHECK(singletons.get<Clock>() == nullptr);
    CHECK(singletons.get<Clock>() == nullptr);
    CHECK(singletons.count() == 0);

    // free() on a store that never allocated is a no-op.
    singletons.free();
    CHECK(singletons.count() == 0);
}

TEST_CASE("utils/singletons: create<T> builds the value and get<T> returns that same object") {
    Singletons singletons;

    Clock* clock = singletons.create<Clock>();
    REQUIRE(clock != nullptr);
    CHECK(clock->time == 0.0);
    CHECK(clock->rate == 1.0);
    CHECK(singletons.count() == 1);
    CHECK(singletons.has<Clock>());

    clock->time = 1.5;
    CHECK(singletons.get<Clock>() == clock);
    CHECK(singletons.get<Clock>()->time == 1.5);
    CHECK(singletons.count() == 1);

    singletons.free();
}

TEST_CASE("utils/singletons: create<T> forwards constructor arguments") {
    Singletons singletons;

    const Clock* clock = singletons.create<Clock>(10.0, 0.5);
    REQUIRE(clock != nullptr);
    CHECK(clock->time == 10.0);
    CHECK(clock->rate == 0.5);

    // A type with no default constructor.
    const Named* named = singletons.create<Named>("player", 3);
    REQUIRE(named != nullptr);
    CHECK(named->name == doctest::String("player"));
    CHECK(named->id == 3);
    CHECK(singletons.get<Named>() == named);

    singletons.free();
}

TEST_CASE("utils/singletons: creating a type twice fails and keeps the first value") {
    Tracked::reset_counters();
    Singletons singletons;

    Tracked* first = singletons.create<Tracked>(1);
    REQUIRE(first != nullptr);
    CHECK(first->value == 1);

    CHECK(singletons.create<Tracked>(2) == nullptr);
    CHECK(singletons.create<Tracked>() == nullptr);
    CHECK(singletons.count() == 1);
    CHECK(singletons.get<Tracked>() == first);
    CHECK(first->value == 1);
    CHECK(Tracked::constructed == 1);
    CHECK(Tracked::destroyed == 0);

    singletons.free();
}

TEST_CASE("utils/singletons: each type is its own singleton") {
    Singletons singletons;

    Clock* clock = singletons.create<Clock>();
    Settings* settings = singletons.create<Settings>();
    REQUIRE(clock != nullptr);
    REQUIRE(settings != nullptr);
    CHECK(static_cast<void*>(clock) != static_cast<void*>(settings));
    CHECK(settings->width == 1280);
    CHECK(settings->height == 720);
    CHECK(singletons.count() == 2);

    settings->width = 1920;
    clock->time = 2.0;
    CHECK(singletons.get<Settings>()->width == 1920);
    CHECK(singletons.get<Clock>()->time == 2.0);
    CHECK_FALSE(singletons.has<Tracked>());
    CHECK(singletons.get<Tracked>() == nullptr);

    singletons.free();
}

TEST_CASE("utils/singletons: pointers stay valid while other singletons are added") {
    Singletons singletons;

    Clock* clock = singletons.create<Clock>();
    REQUIRE(clock != nullptr);
    clock->time = 3.0;

    // Enough distinct types to force the entry map to grow past its first
    // capacity: the values are separate allocations, so the pointer holds.
    singletons.create<Settings>();
    singletons.create<Tracked>();
    singletons.create<Aligned>();
    singletons.create<Position>();
    singletons.create<Velocity>();
    singletons.create<Health>();
    singletons.create<TagA>();
    singletons.create<TagB>();
    singletons.create<Likes>();
    singletons.create<Eats>();
    CHECK(singletons.count() == 11);

    CHECK(singletons.get<Clock>() == clock);
    CHECK(clock->time == 3.0);

    singletons.free();
}

TEST_CASE("utils/singletons: remove<T> destroys the value and allows a new create<T>") {
    Tracked::reset_counters();
    Singletons singletons;

    CHECK_FALSE(singletons.remove<Tracked>());

    Tracked* first = singletons.create<Tracked>(42);
    REQUIRE(first != nullptr);
    CHECK(Tracked::constructed == 1);
    CHECK(Tracked::destroyed == 0);

    CHECK(singletons.remove<Tracked>());
    CHECK(Tracked::destroyed == 1);
    CHECK(singletons.count() == 0);
    CHECK_FALSE(singletons.has<Tracked>());
    CHECK(singletons.get<Tracked>() == nullptr);
    CHECK_FALSE(singletons.remove<Tracked>());

    // A new value, built from the new arguments.
    const Tracked* second = singletons.create<Tracked>();
    REQUIRE(second != nullptr);
    CHECK(second->value == 7);
    CHECK(Tracked::constructed == 2);

    singletons.free();
    CHECK(Tracked::destroyed == 2);
}

TEST_CASE("utils/singletons: free destroys every value and the store is reusable") {
    Tracked::reset_counters();
    Singletons singletons;

    singletons.create<Tracked>();
    singletons.create<Clock>()->time = 9.0;
    singletons.create<Settings>();
    CHECK(singletons.count() == 3);

    singletons.free();
    CHECK(Tracked::destroyed == 1);
    CHECK(singletons.count() == 0);
    CHECK_FALSE(singletons.has<Clock>());
    CHECK(singletons.get<Clock>() == nullptr);
    CHECK(singletons.entries.entries == nullptr);

    // Usable again after free().
    const Clock* clock = singletons.create<Clock>();
    REQUIRE(clock != nullptr);
    CHECK(clock->time == 0.0);
    CHECK(singletons.count() == 1);
    singletons.free();
}

TEST_CASE("utils/singletons: values honor their alignment") {
    Singletons singletons;

    const Aligned* aligned = singletons.create<Aligned>();
    REQUIRE(aligned != nullptr);
    CHECK(reinterpret_cast<usz>(aligned) % alignof(Aligned) == 0);

    singletons.free();
}

TEST_CASE("utils/singletons: a store on an explicit allocator puts the values there") {
    ArenaAllocator arena(16 * MEMORY::KB);

    Singletons singletons(&arena);
    CHECK(singletons.allocator == &arena);

    const usz before = arena.offset;
    singletons.create<Clock>();
    singletons.create<Settings>();
    CHECK(arena.offset > before);
    CHECK(singletons.get<Settings>()->height == 720);

    singletons.free();
    arena.release();
}
