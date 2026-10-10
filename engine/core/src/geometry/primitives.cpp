#include "engine/geometry/primitives.hpp"

#include "engine/math/math.hpp"
#include "engine/memory/temporal_allocator.hpp"
#include "engine/templates/dynamic_array.hpp"

#include <cmath>

namespace {

// One vertex of the interleaved stream: 32 bytes, what the OBJ importer
// writes too.
struct PrimitiveVertex {
    f32 position[3];
    f32 normal[3];
    f32 texcoord[2];
};

static_assert(sizeof(PrimitiveVertex) == 32, "the primitive stream is 32 bytes per vertex");

constexpr f32 HALF = 0.5f;

PrimitiveTessellation clamped(const PrimitiveTessellation& tessellation) {
    PrimitiveTessellation out = tessellation;
    if (out.segments < 3) {
        out.segments = 3;
    }
    if (out.rings < 2) {
        out.rings = 2;
    }
    return out;
}

void push_vertex(DynamicArray<PrimitiveVertex>& vertices, const f32 px, const f32 py, const f32 pz, const f32 nx, const f32 ny, const f32 nz, const f32 u,
    const f32 v) {
    PrimitiveVertex vertex;
    vertex.position[0] = px;
    vertex.position[1] = py;
    vertex.position[2] = pz;
    vertex.normal[0] = nx;
    vertex.normal[1] = ny;
    vertex.normal[2] = nz;
    vertex.texcoord[0] = u;
    vertex.texcoord[1] = v;
    vertices.push(vertex);
}

void push_triangle(DynamicArray<u32>& indices, const u32 a, const u32 b, const u32 c) {
    indices.push(a);
    indices.push(b);
    indices.push(c);
}

// Two counter-clockwise triangles for the quad with `a` top-left, `b`
// top-right, `c` bottom-left and `d` bottom-right as seen from outside.
void push_quad(DynamicArray<u32>& indices, const u32 a, const u32 b, const u32 c, const u32 d) {
    push_triangle(indices, a, c, d);
    push_triangle(indices, a, d, b);
}

// --- Cube ----------------------------------------------------------------------

// Four vertices per face so every face has its own normal, 24 in all. Each
// face lists bottom-left, bottom-right, top-right, top-left seen from
// outside, which is counter-clockwise; the uvs wrap the face once with v = 0
// at the top.
constexpr f32 CUBE_VERTICES[24 * 8] = {
    // front (+Z)
    -HALF, -HALF, HALF, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, HALF, -HALF, HALF, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, //
    HALF, HALF, HALF, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, -HALF, HALF, HALF, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    // back (-Z)
    HALF, -HALF, -HALF, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, -HALF, -HALF, -HALF, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f, //
    -HALF, HALF, -HALF, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f, HALF, HALF, -HALF, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f,
    // left (-X)
    -HALF, -HALF, -HALF, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, -HALF, -HALF, HALF, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, //
    -HALF, HALF, HALF, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f, -HALF, HALF, -HALF, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    // right (+X)
    HALF, -HALF, HALF, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, HALF, -HALF, -HALF, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, //
    HALF, HALF, -HALF, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, HALF, HALF, HALF, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    // top (+Y)
    -HALF, HALF, HALF, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, HALF, HALF, HALF, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, //
    HALF, HALF, -HALF, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, -HALF, HALF, -HALF, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
    // bottom (-Y)
    -HALF, -HALF, -HALF, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f, HALF, -HALF, -HALF, 0.0f, -1.0f, 0.0f, 1.0f, 1.0f, //
    HALF, -HALF, HALF, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, -HALF, -HALF, HALF, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f,
};

// Two triangles per face, the face's four vertices in order.
constexpr u32 CUBE_INDICES[36] = {
    0, 1, 2, 2, 3, 0,       // front
    4, 5, 6, 6, 7, 4,       // back
    8, 9, 10, 10, 11, 8,    // left
    12, 13, 14, 14, 15, 12, // right
    16, 17, 18, 18, 19, 16, // top
    20, 21, 22, 22, 23, 20, // bottom
};

void build_cube(DynamicArray<PrimitiveVertex>& vertices, DynamicArray<u32>& indices) {
    for (u32 v = 0; v < 24; ++v) {
        const f32* row = CUBE_VERTICES + v * 8;
        push_vertex(vertices, row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7]);
    }
    for (u32 i = 0; i < 36; ++i) {
        indices.push(CUBE_INDICES[i]);
    }
}

// --- Sphere --------------------------------------------------------------------

// A UV sphere: rings + 1 rows of segments + 1 vertices (the seam column is
// doubled so u can run 0..1), rows from the top pole down. The pole rows
// collapse to one point each, so the quads touching them lose their
// degenerate triangle.
void build_sphere(DynamicArray<PrimitiveVertex>& vertices, DynamicArray<u32>& indices, const PrimitiveTessellation& t) {
    const u32 columns = t.segments + 1;
    for (u32 ring = 0; ring <= t.rings; ++ring) {
        const f32 v = static_cast<f32>(ring) / static_cast<f32>(t.rings);
        const f32 phi = MATH::PI * v;
        const f32 sin_phi = std::sin(phi);
        const f32 cos_phi = std::cos(phi);
        for (u32 segment = 0; segment <= t.segments; ++segment) {
            const f32 u = static_cast<f32>(segment) / static_cast<f32>(t.segments);
            const f32 theta = MATH::TAU * u;
            const f32 nx = sin_phi * std::sin(theta);
            const f32 ny = cos_phi;
            const f32 nz = sin_phi * std::cos(theta);
            push_vertex(vertices, nx * HALF, ny * HALF, nz * HALF, nx, ny, nz, u, v);
        }
    }
    for (u32 ring = 0; ring < t.rings; ++ring) {
        for (u32 segment = 0; segment < t.segments; ++segment) {
            const u32 a = ring * columns + segment;
            const u32 b = a + 1;
            const u32 c = a + columns;
            const u32 d = c + 1;
            if (ring + 1 < t.rings) {
                push_triangle(indices, a, c, d);
            }
            if (ring > 0) {
                push_triangle(indices, a, d, b);
            }
        }
    }
}

// --- Cylinder ------------------------------------------------------------------

// The side is one quad strip with radial normals (two rows, the seam
// doubled like the sphere's); each cap is a fan around a center vertex
// with its own rim vertices so the normal can be axial.
void build_cylinder(DynamicArray<PrimitiveVertex>& vertices, DynamicArray<u32>& indices, const PrimitiveTessellation& t) {
    const u32 columns = t.segments + 1;
    // Side: row 0 is the top edge.
    for (u32 row = 0; row < 2; ++row) {
        const f32 y = row == 0 ? HALF : -HALF;
        for (u32 segment = 0; segment <= t.segments; ++segment) {
            const f32 u = static_cast<f32>(segment) / static_cast<f32>(t.segments);
            const f32 theta = MATH::TAU * u;
            const f32 nx = std::sin(theta);
            const f32 nz = std::cos(theta);
            push_vertex(vertices, nx * HALF, y, nz * HALF, nx, 0.0f, nz, u, static_cast<f32>(row));
        }
    }
    for (u32 segment = 0; segment < t.segments; ++segment) {
        const u32 a = segment;
        const u32 b = a + 1;
        const u32 c = a + columns;
        const u32 d = c + 1;
        push_quad(indices, a, b, c, d);
    }
    // Caps: the fan runs with theta for the top (seen from +Y that is
    // counter-clockwise) and against it for the bottom.
    for (u32 cap = 0; cap < 2; ++cap) {
        const f32 y = cap == 0 ? HALF : -HALF;
        const f32 ny = cap == 0 ? 1.0f : -1.0f;
        const u32 center = static_cast<u32>(vertices.count);
        push_vertex(vertices, 0.0f, y, 0.0f, 0.0f, ny, 0.0f, 0.5f, 0.5f);
        for (u32 segment = 0; segment <= t.segments; ++segment) {
            const f32 theta = MATH::TAU * static_cast<f32>(segment) / static_cast<f32>(t.segments);
            const f32 sx = std::sin(theta);
            const f32 cz = std::cos(theta);
            push_vertex(vertices, sx * HALF, y, cz * HALF, 0.0f, ny, 0.0f, 0.5f + 0.5f * sx, 0.5f - 0.5f * cz * ny);
        }
        for (u32 segment = 0; segment < t.segments; ++segment) {
            const u32 rim = center + 1 + segment;
            if (cap == 0) {
                push_triangle(indices, center, rim, rim + 1);
            } else {
                push_triangle(indices, center, rim + 1, rim);
            }
        }
    }
}

} // namespace

// --- API -----------------------------------------------------------------------

const char* PRIMITIVES::name(const PrimitiveShape shape) {
    switch (shape) {
    case PRIMITIVE_CUBE:
        return "cube";
    case PRIMITIVE_SPHERE:
        return "sphere";
    case PRIMITIVE_CYLINDER:
        return "cylinder";
    default:
        return "unknown";
    }
}

bool PRIMITIVES::is_valid(const PrimitiveShape shape) {
    return static_cast<u32>(shape) < PRIMITIVE_COUNT;
}

u32 PRIMITIVES::vertex_count(const PrimitiveShape shape, const PrimitiveTessellation& tessellation) {
    const PrimitiveTessellation t = clamped(tessellation);
    switch (shape) {
    case PRIMITIVE_CUBE:
        return 24;
    case PRIMITIVE_SPHERE:
        return (t.rings + 1) * (t.segments + 1);
    case PRIMITIVE_CYLINDER:
        // Side rows plus two caps of center + rim.
        return 2 * (t.segments + 1) + 2 * (t.segments + 2);
    default:
        return 0;
    }
}

u32 PRIMITIVES::index_count(const PrimitiveShape shape, const PrimitiveTessellation& tessellation) {
    const PrimitiveTessellation t = clamped(tessellation);
    switch (shape) {
    case PRIMITIVE_CUBE:
        return 36;
    case PRIMITIVE_SPHERE:
        // Two triangles per quad except one at each pole row.
        return t.segments * (2 * t.rings - 2) * 3;
    case PRIMITIVE_CYLINDER:
        return t.segments * 6 + 2 * t.segments * 3;
    default:
        return 0;
    }
}

MeshWriteError PRIMITIVES::build(const PrimitiveShape shape, MeshAssetWriter& out, const PrimitiveTessellation& tessellation) {
    if (!is_valid(shape)) {
        return MESH_WRITE_BAD_SOURCE;
    }
    const PrimitiveTessellation t = clamped(tessellation);

    TemporalAllocator temp = TemporalAllocator::create();
    DynamicArray<PrimitiveVertex> vertices(&temp);
    DynamicArray<u32> indices(&temp);
    vertices.reserve(vertex_count(shape, t));
    indices.reserve(index_count(shape, t));
    switch (shape) {
    case PRIMITIVE_CUBE:
        build_cube(vertices, indices);
        break;
    case PRIMITIVE_SPHERE:
        build_sphere(vertices, indices, t);
        break;
    case PRIMITIVE_CYLINDER:
        build_cylinder(vertices, indices, t);
        break;
    default:
        break;
    }

    MeshSource source;
    source.vertex_count = static_cast<u32>(vertices.count);
    source.stream_count = 1;
    source.streams[0].stride = sizeof(PrimitiveVertex);
    source.streams[0].vertices = vertices.data;
    source.attribute_count = 3;
    source.attributes[0] = {VERTEX_SEMANTIC_POSITION, 0, VERTEX_FORMAT_F32x3, 0, 0};
    source.attributes[1] = {VERTEX_SEMANTIC_NORMAL, 0, VERTEX_FORMAT_F32x3, 0, 12};
    source.attributes[2] = {VERTEX_SEMANTIC_TEXCOORD, 0, VERTEX_FORMAT_F32x2, 0, 24};
    source.indices = indices.data;
    source.index_count = static_cast<u32>(indices.count);
    const MeshWriteError result = out.build(source, MeshImportOptions{});
    vertices.free();
    indices.free();
    return result;
}
