#include "support/test_support.hpp"

#include "engine/utils/hash.hpp"

#include <cstring>

TEST_CASE("utils/hash: fnv1a matches the reference vectors") {
    CHECK(HASH::fnv1a("", 0) == 0xcbf29ce484222325ull);
    CHECK(HASH::fnv1a("a", 1) == 0xaf63dc4c8601ec8cull);
    CHECK(HASH::fnv1a("foobar", 6) == 0x85944171f73967e8ull);
}

TEST_CASE("utils/hash: fnv1a_str equals fnv1a over strlen and is constexpr") {
    constexpr u64 at_compile_time = HASH::fnv1a_str("forward");
    static_assert(at_compile_time != 0, "constexpr evaluation");
    CHECK(at_compile_time == HASH::fnv1a("forward", strlen("forward")));
    CHECK(HASH::fnv1a_str("") == HASH::FNV1A_OFFSET);
    CHECK(HASH::fnv1a_str("forward") != HASH::fnv1a_str("shadow"));
}

TEST_CASE("utils/hash: fnv1a_append chains like one call over the concatenation") {
    const char* whole = "render_graph";
    const u64 one_shot = HASH::fnv1a(whole, strlen(whole));
    u64 chained = HASH::fnv1a("render", 6);
    chained = HASH::fnv1a_append(chained, "_graph", 6);
    CHECK(chained == one_shot);
    CHECK(HASH::fnv1a_str_append(HASH::fnv1a_str("render"), "_graph") == one_shot);
}
