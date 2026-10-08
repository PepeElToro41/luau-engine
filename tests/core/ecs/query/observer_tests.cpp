#include "support/test_support.hpp"

#include "engine/ecs/component_record.hpp"
#include "engine/ecs/query/observer.hpp"
#include "engine/ecs/query/query_term.hpp"

// Observers: a query's view of an entity changing, either by an archetype
// move involving one of the query's ids or by a write to an output term,
// driven by the observer ids and term entries each archetype carries.

namespace {

struct ObserverRecord {
    EntityId entity;
    ObserverEvent event;
    Id id;
};

struct ObserverLog {
    DynamicArray<ObserverRecord> events;

    static void record(World* world, const EntityId entity, const ObserverEvent event, const Id id, void* user_data) {
        (void)world;
        static_cast<ObserverLog*>(user_data)->events.push({ entity, event, id });
    }

    usz count_of(const ObserverEvent event) const {
        usz n = 0;
        for (const ObserverRecord& e : this->events) {
            if (e.event == event) {
                n += 1;
            }
        }
        return n;
    }

    const ObserverRecord& last() const { return this->events.last(); }

    void free() { this->events.free(); }
};

const ArchetypeObservers& observers_of(World& world, const EntityId entity) {
    return world.entity_index.get_record_alive(entity)->archetype->observers;
}

} // namespace

TEST_CASE("ecs/observer: with() terms report the moves that make the entity match, outputs report writes") {
    World world;
    world.init();
    ObserverLog log;

    // The worked example from observer.hpp: TagA stands in for Alive.
    const ObserverId id = world.query<Position>().with<TagA, Health>().observe(ObserverLog::record, &log);
    REQUIRE(id != 0);

    const EntityId e = world.new_entity();
    world.set(e, Position { 1, 2 });
    CHECK(log.events.count == 0);
    world.add<TagA>(e);
    CHECK(log.events.count == 0);

    // The move that completes the match.
    world.set(e, Health { 50 });
    REQUIRE(log.events.count == 1);
    CHECK(log.last().entity == e);
    CHECK(log.last().event == OBSERVER_MOVED);
    CHECK(log.last().id == world.id<Health>());

    // Health is a with() term: writing it is not a move, so it is silent.
    world.set(e, Health { 100 });
    CHECK(log.events.count == 1);

    // Position is an output: every write reports.
    world.set(e, Position { 3, 4 });
    REQUIRE(log.events.count == 2);
    CHECK(log.last().event == OBSERVER_CHANGED);
    CHECK(log.last().id == world.id<Position>());
    world.set(e, Position { 5, 6 });
    CHECK(log.events.count == 3);
    CHECK(log.last().event == OBSERVER_CHANGED);

    // Leaving the result set reports nothing, and neither do writes after.
    world.remove<TagA>(e);
    CHECK(log.events.count == 3);
    world.set(e, Position { 7, 8 });
    CHECK(log.events.count == 3);

    // Matching again is a move.
    world.add<TagA>(e);
    REQUIRE(log.events.count == 4);
    CHECK(log.last().event == OBSERVER_MOVED);
    CHECK(log.last().id == world.id<TagA>());

    CHECK(world.unobserve(id));
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/observer: a pattern term reports every matching pair that moves while the entity matches") {
    World world;
    world.init();
    ObserverLog log;

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    const ObserverId id = world.query<>().with(world.pair<Likes>(ECS::WILDCARD)).observe(ObserverLog::record, &log);
    REQUIRE(id != 0);

    const EntityId e = world.new_entity();
    world.add<Likes>(e, a);
    REQUIRE(log.events.count == 1);
    CHECK(log.last().event == OBSERVER_MOVED);
    CHECK(log.last().id == world.pair<Likes>(a));

    // Still matching, but a second pair the term matches arrived: a monitor
    // would be silent here.
    world.add<Likes>(e, b);
    REQUIRE(log.events.count == 2);
    CHECK(log.last().id == world.pair<Likes>(b));

    // Still matching through (Likes, b), and one of the query's ids left.
    world.remove<Likes>(e, a);
    REQUIRE(log.events.count == 3);
    CHECK(log.last().event == OBSERVER_MOVED);
    CHECK(log.last().id == world.pair<Likes>(a));

    // Now it stops matching: nothing.
    world.remove<Likes>(e, b);
    CHECK(log.events.count == 3);

    world.unobserve(id);
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/observer: an exclusive relation swapping its target reports the new pair") {
    World world;
    world.init();
    ObserverLog any_parent;
    ObserverLog parent_a_only;
    ObserverLog parent_b_only;

    const EntityId parent_a = world.new_entity();
    const EntityId parent_b = world.new_entity();
    const ObserverId o_any = world.query<>().with(ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)).observe(ObserverLog::record, &any_parent);
    const ObserverId o_a = world.query<>().with(ECS::PAIR(ECS::CHILD_OF, parent_a)).observe(ObserverLog::record, &parent_a_only);
    const ObserverId o_b = world.query<>().with(ECS::PAIR(ECS::CHILD_OF, parent_b)).observe(ObserverLog::record, &parent_b_only);
    REQUIRE(o_any != 0);
    REQUIRE(o_a != 0);
    REQUIRE(o_b != 0);

    const EntityId child = world.new_entity();
    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent_a));
    REQUIRE(any_parent.events.count == 1);
    CHECK(any_parent.last().id == ECS::PAIR(ECS::CHILD_OF, parent_a));
    CHECK(parent_a_only.events.count == 1);
    CHECK(parent_b_only.events.count == 0);

    // CHILD_OF is exclusive: parent_b replaces parent_a in one move. The
    // wildcard observer and the one for parent_b hear about the new pair,
    // once; the one for parent_a lost the entity and hears nothing.
    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent_b));
    REQUIRE(any_parent.events.count == 2);
    CHECK(any_parent.last().event == OBSERVER_MOVED);
    CHECK(any_parent.last().id == ECS::PAIR(ECS::CHILD_OF, parent_b));
    CHECK(parent_a_only.events.count == 1);
    REQUIRE(parent_b_only.events.count == 1);
    CHECK(parent_b_only.last().id == ECS::PAIR(ECS::CHILD_OF, parent_b));

    world.unobserve(o_any);
    world.unobserve(o_a);
    world.unobserve(o_b);
    any_parent.free();
    parent_a_only.free();
    parent_b_only.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/observer: a without() term reports the entity entering when the excluded id goes") {
    World world;
    world.init();
    ObserverLog log;

    const ObserverId id = world.query<Position>().without<TagB>().observe(ObserverLog::record, &log);
    REQUIRE(id != 0);

    const EntityId e = world.new_entity();
    world.set(e, Position { });
    REQUIRE(log.events.count == 1);
    CHECK(log.last().event == OBSERVER_MOVED);
    CHECK(log.last().id == world.id<Position>());

    // Leaving (TagB added) is silent, and so are writes while excluded.
    world.add<TagB>(e);
    CHECK(log.events.count == 1);
    world.set(e, Position { 1, 1 });
    CHECK(log.events.count == 1);

    // Entering again because the excluded id went: reported with that id.
    world.remove<TagB>(e);
    REQUIRE(log.events.count == 2);
    CHECK(log.last().event == OBSERVER_MOVED);
    CHECK(log.last().id == world.id<TagB>());

    world.set(e, Position { 2, 2 });
    REQUIRE(log.events.count == 3);
    CHECK(log.last().event == OBSERVER_CHANGED);

    world.unobserve(id);
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/observer: changes fire only for outputs of a matching entity") {
    World world;
    world.init();
    ObserverLog log;

    const EntityId e = world.new_entity();
    world.set(e, Position { });
    world.set(e, Health { 1 });
    world.add<TagA>(e);

    SUBCASE("set() and modified() on an output") {
        const ObserverId id = world.query<Position>().with<Health, TagA>().observe(ObserverLog::record, &log);
        REQUIRE(id != 0);
        CHECK(log.events.count == 0);

        world.set(e, Position { 1, 1 });
        REQUIRE(log.events.count == 1);
        CHECK(log.last().event == OBSERVER_CHANGED);
        CHECK(log.last().id == world.id<Position>());

        world.get<Position>(e)->x = 5;
        world.modified<Position>(e);
        REQUIRE(log.events.count == 2);
        CHECK(log.last().event == OBSERVER_CHANGED);

        // A with() term's data, a tag already held, and an id the entity
        // does not have are all silent.
        world.set(e, Health { 2 });
        world.modified<Health>(e);
        world.add<TagA>(e);
        world.modified<Velocity>(e);
        CHECK(log.events.count == 2);

        // Not matching: writes to the output are silent.
        world.remove<TagA>(e);
        world.set(e, Position { 2, 2 });
        world.modified<Position>(e);
        CHECK(log.events.count == 2);

        world.unobserve(id);
    }

    SUBCASE("a pair output spelled as a type") {
        const ObserverId id = world.query<ECS::Pair<Position, TagA>>().observe(ObserverLog::record, &log);
        REQUIRE(id != 0);

        world.set<Position, TagA>(e, Position { 1, 1 });
        REQUIRE(log.events.count == 1);
        CHECK(log.last().event == OBSERVER_MOVED);
        CHECK(log.last().id == world.pair<Position, TagA>());

        world.set<Position, TagA>(e, Position { 2, 2 });
        REQUIRE(log.events.count == 2);
        CHECK(log.last().event == OBSERVER_CHANGED);
        CHECK(log.last().id == world.pair<Position, TagA>());

        // Plain Position is not the pair.
        world.set(e, Position { 3, 3 });
        CHECK(log.events.count == 2);

        world.unobserve(id);
    }

    SUBCASE("a pattern output resolves to the concrete pair written") {
        const EntityId a = world.new_entity();
        const EntityId b = world.new_entity();
        QueryTerm term = QueryTerm::make(world.pair<Position>(ECS::WILDCARD), TERM_OUTPUT);
        const ObserverId id = world.observe(&term, 1, ObserverLog::record, &log);
        REQUIRE(id != 0);

        world.set<Position>(e, a, Position { 1, 1 });
        REQUIRE(log.events.count == 1);
        CHECK(log.last().event == OBSERVER_MOVED);
        CHECK(log.last().id == world.pair<Position>(a));

        world.set<Position>(e, b, Position { 1, 1 });
        REQUIRE(log.events.count == 2);
        CHECK(log.last().event == OBSERVER_MOVED);
        CHECK(log.last().id == world.pair<Position>(b));

        world.set<Position>(e, a, Position { 2, 2 });
        REQUIRE(log.events.count == 3);
        CHECK(log.last().event == OBSERVER_CHANGED);
        CHECK(log.last().id == world.pair<Position>(a));

        world.set<Position>(e, b, Position { 2, 2 });
        REQUIRE(log.events.count == 4);
        CHECK(log.last().event == OBSERVER_CHANGED);
        CHECK(log.last().id == world.pair<Position>(b));

        world.unobserve(id);
    }

    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/observer: removals, clear, delete_entity and cascades report nothing") {
    World world;
    world.init();
    ObserverLog log;
    HookLog hooks;

    const ObserverId id = world.query<Position>().with<TagA>().observe(ObserverLog::record, &log);
    const HookId removed = world.hook_removed<Position>(HookLog::record_removed, &hooks);
    REQUIRE(id != 0);
    REQUIRE(removed != 0);

    const EntityId e = world.new_entity();
    world.set(e, Position { });
    world.add<TagA>(e);
    REQUIRE(log.events.count == 1);

    SUBCASE("remove") {
        world.remove<TagA>(e);
        CHECK(log.events.count == 1);
        world.remove<Position>(e);
        CHECK(log.events.count == 1);
        CHECK(hooks.count_of(HOOK_REMOVED) == 1);
    }

    SUBCASE("clear") {
        world.clear(e);
        CHECK(log.events.count == 1);
        CHECK(hooks.count_of(HOOK_REMOVED) == 1);
        CHECK(world.alive(e));
    }

    SUBCASE("delete_entity") {
        world.delete_entity(e);
        CHECK(log.events.count == 1);
        CHECK(hooks.count_of(HOOK_REMOVED) == 1);
    }

    SUBCASE("a cascade deleting children") {
        const ObserverId children = world.query<>().with(ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)).observe(ObserverLog::record, &log);
        REQUIRE(children != 0);
        const EntityId parent = world.new_entity();
        const EntityId child = world.new_entity();
        world.add(child, ECS::PAIR(ECS::CHILD_OF, parent));
        CHECK(log.events.count == 2);
        world.delete_entity(parent);
        CHECK(log.events.count == 2);
        CHECK_FALSE(world.alive(child));
        world.unobserve(children);
    }

    world.unobserve(id);
    world.unhook(world.id<Position>(), removed);
    log.free();
    hooks.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/observer: moves that touch no term of the query are silent") {
    World world;
    world.init();
    ObserverLog log;

    const ObserverId id = world.query<Position>().with<TagA>().observe(ObserverLog::record, &log);
    REQUIRE(id != 0);

    const EntityId e = world.new_entity();
    world.set(e, Position { });
    world.add<TagA>(e);
    REQUIRE(log.events.count == 1);

    world.set(e, Velocity { });
    world.add<TagB>(e);
    world.add<Likes>(e, e);
    world.set(e, Velocity { 1, 1 });
    world.remove<TagB>(e);
    world.remove<Velocity>(e);
    world.remove<Likes>(e, e);
    CHECK(log.events.count == 1);

    world.unobserve(id);
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

namespace {

struct DataSeen {
    f32 x_on_moved = 0;
    f32 x_on_changed = 0;
    bool moved_saw_data = false;
};

void record_data(World* world, const EntityId entity, const ObserverEvent event, const Id id, void* user_data) {
    (void)id;
    DataSeen* seen = static_cast<DataSeen*>(user_data);
    const Position* position = world->get<Position>(entity);
    if (event == OBSERVER_MOVED) {
        seen->moved_saw_data = position != nullptr;
        seen->x_on_moved = position != nullptr ? position->x : 0;
    } else {
        seen->x_on_changed = position != nullptr ? position->x : 0;
    }
}

} // namespace

TEST_CASE("ecs/observer: MOVED and CHANGED see the data set() wrote") {
    World world;
    world.init();
    DataSeen seen;

    const ObserverId id = world.query<Position>().observe(record_data, &seen);
    REQUIRE(id != 0);

    const EntityId e = world.new_entity();
    world.set(e, Position { 7, 0 });
    CHECK(seen.moved_saw_data);
    CHECK(seen.x_on_moved == 7);

    world.set(e, Position { 9, 0 });
    CHECK(seen.x_on_changed == 9);

    world.unobserve(id);
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/observer: entities already matching fire nothing at creation but report later") {
    World world;
    world.init();
    ObserverLog log;

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    world.set(a, Position { });
    world.set(b, Position { });
    world.set(b, Velocity { });

    const ObserverId id = world.query<Position>().observe(ObserverLog::record, &log);
    REQUIRE(id != 0);
    CHECK(log.events.count == 0);

    // The existing archetypes were tagged when the observer was created.
    REQUIRE(observers_of(world, a).observers.count == 1);
    CHECK(observers_of(world, a).observers[0] == id);
    REQUIRE(observers_of(world, a).observer_terms.count == 1);
    CHECK(observers_of(world, a).observer_terms[0].id == world.id<Position>());
    CHECK(observers_of(world, a).observer_terms[0].observer == id);
    CHECK(observers_of(world, a).observer_terms[0].output);
    CHECK(observers_of(world, b).observers.count == 1);

    world.set(a, Position { 1, 1 });
    world.set(b, Position { 1, 1 });
    CHECK(log.count_of(OBSERVER_CHANGED) == 2);
    CHECK(log.count_of(OBSERVER_MOVED) == 0);

    world.unobserve(id);
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/observer: archetype lists hold the ids and term entries sorted and unobserve clears them") {
    World world;
    world.init();
    ObserverLog log;

    const EntityId e = world.new_entity();
    world.set(e, Position { });
    world.set(e, Velocity { });
    world.add<TagA>(e);

    const ObserverId o1 = world.query<Position>().observe(ObserverLog::record, &log);
    const ObserverId o2 = world.query<Velocity>().with<Position>().observe(ObserverLog::record, &log);
    const ObserverId o3 = world.query<Health>().observe(ObserverLog::record, &log);
    const ObserverId o4 = world.query<>().with<TagA>().without<TagB>().observe(ObserverLog::record, &log);
    REQUIRE(o1 != 0);
    REQUIRE(o2 != 0);
    REQUIRE(o3 != 0);
    REQUIRE(o4 != 0);
    CHECK(o1 < o2);
    CHECK(o2 < o3);
    CHECK(o3 < o4);

    const ArchetypeObservers& lists = observers_of(world, e);
    REQUIRE(lists.observers.count == 3);
    CHECK(lists.observers[0] == o1);
    CHECK(lists.observers[1] == o2);
    CHECK(lists.observers[2] == o4);
    // Position < Velocity < TagA (ids are claimed in registration order).
    REQUIRE(lists.observer_terms.count == 4);
    CHECK(lists.observer_terms[0].id == world.id<Position>());
    CHECK(lists.observer_terms[0].observer == o1);
    CHECK(lists.observer_terms[0].output);
    CHECK(lists.observer_terms[1].id == world.id<Position>());
    CHECK(lists.observer_terms[1].observer == o2);
    CHECK_FALSE(lists.observer_terms[1].output);
    CHECK(lists.observer_terms[2].id == world.id<Velocity>());
    CHECK(lists.observer_terms[2].observer == o2);
    CHECK(lists.observer_terms[2].output);
    CHECK(lists.observer_terms[3].id == world.id<TagA>());
    CHECK(lists.observer_terms[3].observer == o4);
    CHECK_FALSE(lists.observer_terms[3].output);
    // The root never carries observers.
    CHECK(world.root_archetype->observers.observers.count == 0);
    CHECK(world.root_archetype->observers.observer_terms.count == 0);

    // Position is an output for o1 and a with() for o2: one CHANGED.
    world.set(e, Position { 1, 1 });
    CHECK(log.events.count == 1);
    CHECK(log.last().id == world.id<Position>());
    world.set(e, Velocity { 1, 1 });
    CHECK(log.events.count == 2);
    CHECK(log.last().id == world.id<Velocity>());

    CHECK(world.unobserve(o2));
    REQUIRE(lists.observers.count == 2);
    CHECK(lists.observers[0] == o1);
    CHECK(lists.observers[1] == o4);
    REQUIRE(lists.observer_terms.count == 2);
    CHECK(lists.observer_terms[0].observer == o1);
    CHECK(lists.observer_terms[1].observer == o4);

    // Gone for good: no events, and a second unobserve says so.
    world.set(e, Velocity { 2, 2 });
    CHECK(log.events.count == 2);
    CHECK_FALSE(world.unobserve(o2));
    CHECK_FALSE(world.unobserve(0));

    // A new archetype created after the observers picks up the right ones
    // through their listeners, keyed under the first with id.
    CHECK(*world.archetype_listener_keys.find(world.observers.find(o1)->listener) == world.id<Position>());
    CHECK(*world.archetype_listener_keys.find(world.observers.find(o4)->listener) == world.id<TagA>());
    const EntityId f = world.new_entity();
    world.set(f, Health { });
    world.set(f, Position { });
    const ArchetypeObservers& other = observers_of(world, f);
    REQUIRE(other.observers.count == 2);
    CHECK(other.observers[0] == o1);
    CHECK(other.observers[1] == o3);
    REQUIRE(other.observer_terms.count == 2);
    CHECK(other.observer_terms[0].id == world.id<Position>());
    CHECK(other.observer_terms[0].observer == o1);
    CHECK(other.observer_terms[1].id == world.id<Health>());
    CHECK(other.observer_terms[1].observer == o3);
    CHECK(log.count_of(OBSERVER_MOVED) == 2);

    world.unobserve(o1);
    world.unobserve(o3);
    world.unobserve(o4);
    CHECK(lists.observers.count == 0);
    CHECK(lists.observer_terms.count == 0);
    CHECK(other.observers.count == 0);
    CHECK(other.observer_terms.count == 0);
    CHECK(world.archetype_listeners.is_empty());

    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

namespace {

struct SelfDestruct {
    ObserverId own = 0;
    ObserverId spawned = 0;
    ObserverLog* log = nullptr;
    usz calls = 0;
};

void destroy_self(World* world, const EntityId entity, const ObserverEvent event, const Id id, void* user_data) {
    (void)entity;
    (void)event;
    (void)id;
    SelfDestruct* self = static_cast<SelfDestruct*>(user_data);
    self->calls++;
    world->unobserve(self->own);
    // Registering from inside a callback edits the lists being fired from;
    // the new observer only hears about later events.
    self->spawned = world->query<Position>().observe(ObserverLog::record, self->log);
}

} // namespace

TEST_CASE("ecs/observer: a callback may destroy its own observer and create another") {
    World world;
    world.init();
    SelfDestruct self;
    ObserverLog log;
    self.log = &log;

    // A second observer on the same query, so the batch being fired has an
    // id removed from under it.
    self.own = world.query<Position>().observe(destroy_self, &self);
    const ObserverId other = world.query<Position>().observe(ObserverLog::record, &log);
    REQUIRE(self.own != 0);
    REQUIRE(other != 0);

    const EntityId e = world.new_entity();
    world.set(e, Position { });
    CHECK(self.calls == 1);
    CHECK(log.count_of(OBSERVER_MOVED) == 1);
    REQUIRE(self.spawned != 0);
    CHECK_FALSE(world.unobserve(self.own));

    // self is gone, both loggers hear the change.
    world.set(e, Position { 1, 1 });
    CHECK(self.calls == 1);
    CHECK(log.count_of(OBSERVER_CHANGED) == 2);

    world.unobserve(other);
    world.unobserve(self.spawned);
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/observer: a query with no terms reports an entity picking up its first id") {
    World world;
    world.init();
    ObserverLog log;

    const ObserverId id = world.query<>().observe(ObserverLog::record, &log);
    REQUIRE(id != 0);

    // Creation parks the entity in the root, which is in no observer.
    const EntityId e = world.new_entity();
    CHECK(log.events.count == 0);

    world.add<TagA>(e);
    REQUIRE(log.events.count == 1);
    CHECK(log.last().event == OBSERVER_MOVED);
    CHECK(log.last().id == world.id<TagA>());

    // No term matches anything else, and the entity already matched.
    world.add<TagB>(e);
    world.remove<TagB>(e);
    CHECK(log.events.count == 1);

    world.clear(e);
    CHECK(log.events.count == 1);

    world.unobserve(id);
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/observer: unsupported shapes are refused with 0") {
    World world;
    world.init();
    ObserverLog log;

    QueryTerm term = QueryTerm::make(world.id<Position>(), TERM_OUTPUT);
    CHECK(world.observe(&term, 1, nullptr, &log) == 0);

    term.flags |= TERM_OPTIONAL;
    CHECK(world.observe(&term, 1, ObserverLog::record, &log) == 0);

    term = QueryTerm::make(world.id<Position>(), TERM_OUTPUT | TERM_OR);
    CHECK(world.observe(&term, 1, ObserverLog::record, &log) == 0);

    term = QueryTerm::make(world.id<Position>(), TERM_OUTPUT);
    term.src_var = QueryVar { 1 };
    CHECK(world.observe(&term, 1, ObserverLog::record, &log) == 0);

    term = QueryTerm::make(0, 0);
    CHECK(world.observe(&term, 1, ObserverLog::record, &log) == 0);

    // Nothing was registered.
    CHECK(world.observers.is_empty());
    CHECK(world.archetype_listeners.is_empty());
    const EntityId e = world.new_entity();
    world.set(e, Position { });
    CHECK(log.events.count == 0);

    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/observer: monitors and observers share the id counter and fire side by side") {
    World world;
    world.init();
    ObserverLog log;
    usz enters = 0;

    const ObserverId observer = world.query<Position>().observe(ObserverLog::record, &log);
    const ObserverId monitor = world.query<Position>().monitor(
        [](World*, EntityId, MonitorEvent event, void* user_data) {
            if (event == MONITOR_ENTER) {
                (*static_cast<usz*>(user_data))++;
            }
        }, &enters);
    REQUIRE(observer != 0);
    REQUIRE(monitor != 0);
    CHECK(observer != monitor);
    CHECK(world.monitors.find(observer) == nullptr);
    CHECK(world.observers.find(monitor) == nullptr);
    CHECK_FALSE(world.unmonitor(observer));
    CHECK_FALSE(world.unobserve(monitor));

    const EntityId e = world.new_entity();
    world.set(e, Position { });
    CHECK(log.count_of(OBSERVER_MOVED) == 1);
    CHECK(enters == 1);
    const ArchetypeObservers& lists = observers_of(world, e);
    REQUIRE(lists.monitors.count == 1);
    CHECK(lists.monitors[0] == monitor);
    REQUIRE(lists.observers.count == 1);
    CHECK(lists.observers[0] == observer);

    CHECK(world.unobserve(observer));
    CHECK(world.unmonitor(monitor));
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/observer: create and destroy walk the archetypes of the rarest with id") {
    World world;
    world.init();
    ObserverLog log;

    EntityId rare = 0;
    for (usz i = 0; i < 8; i++) {
        const EntityId e = world.new_entity();
        world.set(e, Position { });
        if (i & 1) world.add<TagA>(e);
        if (i & 2) world.add<TagB>(e);
        if (i == 5) {
            world.set(e, Health { });
            rare = e;
        }
    }
    const EntityId health_only = world.new_entity();
    world.set(health_only, Health { });

    const ObserverId id = world.query<Position>().with<Health>().observe(ObserverLog::record, &log);
    REQUIRE(id != 0);
    CHECK(world.observers.find(id)->record_id == world.id<Health>());
    REQUIRE(observers_of(world, rare).observers.count == 1);
    CHECK(observers_of(world, health_only).observers.count == 0);

    // A matching table created after the observer is tagged by the listener
    // and untagged by destroy, which walks the record afresh.
    const EntityId later = world.new_entity();
    world.set(later, Position { });
    world.set(later, Health { });
    world.add<Likes>(later, rare);
    REQUIRE(observers_of(world, later).observers.count == 1);
    CHECK(log.count_of(OBSERVER_MOVED) == 1);

    CHECK(world.unobserve(id));
    CHECK(observers_of(world, rare).observers.count == 0);
    CHECK(observers_of(world, later).observers.count == 0);
    CHECK(observers_of(world, later).observer_terms.count == 0);
    world.set(later, Position { 1, 1 });
    CHECK(log.events.count == 1);

    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/observer: World::free releases observers that were never removed") {
    World world;
    world.init();
    ObserverLog log;

    CHECK(world.query<Position>().observe(ObserverLog::record, &log) != 0);
    CHECK(world.query<Velocity>().with(ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)).observe(ObserverLog::record, &log) != 0);
    const EntityId e = world.new_entity();
    world.set(e, Position { });
    CHECK(log.count_of(OBSERVER_MOVED) == 1);

    world.free();
    CHECK(world.observers.is_empty());
    log.free();
    CHECK_ARENA_CLEAN();
}
