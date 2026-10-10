#include "support/test_support.hpp"

#include "engine/asset/asset_types/mesh_asset.hpp"
#include "engine/geometry/primitives.hpp"
#include "engine/math/math.hpp"

#include <cmath>
#include <cstring>

namespace {

constexpr PrimitiveShape SHAPES[PRIMITIVE_COUNT] = {PRIMITIVE_CUBE, PRIMITIVE_SPHERE, PRIMITIVE_CYLINDER};

struct BuiltVertex {
    f32 position[3];
    f32 normal[3];
    f32 texcoord[2];
};

// The interleaved stream the generator writes, as vertices.
const BuiltVertex* vertices_of(const MeshAssetWriter& mesh) {
    return reinterpret_cast<const BuiltVertex*>(mesh.stream_data(0));
}

// The writer compacts indices to 16 bits when they fit; read either width.
u32 index_at(const MeshAssetWriter& mesh, const u32 i) {
    if (mesh.desc.index_format == MESH_INDEX_U16) {
        u16 value;
        memcpy(&value, mesh.indices.data + i * 2, sizeof(value));
        return value;
    }
    u32 value;
    memcpy(&value, mesh.indices.data + i * 4, sizeof(value));
    return value;
}

Vector3 position_of(const BuiltVertex& v) {
    return Vector3(v.position[0], v.position[1], v.position[2]);
}

Vector3 normal_of(const BuiltVertex& v) {
    return Vector3(v.normal[0], v.normal[1], v.normal[2]);
}

} // namespace

TEST_CASE("geometry/primitives: names and validity") {
    CHECK(strcmp(PRIMITIVES::name(PRIMITIVE_CUBE), "cube") == 0);
    CHECK(strcmp(PRIMITIVES::name(PRIMITIVE_SPHERE), "sphere") == 0);
    CHECK(strcmp(PRIMITIVES::name(PRIMITIVE_CYLINDER), "cylinder") == 0);
    CHECK(strcmp(PRIMITIVES::name(PRIMITIVE_COUNT), "unknown") == 0);
    CHECK(PRIMITIVES::is_valid(PRIMITIVE_CYLINDER));
    CHECK_FALSE(PRIMITIVES::is_valid(PRIMITIVE_COUNT));
    CHECK_FALSE(PRIMITIVES::is_valid(static_cast<PrimitiveShape>(99)));
}

TEST_CASE("geometry/primitives: an unknown shape does not build") {
    MeshAssetWriter mesh;
    CHECK(PRIMITIVES::build(PRIMITIVE_COUNT, mesh) == MESH_WRITE_BAD_SOURCE);
    CHECK_FALSE(mesh.is_built());
    CHECK(PRIMITIVES::vertex_count(PRIMITIVE_COUNT) == 0);
    CHECK(PRIMITIVES::index_count(PRIMITIVE_COUNT) == 0);
    mesh.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("geometry/primitives: every shape builds with the documented layout and counts") {
    for (const PrimitiveShape shape : SHAPES) {
        CAPTURE(PRIMITIVES::name(shape));
        MeshAssetWriter mesh;
        REQUIRE(PRIMITIVES::build(shape, mesh) == MESH_WRITE_OK);
        CHECK(mesh.desc.vertex_count == PRIMITIVES::vertex_count(shape));
        CHECK(mesh.desc.index_count == PRIMITIVES::index_count(shape));
        CHECK(mesh.desc.index_count % 3 == 0);
        CHECK(mesh.desc.stream_count == 1);
        CHECK(mesh.streams[0].stride == 32);
        CHECK(mesh.desc.attribute_count == 3);
        CHECK(mesh.attributes[0].semantic == VERTEX_SEMANTIC_POSITION);
        CHECK(mesh.attributes[0].format == VERTEX_FORMAT_F32x3);
        CHECK(mesh.attributes[0].offset == 0);
        CHECK(mesh.attributes[1].semantic == VERTEX_SEMANTIC_NORMAL);
        CHECK(mesh.attributes[1].format == VERTEX_FORMAT_F32x3);
        CHECK(mesh.attributes[1].offset == 12);
        CHECK(mesh.attributes[2].semantic == VERTEX_SEMANTIC_TEXCOORD);
        CHECK(mesh.attributes[2].format == VERTEX_FORMAT_F32x2);
        CHECK(mesh.attributes[2].offset == 24);
        CHECK(mesh.desc.submesh_count == 1);
        CHECK(mesh.submeshes[0].first_index == 0);
        CHECK(mesh.submeshes[0].index_count == mesh.desc.index_count);
        mesh.free();
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("geometry/primitives: the cube is 24 vertices and 12 triangles") {
    CHECK(PRIMITIVES::vertex_count(PRIMITIVE_CUBE) == 24);
    CHECK(PRIMITIVES::index_count(PRIMITIVE_CUBE) == 36);
    // The cube ignores tessellation.
    PrimitiveTessellation fine;
    fine.segments = 64;
    fine.rings = 64;
    CHECK(PRIMITIVES::vertex_count(PRIMITIVE_CUBE, fine) == 24);
}

TEST_CASE("geometry/primitives: tessellation is clamped to its minimums") {
    PrimitiveTessellation tiny;
    tiny.segments = 1;
    tiny.rings = 0;
    // 3 segments, 2 rings: 4 columns x 3 rows; two rings of 3 quads with
    // one triangle each (both rows touch a pole).
    CHECK(PRIMITIVES::vertex_count(PRIMITIVE_SPHERE, tiny) == 12);
    CHECK(PRIMITIVES::index_count(PRIMITIVE_SPHERE, tiny) == 3 * 2 * 3);
    // Side 2 x 4, caps 2 x 5; side 3 quads, caps 3 triangles each.
    CHECK(PRIMITIVES::vertex_count(PRIMITIVE_CYLINDER, tiny) == 18);
    CHECK(PRIMITIVES::index_count(PRIMITIVE_CYLINDER, tiny) == 3 * 6 + 2 * 3 * 3);

    MeshAssetWriter mesh;
    REQUIRE(PRIMITIVES::build(PRIMITIVE_SPHERE, mesh, tiny) == MESH_WRITE_OK);
    CHECK(mesh.desc.vertex_count == 12);
    CHECK(mesh.desc.index_count == 18);
    mesh.free();
    CHECK_ARENA_CLEAN();
}

TEST_CASE("geometry/primitives: every shape fits the unit box and reports it as bounds") {
    for (const PrimitiveShape shape : SHAPES) {
        CAPTURE(PRIMITIVES::name(shape));
        MeshAssetWriter mesh;
        REQUIRE(PRIMITIVES::build(shape, mesh) == MESH_WRITE_OK);
        const BuiltVertex* vertices = vertices_of(mesh);
        for (u32 i = 0; i < mesh.desc.vertex_count; ++i) {
            for (u32 axis = 0; axis < 3; ++axis) {
                CHECK(vertices[i].position[axis] >= -0.5f - 1e-6f);
                CHECK(vertices[i].position[axis] <= 0.5f + 1e-6f);
            }
        }
        for (u32 axis = 0; axis < 3; ++axis) {
            // The top of a sphere with an even ring count is exactly a pole,
            // its equator exactly on the box; the odd-segment directions
            // never reach +X exactly, so the X extents may fall slightly
            // inside with few segments. Default tessellation hits them.
            CHECK(mesh.bounds.min[axis] == doctest::Approx(-0.5f).epsilon(0.01));
            CHECK(mesh.bounds.max[axis] == doctest::Approx(0.5f).epsilon(0.01));
            CHECK(mesh.bounds.center[axis] == doctest::Approx(0.0f).epsilon(0.01));
        }
        mesh.free();
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("geometry/primitives: normals are unit length and point away from the origin") {
    for (const PrimitiveShape shape : SHAPES) {
        CAPTURE(PRIMITIVES::name(shape));
        MeshAssetWriter mesh;
        REQUIRE(PRIMITIVES::build(shape, mesh) == MESH_WRITE_OK);
        const BuiltVertex* vertices = vertices_of(mesh);
        for (u32 i = 0; i < mesh.desc.vertex_count; ++i) {
            const Vector3 normal = normal_of(vertices[i]);
            CHECK(normal.length() == doctest::Approx(1.0f).epsilon(1e-4));
            // Every shape is convex and centered: an outward normal makes a
            // non-negative angle with the position (zero only at a cap
            // center, which sits on the axis).
            CHECK(MATH::dot(normal, position_of(vertices[i])) >= -1e-5f);
        }
        mesh.free();
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("geometry/primitives: triangles wind counter-clockwise seen from outside") {
    for (const PrimitiveShape shape : SHAPES) {
        CAPTURE(PRIMITIVES::name(shape));
        MeshAssetWriter mesh;
        REQUIRE(PRIMITIVES::build(shape, mesh) == MESH_WRITE_OK);
        const BuiltVertex* vertices = vertices_of(mesh);
        u32 degenerate = 0;
        for (u32 i = 0; i < mesh.desc.index_count; i += 3) {
            const BuiltVertex& a = vertices[index_at(mesh, i)];
            const BuiltVertex& b = vertices[index_at(mesh, i + 1)];
            const BuiltVertex& c = vertices[index_at(mesh, i + 2)];
            const Vector3 pa = position_of(a);
            const Vector3 pb = position_of(b);
            const Vector3 pc = position_of(c);
            const Vector3 face = MATH::cross(pb - pa, pc - pa);
            if (face.length() < 1e-7f) {
                ++degenerate;
                continue;
            }
            // Counter-clockwise from outside means the geometric normal
            // points away from the center of a convex, centered shape...
            const Vector3 centroid = (pa + pb + pc) * (1.0f / 3.0f);
            CHECK(MATH::dot(face, centroid) > 0.0f);
            // ...and agrees with the shading normals of its corners.
            CHECK(MATH::dot(face, normal_of(a)) > 0.0f);
            CHECK(MATH::dot(face, normal_of(b)) > 0.0f);
            CHECK(MATH::dot(face, normal_of(c)) > 0.0f);
        }
        // The sphere drops its pole triangles instead of emitting slivers.
        CHECK(degenerate == 0);
        mesh.free();
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("geometry/primitives: texcoords stay in 0..1") {
    for (const PrimitiveShape shape : SHAPES) {
        CAPTURE(PRIMITIVES::name(shape));
        MeshAssetWriter mesh;
        REQUIRE(PRIMITIVES::build(shape, mesh) == MESH_WRITE_OK);
        const BuiltVertex* vertices = vertices_of(mesh);
        for (u32 i = 0; i < mesh.desc.vertex_count; ++i) {
            CHECK(vertices[i].texcoord[0] >= -1e-6f);
            CHECK(vertices[i].texcoord[0] <= 1.0f + 1e-6f);
            CHECK(vertices[i].texcoord[1] >= -1e-6f);
            CHECK(vertices[i].texcoord[1] <= 1.0f + 1e-6f);
        }
        mesh.free();
    }
    CHECK_ARENA_CLEAN();
}

TEST_CASE("geometry/primitives: the sphere has radius 0.5 everywhere and the cylinder's side too") {
    MeshAssetWriter sphere;
    REQUIRE(PRIMITIVES::build(PRIMITIVE_SPHERE, sphere) == MESH_WRITE_OK);
    const BuiltVertex* vertices = vertices_of(sphere);
    for (u32 i = 0; i < sphere.desc.vertex_count; ++i) {
        CHECK(position_of(vertices[i]).length() == doctest::Approx(0.5f).epsilon(1e-4));
    }
    CHECK(sphere.bounds.radius == doctest::Approx(0.5f).epsilon(1e-3));
    sphere.free();

    PrimitiveTessellation t;
    MeshAssetWriter cylinder;
    REQUIRE(PRIMITIVES::build(PRIMITIVE_CYLINDER, cylinder, t) == MESH_WRITE_OK);
    vertices = vertices_of(cylinder);
    // The first 2 * (segments + 1) vertices are the side.
    for (u32 i = 0; i < 2 * (t.segments + 1); ++i) {
        const f32 radial = std::sqrt(vertices[i].position[0] * vertices[i].position[0] + vertices[i].position[2] * vertices[i].position[2]);
        CHECK(radial == doctest::Approx(0.5f).epsilon(1e-4));
        CHECK(std::fabs(vertices[i].position[1]) == doctest::Approx(0.5f));
        CHECK(vertices[i].normal[1] == 0.0f);
    }
    // Then the caps: every vertex of a cap shares its axial normal.
    for (u32 i = 2 * (t.segments + 1); i < cylinder.desc.vertex_count; ++i) {
        CHECK(std::fabs(vertices[i].normal[1]) == 1.0f);
        CHECK(vertices[i].normal[1] * vertices[i].position[1] > 0.0f);
    }
    cylinder.free();
    CHECK_ARENA_CLEAN();
}
