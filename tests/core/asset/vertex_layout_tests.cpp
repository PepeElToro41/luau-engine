#include "support/test_support.hpp"

#include "engine/asset/asset_types/mesh_asset.hpp"

#include <cstring>

TEST_CASE("asset/vertex_layout: the location table is fixed and constexpr") {
    static_assert(VERTEX_LAYOUT::location(VERTEX_SEMANTIC_POSITION, 0) == 0, "position");
    CHECK(VERTEX_LAYOUT::location(VERTEX_SEMANTIC_NORMAL, 0) == 1);
    CHECK(VERTEX_LAYOUT::location(VERTEX_SEMANTIC_TANGENT, 0) == 2);
    CHECK(VERTEX_LAYOUT::location(VERTEX_SEMANTIC_COLOR, 0) == 3);
    CHECK(VERTEX_LAYOUT::location(VERTEX_SEMANTIC_TEXCOORD, 0) == 4);
    CHECK(VERTEX_LAYOUT::location(VERTEX_SEMANTIC_TEXCOORD, 3) == 7);
    CHECK(VERTEX_LAYOUT::location(VERTEX_SEMANTIC_JOINTS, 0) == 8);
    CHECK(VERTEX_LAYOUT::location(VERTEX_SEMANTIC_WEIGHTS, 0) == 9);
    CHECK(VERTEX_LAYOUT::LOCATION_COUNT == 10);

    SUBCASE("pairs outside the convention have no location") {
        CHECK(VERTEX_LAYOUT::location(VERTEX_SEMANTIC_POSITION, 1) == VERTEX_LAYOUT::LOCATION_NONE);
        CHECK(VERTEX_LAYOUT::location(VERTEX_SEMANTIC_COLOR, 1) == VERTEX_LAYOUT::LOCATION_NONE);
        CHECK(VERTEX_LAYOUT::location(VERTEX_SEMANTIC_TEXCOORD, VERTEX_LAYOUT::MAX_TEXCOORDS) == VERTEX_LAYOUT::LOCATION_NONE);
        CHECK(VERTEX_LAYOUT::location(99, 0) == VERTEX_LAYOUT::LOCATION_NONE);
    }
    SUBCASE("every location is distinct") {
        const u32 locations[] = {
            VERTEX_LAYOUT::location(VERTEX_SEMANTIC_POSITION, 0), VERTEX_LAYOUT::location(VERTEX_SEMANTIC_NORMAL, 0),
            VERTEX_LAYOUT::location(VERTEX_SEMANTIC_TANGENT, 0),  VERTEX_LAYOUT::location(VERTEX_SEMANTIC_COLOR, 0),
            VERTEX_LAYOUT::location(VERTEX_SEMANTIC_TEXCOORD, 0), VERTEX_LAYOUT::location(VERTEX_SEMANTIC_TEXCOORD, 1),
            VERTEX_LAYOUT::location(VERTEX_SEMANTIC_TEXCOORD, 2), VERTEX_LAYOUT::location(VERTEX_SEMANTIC_TEXCOORD, 3),
            VERTEX_LAYOUT::location(VERTEX_SEMANTIC_JOINTS, 0),   VERTEX_LAYOUT::location(VERTEX_SEMANTIC_WEIGHTS, 0),
        };
        for (u32 i = 0; i < 10; ++i) {
            CHECK(locations[i] < VERTEX_LAYOUT::LOCATION_COUNT);
            for (u32 j = i + 1; j < 10; ++j) {
                CHECK(locations[i] != locations[j]);
            }
        }
    }
}

TEST_CASE("asset/vertex_layout: semantic_name covers every semantic") {
    CHECK(strcmp(VERTEX_LAYOUT::semantic_name(VERTEX_SEMANTIC_POSITION), "POSITION") == 0);
    CHECK(strcmp(VERTEX_LAYOUT::semantic_name(VERTEX_SEMANTIC_TEXCOORD), "TEXCOORD") == 0);
    CHECK(strcmp(VERTEX_LAYOUT::semantic_name(VERTEX_SEMANTIC_WEIGHTS), "WEIGHTS") == 0);
    CHECK(strcmp(VERTEX_LAYOUT::semantic_name(1234), "UNKNOWN") == 0);
}

TEST_CASE("asset/vertex_layout: hash is a function of the layout, not of the mesh") {
    VertexStreamDesc streams[2] = {{32, 0}, {8, 0}};
    VertexAttributeDesc attributes[3] = {
        {VERTEX_SEMANTIC_POSITION, 0, VERTEX_FORMAT_F32x3, 0, 0},
        {VERTEX_SEMANTIC_NORMAL, 0, VERTEX_FORMAT_F32x3, 0, 12},
        {VERTEX_SEMANTIC_TEXCOORD, 0, VERTEX_FORMAT_F32x2, 1, 0},
    };
    const u64 base = VERTEX_LAYOUT::hash(streams, 2, attributes, 3);

    // A copy of the same tables hashes the same.
    VertexStreamDesc streams_copy[2];
    VertexAttributeDesc attributes_copy[3];
    memcpy(streams_copy, streams, sizeof(streams));
    memcpy(attributes_copy, attributes, sizeof(attributes));
    CHECK(VERTEX_LAYOUT::hash(streams_copy, 2, attributes_copy, 3) == base);

    SUBCASE("a different stride changes it") {
        streams[0].stride = 36;
        CHECK(VERTEX_LAYOUT::hash(streams, 2, attributes, 3) != base);
    }
    SUBCASE("a different offset changes it") {
        attributes[1].offset = 16;
        CHECK(VERTEX_LAYOUT::hash(streams, 2, attributes, 3) != base);
    }
    SUBCASE("a different format changes it") {
        attributes[1].format = VERTEX_FORMAT_SNORM8x4;
        CHECK(VERTEX_LAYOUT::hash(streams, 2, attributes, 3) != base);
    }
    SUBCASE("a different stream assignment changes it") {
        attributes[2].stream = 0;
        CHECK(VERTEX_LAYOUT::hash(streams, 2, attributes, 3) != base);
    }
    SUBCASE("fewer attributes changes it") {
        CHECK(VERTEX_LAYOUT::hash(streams, 2, attributes, 2) != base);
    }
    SUBCASE("the reserved stream word does not take part") {
        streams[1].reserved = 7;
        CHECK(VERTEX_LAYOUT::hash(streams, 2, attributes, 3) == base);
    }
}
