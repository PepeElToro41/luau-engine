#include "support/test_support.hpp"

TEST_CASE("core/smoke: a world can be created and freed") {
    World world;
    world.init();
    EntityId e = world.new_entity();
    CHECK(world.alive(e));
    world.free();
    CHECK_ARENA_CLEAN();
}
