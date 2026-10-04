#include "support/test_support.hpp"

#include "engine/ecs/archetype_listener.hpp"

// Archetype lifecycle listeners: callbacks keyed by id that fire when an
// archetype covering that id is created or destroyed.

namespace {

struct ArchetypeRecord {
    ArchetypeId archetype;
    ArchetypeEvent event;
    usz id_count;
    usz entity_count;
    // The archetype's slot was alive and pointed at the archetype when the
    // callback ran.
    bool slot_alive;
};

struct ArchetypeLog {
    DynamicArray<ArchetypeRecord> events;

    static void record(World* world, Archetype* archetype, const ArchetypeEvent event, void* user_data) {
        ArchetypeRecord entry;
        entry.archetype = archetype->archetype_id;
        entry.event = event;
        entry.id_count = archetype->type.id_count;
        entry.entity_count = archetype->data.entity_count;
        entry.slot_alive = world->archetypes.get_element_alive(archetype->archetype_id) == archetype;
        static_cast<ArchetypeLog*>(user_data)->events.push(entry);
    }

    usz count_of(const ArchetypeEvent event) const {
        usz n = 0;
        for (const ArchetypeRecord& e : this->events) {
            if (e.event == event) {
                n += 1;
            }
        }
        return n;
    }

    // How many times `archetype` was reported with `event`.
    usz count_for(const ArchetypeId archetype, const ArchetypeEvent event) const {
        usz n = 0;
        for (const ArchetypeRecord& e : this->events) {
            if (e.archetype == archetype && e.event == event) {
                n += 1;
            }
        }
        return n;
    }

    const ArchetypeRecord& last() const { return this->events.last(); }

    void free() { this->events.free(); }
};

ArchetypeId archetype_of(World& world, const EntityId entity) {
    return world.entity_index.get_record_alive(entity)->archetype->archetype_id;
}

} // namespace

TEST_CASE("ecs/archetype_listener: created fires only for archetypes holding the key") {
    World world;
    world.init();
    ArchetypeLog log;

    const ArchetypeListenerId id = ARCHETYPE_LISTENER::add(&world, world.id<Position>(), ArchetypeLog::record, &log);
    REQUIRE(id != 0);

    // [Velocity] has no Position: nothing.
    const EntityId e = world.new_entity();
    world.set(e, Velocity { });
    CHECK(log.events.count == 0);

    // [Velocity, Position] is new and holds the key.
    world.set(e, Position { });
    REQUIRE(log.events.count == 1);
    CHECK(log.last().event == ARCHETYPE_CREATED);
    CHECK(log.last().archetype == archetype_of(world, e));
    CHECK(log.last().id_count == 2);
    // Fired before any row was moved in.
    CHECK(log.last().entity_count == 0);
    CHECK(log.last().slot_alive);

    // A second entity taking the same path reuses the archetypes: nothing.
    const EntityId f = world.new_entity();
    world.set(f, Velocity { });
    world.set(f, Position { });
    CHECK(log.events.count == 1);

    // [Velocity, Position, Health] is new and still holds Position.
    world.set(f, Health { });
    CHECK(log.events.count == 2);
    CHECK(log.last().archetype == archetype_of(world, f));

    // Removing Position lands in [Velocity, Health], which lacks the key.
    world.remove<Position>(f);
    CHECK(log.events.count == 2);

    CHECK(ARCHETYPE_LISTENER::remove(&world, id));
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype_listener: pair keys fire for their patterns once per archetype") {
    World world;
    world.init();
    ArchetypeLog likes_any;
    ArchetypeLog any_apple;
    ArchetypeLog any_pair;
    ArchetypeLog exact;

    const EntityId apple = world.new_entity();
    const EntityId pear = world.new_entity();
    const Id likes = world.id<Likes>();
    const Id eats = world.id<Eats>();

    const ArchetypeListenerId l1 = ARCHETYPE_LISTENER::add(&world, ECS::PAIR(likes, ECS::WILDCARD), ArchetypeLog::record, &likes_any);
    const ArchetypeListenerId l2 = ARCHETYPE_LISTENER::add(&world, ECS::PAIR(ECS::WILDCARD, apple), ArchetypeLog::record, &any_apple);
    const ArchetypeListenerId l3 = ARCHETYPE_LISTENER::add(&world, ECS::PAIR(ECS::WILDCARD, ECS::WILDCARD), ArchetypeLog::record, &any_pair);
    const ArchetypeListenerId l4 = ARCHETYPE_LISTENER::add(&world, ECS::PAIR(likes, apple), ArchetypeLog::record, &exact);
    REQUIRE(l1 != 0);
    REQUIRE(l2 != 0);
    REQUIRE(l3 != 0);
    REQUIRE(l4 != 0);

    // [Position]: no pair at all.
    const EntityId e = world.new_entity();
    world.set(e, Position { });
    CHECK(likes_any.events.count == 0);
    CHECK(any_apple.events.count == 0);
    CHECK(any_pair.events.count == 0);
    CHECK(exact.events.count == 0);

    // [Position, (Likes, apple)]: every key matches.
    world.add(e, ECS::PAIR(likes, apple));
    CHECK(likes_any.events.count == 1);
    CHECK(any_apple.events.count == 1);
    CHECK(any_pair.events.count == 1);
    CHECK(exact.events.count == 1);

    // [Position, (Likes, apple), (Likes, pear)]: two (Likes, *) ids, but the
    // archetype is reported once per listener.
    world.add(e, ECS::PAIR(likes, pear));
    CHECK(likes_any.events.count == 2);
    CHECK(likes_any.count_for(archetype_of(world, e), ARCHETYPE_CREATED) == 1);
    CHECK(any_apple.events.count == 2);
    CHECK(any_pair.events.count == 2);
    CHECK(any_pair.count_for(archetype_of(world, e), ARCHETYPE_CREATED) == 1);
    CHECK(exact.events.count == 2);

    // [..., (Eats, apple)]: two (*, apple) ids, still once.
    world.add(e, ECS::PAIR(eats, apple));
    CHECK(any_apple.events.count == 3);
    CHECK(any_apple.count_for(archetype_of(world, e), ARCHETYPE_CREATED) == 1);
    CHECK(likes_any.events.count == 3);

    // [(Eats, pear)]: only the (*, *) key.
    const EntityId f = world.new_entity();
    world.add(f, ECS::PAIR(eats, pear));
    CHECK(any_pair.events.count == 4);
    CHECK(likes_any.events.count == 3);
    CHECK(any_apple.events.count == 3);
    CHECK(exact.events.count == 3);

    CHECK(ARCHETYPE_LISTENER::remove(&world, l1));
    CHECK(ARCHETYPE_LISTENER::remove(&world, l2));
    CHECK(ARCHETYPE_LISTENER::remove(&world, l3));
    CHECK(ARCHETYPE_LISTENER::remove(&world, l4));
    likes_any.free();
    any_apple.free();
    any_pair.free();
    exact.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype_listener: WILDCARD and ANY listen to every archetype") {
    World world;
    world.init();
    ArchetypeLog wildcard;
    ArchetypeLog any;

    const ArchetypeListenerId l1 = ARCHETYPE_LISTENER::add(&world, ECS::WILDCARD, ArchetypeLog::record, &wildcard);
    const ArchetypeListenerId l2 = ARCHETYPE_LISTENER::add(&world, ECS::ANY, ArchetypeLog::record, &any);
    REQUIRE(l1 != 0);
    REQUIRE(l2 != 0);
    // ANY is folded to WILDCARD: both sit under the same key.
    CHECK(*world.archetype_listener_keys.find(l1) == ECS::WILDCARD);
    CHECK(*world.archetype_listener_keys.find(l2) == ECS::WILDCARD);
    CHECK(world.archetype_listeners.count == 1);

    // Register the types first so claiming them is not counted below.
    const Id likes = world.id<Likes>();
    world.id<Position>();
    world.id<TagA>();
    const usz before = world.archetypes.alive_count;
    const EntityId e = world.new_entity();
    world.set(e, Position { });
    world.add<TagA>(e);
    world.add(e, ECS::PAIR(likes, e));
    const usz created = world.archetypes.alive_count - before;
    CHECK(created == 3);
    CHECK(wildcard.count_of(ARCHETYPE_CREATED) == created);
    CHECK(any.count_of(ARCHETYPE_CREATED) == created);

    // Reusing archetypes creates nothing.
    const EntityId f = world.new_entity();
    world.set(f, Position { });
    CHECK(wildcard.count_of(ARCHETYPE_CREATED) == created);

    CHECK(ARCHETYPE_LISTENER::remove(&world, l1));
    CHECK(ARCHETYPE_LISTENER::remove(&world, l2));
    CHECK(world.archetype_listeners.is_empty());
    wildcard.free();
    any.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype_listener: key_for picks the first with id or WILDCARD") {
    const Id a = 5;
    const Id b = 9;
    const Id likes = 12;

    const Id plain[] = { a, b };
    CHECK(ARCHETYPE_LISTENER::key_for(plain, 2) == a);

    // Leading zeros (ids that failed to register) are skipped.
    const Id with_zero[] = { 0, b };
    CHECK(ARCHETYPE_LISTENER::key_for(with_zero, 2) == b);

    // ANY sides fold to WILDCARD so they land on the same list as WILDCARD.
    const Id any_pair[] = { ECS::PAIR(likes, ECS::ANY) };
    CHECK(ARCHETYPE_LISTENER::key_for(any_pair, 1) == ECS::PAIR(likes, ECS::WILDCARD));
    const Id any[] = { ECS::ANY };
    CHECK(ARCHETYPE_LISTENER::key_for(any, 1) == ECS::WILDCARD);

    // No with side at all: every archetype.
    CHECK(ARCHETYPE_LISTENER::key_for(nullptr, 0) == ECS::WILDCARD);
    const Id zeros[] = { 0, 0 };
    CHECK(ARCHETYPE_LISTENER::key_for(zeros, 2) == ECS::WILDCARD);
}

TEST_CASE("ecs/archetype_listener: destroyed fires with the archetype still intact and empty") {
    World world;
    world.init();
    ArchetypeLog log;

    // Deleting a tag entity empties and destroys every archetype holding it.
    const EntityId tag = world.new_entity();
    const ArchetypeListenerId id = ARCHETYPE_LISTENER::add(&world, tag, ArchetypeLog::record, &log);
    REQUIRE(id != 0);

    const EntityId e1 = world.new_entity();
    const EntityId e2 = world.new_entity();
    world.add(e1, tag);
    world.add(e2, tag);
    world.set(e2, Position { });
    CHECK(log.count_of(ARCHETYPE_CREATED) == 2);
    const ArchetypeId a1 = archetype_of(world, e1);
    const ArchetypeId a2 = archetype_of(world, e2);

    // Both tag archetypes go; e2 lands in a new [Position], so the net is one.
    const usz before = world.archetypes.alive_count;
    CHECK(world.delete_entity(tag));
    CHECK(world.archetypes.alive_count == before - 1);
    CHECK(archetype_of(world, e2) != a2);

    CHECK(log.count_of(ARCHETYPE_DESTROYED) == 2);
    CHECK(log.count_for(a1, ARCHETYPE_DESTROYED) == 1);
    CHECK(log.count_for(a2, ARCHETYPE_DESTROYED) == 1);
    for (const ArchetypeRecord& entry : log.events) {
        if (entry.event != ARCHETYPE_DESTROYED) {
            continue;
        }
        // Rows were moved out before the destroy, but the type and slot
        // still stand when the listener runs.
        CHECK(entry.entity_count == 0);
        CHECK(entry.id_count > 0);
        CHECK(entry.slot_alive);
    }

    // [Position] (where e2 ended up) did not hold the tag: not reported.
    CHECK(log.count_for(archetype_of(world, e2), ARCHETYPE_DESTROYED) == 0);

    CHECK(ARCHETYPE_LISTENER::remove(&world, id));
    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

namespace {

struct Reentrant {
    ArchetypeListenerId own = 0;
    ArchetypeListenerId added = 0;
    usz calls = 0;
    ArchetypeLog* late_log = nullptr;
    Id key = 0;
};

// Removes itself and registers another listener under the same key.
void remove_self_and_add(World* world, Archetype* archetype, const ArchetypeEvent event, void* user_data) {
    (void)archetype;
    (void)event;
    Reentrant* self = static_cast<Reentrant*>(user_data);
    self->calls++;
    ARCHETYPE_LISTENER::remove(world, self->own);
    self->added = ARCHETYPE_LISTENER::add(world, self->key, ArchetypeLog::record, self->late_log);
}

} // namespace

TEST_CASE("ecs/archetype_listener: a callback may remove itself and add listeners") {
    World world;
    world.init();
    ArchetypeLog other;
    ArchetypeLog late;
    Reentrant self;
    self.late_log = &late;
    self.key = world.id<Position>();

    // The self-removing listener is first in its list; `other` follows it,
    // so the walk continues past a removed entry.
    self.own = ARCHETYPE_LISTENER::add(&world, self.key, remove_self_and_add, &self);
    const ArchetypeListenerId other_id = ARCHETYPE_LISTENER::add(&world, self.key, ArchetypeLog::record, &other);
    REQUIRE(self.own != 0);
    REQUIRE(other_id != 0);

    const EntityId e = world.new_entity();
    world.set(e, Position { });
    CHECK(self.calls == 1);
    CHECK(other.events.count == 1);
    // The listener added mid-walk does not see the archetype that was firing.
    CHECK(late.events.count == 0);
    CHECK(self.added != 0);
    CHECK_FALSE(ARCHETYPE_LISTENER::remove(&world, self.own));

    // From now on only `other` and the late listener run.
    world.set(e, Velocity { });
    CHECK(self.calls == 1);
    CHECK(other.events.count == 2);
    CHECK(late.events.count == 1);

    CHECK(ARCHETYPE_LISTENER::remove(&world, other_id));
    CHECK(ARCHETYPE_LISTENER::remove(&world, self.added));
    CHECK(world.archetype_listeners.is_empty());
    CHECK(world.archetype_listener_keys.is_empty());
    other.free();
    late.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype_listener: bad registrations are refused and unknown ids are not removed") {
    World world;
    world.init();
    ArchetypeLog log;

    CHECK(ARCHETYPE_LISTENER::add(&world, world.id<Position>(), nullptr, &log) == 0);
    CHECK(ARCHETYPE_LISTENER::add(&world, 0, ArchetypeLog::record, &log) == 0);
    CHECK(world.archetype_listeners.is_empty());
    CHECK(world.archetype_listener_keys.is_empty());

    CHECK_FALSE(ARCHETYPE_LISTENER::remove(&world, 0));
    CHECK_FALSE(ARCHETYPE_LISTENER::remove(&world, 42));

    const ArchetypeListenerId id = ARCHETYPE_LISTENER::add(&world, world.id<Position>(), ArchetypeLog::record, &log);
    REQUIRE(id != 0);
    CHECK(ARCHETYPE_LISTENER::remove(&world, id));
    CHECK_FALSE(ARCHETYPE_LISTENER::remove(&world, id));

    // Nothing left behind to fire.
    const EntityId e = world.new_entity();
    world.set(e, Position { });
    CHECK(log.events.count == 0);

    log.free();
    world.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("ecs/archetype_listener: World::free releases listeners that were never removed") {
    World world;
    world.init();
    ArchetypeLog log;

    CHECK(ARCHETYPE_LISTENER::add(&world, world.id<Position>(), ArchetypeLog::record, &log) != 0);
    CHECK(ARCHETYPE_LISTENER::add(&world, ECS::WILDCARD, ArchetypeLog::record, &log) != 0);
    const EntityId e = world.new_entity();
    world.set(e, Position { });
    CHECK(log.count_of(ARCHETYPE_CREATED) == 2);

    // Tearing the world down fires nothing for the archetypes that go with it.
    world.free();
    CHECK(log.count_of(ARCHETYPE_DESTROYED) == 0);
    CHECK(world.archetype_listeners.is_empty());
    CHECK(world.archetype_listener_keys.is_empty());
    log.free();
    CHECK_ARENA_CLEAN();
}
