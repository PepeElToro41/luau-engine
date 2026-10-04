#include "support/test_support.hpp"

#include "engine/ecs/component_record.hpp"
#include "engine/ecs/monitor.hpp"
#include "engine/ecs/query_term.hpp"

// Monitors: entities entering and leaving a trivial query's result set,
// driven by the monitor ids each archetype carries.

namespace {

struct MonitorRecord {
    EntityId entity;
    MonitorEvent event;
};

struct MonitorLog {
    DynamicArray<MonitorRecord> events;

    static void record(World* world, const EntityId entity, const MonitorEvent event, void* user_data) {
        (void)world;
        static_cast<MonitorLog*>(user_data)->events.push({ entity, event });
    }

    usz count_of(const MonitorEvent event) const {
        usz n = 0;
        for (const MonitorRecord& e : this->events) {
            if (e.event == event) {
                n += 1;
            }
        }
        return n;
    }

    const MonitorRecord& last() const { return this->events.last(); }

    void free() { this->events.free(); }
};

const DynamicArray<ObserverId>& monitors_of(World& world, const EntityId entity) {
    return world.entity_index.get_record_alive(entity)->archetype->observers.monitors;
}

} // namespace

TEST_CASE("ecs/monitor: enters when the entity starts matching and leaves when it stops") {
    World world;
    world.init();
    MonitorLog log;

    const ObserverId id = world.query<Position>().with<TagA>().monitor(MonitorLog::record, &log);
    REQUIRE(id != 0);

    const EntityId e = world.new_entity();
    world.set(e, Position { 1, 2 });
    CHECK(log.events.count == 0);

    world.add<TagA>(e);
    REQUIRE(log.events.count == 1);
    CHECK(log.last().entity == e);
    CHECK(log.last().event == MONITOR_ENTER);

    // Adding something unrelated changes nothing.
    world.set(e, Velocity { 1, 1 });
    CHECK(log.events.count == 1);

    world.remove<TagA>(e);
    REQUIRE(log.events.count == 2);
    CHECK(log.last().event == MONITOR_LEAVE);

    world.add<TagA>(e);
    REQUIRE(log.events.count == 3);
    CHECK(log.last().event == MONITOR_ENTER);

    world.remove<Position>(e);
    REQUIRE(log.events.count == 4);
    CHECK(log.last().event == MONITOR_LEAVE);

    CHECK(world.unmonitor(id));
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/monitor: without() terms enter on remove and leave on add") {
    World world;
    world.init();
    MonitorLog log;

    const ObserverId id = world.query<Position>().without<TagB>().monitor(MonitorLog::record, &log);
    REQUIRE(id != 0);

    const EntityId e = world.new_entity();
    world.set(e, Position { });
    REQUIRE(log.events.count == 1);
    CHECK(log.last().event == MONITOR_ENTER);

    world.add<TagB>(e);
    REQUIRE(log.events.count == 2);
    CHECK(log.last().event == MONITOR_LEAVE);

    world.remove<TagB>(e);
    REQUIRE(log.events.count == 3);
    CHECK(log.last().event == MONITOR_ENTER);

    world.unmonitor(id);
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

namespace {

struct DataSeen {
    bool enter_saw_data = false;
    bool leave_saw_data = false;
    f32 x_on_enter = 0;
    f32 x_on_leave = 0;
};

void record_data(World* world, const EntityId entity, const MonitorEvent event, void* user_data) {
    DataSeen* seen = static_cast<DataSeen*>(user_data);
    const Position* position = world->get<Position>(entity);
    if (event == MONITOR_ENTER) {
        seen->enter_saw_data = position != nullptr;
        seen->x_on_enter = position != nullptr ? position->x : 0;
    } else {
        seen->leave_saw_data = position != nullptr;
        seen->x_on_leave = position != nullptr ? position->x : 0;
    }
}

} // namespace

TEST_CASE("ecs/monitor: enter sees the data set() wrote and leave still reads it") {
    World world;
    world.init();
    DataSeen seen;

    const ObserverId id = world.query<Position>().monitor(record_data, &seen);
    REQUIRE(id != 0);

    const EntityId e = world.new_entity();
    world.set(e, Position { 7, 0 });
    CHECK(seen.enter_saw_data);
    CHECK(seen.x_on_enter == 7);

    world.remove<Position>(e);
    CHECK(seen.leave_saw_data);
    CHECK(seen.x_on_leave == 7);
    CHECK(world.get<Position>(e) == nullptr);

    SUBCASE("delete_entity leaves with the data readable too") {
        seen = DataSeen { };
        world.set(e, Position { 9, 0 });
        world.delete_entity(e);
        CHECK(seen.leave_saw_data);
        CHECK(seen.x_on_leave == 9);
    }

    world.unmonitor(id);
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/monitor: delete_entity and clear leave every monitor the entity is in") {
    World world;
    world.init();
    MonitorLog positions;
    MonitorLog tagged;

    const ObserverId m1 = world.query<Position>().monitor(MonitorLog::record, &positions);
    const ObserverId m2 = world.query<>().with<TagA>().monitor(MonitorLog::record, &tagged);
    REQUIRE(m1 != 0);
    REQUIRE(m2 != 0);

    SUBCASE("clear") {
        const EntityId e = world.new_entity();
        world.set(e, Position { });
        world.add<TagA>(e);
        CHECK(positions.count_of(MONITOR_ENTER) == 1);
        CHECK(tagged.count_of(MONITOR_ENTER) == 1);

        world.clear(e);
        CHECK(positions.count_of(MONITOR_LEAVE) == 1);
        CHECK(tagged.count_of(MONITOR_LEAVE) == 1);
        CHECK(world.alive(e));

        // Clearing again does nothing: the root is in no monitor.
        world.clear(e);
        CHECK(positions.events.count == 2);
        CHECK(tagged.events.count == 2);
    }

    SUBCASE("delete_entity") {
        const EntityId e = world.new_entity();
        world.set(e, Position { });
        world.add<TagA>(e);
        world.delete_entity(e);
        CHECK(positions.count_of(MONITOR_LEAVE) == 1);
        CHECK(tagged.count_of(MONITOR_LEAVE) == 1);
        CHECK(positions.last().entity == e);
    }

    SUBCASE("deleting an entity parked in the root fires nothing") {
        const EntityId e = world.new_entity();
        world.delete_entity(e);
        CHECK(positions.events.count == 0);
        CHECK(tagged.events.count == 0);
    }

    world.unmonitor(m1);
    world.unmonitor(m2);
    positions.free();
    tagged.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/monitor: an exclusive relation swapping its target leaves and enters") {
    World world;
    world.init();
    MonitorLog log;

    const EntityId parent_a = world.new_entity();
    const EntityId parent_b = world.new_entity();
    const ObserverId id = world.query<>().with(ECS::PAIR(ECS::CHILD_OF, parent_a)).monitor(MonitorLog::record, &log);
    REQUIRE(id != 0);

    const EntityId child = world.new_entity();
    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent_a));
    REQUIRE(log.events.count == 1);
    CHECK(log.last().event == MONITOR_ENTER);

    // CHILD_OF is exclusive: this replaces parent_a, so the child leaves.
    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent_b));
    REQUIRE(log.events.count == 2);
    CHECK(log.last().event == MONITOR_LEAVE);
    CHECK(log.last().entity == child);

    world.add(child, ECS::PAIR(ECS::CHILD_OF, parent_a));
    REQUIRE(log.events.count == 3);
    CHECK(log.last().event == MONITOR_ENTER);

    world.unmonitor(id);
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/monitor: a cascade deleting children makes them leave") {
    World world;
    world.init();
    MonitorLog log;

    const ObserverId id = world.query<>().with(ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)).monitor(MonitorLog::record, &log);
    REQUIRE(id != 0);

    const EntityId parent = world.new_entity();
    const EntityId child_a = world.new_entity();
    const EntityId child_b = world.new_entity();
    world.add(child_a, ECS::PAIR(ECS::CHILD_OF, parent));
    world.add(child_b, ECS::PAIR(ECS::CHILD_OF, parent));
    CHECK(log.count_of(MONITOR_ENTER) == 2);

    world.delete_entity(parent);
    CHECK(log.count_of(MONITOR_LEAVE) == 2);
    CHECK_FALSE(world.alive(child_a));
    CHECK_FALSE(world.alive(child_b));

    world.unmonitor(id);
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/monitor: entities already matching do not enter, but leave later") {
    World world;
    world.init();
    MonitorLog log;

    const EntityId a = world.new_entity();
    const EntityId b = world.new_entity();
    world.set(a, Position { });
    world.set(b, Position { });
    world.set(b, Velocity { });

    const ObserverId id = world.query<Position>().monitor(MonitorLog::record, &log);
    REQUIRE(id != 0);
    CHECK(log.events.count == 0);

    // The existing archetypes were tagged when the monitor was created.
    CHECK(monitors_of(world, a).count == 1);
    CHECK(monitors_of(world, a)[0] == id);
    CHECK(monitors_of(world, b).count == 1);

    world.remove<Position>(a);
    world.remove<Position>(b);
    CHECK(log.count_of(MONITOR_LEAVE) == 2);
    CHECK(log.count_of(MONITOR_ENTER) == 0);

    world.unmonitor(id);
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/monitor: archetype lists hold the ids sorted and unmonitor clears them") {
    World world;
    world.init();
    MonitorLog log;

    const EntityId e = world.new_entity();
    world.set(e, Position { });
    world.set(e, Velocity { });

    const ObserverId m1 = world.query<Position>().monitor(MonitorLog::record, &log);
    const ObserverId m2 = world.query<Velocity>().monitor(MonitorLog::record, &log);
    const ObserverId m3 = world.query<Health>().monitor(MonitorLog::record, &log);
    const ObserverId m4 = world.query<Position, Velocity>().monitor(MonitorLog::record, &log);
    REQUIRE(m1 != 0);
    REQUIRE(m2 != 0);
    REQUIRE(m3 != 0);
    REQUIRE(m4 != 0);
    CHECK(m1 < m2);
    CHECK(m2 < m3);
    CHECK(m3 < m4);

    const DynamicArray<ObserverId>& list = monitors_of(world, e);
    REQUIRE(list.count == 3);
    CHECK(list[0] == m1);
    CHECK(list[1] == m2);
    CHECK(list[2] == m4);
    // The root never carries monitors.
    CHECK(world.root_archetype->observers.monitors.count == 0);

    CHECK(world.unmonitor(m2));
    REQUIRE(list.count == 2);
    CHECK(list[0] == m1);
    CHECK(list[1] == m4);

    // Gone for good: no events, and a second unmonitor says so.
    world.remove<Velocity>(e);
    CHECK(log.count_of(MONITOR_LEAVE) == 1); // only m4
    CHECK(log.last().entity == e);
    CHECK_FALSE(world.unmonitor(m2));
    CHECK_FALSE(world.unmonitor(0));

    // A new archetype created after the monitors picks up the right ones.
    const EntityId f = world.new_entity();
    world.set(f, Health { });
    world.set(f, Position { });
    const DynamicArray<ObserverId>& other = monitors_of(world, f);
    REQUIRE(other.count == 2);
    CHECK(other[0] == m1);
    CHECK(other[1] == m3);

    world.unmonitor(m1);
    world.unmonitor(m3);
    world.unmonitor(m4);
    CHECK(list.count == 0);
    CHECK(other.count == 0);

    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

namespace {

struct SelfDestruct {
    ObserverId own = 0;
    usz calls = 0;
};

void destroy_self(World* world, const EntityId entity, const MonitorEvent event, void* user_data) {
    (void)entity;
    (void)event;
    SelfDestruct* self = static_cast<SelfDestruct*>(user_data);
    self->calls++;
    world->unmonitor(self->own);
}

} // namespace

TEST_CASE("ecs/monitor: a callback may destroy its own monitor") {
    World world;
    world.init();
    SelfDestruct self;
    MonitorLog log;

    // A second monitor on the same query, so the batch being fired has an
    // id removed from under it.
    self.own = world.query<Position>().monitor(destroy_self, &self);
    const ObserverId other = world.query<Position>().monitor(MonitorLog::record, &log);
    REQUIRE(self.own != 0);
    REQUIRE(other != 0);

    const EntityId e = world.new_entity();
    world.set(e, Position { });
    CHECK(self.calls == 1);
    CHECK(log.count_of(MONITOR_ENTER) == 1);

    world.remove<Position>(e);
    CHECK(self.calls == 1);
    CHECK(log.count_of(MONITOR_LEAVE) == 1);
    CHECK_FALSE(world.unmonitor(self.own));

    world.unmonitor(other);
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/monitor: a query with no terms watches every entity with an id") {
    World world;
    world.init();
    MonitorLog log;

    const ObserverId id = world.query<>().monitor(MonitorLog::record, &log);
    REQUIRE(id != 0);

    // Creation parks the entity in the root, which is never in a monitor.
    const EntityId e = world.new_entity();
    CHECK(log.events.count == 0);

    world.add<TagA>(e);
    REQUIRE(log.events.count == 1);
    CHECK(log.last().event == MONITOR_ENTER);

    world.add<TagB>(e);
    CHECK(log.events.count == 1);

    world.clear(e);
    REQUIRE(log.events.count == 2);
    CHECK(log.last().event == MONITOR_LEAVE);

    world.unmonitor(id);
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/monitor: unsupported shapes are refused with 0") {
    World world;
    world.init();
    MonitorLog log;

    QueryTerm term = QueryTerm::make(world.id<Position>(), TERM_OUTPUT);
    CHECK(world.monitor(&term, 1, nullptr, &log) == 0);

    term.flags |= TERM_OPTIONAL;
    CHECK(world.monitor(&term, 1, MonitorLog::record, &log) == 0);

    term = QueryTerm::make(world.id<Position>(), TERM_OUTPUT | TERM_OR);
    CHECK(world.monitor(&term, 1, MonitorLog::record, &log) == 0);

    term = QueryTerm::make(world.id<Position>(), TERM_OUTPUT);
    term.src_var = QueryVar { 1 };
    CHECK(world.monitor(&term, 1, MonitorLog::record, &log) == 0);

    term = QueryTerm::make(0, 0);
    CHECK(world.monitor(&term, 1, MonitorLog::record, &log) == 0);

    // Nothing was registered.
    CHECK(world.monitors.is_empty());
    const EntityId e = world.new_entity();
    world.set(e, Position { });
    CHECK(log.events.count == 0);

    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/monitor: each monitor listens for new archetypes under its first with id") {
    World world;
    world.init();
    MonitorLog log;

    const ObserverId m1 = world.query<Position>().with<TagA>().monitor(MonitorLog::record, &log);
    const ObserverId m2 = world.query<>().without<TagB>().monitor(MonitorLog::record, &log);
    const ObserverId m3 = world.query<>().with(ECS::PAIR(ECS::CHILD_OF, ECS::ANY)).monitor(MonitorLog::record, &log);
    REQUIRE(m1 != 0);
    REQUIRE(m2 != 0);
    REQUIRE(m3 != 0);

    const Monitor* monitor1 = world.monitors.find(m1);
    const Monitor* monitor2 = world.monitors.find(m2);
    const Monitor* monitor3 = world.monitors.find(m3);
    REQUIRE(monitor1 != nullptr);
    REQUIRE(monitor2 != nullptr);
    REQUIRE(monitor3 != nullptr);
    CHECK(*world.archetype_listener_keys.find(monitor1->listener) == world.id<Position>());
    // No with side: every archetype has to be tested.
    CHECK(*world.archetype_listener_keys.find(monitor2->listener) == ECS::WILDCARD);
    CHECK(*world.archetype_listener_keys.find(monitor3->listener) == ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD));

    // Archetypes created after the monitors are tagged through the listeners.
    const EntityId parent = world.new_entity();
    const EntityId e = world.new_entity();
    world.set(e, Position { });
    world.add<TagA>(e);
    world.add(e, ECS::PAIR(ECS::CHILD_OF, parent));
    const DynamicArray<ObserverId>& list = monitors_of(world, e);
    REQUIRE(list.count == 3);
    CHECK(list[0] == m1);
    CHECK(list[1] == m2);
    CHECK(list[2] == m3);
    CHECK(log.count_of(MONITOR_ENTER) == 3);

    const EntityId f = world.new_entity();
    world.add<TagB>(f);
    CHECK(monitors_of(world, f).count == 0);

    // unmonitor takes the listener with it.
    CHECK(world.unmonitor(m1));
    CHECK(world.unmonitor(m2));
    CHECK(world.unmonitor(m3));
    CHECK(world.archetype_listeners.is_empty());
    CHECK(world.archetype_listener_keys.is_empty());

    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/monitor: create and destroy walk the archetypes of the rarest with id") {
    World world;
    world.init();
    MonitorLog log;

    // Many Position tables, Health on one of them, and Health without
    // Position elsewhere: the monitor is narrowed through Health's record.
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

    const ObserverId m = world.query<Position>().with<Health>().monitor(MonitorLog::record, &log);
    REQUIRE(m != 0);
    const Monitor* monitor = world.monitors.find(m);
    REQUIRE(monitor != nullptr);
    CHECK(monitor->record_id == world.id<Health>());

    REQUIRE(monitors_of(world, rare).count == 1);
    CHECK(monitors_of(world, rare)[0] == m);
    CHECK(monitors_of(world, health_only).count == 0);

    // A matching table created after the monitor is tagged by the listener
    // and has to be untagged by destroy, which walks the record afresh.
    const EntityId later = world.new_entity();
    world.set(later, Position { });
    world.set(later, Health { });
    world.add<Likes>(later, rare);
    REQUIRE(monitors_of(world, later).count == 1);
    CHECK(log.count_of(MONITOR_ENTER) == 1);

    CHECK(world.unmonitor(m));
    CHECK(monitors_of(world, rare).count == 0);
    CHECK(monitors_of(world, later).count == 0);
    world.remove<Health>(later);
    CHECK(log.count_of(MONITOR_LEAVE) == 0);

    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/monitor: a with id held by no archetype still monitors, and its record may vanish before destroy") {
    World world;
    world.init();
    MonitorLog log;

    // A plain entity used as a tag id: deleting it deletes its record.
    const EntityId tag = world.new_entity();
    // Position has a record from here on, so `tag` is the id without one.
    const EntityId positioned = world.new_entity();
    world.set(positioned, Position { });

    SUBCASE("no archetype holds the id when the monitor is created") {
        const ObserverId m = world.query<Position>().with(tag).monitor(MonitorLog::record, &log);
        REQUIRE(m != 0);
        CHECK(world.monitors.find(m)->record_id == tag);

        const EntityId e = world.new_entity();
        world.set(e, Position { });
        CHECK(monitors_of(world, e).count == 0);
        world.add(e, tag);
        REQUIRE(monitors_of(world, e).count == 1);
        CHECK(log.count_of(MONITOR_ENTER) == 1);

        CHECK(world.unmonitor(m));
        CHECK(monitors_of(world, e).count == 0);
    }

    SUBCASE("the id's record is deleted before the monitor") {
        const EntityId e = world.new_entity();
        world.set(e, Position { });
        world.add(e, tag);
        const ObserverId m = world.query<Position>().with(tag).monitor(MonitorLog::record, &log);
        REQUIRE(m != 0);
        REQUIRE(monitors_of(world, e).count == 1);

        // Deleting the tag entity moves e off the tag (LEAVE) and drops the
        // tag's record, so destroy finds nothing to walk.
        world.delete_entity(tag);
        CHECK(log.count_of(MONITOR_LEAVE) == 1);
        CHECK(ComponentRecord::component_record_find(&world, tag) == nullptr);
        CHECK(monitors_of(world, e).count == 0);

        CHECK(world.unmonitor(m));
        CHECK_FALSE(world.unmonitor(m));
    }

    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/monitor: World::free releases monitors that were never removed") {
    World world;
    world.init();
    MonitorLog log;

    CHECK(world.query<Position>().monitor(MonitorLog::record, &log) != 0);
    CHECK(world.query<Velocity>().with(ECS::PAIR(ECS::CHILD_OF, ECS::WILDCARD)).monitor(MonitorLog::record, &log) != 0);
    const EntityId e = world.new_entity();
    world.set(e, Position { });
    CHECK(log.count_of(MONITOR_ENTER) == 1);

    world.free();
    CHECK(world.monitors.is_empty());
    log.free();
    CHECK_ARENA_CLEAN();
}
