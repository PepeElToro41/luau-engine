#include "support/test_support.hpp"

#include "engine/memory/arena_allocator.hpp"
#include "engine/utils/log.hpp"
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

// --- Hierarchy -----------------------------------------------------------------

namespace {

struct SharedThing {
    i32 value = 1;
};

struct LocalThing {
    i32 value = 2;
};

struct Undeclared {
    i32 value = 3;
};

struct QuietLog {
    LogLevel level;
    QuietLog() : level(LOG::stderr_level()) { LOG::set_stderr_level(LOG_LEVEL_NONE); }
    void restore() const { LOG::set_stderr_level(this->level); }
};

} // namespace

TEST_CASE("utils/singletons: scopes are declared once per type, process-wide") {
    SINGLETON_SCOPE::reset();
    CHECK(SINGLETON_SCOPE::count() == 0);
    CHECK(SINGLETON_SCOPE::of<SharedThing>() == SCOPE_UNDECLARED);
    CHECK(SINGLETON_SCOPE::name_of(TYPE_ID::get<SharedThing>()) == nullptr);

    CHECK(SINGLETON_SCOPE::declare<SharedThing>(SCOPE_SHARED, "SharedThing"));
    CHECK(SINGLETON_SCOPE::declare<LocalThing>(SCOPE_LOCAL, "LocalThing"));
    CHECK(SINGLETON_SCOPE::count() == 2);
    CHECK(SINGLETON_SCOPE::of<SharedThing>() == SCOPE_SHARED);
    CHECK(SINGLETON_SCOPE::of<LocalThing>() == SCOPE_LOCAL);
    CHECK(doctest::String(SINGLETON_SCOPE::name_of(TYPE_ID::get<LocalThing>())) == "LocalThing");
    CHECK(SINGLETON_SCOPE::entry(0).type == TYPE_ID::get<SharedThing>());

    // The same again is fine; another scope is refused and the first stays.
    CHECK(SINGLETON_SCOPE::declare<SharedThing>(SCOPE_SHARED));
    const QuietLog quiet;
    CHECK_FALSE(SINGLETON_SCOPE::declare<SharedThing>(SCOPE_LOCAL));
    CHECK_FALSE(SINGLETON_SCOPE::declare<Undeclared>(SCOPE_UNDECLARED));
    quiet.restore();
    CHECK(SINGLETON_SCOPE::of<SharedThing>() == SCOPE_SHARED);
    CHECK(SINGLETON_SCOPE::count() == 2);

    SINGLETON_SCOPE::reset();
    CHECK(SINGLETON_SCOPE::count() == 0);
    CHECK(SINGLETON_SCOPE::of<LocalThing>() == SCOPE_UNDECLARED);
}

TEST_CASE("utils/singletons: a child resolves shared types upward and shadows local ones") {
    SINGLETON_SCOPE::reset();
    SINGLETON_SCOPE::declare<SharedThing>(SCOPE_SHARED, "SharedThing");
    SINGLETON_SCOPE::declare<LocalThing>(SCOPE_LOCAL, "LocalThing");
    SINGLETON_SCOPE::declare<Clock>(SCOPE_LOCAL, "Clock");

    Singletons root;
    SharedThing* shared = root.create<SharedThing>();
    LocalThing* root_local = root.create<LocalThing>();
    REQUIRE(shared != nullptr);
    REQUIRE(root_local != nullptr);
    root_local->value = 10;

    Singletons child;
    child.parent = &root;
    CHECK(child.count() == 0);

    // Shared: the parent's, through the child.
    CHECK(child.get<SharedThing>() == shared);
    CHECK(child.has<SharedThing>());
    CHECK_FALSE(child.has_local<SharedThing>());
    shared->value = 5;
    CHECK(child.get<SharedThing>()->value == 5);

    // Local: the child must have its own; the parent's is never handed out.
    const QuietLog quiet;
    CHECK(child.get<LocalThing>() == nullptr);
    CHECK_FALSE(child.has<LocalThing>());
    CHECK_FALSE(child.check_locals());
    quiet.restore();
    LocalThing* child_local = child.create<LocalThing>();
    REQUIRE(child_local != nullptr);
    CHECK(child_local != root_local);
    CHECK(child.get<LocalThing>() == child_local);
    CHECK(child.get<LocalThing>()->value == 2);
    CHECK(root.get<LocalThing>() == root_local);
    CHECK(root.get<LocalThing>()->value == 10);
    CHECK(child.has_local<LocalThing>());
    CHECK(child.check_locals());
    CHECK(root.check_locals());

    // A local type the parent lacks too is simply absent, no complaint.
    CHECK(child.get<Clock>() == nullptr);
    CHECK_FALSE(child.has<Clock>());

    SUBCASE("a grandchild walks the whole chain for shared types") {
        Singletons grandchild;
        grandchild.parent = &child;
        CHECK(grandchild.get<SharedThing>() == shared);
        const QuietLog quiet_again;
        CHECK(grandchild.get<LocalThing>() == nullptr);
        CHECK_FALSE(grandchild.check_locals());
        quiet_again.restore();
        grandchild.create<LocalThing>()->value = 30;
        CHECK(grandchild.get<LocalThing>()->value == 30);
        CHECK(child.get<LocalThing>()->value == 2);
        CHECK(grandchild.check_locals());
        grandchild.free();
    }
    SUBCASE("a child shadows a shared type too when it creates its own") {
        SharedThing* own = child.create<SharedThing>();
        REQUIRE(own != nullptr);
        CHECK(child.get<SharedThing>() == own);
        CHECK(root.get<SharedThing>() == shared);
        CHECK(child.remove<SharedThing>());
        CHECK(child.get<SharedThing>() == shared);
    }

    child.free();
    root.free();
    SINGLETON_SCOPE::reset();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("utils/singletons: a child refuses to traverse for an undeclared type") {
    SINGLETON_SCOPE::reset();
    Singletons root;
    Undeclared* value = root.create<Undeclared>();
    REQUIRE(value != nullptr);
    Singletons child;
    child.parent = &root;

    // Undeclared is as good as local: nothing comes down, and the first
    // miss is warned about.
    const QuietLog quiet;
    CHECK(child.get<Undeclared>() == nullptr);
    CHECK(child.get<Undeclared>() == nullptr);
    CHECK_FALSE(child.has<Undeclared>());
    quiet.restore();
    // The root itself is unaffected: no parent, no scope check.
    CHECK(root.get<Undeclared>() == value);
    // Undeclared types are not what check_locals is about.
    CHECK(child.check_locals());

    // Declaring it shared afterwards opens the way up.
    SINGLETON_SCOPE::declare<Undeclared>(SCOPE_SHARED, "Undeclared");
    CHECK(child.get<Undeclared>() == value);

    child.free();
    root.free();
    SINGLETON_SCOPE::reset();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("utils/singletons: remove and free on a child never touch the parent") {
    SINGLETON_SCOPE::reset();
    SINGLETON_SCOPE::declare<SharedThing>(SCOPE_SHARED, "SharedThing");
    SINGLETON_SCOPE::declare<LocalThing>(SCOPE_LOCAL, "LocalThing");
    SINGLETON_SCOPE::declare<Tracked>(SCOPE_LOCAL, "Tracked");
    Tracked::reset_counters();

    Singletons root;
    SharedThing* shared = root.create<SharedThing>();
    LocalThing* root_local = root.create<LocalThing>();
    Tracked* root_tracked = root.create<Tracked>(1);
    Singletons child;
    child.parent = &root;
    child.create<LocalThing>();
    child.create<Tracked>(2);
    CHECK(Tracked::constructed == 2);

    // remove<T>() on the child: its own only, false for what it lacks.
    CHECK_FALSE(child.remove<SharedThing>());
    CHECK(root.get<SharedThing>() == shared);
    CHECK(child.remove<Tracked>());
    CHECK(Tracked::destroyed == 1);
    CHECK(root.get<Tracked>() == root_tracked);
    CHECK(root.get<Tracked>()->value == 1);
    CHECK(child.count() == 1);

    // free() on the child: the parent is intact and the link stays.
    child.free();
    CHECK(child.count() == 0);
    CHECK(child.parent == &root);
    CHECK(root.count() == 3);
    CHECK(root.get<LocalThing>() == root_local);
    CHECK(root.get<Tracked>() == root_tracked);
    CHECK(Tracked::destroyed == 1);
    CHECK(child.get<SharedThing>() == shared);

    // Reusable afterwards.
    CHECK(child.create<LocalThing>() != nullptr);
    child.free();
    root.free();
    CHECK(Tracked::destroyed == 2);
    SINGLETON_SCOPE::reset();
    CHECK_ARENA_CLEAN();
}
